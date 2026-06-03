/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/jit_compiler.cpp
 * @brief Implementacion del orquestador JIT (IR SSA -> codigo nativo ejecutable).
 *
 * Pipeline completo en 4 fases (mismo orden que un compilador AOT normal,
 * solo que comprimido en una sola llamada para baja latencia):
 *
 *   1. **Selector** (instruction selector): transforma cada @c IrInstr SSA
 *      en una secuencia de @c MInstr de la MachineIR target-specific
 *      (x86-64 en v1).  Emite stackmaps que mapean cada safepoint a los
 *      slots del frame que contienen GcHandles vivos.
 *
 *   2. **Encoder** (x86 assembler hand-rolled): convierte MachineIR a
 *      bytes maquina aplicando el ABI x86-64.  Resuelve fixups internos
 *      (branches forward, references entre bloques) en una pasada final.
 *
 *   3. **CodeCache**: aloca un bloque de memoria RWX, copia los bytes
 *      generados, hace commit (transition a executable + flush icache).
 *
 *   4. **JitRegistry**: registra el rango del codigo + los stackmaps
 *      para que el GC pueda escanear los frames de esta funcion durante
 *      collections.
 *
 * Atomicidad: si CUALQUIER paso falla, devolvemos un @c CompileResult con
 * @c fn=nullptr y @c unsupported=true.  Ninguna operacion intermedia
 * tiene side-effects observables fuera (el code cache no se publica al
 * registry sin haber pasado por commit).  El caller puede caer al
 * interprete sin temer estado inconsistente.
 *
 * Coste tipico medido:
 *   - Funciones pequenas (<50 IR instrs): ~50-100 us.
 *   - Funciones medianas (200 IR instrs): ~200-400 us.
 *   - Funciones grandes (>500 IR instrs): ~1-3 ms.
 * El selector es el cuello dominante; el encoder y el cache son
 * proporcionalmente baratos.
 */

#include "jit/jit_compiler.h"

#include "ir/ssa_ir.h"
#include "jit/jit_registry.h"
#include "jit/machine_ir.h"
#include "jit/x86_encoder.h"

#include <cstring>
#include <utility>
#include <vector>

namespace jit {

    /**
     * @brief Entrypoint mas comun: compila con opciones default para
     *        @c mode (NATIVE_ABI para tests, VM_ABI para runtime real).
     *
     * Construye un @c SelectorOptions con el modo y, si el modo es VM_ABI
     * y hay safepoint handler resuelto, anyade su direccion para que el
     * selector emita las llamadas correctamente.  Despues delega en
     * @c compile_with_opts que hace el trabajo real.
     */
    CompileResult JitCompiler::compile(const ir::IrFunction &ir_fn,
                                       SelectorMode mode) noexcept {
        SelectorOptions opts;
        opts.mode = mode;
        // Pasar la tabla de runtime entries al selector para que pueda
        // resolver @c IrOp::CALL con func_name "vrt_*" a la direccion
        // real del runtime (en lugar de generar un symbol que el linker
        // tendria que resolver luego).
        opts.runtime = &rt_;
        // Solo si vamos a VM_ABI (codigo que correra como parte de un
        // proceso VestaVM normal) anyadimos la direccion del handler de
        // safepoint.  En NATIVE_ABI (tests aislados) los polls salen
        // como no-ops o se omiten.
        if (mode == SelectorMode::VM_ABI && rt_.safepoint_handler) {
            opts.safepoint_handler_addr =
                reinterpret_cast<uint64_t>(rt_.safepoint_handler);
        }
        return compile_with_opts(ir_fn, std::move(opts));
    }

    /**
     * @brief Ruta de compilacion con opciones custom (uso avanzado / tests).
     *
     * Implementa el pipeline 4-fase descrito en el doc del archivo.  Si
     * alguna fase falla:
     *   - Si fue el selector: @c result.unsupported queda en true;
     *     no se alocan bytes ni se registra nada.
     *   - Si fue el encoder o cache: @c result.fn permanece nullptr; el
     *     codigo ya alocado en el cache, si lo hubo, NO se devuelve (se
     *     queda como "wasted space" en el bump allocator; sera reusado
     *     solo si se llama @c invalidate, no automatico).
     *   - Solo si TODO funciona: @c result.fn = puntero al codigo
     *     ejecutable + se registra en el JitRegistry para el GC.
     */
    CompileResult JitCompiler::compile_with_opts(const ir::IrFunction &ir_fn,
                                                  SelectorOptions opts) noexcept {
        CompileResult result{};

        // ---------------------------------------------------------------
        // Fase 1: Selector (IR SSA -> MachineIR target-specific).
        // ---------------------------------------------------------------
        // El selector puede marcar @c unsupported si encuentra un IrOp
        // que aun no maneja (e.g. operaciones float especificas, ciertos
        // RAW_ASM no patterned).  En ese caso retornamos inmediatamente
        // dejando el flag visible para que el caller caiga al interprete.
        Selector sel(std::move(opts));
        MFunction mf = sel.select(ir_fn, &result.unsupported);
        if (result.unsupported) {
            return result;
        }

        // ---------------------------------------------------------------
        // Fase 2: Encoder (MachineIR -> bytes).
        // ---------------------------------------------------------------
        // El encoder consume MFunction y produce un vector<uint8_t> con
        // los bytes x86-64 ya con todos los fixups resueltos (branches
        // patcheados, immediates en su lugar).  Si la cuenta de
        // instrucciones es 0 o el vector queda vacio, el encoder fallo
        // (caso extremadamente raro: indica MFunction malformada).
        X86Encoder enc;
        std::vector<uint8_t> bytes;
        const size_t n = enc.encode(mf, bytes);
        if (n == 0 || bytes.empty()) {
            return result;  // result.fn sigue nullptr
        }
        // Reportar al caller cuantas instrs maquina se emitieron (para
        // metricas / debugging).  No coincide necesariamente con el
        // count de IR instrs por uno: una IR op puede expandir a varias
        // MInstrs y el encoder a un numero variable de bytes.
        result.instr_count = enc.instr_count();

        // ---------------------------------------------------------------
        // Fase 3: Code cache (publicacion en memoria ejecutable).
        // ---------------------------------------------------------------
        // alineamiento a 16 bytes: estandar x86-64 para garantizar que
        // ciertos opcodes (saltos cortos, NOPs alineados) funcionen y
        // que el branch predictor opere optimamente.
        uint8_t *code = cache_.alloc(bytes.size(), 16);
        if (!code) {
            // OOM del code cache (limit reached o reserve_chunk fallo).
            // Esto NO marca unsupported (el codigo se podria emitir, solo
            // que no cabe en el cache); el caller puede invalidar funciones
            // viejas y reintentar, o caer al interprete.
            return result;
        }
        // Copiar los bytes al cache.  No usamos un emit directo porque el
        // encoder no tiene visibilidad del cache (separation of concerns:
        // genera bytes en un buffer propio que despues copiamos).
        std::memcpy(code, bytes.data(), bytes.size());

        // Sprint fib-recursion (2026-06-02): patch de self-references.
        // El encoder anyadio a @c mf.self_ref_byte_offsets las posiciones
        // donde se emitio un MOV reg,imm64 con valor placeholder 0 que
        // referencia a la PROPIA funcion.  Ahora que conocemos la
        // direccion final del codigo (= @c code), escribimos esa
        // direccion en cada posicion -> los self-recursive calls
        // dispatchan directo en lugar de via trampoline JIT->interp.
        const uint64_t code_addr = reinterpret_cast<uint64_t>(code);
        for (size_t pos : mf.self_ref_byte_offsets) {
            if (pos + 8 <= bytes.size()) {
                std::memcpy(code + pos, &code_addr, 8);
            }
        }

        // Commit hace la transicion final RW -> RX (cuando soportemos W^X
        // en Phase E) y el flush de icache para que el CPU vea los
        // bytes nuevos en arquitecturas con icache no-coherente (ARM).
        cache_.commit(code, bytes.size());

        // Rellenar el resultado con los datos para que el caller pueda
        // invocar la funcion + invalidarla mas tarde si necesario.
        result.code_start = code;
        result.code_size  = bytes.size();
        // Reinterpret_cast a JitFn (puntero a funcion con signature
        // @c uint64_t fn(ProcessVM*)).  Es la convencion VM_ABI; en
        // NATIVE_ABI el caller debe re-castear al tipo real de la fn.
        result.fn         = reinterpret_cast<JitFn>(code);

        // ---------------------------------------------------------------
        // Fase 4: Registro en JitRegistry.
        // ---------------------------------------------------------------
        // El registry recibe la copia movida (std::move) de los stackmaps:
        // ya no necesitamos la MFunction tras esto.  Si la copia se omitiera,
        // los stackmaps se dropearian al salir del scope y el GC no
        // tendria info para escanear los frames de esta fn -> roots
        // perdidos -> objetos liberados prematuramente.
        JitRegistry::instance().register_function(
            code,
            code + bytes.size(),
            std::move(mf.stackmaps),
            mf.stack_frame_size,
            ir_fn.name.c_str());

        return result;
    }

    /**
     * @brief Invalida una funcion compilada (deopt o cleanup).
     *
     * Despues de @c invalidate, el codigo previamente devuelto por
     * @c compile queda no-funcional: cualquier intento de invocarlo
     * dispara @c INT3 (breakpoint trap).  El espacio NO se reutiliza
     * automaticamente (el bump allocator no lo libera).
     *
     * Orden de operaciones critico:
     *   1. Primero @c unregister_function: asi si una GC pase ocurre
     *      durante la invalidacion, NO ve esta funcion como activa y
     *      no intenta escanear sus stackmaps (que estan en proceso de
     *      ser invalidados).
     *   2. Despues @c cache_.invalidate: rellena con INT3 (0xCC) y
     *      flush icache para garantizar que ningun thread ejecuta los
     *      bytes viejos cacheados en su icache.
     */
    void JitCompiler::invalidate(const CompileResult &res) noexcept {
        // Defensa: si la funcion nunca se compilo correctamente, nada
        // que hacer.  Esto permite @c invalidate(result) seguro sin
        // checks por parte del caller incluso si compile fallo.
        if (!res.code_start) return;
        // Primero del registry (atomico vs GC scan): el GC empieza con
        // un snapshot del registry; quitarla aqui evita que un scan en
        // curso vea bytes invalidados.
        JitRegistry::instance().unregister_function(res.code_start);
        // Rellenar con 0xCC = INT3.  Si algun thread aun tiene un return
        // address apuntando al codigo invalidado (no deberia, pero el
        // mundo concurrente da sorpresas), el return crash-eara
        // controladamente con SIGTRAP / EXCEPTION_BREAKPOINT en lugar de
        // ejecutar codigo random.
        cache_.invalidate(const_cast<uint8_t *>(res.code_start), res.code_size);
    }

} // namespace jit
