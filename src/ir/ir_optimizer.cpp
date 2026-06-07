/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file ir_optimizer.cpp
 * @brief Implementacion de los pases de optimizacion sobre la SSA IR.
 */

#include "ir/ir_optimizer.h"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <queue>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <limits>
#include <functional>
#include <sstream>
#include <algorithm>

namespace ir {

/* Helpers locales para constant folding de math IR ops (Math-IR-promote
 * v2.2c).  Preservan bits IEEE 754 via memcpy para no perder precision
 * en el round-trip uint64 <-> double que el IR usa. */
namespace {
    inline double  bits_to_f64(uint64_t b) noexcept {
        double d; std::memcpy(&d, &b, sizeof(d)); return d;
    }
    inline uint64_t f64_to_bits(double d) noexcept {
        uint64_t b; std::memcpy(&b, &d, sizeof(b)); return b;
    }
    inline float   bits_to_f32(uint32_t b) noexcept {
        float f; std::memcpy(&f, &b, sizeof(f)); return f;
    }
    inline uint32_t f32_to_bits(float f) noexcept {
        uint32_t b; std::memcpy(&b, &f, sizeof(b)); return b;
    }
}  // namespace

// =========================================================================
//  Utilidades internas
// =========================================================================

/** @brief Devuelve true si la instruccion tiene efectos laterales visibles. */
static bool is_side_effecting(IrOp op) {
    switch (op) {
        // llamadas (pueden lanzar excepciones o modificar estado)
        case IrOp::CALL:    case IrOp::CALLIND: case IrOp::CALLVIRT:
        case IrOp::CALLN:   case IrOp::TAILCALL: case IrOp::CALLM:
        case IrOp::CALLCLOSURE:
        // para C2 (escape analysis + case-splitting);
        // NUNCA eliminar aunque dst sea IR_NO_VALUE.  Sin efecto en codegen
        // (emitter los trata como no-op), pero deben sobrevivir DCE.
        case IrOp::MAKE_CLOSURE:
        case IrOp::MAKE_VARIANT:
        case IrOp::MATCH_VARIANT:
        // control de flujo
        case IrOp::BR:      case IrOp::BR_COND: case IrOp::RET:
        case IrOp::UNREACHABLE:
        // excepciones
        case IrOp::THROW:   case IrOp::TRYENTER: case IrOp::TRYLEAVE:
        case IrOp::LANDINGPAD:
        // async / distribucion
        // BugFix audit B-future: FUTURE aloca una NUEVA entry en
        // vm.shared_futures cada vez.  Sin marcarlo side-effecting,
        // CSE deduplica multiples `future` ops creyendo que producen
        // el mismo valor, lo que rompe @Async chain (todos los awaits
        // se quedan esperando el MISMO future fulfilled una sola vez).
        case IrOp::FUTURE:
        case IrOp::AWAIT:   case IrOp::FULFILL:  case IrOp::REJECT:
        case IrOp::MSGSEND: case IrOp::MSGRECV:  case IrOp::RSPAWN:
        // monitores
        case IrOp::MONENTER: case IrOp::MONEXIT: case IrOp::MONWAIT:
        case IrOp::MONNOTI:  case IrOp::MONNOTA:
        // memoria
        case IrOp::STORE:   case IrOp::MEMCPY:   case IrOp::SETFIELD:
        // OOP con efectos
        case IrOp::NEWOBJ:  case IrOp::CHECKCAST: case IrOp::UNWRAP:
        case IrOp::SPECIALIZE:
        // GC_ALLOC: consume memoria del heap GC + puede disparar minor/major
        // GC + el payload puede ser referenciado posteriormente.  Tratarlo
        // como CALL evita que el DCE elimine el alloc cuando el dst es
        // temporariamente parecido a "no usado" en algun analisis local.
        case IrOp::GC_ALLOC:
        // arrays con efectos
        case IrOp::ARRAY_ALLOC: case IrOp::ARRAY_STORE: case IrOp::GCWB_IR:
        case IrOp::GCDEREF_IR:
        // raw_alloc/raw_free: cada alloc devuelve un host_ptr UNICO; no
        // se pueden deduplicar.  raw_free libera memoria (side-effect).
        case IrOp::RAW_ALLOC: case IrOp::RAW_FREE:
        // cadenas con efectos (alloc o mutacion)
        case IrOp::STRMAKE:   case IrOp::STRCAT:    case IrOp::STRCONV:
        case IrOp::STRFLAT:   case IrOp::STRINTERN: case IrOp::STRRESERVE:
        case IrOp::STRFINALIZE:
        // scheduler / proceso
        case IrOp::SPAWN:   case IrOp::RESUME:   case IrOp::YIELD:
        case IrOp::SWAPCTX: case IrOp::SPAWN_ARGS: case IrOp::SPAWN_ON:
        case IrOp::HLT:     case IrOp::PANIC:
        // asignacion
        case IrOp::ALLOCA:
        // recuperados fase B: lecturas/escrituras que consultan estado
        // global del runtime y NO pueden reordenarse contra los STOREs
        // que arman sus structs de parametros.  Tratarlos como llamadas.
        case IrOp::MVTAKE_IR:
        case IrOp::GC_ALLOCP:
        case IrOp::GC_PROMOTE: case IrOp::GC_DEMOTE:
        case IrOp::GC_HANDLE_FOR_PTR:
        // GC_DEREF_HOST: handle -> host_ptr.  Conservative: marked side-effecting
        // porque el host_ptr puede cambiar tras un major_gc (moving GC) y CSE
        // erroneamente fusionaria dos derefs separados por un CALL.  Cuando
        // llegue Phase D.8 con CSE block-aware con clobber model, se puede
        // relajar a "pure within block until next CALL/alloc".
        case IrOp::GC_DEREF_HOST:
        case IrOp::ATOMIC_LD_I64: case IrOp::ATOMIC_ST_I64:
        case IrOp::ATOMIC_CAS_I64: case IrOp::ATOMIC_ADD_I64:
        case IrOp::GETSTATIC: case IrOp::SETSTATIC:
        case IrOp::FINDCLASS: case IrOp::DEFCLASS:
        case IrOp::DEFFIELD:  case IrOp::DEFMETHOD:  case IrOp::ADDADVICE:
        case IrOp::FINDMETHOD: case IrOp::FINDFIELD:
        case IrOp::CALLSUPER:  case IrOp::PROCEED:
        case IrOp::FULFILL_HLT:
        case IrOp::STRGETBYTES:
        // Sprint edge-bugs (2026-06-02): ops que pueden lanzar FatalError
        // capturable o disparar AV recovery.  DCE no debe eliminarlas aunque
        // su dst quede no-usado: el side-effect (fault) es observable.  La
        // penalizacion en perf es modesta porque mem-CSE/constant-fold ya
        // simplifican casos seguros (e.g. DIV por constante != 0 se folde).
        case IrOp::LOAD:
        case IrOp::DIV: case IrOp::MOD:
        case IrOp::FDIV:
        // raw_asm-elim wave 3: nuevos ops con efecto observable.
        case IrOp::RETHROW:         // relanza excepcion (side-effect explicito)
        case IrOp::SHARED_STAT:     // op=2 (gc_collect) dispara STW + sweep
        case IrOp::READ_VM_REG:     // lee reg arbitrario; el optimizer no debe asumir purity
        case IrOp::RSPAWN_RETURN:   // mov r0 + hlt fusionado, terminator
        // raw_asm-elim wave 2: cleanup deterministico de smart pointers.
        case IrOp::SMARTPTR_FREE:   // invoca deleter (free/CALLN/CALLVM); side-effect
        // raw_asm-elim wave 2: reflexion queries (escriben a R0, consultan estado).
        case IrOp::REFLECT_COUNT:
        case IrOp::REFLECT_AT:
        // raw_asm-elim wave 2: FFI runtime ops (cargan/descargan DLLs, side-effects).
        case IrOp::MOD_LOAD:      // loadmod ejecuta main del plugin (call site)
        case IrOp::DLOPEN:        // LoadLibrary/dlopen (side-effect en OS)
        case IrOp::DLSYM:         // GetProcAddress/dlsym (lookup en DLL state)
        case IrOp::GETPID:  case IrOp::GETARGC: case IrOp::GETARG:
        // ensamblador incrustado (nunca eliminar; semantica opaca)
        case IrOp::RAW_ASM:
            return true;
        default:
            return false;
    }
}

/** @brief Devuelve true si la instruccion es un terminador de bloque. */
static bool is_terminator(IrOp op) {
    return op == IrOp::BR   || op == IrOp::BR_COND ||
           op == IrOp::RET  || op == IrOp::UNREACHABLE ||
           op == IrOp::THROW || op == IrOp::RETHROW ||
           op == IrOp::RSPAWN_RETURN || op == IrOp::FULFILL_HLT;
}

/** @brief Devuelve true si la instruccion es pura (apta para DCE y CSE). */
static bool is_pure(IrOp op) {
    return !is_side_effecting(op);
}

/**
 * @brief Sprint mem-perf string_hot (2026-06-02): ops "alloc-pure"
 * hoistables por LICM.
 *
 * Algunas operaciones de strings (STRMAKE, STRCAT, STRINTERN, STRCONV,
 * STRRESERVE) son side-effecting (alocan en GcHeap) PERO su semantica
 * solo depende del CONTENIDO de los operandos.  Es decir, dos invocaciones
 * con los mismos operandos producen StringObjects con identical bytes;
 * solo difieren en el handle (GcHandle es opaco al programador y los
 * builtins de string tipo STRLEN/STRCMP/STRRAW/STRGETBYTES operan sobre
 * bytes, no comparan handles).
 *
 * Por tanto LICM puede hoistar estas ops cuando sus operandos son
 * loop-invariant.  Beneficio masivo en patrones como
 *
 *   while (i < N) {
 *       string s = base + suffix;     // STRCAT con bases invariant
 *       if (str_equals(s, "lit")) {   // STRMAKE "lit" invariant
 *           ...
 *       }
 *   }
 *
 * donde la version sin hoist hace N allocs de ROPEs identicos; con hoist
 * 1 sola alloc.  El bench string_hot pasa de 393 ms a sub-100 ms en interp.
 *
 * NO incluye STRCMP/STRLEN/STRRAW/STRGETBYTES/STRHASH (esos NO alocan;
 * ya son @c is_pure y LICM los toma).  NO incluye STRFLAT/STRFINALIZE
 * (mutan estructuras compartidas).
 */
static bool is_licm_hoistable_alloc(IrOp op) {
    switch (op) {
        // STRCAT/STRINTERN/STRCONV/STRFLAT operan SOBRE HANDLES (GcHandles),
        // no leen memoria mutable; alocacion idempotente con resultado
        // content-equal por args.  Safe para hoist.
        case IrOp::STRCAT:
        case IrOp::STRINTERN:
        case IrOp::STRCONV:
        case IrOp::STRRESERVE:
            return true;
        // STRMAKE requiere chequeo extra (ver @c strmake_reads_immutable).
        case IrOp::STRMAKE:
            return true;
        default:
            return false;
    }
}

/**
 * @brief Verifica si el vm_addr operand de un STRMAKE viene de
 * memoria INMUTABLE (STR_LIT_ADDR -> static_data).
 *
 * Sprint string-perf-2 bug fix (2026-06-02): LICM solo puede hoistar
 * STRMAKE si el operand vm_addr apunta a memoria que NO muta dentro
 * del loop.  STR_LIT_ADDR retorna un offset al static_data del modulo
 * (read-only por convencion).  Cualquier otro origen (ALLOCA, malloc,
 * field load, etc.) puede ser mutable.
 *
 * Tracing simple: busca el IrOp que produce @p vm_addr.  Si es
 * STR_LIT_ADDR, safe.  Si es ADD entre STR_LIT_ADDR y CONST (offset
 * dentro del bloque literal), tambien safe.  Otros casos -> unsafe.
 */
static bool strmake_reads_immutable(const IrFunction &fn, IrValueId vm_addr) {
    if (vm_addr == IR_NO_VALUE || vm_addr >= fn.values.size()) return false;
    // Buscar el IrOp que define vm_addr.
    for (const auto &bb : fn.blocks) {
        for (const auto &ins : bb.instrs) {
            if (ins.dst != vm_addr) continue;
            switch (ins.op) {
                case IrOp::STR_LIT_ADDR:
                    return true;
                case IrOp::ADD:
                case IrOp::SUB: {
                    // ADD/SUB de STR_LIT_ADDR + const -> tambien immutable.
                    if (ins.operands.size() != 2) return false;
                    // Recurse en operand[0] (asumiendo que es la base ptr).
                    if (strmake_reads_immutable(fn, ins.operands[0])) {
                        // operand[1] debe ser CONST (offset literal).
                        IrValueId off = ins.operands[1];
                        if (off < fn.values.size() && fn.values[off].is_const) {
                            return true;
                        }
                    }
                    return false;
                }
                case IrOp::BITCAST:
                case IrOp::MOV:
                    if (ins.operands.size() == 1) {
                        return strmake_reads_immutable(fn, ins.operands[0]);
                    }
                    return false;
                default:
                    return false;
            }
        }
    }
    return false;
}

// =========================================================================
//  Pase DCE (Dead Code Elimination)
// =========================================================================

// =========================================================================
//  Pase Dead Alloc Elimination
// =========================================================================
//
// Elimina CALLs a funciones synthetic @c __new_<X> (helpers de allocacion
// emitidos por el frontend Vex para @c new ClassName()) cuyo resultado
// nunca se usa.  Estos calls solo hacen @c newobj + @c gcderef + @c mov
// internos y no tienen otros efectos secundarios observables salvo
// presion GC -- que es aceptable eliminar para casos no referenciados.
//
// Patron tipico:
//   pruebas() { Calculadora a = new Calculadora(); return 0; }
//   IR:
//     %0 = call.ptr @__new_Calculadora()
//     %1 = const.i64 0
//     %2 = trunc.i32 %1
//     ret.i32 %2
// %0 nunca se usa.  La call a @c __new_Calculadora se puede eliminar.
//
// Tambien aplica a calls a @c vrt_newobj y otros allocators puros.

/** @brief True si el nombre identifica un allocator puro (sin efectos
 *  observables salvo presion GC). */
static bool is_pure_allocator_name(const std::string &name) {
    /* Allocs SHARED (@c __new_<X>_shared) NO son puros: registran el objeto en
     * la @c SharedHandleTable, un efecto OBSERVABLE via @c shared_heap_live_count().
     * Eliminar un @c new shared X() no usado cambiaria el conteo de objetos
     * shared vivos (regresion del bug de 167_z_gc_sweep: con DCE, crear 6 shared
     * sin usar 4 dejaba before=2 en vez de 6).  Debe ir ANTES del check __new_. */
    if (name.size() >= 7
     && name.compare(name.size() - 7, 7, "_shared") == 0) return false;
    /* Frontend Vex emite @c __new_<ClassName> para cada @c new X(). */
    if (name.size() > 6 && name.compare(0, 6, "__new_") == 0) return true;
    /* Runtime entries de alloc puros. */
    if (name == "vrt_newobj") return true;
    if (name == "vrt_newobj_handle") return true;
    if (name == "vrt_register_alloc") return true;
    return false;
}

bool ir_pass_dead_alloc_elim(IrFunction &fn) {
    /* Pasada 1: encontrar valores usados (mismo que DCE). */
    std::unordered_set<IrValueId> used;
    for (const auto &bb : fn.blocks) {
        for (const auto &ins : bb.instrs) {
            for (IrValueId op : ins.operands) {
                if (op != IR_NO_VALUE) used.insert(op);
            }
            if ((ins.op == IrOp::CALLIND || ins.op == IrOp::CALLCLOSURE)
             && ins.func_ptr != IR_NO_VALUE) {
                used.insert(ins.func_ptr);
            }
            for (const auto &pa : ins.phi_args) {
                if (pa.value != IR_NO_VALUE) used.insert(pa.value);
            }
        }
    }

    /* Pasada 2: eliminar CALLs a allocators puros cuyo dst no se usa. */
    bool changed = false;
    for (auto &bb : fn.blocks) {
        auto &instrs = bb.instrs;
        size_t write = 0;
        for (size_t i = 0; i < instrs.size(); ++i) {
            const IrInstr &ins = instrs[i];
            bool keep = true;
            if (ins.op == IrOp::CALL
             && ins.dst != IR_NO_VALUE
             && !used.count(ins.dst)
             && !ins.preserve
             && is_pure_allocator_name(ins.func_name)) {
                /* CALL a allocator puro, resultado no usado -> eliminar.
                 * El frontend Vex no espera efectos secundarios visibles
                 * de @c new X() salvo el handle/host_ptr (que se descarta). */
                keep = false;
                changed = true;
            }
            if (keep) {
                if (write != i) instrs[write] = std::move(instrs[i]);
                ++write;
            }
        }
        instrs.resize(write);
    }
    return changed;
}

// =========================================================================
//  Pase ir_pass_promote_callned_allocas
//
//  Phase D.jit-mem-model AUTO-PROMOTE: detecta `&local` (ALLOCAs) que
//  fluyen a CALLN (funciones nativas).  Esos ALLOCAs SE PROMUEVEN a host
//  stack via marca `is_host_ptr=true` en el dst del ALLOCA.  El JIT
//  selector consulta esa marca y emite host stack en lugar de VM-stack.
//
//  Sin esta promocion, &local seria una VM-addr que la funcion nativa
//  trataria como host_ptr -> garbage/crash.  Con la promocion, &local
//  es un host_ptr genuino dereferenciable directamente por code C.
//
//  Permite escribir codigo natural C-style:
//
//      u8[1024] buf;
//      ReadFile(handle, &buf[0], 1024, &bytes_read, null);
//
//  El frontend NO necesita anotaciones del usuario (@host etc).  El
//  analisis es backward-flow desde args PTR de cada CALLN.
//
//  Algoritmo:
//    1. Forward seed: por cada CALLN, marca todos sus operands como
//       "reaches_calln".
//    2. Backward fix-point: si dst ya marcado, propagar a operands a
//       traves de ADD/SUB/BITCAST/MOV/CAST/SEXT/ZEXT/TRUNC/PHI/LOAD.
//    3. Final: ALLOCAs cuyo dst esta marcado -> set is_host_ptr=true.
//       Tambien propagar is_host_ptr forward por la cadena de uses para
//       que LOAD/STORE de pointers derivados emita native mov.
// =========================================================================

bool ir_pass_promote_callned_allocas(IrFunction &fn) {
    if (fn.is_native) return false;
    if (fn.values.empty()) return false;

    /* Step 1: detectar valores que llegan a args de CALLN. */
    std::vector<bool> reaches_calln(fn.values.size(), false);
    bool found_any_calln = false;
    for (const auto &blk : fn.blocks) {
        for (const auto &ins : blk.instrs) {
            if (ins.op == IrOp::CALLN) {
                found_any_calln = true;
                for (auto opv : ins.operands) {
                    if (opv != IR_NO_VALUE && opv < reaches_calln.size()) {
                        reaches_calln[opv] = true;
                    }
                }
            }
        }
    }
    if (!found_any_calln) return false;

    /* Step 2: backward fix-point a traves de ops ptr-arithmetic. */
    bool changed_bp = true;
    int max_iter = 16;
    while (changed_bp && max_iter-- > 0) {
        changed_bp = false;
        for (const auto &blk : fn.blocks) {
            for (const auto &ins : blk.instrs) {
                if (ins.dst == IR_NO_VALUE) continue;
                if (ins.dst >= reaches_calln.size()) continue;
                if (!reaches_calln[ins.dst]) continue;
                /* Propagar a operands segun op. */
                switch (ins.op) {
                    case IrOp::ADD:
                    case IrOp::SUB:
                    case IrOp::BITCAST:
                    case IrOp::MOV:
                    case IrOp::CAST:
                    case IrOp::SEXT:
                    case IrOp::ZEXT:
                    case IrOp::TRUNC:
                    case IrOp::LOAD:
                        for (auto opv : ins.operands) {
                            if (opv != IR_NO_VALUE && opv < reaches_calln.size()
                             && !reaches_calln[opv]) {
                                reaches_calln[opv] = true;
                                changed_bp = true;
                            }
                        }
                        break;
                    case IrOp::PHI:
                        for (const auto &pa : ins.phi_args) {
                            if (pa.value != IR_NO_VALUE && pa.value < reaches_calln.size()
                             && !reaches_calln[pa.value]) {
                                reaches_calln[pa.value] = true;
                                changed_bp = true;
                            }
                        }
                        break;
                    default: break;
                }
            }
        }
    }

    /* Step 3: marcar ALLOCAs cuyo dst alcanza CALLN con
     * `host_alloca=true` y recolectar sus dsts para insertar
     * `RAW_FREE` antes de cada RET de la funcion.  El interp YA respeta
     * `host_alloca` en su bytecode emit: emite `alloc N` (RAW_ALLOC
     * bytecode) en lugar de `subsp`, y el `free` correspondiente lo
     * provee el RAW_FREE insertado aqui. */
    bool changed = false;
    std::vector<IrValueId> promoted_dsts;
    for (auto &blk : fn.blocks) {
        for (auto &ins : blk.instrs) {
            if (ins.op == IrOp::ALLOCA
             && ins.dst != IR_NO_VALUE
             && ins.dst < reaches_calln.size()
             && reaches_calln[ins.dst]
             && !ins.host_alloca) {
                ins.host_alloca = true;
                promoted_dsts.push_back(ins.dst);
                changed = true;
            }
        }
    }

    /* Step 4 (post-leak-fix 2026-06-01): el cleanup en exit-points
     * ahora lo hace el runtime via `htrack` + cleanup automatico al
     * destruir el frame.  El bytecode emit del case ALLOCA emite
     * `htrack r_dst` tras el `alloc`; el frame guarda los ptrs en una
     * lista lazy y los libera en RET / do_throw / TAILCALL.
     *
     * Ventajas vs la version anterior (RAW_FREE inserted en IR):
     *   - Cubre THROW cross-frame correctamente (do_throw libera al
     *     pop frames durante unwind).
     *   - Cubre todos los exit paths sin enumerar IR ops (los exits
     *     del runtime son responsables del cleanup).
     *   - Sin transformaciones IR extra: el IR queda mas limpio. */

    /* Step 5: propagar is_host_ptr=true forward por las ops derivadas
     * (ADD/SUB/BITCAST/MOV/CAST/*EXT/TRUNC/PHI) desde el dst de cada
     * ALLOCA promovida.  Necesario para que LOAD/STORE downstream
     * emitan `movh` (host mem) en lugar de `mov` (vm mem).
     *
     * Ahora que el interp tambien respeta `host_alloca` y emite `alloc
     * N` (RAW_ALLOC bytecode) en lugar de `subsp` VM-stack, el ptr ES
     * host genuino: propagar es seguro y correcto tanto para interp
     * como para JIT. */
    if (!promoted_dsts.empty()) {
        /* Seed: marcar los dsts promovidos. */
        for (auto vid : promoted_dsts) {
            if (vid < fn.values.size()) {
                fn.values[vid].is_host_ptr = true;
            }
        }
        /* Fix-point forward propagation. */
        bool prop_changed = true;
        while (prop_changed) {
            prop_changed = false;
            for (auto &blk : fn.blocks) {
                for (auto &ins : blk.instrs) {
                    if (ins.dst == IR_NO_VALUE
                     || ins.dst >= fn.values.size()) continue;
                    auto &dst_v = fn.values[ins.dst];
                    if (dst_v.is_host_ptr) continue;
                    bool any_host = false;
                    auto check = [&](IrValueId v) {
                        if (v == IR_NO_VALUE
                         || v >= fn.values.size()) return;
                        if (fn.values[v].is_host_ptr) any_host = true;
                    };
                    switch (ins.op) {
                        case IrOp::ADD: case IrOp::SUB:
                        case IrOp::BITCAST: case IrOp::MOV:
                        case IrOp::CAST: case IrOp::SEXT:
                        case IrOp::ZEXT: case IrOp::TRUNC:
                            for (auto v : ins.operands) check(v);
                            break;
                        case IrOp::PHI:
                            for (auto &pa : ins.phi_args) check(pa.value);
                            break;
                        default: break;
                    }
                    if (any_host) {
                        dst_v.is_host_ptr = true;
                        prop_changed = true;
                    }
                }
            }
        }
    }

    return changed;
}

//==============================================================================
//  Sprint string-perf-8 (2026-06-02): ir_pass_promote_local_allocas
//
//  Promueve ALLOCAs LOCALES (no escapan al interp, no se pasan a CALL*,
//  no se almacenan en memoria heap) a `host_alloca=true`.  Esto permite
//  que el JIT emita `sub rsp, N` en host stack y los LOAD/STORE
//  derivados usen `mov [rbp+offset]` nativo (1 instr) en lugar del
//  inline page cache hit + fallback runtime call (~10 instr).
//
//  Caso tipico: struct value-type local (e.g. `Vec3 v = {1,2,3};` con
//  field access en hot loop).  bench_struct_field paga ~10 instrs por
//  cada `v.x` o `v.x = ...`; tras la promocion, 1 instr.
//
//  Algoritmo:
//    1. Recolectar ALLOCAs candidatas (no `host_alloca` ya, dst valido).
//    2. Por cada candidate, calcular el set transitivo `derived` via
//       forward-flow desde su dst a traves de ADD/SUB/BITCAST/MOV/etc.
//    3. Por cada uso del candidate o sus derivados, clasificar:
//       - SAFE: LOAD/STORE addr/ADD/SUB/CMP/BITCAST/MOV/CAST/etc.
//       - UNSAFE: CALL*/RET/THROW/TAILCALL si operand esta en derived;
//         STORE val (no addr) en derived implica escape.
//    4. Si TODOS los usos son SAFE, set host_alloca=true.
//
//  Diferencias con `ir_pass_promote_callned_allocas`:
//    - Aquel promueve para CALLN nativos (host_ptr REQUERIDO para
//      pasarlo a la fn nativa).  Este promueve por OPORTUNIDAD (host
//      stack es mas rapido que VM stack en JIT).
//    - Aquel marca ALLOCAs que ALCANZAN un CALLN.  Este marca ALLOCAs
//      que NO escapan a ningun sitio.
//==============================================================================

bool ir_pass_promote_local_allocas(IrFunction &fn) {
    if (fn.is_native) return false;
    if (fn.values.empty()) return false;

    /* Step 1: identificar ALLOCAs candidatas (no host_alloca ya). */
    std::vector<IrValueId> candidates;
    candidates.reserve(8);
    for (const auto &blk : fn.blocks) {
        for (const auto &ins : blk.instrs) {
            if (ins.op == IrOp::ALLOCA
             && ins.dst != IR_NO_VALUE
             && !ins.host_alloca) {
                candidates.push_back(ins.dst);
            }
        }
    }
    if (candidates.empty()) return false;

    /* Step 2: forward-flow del conjunto "derivado" desde TODAS las
     * ALLOCAs candidatas.  Mantenemos una map vid -> origen (uno de los
     * candidates) para que el escape de UN candidato no contamine los
     * otros.
     *
     * Simplificacion: usamos un solo set "all_derived" + map dst->src.
     * Si un dst tiene multiple sources (PHI con args de distintos
     * candidates), conservativo: marcamos AMBOS como unsafe. */
    std::vector<int8_t> derived_from(fn.values.size(), -1);  /* -1 = no, >=0 = idx en candidates */
    std::vector<bool>   ambiguous(fn.values.size(), false);  /* derivado de >1 candidate */

    auto set_derived = [&](IrValueId v, int8_t origin) {
        if (v >= fn.values.size()) return false;
        if (derived_from[v] == -1) {
            derived_from[v] = origin;
            return true;
        }
        if (derived_from[v] != origin) {
            ambiguous[v] = true;
        }
        return false;
    };
    /* Seed: cada candidate es derived from itself. */
    for (size_t i = 0; i < candidates.size(); ++i) {
        set_derived(candidates[i], static_cast<int8_t>(i & 0x7F));
    }

    /* Propagacion forward.  Cota dura 16 iter para convergencia. */
    bool changed = true;
    int it = 16;
    while (changed && it-- > 0) {
        changed = false;
        for (const auto &blk : fn.blocks) {
            for (const auto &ins : blk.instrs) {
                if (ins.dst == IR_NO_VALUE || ins.dst >= fn.values.size()) continue;
                if (derived_from[ins.dst] >= 0) continue;  /* ya marcado */
                auto from_op = [&](IrValueId v) -> int {
                    if (v == IR_NO_VALUE || v >= fn.values.size()) return -1;
                    return derived_from[v];
                };
                switch (ins.op) {
                    case IrOp::ADD: case IrOp::SUB:
                    case IrOp::BITCAST: case IrOp::MOV:
                    case IrOp::CAST: case IrOp::SEXT:
                    case IrOp::ZEXT: case IrOp::TRUNC:
                        for (auto opv : ins.operands) {
                            int from = from_op(opv);
                            if (from >= 0) {
                                if (set_derived(ins.dst, static_cast<int8_t>(from)))
                                    changed = true;
                                break;
                            }
                        }
                        break;
                    case IrOp::PHI:
                        for (const auto &pa : ins.phi_args) {
                            int from = from_op(pa.value);
                            if (from >= 0) {
                                if (set_derived(ins.dst, static_cast<int8_t>(from)))
                                    changed = true;
                                break;
                            }
                        }
                        break;
                    default: break;
                }
            }
        }
    }

    /* Step 3: clasificar usos.  Para cada candidate, escape=true si
     * cualquier uso del candidate o sus derivados es UNSAFE.
     *
     * UNSAFE ops:
     *   - CALL/CALLVIRT/CALLM/CALLN/CALLIND/CALLCLOSURE: cualquier operand
     *     que sea derived es escape (el callee puede ser interp).
     *   - RET/THROW/TAILCALL: cualquier operand derived es escape.
     *   - STORE: si el VAL (operands[0]) es derived (pero NO el addr en
     *     operands[1]), escapa a memoria fuera del candidato.  Si addr
     *     ES derived, OK (es access local).
     *
     * SAFE ops sobre derived:
     *   - LOAD addr=derived: OK (lee del slot local).
     *   - STORE addr=derived val=non-derived: OK (escribe al slot local).
     *   - ADD/SUB/BITCAST/MOV/CAST/SEXT/ZEXT/TRUNC/PHI: ya tracked.
     *   - CMP: read-only.
     *   - ALLOCA itself: el propio seed.
     */
    /* uint8_t en lugar de bool para evitar std::vector<bool>::reference. */
    std::vector<uint8_t> escapes(candidates.size(), 0u);
    auto mark_escape = [&](IrValueId v) {
        if (v == IR_NO_VALUE || v >= derived_from.size()) return;
        if (derived_from[v] < 0) return;
        if (ambiguous[v]) {
            /* multiple candidates -> escapan TODOS por conservadurismo. */
            for (size_t k = 0; k < escapes.size(); ++k) escapes[k] = 1u;
            return;
        }
        int idx = derived_from[v];
        if (idx >= 0 && static_cast<size_t>(idx) < escapes.size()) {
            escapes[idx] = 1u;
        }
    };

    auto is_derived = [&](IrValueId v) -> bool {
        if (v == IR_NO_VALUE || v >= derived_from.size()) return false;
        return derived_from[v] >= 0;
    };

    /* Whitelist de SAFE ops: solo estas pueden tener operands derived
     * sin que el ptr "escape" del alcance JIT-local.  Cualquier OTRA op
     * (RAW_ASM, CALL*, GC_*, FINDCLASS, etc.) se trata como UNSAFE por
     * defecto (los operands derived marcan escape). */
    auto is_safe_op = [](IrOp op) -> bool {
        switch (op) {
            /* ALLOCA: seed.  No escapa por si misma. */
            case IrOp::ALLOCA:
            /* Aritmetica entera: produce nuevo valor, tracked via
             * derived_from. */
            case IrOp::ADD: case IrOp::SUB: case IrOp::MUL:
            case IrOp::DIV: case IrOp::MOD: case IrOp::NEG:
            case IrOp::AND: case IrOp::OR:  case IrOp::XOR: case IrOp::NOT:
            case IrOp::SHL: case IrOp::SHR: case IrOp::SAR:
            /* Casts: forward. */
            case IrOp::BITCAST: case IrOp::MOV:
            case IrOp::CAST: case IrOp::SEXT:
            case IrOp::ZEXT: case IrOp::TRUNC:
            /* CMP: read-only. */
            case IrOp::CMP_EQ: case IrOp::CMP_NE:
            case IrOp::CMP_LT: case IrOp::CMP_GT:
            case IrOp::CMP_LE: case IrOp::CMP_GE:
            case IrOp::CMP_ULT: case IrOp::CMP_UGT:
            case IrOp::CMP_ULE: case IrOp::CMP_UGE:
            /* Control flow: no escapa el ptr. */
            case IrOp::BR: case IrOp::BR_COND:
            case IrOp::NOP: case IrOp::CONST:
                return true;
            default:
                return false;
        }
    };

    for (const auto &blk : fn.blocks) {
        for (const auto &ins : blk.instrs) {
            if (ins.op == IrOp::LOAD) {
                /* LOAD addr=operands[0]: addr derived OK (lee slot).
                 * dst NO se considera derived del candidate (es el
                 * valor cargado de memoria, no el ptr). */
                continue;
            }
            if (ins.op == IrOp::STORE) {
                /* STORE val=operands[0], addr=operands[1].
                 * addr derived -> OK (escribe al slot).
                 * val derived -> ESCAPA (escribe el PTR a memoria). */
                if (ins.operands.size() >= 2 && is_derived(ins.operands[0])) {
                    mark_escape(ins.operands[0]);
                }
                continue;
            }
            if (ins.op == IrOp::PHI) {
                /* PHI: si el dst NO es derived pero phi_args SI lo son,
                 * los args "salen" del dominio local -> escape. */
                if (ins.dst < derived_from.size()
                 && derived_from[ins.dst] < 0) {
                    for (const auto &pa : ins.phi_args) mark_escape(pa.value);
                }
                continue;
            }
            if (is_safe_op(ins.op)) continue;

            /* UNSAFE op (CALL*, RAW_ASM, GC_*, FINDCLASS, RET, THROW, ...).
             * Cualquier operand derived escapa. */
            for (auto opv : ins.operands) mark_escape(opv);
            for (const auto &pa : ins.phi_args) mark_escape(pa.value);
        }
    }

    /* Step 4: promover ALLOCAs que NO escapan. */
    bool any_promoted = false;
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (escapes[i]) continue;
        IrValueId v = candidates[i];
        for (auto &blk : fn.blocks) {
            for (auto &ins : blk.instrs) {
                if (ins.op == IrOp::ALLOCA && ins.dst == v && !ins.host_alloca) {
                    ins.host_alloca = true;
                    /* NO explicit_free: el JIT libera con leave/ret;
                     * el interp con htrack + frame cleanup. */
                    any_promoted = true;
                    /* Propagar is_host_ptr al dst para que el dataflow
                     * de host_in_jit del JIT lo recoja. */
                    if (v < fn.values.size()) {
                        fn.values[v].is_host_ptr = true;
                    }
                }
            }
        }
    }
    if (!any_promoted) return false;

    /* Step 5: propagar is_host_ptr forward por las cadenas de derived. */
    bool prop = true; int p_it = 16;
    while (prop && p_it-- > 0) {
        prop = false;
        for (auto &blk : fn.blocks) {
            for (auto &ins : blk.instrs) {
                if (ins.dst == IR_NO_VALUE || ins.dst >= fn.values.size()) continue;
                auto &dv = fn.values[ins.dst];
                if (dv.is_host_ptr) continue;
                bool any_host = false;
                auto chk = [&](IrValueId v) {
                    if (v != IR_NO_VALUE && v < fn.values.size()
                     && fn.values[v].is_host_ptr) any_host = true;
                };
                switch (ins.op) {
                    case IrOp::ADD: case IrOp::SUB:
                    case IrOp::BITCAST: case IrOp::MOV:
                    case IrOp::CAST: case IrOp::SEXT:
                    case IrOp::ZEXT: case IrOp::TRUNC:
                        for (auto opv : ins.operands) { chk(opv); if (any_host) break; }
                        break;
                    case IrOp::PHI:
                        for (const auto &pa : ins.phi_args) { chk(pa.value); if (any_host) break; }
                        break;
                    default: break;
                }
                if (any_host) {
                    dv.is_host_ptr = true;
                    prop = true;
                }
            }
        }
    }
    return true;
}


//==============================================================================
//  Pase ir_pass_promote_local_raw_alloc
//
//  Convierte `malloc(N_const) + ... + free(p)` locales sin escape a un
//  `ALLOCA host_alloca`.  El JIT selector emite `sub rsp, N` (host stack);
//  el RAW_FREE correspondiente se reemplaza por NOP porque el stack se
//  libera automaticamente al exit de la funcion (mov rsp, rbp + pop rbp).
//
//  Beneficio: malloc/free de ~200-500 ns por iter en hot loops -> ~1 ns
//  (sub/add rsp).  Speedup del alloc puro ~100-500x.
//==============================================================================

bool ir_pass_promote_local_raw_alloc(IrFunction &fn) {
    if (fn.is_native) return false;
    if (fn.values.empty()) return false;

    constexpr uint64_t MAX_PROMOTE_SIZE = 65536; // bytes

    // Collect RAW_ALLOC candidates (CONST size, size razonable).
    struct AllocSite {
        size_t       block_idx;
        size_t       ins_idx;
        IrValueId    dst;
        uint64_t     size_bytes;
    };
    std::vector<AllocSite> candidates;

    for (size_t bi = 0; bi < fn.blocks.size(); ++bi) {
        const auto &blk = fn.blocks[bi];
        for (size_t ii = 0; ii < blk.instrs.size(); ++ii) {
            const auto &ins = blk.instrs[ii];
            if (ins.op != IrOp::RAW_ALLOC) continue;
            if (ins.operands.empty()) continue;
            if (ins.dst == IR_NO_VALUE) continue;
            const IrValueId size_vid = ins.operands[0];
            if (size_vid >= fn.values.size()) continue;
            const auto &size_v = fn.values[size_vid];
            if (!size_v.is_const) continue;
            const uint64_t bytes = size_v.const_val;
            if (bytes == 0 || bytes > MAX_PROMOTE_SIZE) continue;
            candidates.push_back({bi, ii, ins.dst, bytes});
        }
    }
    if (candidates.empty()) return false;

    // Para cada candidato, hacer escape analysis:
    //   - forward fix-point: marcar valores derivados del dst.
    //   - rechazar si algun derivado llega a un uso prohibido (RETURN,
    //     STORE a memoria GC, CALL externo distinto del RAW_FREE).
    bool changed = false;
    for (auto &c : candidates) {
        // derivados[v] = true si v puede ser un alias del dst (forward
        // flow desde el RAW_ALLOC dst a traves de ADD/SUB/BITCAST/MOV/
        // CAST/*EXT/TRUNC/PHI).
        std::vector<bool> derivados(fn.values.size(), false);
        derivados[c.dst] = true;
        bool prop = true;
        int iters = 0;
        while (prop && iters++ < 32) {
            prop = false;
            for (const auto &blk : fn.blocks) {
                for (const auto &ins : blk.instrs) {
                    if (ins.dst == IR_NO_VALUE) continue;
                    if (ins.dst >= derivados.size()) continue;
                    if (derivados[ins.dst]) continue;
                    auto check = [&](IrValueId v) -> bool {
                        return v != IR_NO_VALUE
                            && v < derivados.size()
                            && derivados[v];
                    };
                    bool any_d = false;
                    switch (ins.op) {
                        case IrOp::ADD:    case IrOp::SUB:
                        case IrOp::BITCAST:case IrOp::MOV:
                        case IrOp::CAST:   case IrOp::SEXT:
                        case IrOp::ZEXT:   case IrOp::TRUNC:
                            for (auto v : ins.operands) if (check(v)) { any_d = true; break; }
                            break;
                        case IrOp::PHI:
                            for (auto &pa : ins.phi_args) if (check(pa.value)) { any_d = true; break; }
                            break;
                        default: break;
                    }
                    if (any_d) {
                        derivados[ins.dst] = true;
                        prop = true;
                    }
                }
            }
        }

        // Escape check: contar RAW_FREEs que reciben un derivado, y
        // rechazar si hay usos prohibidos.
        bool escapes = false;
        std::vector<std::pair<size_t,size_t>> free_sites;  // (block_idx, ins_idx)

        auto is_derived = [&](IrValueId v) -> bool {
            return v != IR_NO_VALUE
                && v < derivados.size()
                && derivados[v];
        };

        for (size_t bi = 0; bi < fn.blocks.size() && !escapes; ++bi) {
            const auto &blk = fn.blocks[bi];
            for (size_t ii = 0; ii < blk.instrs.size() && !escapes; ++ii) {
                const auto &ins = blk.instrs[ii];

                switch (ins.op) {
                    case IrOp::RET:
                    case IrOp::TAILCALL:
                    case IrOp::RSPAWN_RETURN:
                    case IrOp::FULFILL_HLT:
                        // El ptr llega a return / tailcall / fulfill -> escapa.
                        for (auto v : ins.operands) {
                            if (is_derived(v)) { escapes = true; break; }
                        }
                        break;

                    case IrOp::RAW_FREE:
                        // Free directo: marca el site para eliminar
                        // si la promocion procede.  NO marca escape.
                        if (!ins.operands.empty() && is_derived(ins.operands[0])) {
                            free_sites.push_back({bi, ii});
                        }
                        break;

                    case IrOp::STORE: {
                        // STORE val, addr.  Si val es derivado y la addr
                        // apunta a memoria GC o globals, ESCAPA.  Si addr
                        // es local (otro ALLOCA / RAW_ALLOC del mismo
                        // scope), tracking conservativo: marcar escape
                        // SOLO si la addr es is_host_ptr=false (= memoria
                        // GC) o si el target es un global.
                        //
                        // MVP conservativo: cualquier STORE de un derivado
                        // a una direccion que NO sea otro derivado del
                        // mismo ALLOCA cuenta como escape.
                        if (ins.operands.size() >= 2) {
                            const IrValueId val_v = ins.operands[0];
                            const IrValueId addr_v = ins.operands[1];
                            if (is_derived(val_v) && !is_derived(addr_v)) {
                                escapes = true;
                            }
                        }
                        break;
                    }

                    case IrOp::CALL:
                    case IrOp::CALLVIRT:
                    case IrOp::CALLM:
                    case IrOp::CALLIND:
                    case IrOp::CALLCLOSURE:
                    case IrOp::CALLN: {
                        // El ptr pasado a otra fn ESCAPA conservativamente.
                        // Excepcion: si fuera el callee es de tipo "trampoline
                        // pure" (no toca el ptr), seria seguro -- pero no
                        // tenemos esa info aqui.  Conservador: escape.
                        for (auto v : ins.operands) {
                            if (is_derived(v)) { escapes = true; break; }
                        }
                        if (ins.func_ptr != IR_NO_VALUE && is_derived(ins.func_ptr)) {
                            escapes = true;
                        }
                        break;
                    }

                    case IrOp::THROW:
                        // El ptr llega a throw -> escapa cross-frame.
                        for (auto v : ins.operands) {
                            if (is_derived(v)) { escapes = true; break; }
                        }
                        break;

                    default: break;
                }
            }
        }

        if (escapes) continue;
        if (free_sites.empty()) continue;  // sin free -> es leak, no promovemos

        // Promote: convertir RAW_ALLOC en ALLOCA con host_alloca=true.
        // El bytecode emitter ya respeta host_alloca y emite el path
        // de `alloc N + htrack` (que el runtime libera al RET del frame
        // automaticamente).  El JIT selector emite `sub rsp, N` (host
        // stack directo, sin allocator).
        auto &alloc_ins = fn.blocks[c.block_idx].instrs[c.ins_idx];
        alloc_ins.op = IrOp::ALLOCA;
        alloc_ins.imm = c.size_bytes;
        alloc_ins.type = IrType::I8;  // ALLOCA convencion: type=I8, imm=N bytes
        alloc_ins.host_alloca = true;
        alloc_ins.operands.clear();  // ALLOCA no toma operands (tamano en imm)

        // Marcar el dst como is_host_ptr para que LOAD/STORE emitan movh.
        if (c.dst < fn.values.size()) {
            fn.values[c.dst].is_host_ptr = true;
        }

        // Sprint mem-loop-fix (2026-06-02): PRESERVAR los RAW_FREE
        // explicitos en lugar de eliminarlos.  Bottleneck encontrado
        // en bench mem_malloc_free: el path original eliminaba RAW_FREE
        // y dependia de `host_alloca_release_all` al RET del frame,
        // pero si el ALLOCA esta DENTRO de un loop (inlineado o no),
        // el vector @c host_allocas del frame acumula N punteros
        // tracked sin liberar -- O(N) memoria + O(N) cleanup al RET.
        //
        // Con el RAW_FREE preservado, el alloc/free emparejan
        // correctamente DENTRO de cada iteracion del loop.  El ALLOCA
        // se marca con @c host_alloca_explicit_free=true para que el
        // bytecode emit del interp SKIPE el `htrack` (porque el free
        // explicito ya libera el ptr en su sitio).
        alloc_ins.host_alloca_explicit_free = true;
        // NO eliminar los RAW_FREE: dejarlos para que el bytecode emit
        // los convierta en `free` opcodes correctamente.

        changed = true;
    }

    // Compactar bloques: eliminar instrucciones NOP introducidas por
    // este pass.  El bytecode emitter de NOP emite `nop1` que NO esta
    // soportado correctamente por el decoder (decode_fn=nullptr).
    // Mas seguro eliminar fisicamente las RAW_FREE convertidas.
    if (changed) {
        for (auto &blk : fn.blocks) {
            auto &is = blk.instrs;
            is.erase(std::remove_if(is.begin(), is.end(), [](const IrInstr &i) {
                return i.op == IrOp::NOP && i.operands.empty() && i.dst == IR_NO_VALUE;
            }), is.end());
        }
    }

    return changed;
}

//==============================================================================
//  Phase C2.13: Escape Analysis + Scalar Replacement de objetos GC
//
//  Detecta objetos `new X(...)` (emitidos como `call @__new_X(args)`) que NO
//  ESCAPAN del frame en el que se crean: su host_ptr solo se usa para leer/
//  escribir campos locales, nunca se retorna, almacena en memoria heap, ni
//  pasa a otra funcion.  Un objeto asi puede materializarse SIN tocar el GC
//  heap (scalar replacement): el alloc se elimina y los `load (obj+off)` se
//  reemplazan por el valor con el que el ctor inicializo ese campo.
//
//  ADVERTENCIA DE SEGURIDAD: marcar como no-escapante un objeto que SI escapa
//  produce use-after-free / corrupcion de heap silenciosa.  El analisis es
//  CONSERVADOR por diseno: asume que el objeto escapa salvo prueba explicita.
//  La clasificacion de usos reusa exactamente el patron validado de
//  @c ir_pass_promote_local_allocas (whitelist de safe-ops; cualquier op no
//  reconocida marca escape).
//
//  Esta primera parte implementa SOLO la DETECCION (log-only via la env var
//  VESTA_ESCAPE_DEBUG).  La transformacion (scalar replacement) se construye
//  encima del mismo analisis una vez validada la deteccion.
//==============================================================================

namespace {

/** @brief Un sitio `call @__new_X(...)` candidato a scalar replacement. */
struct GcAllocSite {
    size_t      block_idx = 0;   ///< indice del bloque del CALL
    size_t      ins_idx   = 0;   ///< indice de la instr dentro del bloque
    IrValueId   dst       = IR_NO_VALUE; ///< SSA value del objeto (host_ptr)
    std::string class_name;      ///< "Foo" extraido de "__new_Foo"
    bool        escapes   = true;///< veredicto del analisis (conservador: true)
};

/**
 * @brief Devuelve true si @p name tiene la forma "__new_<ClassName>".
 *        Si lo es, escribe el ClassName en @p out_class.
 */
bool is_new_helper_name(const std::string &name, std::string *out_class) {
    if (name.size() <= 6) return false;
    if (name.compare(0, 6, "__new_") != 0) return false;
    /* Excluir variantes shared (`__new_X_shared`) que registran el objeto en
     * la SharedHandleTable -- eliminar ese alloc cambia shared_heap_live_count
     * (efecto observable), igual que en is_pure_allocator_name. */
    if (name.size() >= 7
     && name.compare(name.size() - 7, 7, "_shared") == 0) return false;
    if (out_class) *out_class = name.substr(6);
    return true;
}

/**
 * @brief Analisis de escape para los objetos `new X()` de una funcion.
 *
 * Para cada `call @__new_X` con dst valido, calcula si el host_ptr del objeto
 * escapa.  Algoritmo identico al de @c ir_pass_promote_local_allocas pero
 * seedeado en los dsts de los CALL `__new_*` en lugar de los dsts de ALLOCA:
 *
 *   1. Forward-flow del conjunto "derivado" desde cada candidato a traves de
 *      ADD/SUB/BITCAST/MOV/CAST/*EXT/TRUNC/PHI/GEP.  Un dst derivado de >1
 *      candidato distinto se marca @c ambiguous (escapan todos).
 *   2. Clasificacion de usos:
 *        - SEED (el propio `call __new_X`): no escapa su dst.  Sus OPERANDS
 *          (los args del ctor) SI pueden escapar otros candidatos (p.ej.
 *          `new Outer(inner)` -> inner escapa).
 *        - LOAD/GETFIELD addr=derivado: SAFE (lee campo; el valor leido NO
 *          es derivado).
 *        - STORE/SETFIELD addr=derivado, val=NO-derivado: SAFE (escribe campo).
 *          val=derivado -> ESCAPA (el ptr se guarda en memoria).
 *        - ADD/SUB/CAST/.../PHI sobre derivados: tracked, no escapa.
 *        - CMP/BR: read-only, no escapa.
 *        - Cualquier OTRA op con un operand derivado: ESCAPA (conservador).
 *
 * @return vector de @c GcAllocSite con el campo @c escapes resuelto.
 */
std::vector<GcAllocSite> analyze_gc_escape(const IrFunction &fn) {
    std::vector<GcAllocSite> sites;
    if (fn.is_native || fn.values.empty()) return sites;

    /* Step 1: recolectar candidatos `call __new_X`. */
    for (size_t bi = 0; bi < fn.blocks.size(); ++bi) {
        const auto &blk = fn.blocks[bi];
        for (size_t ii = 0; ii < blk.instrs.size(); ++ii) {
            const auto &ins = blk.instrs[ii];
            if (ins.op != IrOp::CALL) continue;
            if (ins.dst == IR_NO_VALUE || ins.dst >= fn.values.size()) continue;
            std::string cls;
            if (!is_new_helper_name(ins.func_name, &cls)) continue;
            GcAllocSite s;
            s.block_idx  = bi;
            s.ins_idx    = ii;
            s.dst        = ins.dst;
            s.class_name = std::move(cls);
            s.escapes    = true;  /* default conservador */
            sites.push_back(std::move(s));
        }
    }
    if (sites.empty()) return sites;

    /* Cota dura de candidatos por funcion (indice cabe en int8 del map). */
    if (sites.size() > 127) sites.resize(127);

    /* Step 2: forward-flow del set derivado.  derived_from[v] = idx del
     * candidato del que v deriva (-1 = ninguno); ambiguous[v] si >1. */
    std::vector<int8_t> derived_from(fn.values.size(), -1);
    std::vector<bool>   ambiguous(fn.values.size(), false);

    auto set_derived = [&](IrValueId v, int8_t origin) -> bool {
        if (v == IR_NO_VALUE || v >= fn.values.size()) return false;
        if (derived_from[v] == -1) { derived_from[v] = origin; return true; }
        if (derived_from[v] != origin) ambiguous[v] = true;
        return false;
    };
    for (size_t i = 0; i < sites.size(); ++i) {
        set_derived(sites[i].dst, static_cast<int8_t>(i & 0x7F));
    }

    bool changed = true;
    int  it = 16;
    while (changed && it-- > 0) {
        changed = false;
        for (const auto &blk : fn.blocks) {
            for (const auto &ins : blk.instrs) {
                if (ins.dst == IR_NO_VALUE || ins.dst >= fn.values.size()) continue;
                if (derived_from[ins.dst] >= 0) continue;  /* ya marcado */
                auto from_op = [&](IrValueId v) -> int {
                    if (v == IR_NO_VALUE || v >= fn.values.size()) return -1;
                    return derived_from[v];
                };
                switch (ins.op) {
                    case IrOp::ADD: case IrOp::SUB:
                    case IrOp::BITCAST: case IrOp::MOV:
                    case IrOp::CAST: case IrOp::SEXT:
                    case IrOp::ZEXT: case IrOp::TRUNC:
                    case IrOp::GEP:
                        for (auto opv : ins.operands) {
                            int from = from_op(opv);
                            if (from >= 0) {
                                if (set_derived(ins.dst, static_cast<int8_t>(from)))
                                    changed = true;
                                break;
                            }
                        }
                        break;
                    case IrOp::PHI:
                        for (const auto &pa : ins.phi_args) {
                            int from = from_op(pa.value);
                            if (from >= 0) {
                                if (set_derived(ins.dst, static_cast<int8_t>(from)))
                                    changed = true;
                                break;
                            }
                        }
                        break;
                    default: break;
                }
            }
        }
    }

    /* Step 3: clasificar usos.  escapes[i]=1 si algun uso de un derivado del
     * candidato i es UNSAFE. */
    std::vector<uint8_t> escapes(sites.size(), 0u);

    auto mark_escape = [&](IrValueId v) {
        if (v == IR_NO_VALUE || v >= derived_from.size()) return;
        if (derived_from[v] < 0) return;
        if (ambiguous[v]) {
            for (size_t k = 0; k < escapes.size(); ++k) escapes[k] = 1u;
            return;
        }
        int idx = derived_from[v];
        if (idx >= 0 && static_cast<size_t>(idx) < escapes.size()) escapes[idx] = 1u;
    };
    auto is_derived = [&](IrValueId v) -> bool {
        return v != IR_NO_VALUE && v < derived_from.size() && derived_from[v] >= 0;
    };

    /* Whitelist de SAFE ops (identica a promote_local_allocas).  Cualquier op
     * fuera de esta lista (RAW_ASM, CALL*, GC_*, FINDCLASS, RET, THROW, ...)
     * con un operand derivado marca escape. */
    auto is_safe_op = [](IrOp op) -> bool {
        switch (op) {
            case IrOp::ADD: case IrOp::SUB: case IrOp::MUL:
            case IrOp::DIV: case IrOp::MOD: case IrOp::NEG:
            case IrOp::AND: case IrOp::OR:  case IrOp::XOR: case IrOp::NOT:
            case IrOp::SHL: case IrOp::SHR: case IrOp::SAR:
            case IrOp::BITCAST: case IrOp::MOV:
            case IrOp::CAST: case IrOp::SEXT:
            case IrOp::ZEXT: case IrOp::TRUNC:
            case IrOp::GEP:
            case IrOp::CMP_EQ: case IrOp::CMP_NE:
            case IrOp::CMP_LT: case IrOp::CMP_GT:
            case IrOp::CMP_LE: case IrOp::CMP_GE:
            case IrOp::CMP_ULT: case IrOp::CMP_UGT:
            case IrOp::CMP_ULE: case IrOp::CMP_UGE:
            case IrOp::BR: case IrOp::BR_COND:
            case IrOp::NOP: case IrOp::CONST:
                return true;
            default:
                return false;
        }
    };

    for (size_t bi = 0; bi < fn.blocks.size(); ++bi) {
        const auto &blk = fn.blocks[bi];
        for (size_t ii = 0; ii < blk.instrs.size(); ++ii) {
            const auto &ins = blk.instrs[ii];

            /* El propio CALL seed NO escapa su dst (es el alloc); pero sus
             * operands (args del ctor) PUEDEN escapar OTROS candidatos. */
            if (ins.op == IrOp::CALL) {
                std::string cls;
                if (ins.dst != IR_NO_VALUE && is_new_helper_name(ins.func_name, &cls)) {
                    for (auto opv : ins.operands) mark_escape(opv);
                    continue;
                }
            }

            if (ins.op == IrOp::LOAD || ins.op == IrOp::GETFIELD) {
                /* addr=operands[0] derivado -> lee campo, SAFE.  El dst (valor
                 * leido) NO se considera derivado del objeto. */
                continue;
            }
            if (ins.op == IrOp::STORE) {
                /* STORE val=operands[0], addr=operands[1].
                 * val derivado -> el ptr se escribe en memoria -> ESCAPA. */
                if (ins.operands.size() >= 2 && is_derived(ins.operands[0])) {
                    mark_escape(ins.operands[0]);
                }
                continue;
            }
            if (ins.op == IrOp::SETFIELD) {
                /* SETFIELD obj=operands[0], val=operands[1].
                 * val derivado -> ESCAPA. obj derivado -> SAFE (escribe campo).*/
                if (ins.operands.size() >= 2 && is_derived(ins.operands[1])) {
                    mark_escape(ins.operands[1]);
                }
                continue;
            }
            if (ins.op == IrOp::PHI) {
                /* Si el dst NO es derivado pero algun phi_arg SI -> escapa. */
                if (ins.dst < derived_from.size() && derived_from[ins.dst] < 0) {
                    for (const auto &pa : ins.phi_args) mark_escape(pa.value);
                }
                continue;
            }
            if (is_safe_op(ins.op)) continue;

            /* UNSAFE op: cualquier operand derivado escapa. */
            for (auto opv : ins.operands) mark_escape(opv);
            for (const auto &pa : ins.phi_args) mark_escape(pa.value);
            if (ins.func_ptr != IR_NO_VALUE) mark_escape(ins.func_ptr);
        }
    }

    for (size_t i = 0; i < sites.size(); ++i) sites[i].escapes = (escapes[i] != 0u);
    return sites;
}

/** @brief true si la env var @p name esta activa (presente y != "0"/""). */
bool env_flag_on(const char *name) {
    const char *v = std::getenv(name);
    return v && v[0] != '\0' && v[0] != '0';
}

}  // namespace

/**
 * @brief Pase de DETECCION de objetos GC no-escapantes (log-only).
 *
 * Ejecuta @c analyze_gc_escape y, si VESTA_ESCAPE_DEBUG esta activo, loguea
 * cada sitio `new X()` con su veredicto (ESCAPA / NO-ESCAPA).  NO transforma
 * el IR: siempre devuelve false.  Sirve para validar el analisis con cero
 * riesgo antes de habilitar la transformacion (scalar replacement).
 */
bool ir_pass_escape_detect_gc(IrFunction &fn) {
    if (!env_flag_on("VESTA_ESCAPE_DEBUG")) return false;
    auto sites = analyze_gc_escape(fn);
    if (sites.empty()) return false;
    for (const auto &s : sites) {
        std::fprintf(stderr,
            "[escape] fn '%s': new %s() (dst %%%u) -> %s\n",
            fn.name.c_str(), s.class_name.c_str(),
            static_cast<unsigned>(s.dst),
            s.escapes ? "ESCAPA" : "NO-ESCAPA (candidato scalar-replace)");
    }
    return false;
}

//==============================================================================
//  Phase C2.13: Scalar Replacement (transformacion)
//
//  Para un `%obj = call @__new_X(args)` NO-ESCAPANTE cuyo constructor es un
//  "inicializador trivial de campos", elimina el alloc GC y reemplaza cada
//  `load.T (obj + off)` por el valor con el que el ctor inicializo ese campo
//  (un arg del `new` o una constante), convertido al tipo del campo.
//==============================================================================

namespace {

/** @brief Tamano en bytes de un IrType (para elegir trunc/widen). */
int sr_type_size(IrType t) {
    switch (t) {
        case IrType::I8:  case IrType::U8:  case IrType::BOOL: return 1;
        case IrType::I16: case IrType::U16: return 2;
        case IrType::I32: case IrType::U32: case IrType::F32: return 4;
        default: return 8;  /* I64/U64/F64/PTR/HANDLE */
    }
}
/** @brief true si @p t es un entero (no float, no ptr, no handle). */
bool sr_type_is_int(IrType t) {
    switch (t) {
        case IrType::I8:  case IrType::I16: case IrType::I32: case IrType::I64:
        case IrType::U8:  case IrType::U16: case IrType::U32: case IrType::U64:
        case IrType::BOOL:
            return true;
        default:
            return false;
    }
}
/** @brief true si @p t es un entero con signo. */
bool sr_type_is_signed(IrType t) {
    return t == IrType::I8 || t == IrType::I16
        || t == IrType::I32 || t == IrType::I64;
}

/** @brief Inicializacion de un campo por parte del ctor. */
struct SrFieldInit {
    enum Kind { PARAM, CONST } kind = PARAM;
    uint32_t  offset    = 0;          ///< offset del campo desde la base del objeto
    IrType    field_type = IrType::I64; ///< tipo con el que el ctor escribio el campo
    int       new_arg_index = -1;     ///< (PARAM) indice en los args de __new_X
    uint64_t  const_val = 0;          ///< (CONST) valor literal
};

/** @brief Modelo de un ctor "inicializador trivial de campos". */
struct SrCtorModel {
    bool                     valid = false;
    uint32_t                 num_new_args = 0; ///< params del ctor sin contar `this`
    std::vector<SrFieldInit> inits;            ///< una entrada por campo inicializado

    const SrFieldInit *find(uint32_t off) const {
        for (const auto &i : inits) if (i.offset == off) return &i;
        return nullptr;
    }
};

/** @brief Busca una IrFunction por nombre exacto. */
const IrFunction *sr_find_fn(const IrModule &mod, const std::string &name) {
    for (const auto &f : mod.functions) if (f.name == name) return &f;
    return nullptr;
}

/**
 * @brief Construye el modelo del ctor de la clase @p class_name.
 *
 * Solo tiene exito si:
 *   - La clase existe en @p mod.classes, NO tiene destructor ni campos
 *     destructibles, y NO es un aspecto (AOP).
 *   - Tiene EXACTAMENTE un metodo @c is_constructor (sin sobrecargas).
 *   - El cuerpo del ctor es un unico bloque cuyas unicas ops son:
 *     CONST, ADD(this, const_offset) para direcciones de campo, STORE de un
 *     param/const a una de esas direcciones, y RET.  @c this solo puede
 *     usarse como base de esas direcciones.  Cualquier otra op -> invalido.
 */
bool sr_build_ctor_model(const IrModule &mod, const std::string &class_name,
                         SrCtorModel &out, std::string *reason = nullptr) {
    out = SrCtorModel{};
    auto bail = [&](const char *r) -> bool { if (reason) *reason = r; return false; };

    /* 1) Localizar la clase + checks de seguridad. */
    const IrClass *cls = nullptr;
    for (const auto &c : mod.classes) {
        if (c.name == class_name) { cls = &c; break; }
    }
    if (!cls) return bail("clase no encontrada en mod.classes");
    if (cls->has_destructor)         return bail("clase con destructor");
    if (cls->has_destructible_field) return bail("clase con campo destructible");
    if (cls->is_aspect)              return bail("clase es @Aspect");

    /* El modulo entero usa AOP -> los CALLVIRT (incluido el del ctor) pueden
     * disparar advice chains; eliminar el ctor las saltaria. */
    for (const auto &c : mod.classes) {
        if (c.is_aspect) return bail("modulo usa AOP");
    }

    /* 2) Un unico constructor DEFINIDO en esta clase (los heredados tienen
     * defining_class distinto -> no cuentan; las sobrecargas reales si). */
    const IrMethod *ctor_m = nullptr;
    int ctor_count = 0;
    for (const auto &m : cls->methods) {
        if (!m.is_constructor) continue;
        /* Filtrar constructores heredados: solo el de esta clase.  Si
         * defining_class esta vacio (metadata incompleta), usar el match por
         * nombre ir_fn_name == "<clase>__ctor". */
        const bool own = m.defining_class.empty()
            ? (m.ir_fn_name == class_name + "__ctor")
            : (m.defining_class == class_name);
        if (!own) continue;
        ctor_m = &m; ++ctor_count;
    }
    if (ctor_count != 1)               return bail("0 o >1 constructores propios");
    if (!ctor_m || ctor_m->ir_fn_name.empty()) return bail("ctor sin ir_fn_name");

    const IrFunction *ctor = sr_find_fn(mod, ctor_m->ir_fn_name);
    if (!ctor || ctor->is_native) return bail("ctor IrFunction no hallada/native");
    if (ctor->blocks.size() != 1) return bail("ctor con control de flujo (>1 bloque)");
    if (ctor->params.empty())     return bail("ctor sin param this");

    const IrValueId this_vid = ctor->params[0];
    out.num_new_args = static_cast<uint32_t>(ctor->params.size() - 1);

    /* indice de param (en ctor->params) -> ; -1 si no es param. */
    auto param_index_of = [&](IrValueId v) -> int {
        for (size_t i = 0; i < ctor->params.size(); ++i) {
            if (ctor->params[i] == v) return static_cast<int>(i);
        }
        return -1;
    };

    /* Mapa fieldaddr_vid -> offset (direcciones `this + const`). */
    std::unordered_map<IrValueId, uint32_t> field_addr;
    /* CONST values definidos en el ctor (para resolver offsets y store-vals). */
    std::unordered_map<IrValueId, uint64_t> const_vals;

    const auto &blk = ctor->blocks[0];

    /* Pasada 1: recolectar consts. */
    for (const auto &ins : blk.instrs) {
        if (ins.op == IrOp::CONST && ins.dst != IR_NO_VALUE) {
            const_vals[ins.dst] = ins.imm;
        }
    }

    /* Pasada 2: validar cada instr + recolectar field addrs + stores. */
    for (const auto &ins : blk.instrs) {
        switch (ins.op) {
            case IrOp::CONST:
            case IrOp::NOP:
                break;  /* inocuos */

            case IrOp::RET:
                /* ret.void: no debe retornar `this` ni un derivado. */
                for (auto v : ins.operands) {
                    if (v == this_vid || field_addr.count(v))
                        return bail("ctor retorna this/field-addr");
                }
                break;

            case IrOp::ADD: {
                /* Solo permitido como `add this, const` -> direccion de campo.
                 * Cualquier otro ADD que toque `this` invalida el modelo. */
                if (ins.operands.size() != 2) {
                    /* ADD que no toca this es inocuo; si toca this, invalido. */
                    for (auto v : ins.operands)
                        if (v == this_vid) return bail("ADD raro sobre this");
                    break;
                }
                const IrValueId a = ins.operands[0];
                const IrValueId b = ins.operands[1];
                IrValueId base = IR_NO_VALUE, offv = IR_NO_VALUE;
                if (a == this_vid)      { base = a; offv = b; }
                else if (b == this_vid) { base = b; offv = a; }
                if (base == this_vid) {
                    auto it = const_vals.find(offv);
                    if (it == const_vals.end()) return bail("offset de campo no const");
                    if (ins.dst == IR_NO_VALUE) return bail("field-addr sin dst");
                    field_addr[ins.dst] = static_cast<uint32_t>(it->second);
                } else {
                    /* ADD sin this; pero si algun operando es una field-addr
                     * derivada, no lo soportamos. */
                    if (field_addr.count(a) || field_addr.count(b))
                        return bail("aritmetica sobre field-addr");
                }
                break;
            }

            case IrOp::STORE: {
                /* store val=operands[0], addr=operands[1]. */
                if (ins.operands.size() < 2) return bail("STORE mal formado");
                const IrValueId val  = ins.operands[0];
                const IrValueId addr = ins.operands[1];
                uint32_t off;
                if (addr == this_vid) {
                    off = 0;
                } else {
                    auto it = field_addr.find(addr);
                    if (it == field_addr.end()) return bail("STORE a addr no-campo");
                    off = it->second;
                }
                /* val debe ser un param (>=1) o un const. */
                SrFieldInit fi;
                fi.offset = off;
                int pidx = param_index_of(val);
                if (pidx == 0) {
                    return bail("ctor guarda this en un campo (self-ref)");
                } else if (pidx >= 1) {
                    fi.kind          = SrFieldInit::PARAM;
                    fi.new_arg_index = pidx - 1;
                    if (val >= ctor->values.size()) return bail("param fuera de rango");
                    fi.field_type    = ctor->values[val].type;
                } else {
                    auto cit = const_vals.find(val);
                    if (cit == const_vals.end())
                        return bail("store-val no es param ni const (cast/expr)");
                    fi.kind       = SrFieldInit::CONST;
                    fi.const_val  = cit->second;
                    fi.field_type = (val < ctor->values.size())
                                  ? ctor->values[val].type : IrType::I64;
                }
                /* No permitir dos stores al mismo offset (ambiguo). */
                if (out.find(off)) return bail("dos stores al mismo campo");
                out.inits.push_back(fi);
                break;
            }

            default:
                /* Cualquier otra op (CALL, LOAD, NEWOBJ, GC*, RAW_ASM, MOV,
                 * casts, super-ctor, ...) invalida el modelo trivial. */
                return bail("ctor con op no-trivial (CALL/LOAD/cast/super/...)");
        }
    }

    out.valid = true;
    return true;
}

/**
 * @brief Reescribe (o solo valida) la instr @p ld (un LOAD) para producir el
 *        valor del campo a partir del arg/const con el que el ctor lo inicializo.
 *
 * Con @p apply == false NO muta nada (ni @p ld ni @c fn.values): solo
 * comprueba si la reescritura es posible.  Con @p apply == true aplica la
 * reescritura (asume que la validacion previa devolvio true).  Esto permite
 * un transform transaccional: validar TODOS los loads antes de tocar nada.
 *
 * @return true si la reescritura es posible / se aplico; false -> abortar.
 */
bool sr_rewrite_load(IrInstr &ld, const SrFieldInit &fi,
                     const std::vector<IrValueId> &args, IrFunction &fn,
                     bool apply) {
    const IrType T = ld.type;  /* tipo leido del campo */
    /* Solo enteros por ahora (float/ptr/handle -> abortar, conservador). */
    if (!sr_type_is_int(T)) return false;
    /* El ctor escribio el campo con field_type; debe coincidir con el read. */
    if (fi.field_type != T) return false;

    if (fi.kind == SrFieldInit::CONST) {
        uint64_t v = fi.const_val;
        int sz = sr_type_size(T);
        if (sz < 8) v &= ((uint64_t{1} << (sz * 8)) - 1);
        if (apply) {
            /* Reescribir el LOAD como CONST T value (truncado al ancho de T). */
            ld.op = IrOp::CONST;
            ld.imm = v;
            ld.operands.clear();
            ld.func_name.clear();
            if (ld.dst != IR_NO_VALUE && ld.dst < fn.values.size()) {
                fn.values[ld.dst].is_const    = true;
                fn.values[ld.dst].const_val   = v;
                fn.values[ld.dst].is_host_ptr = false;
            }
        }
        return true;
    }

    /* PARAM: el valor del campo = convert(args[new_arg_index], T). */
    if (fi.new_arg_index < 0
     || static_cast<size_t>(fi.new_arg_index) >= args.size()) return false;
    const IrValueId arg = args[fi.new_arg_index];
    if (arg == IR_NO_VALUE || arg >= fn.values.size()) return false;
    const IrType Ta = fn.values[arg].type;
    if (!sr_type_is_int(Ta)) return false;  /* arg no entero -> abortar */

    const int szA = sr_type_size(Ta);
    const int szT = sr_type_size(T);
    /* arg mas estrecho que el campo: widening (raro al pasar literales).
     * v1 no lo modela con seguridad -> abortar. */
    if (szA < szT) return false;

    if (apply) {
        ld.operands.clear();
        ld.operands.push_back(arg);
        ld.func_name.clear();
        if (ld.dst != IR_NO_VALUE && ld.dst < fn.values.size()) {
            fn.values[ld.dst].is_const    = false;
            fn.values[ld.dst].is_host_ptr = false;
        }
        /* szA > szT -> truncar; szA == szT -> copia directa. */
        ld.op = (szA > szT) ? IrOp::TRUNC : IrOp::MOV;
    }
    return true;
}

//==============================================================================
//  Dominancia: idom + dominance frontier + dom-tree (para SROA/mem2reg).
//==============================================================================

struct SrDom {
    size_t                              N = 0;
    IrBlockId                           UNDEF = 0;
    std::vector<std::vector<IrBlockId>> preds, succs;
    std::vector<IrBlockId>              idom;          ///< inmediato dominador
    std::vector<std::vector<IrBlockId>> df;            ///< dominance frontier
    std::vector<std::vector<IrBlockId>> dom_children;  ///< hijos en el dom-tree
    std::vector<uint8_t>                reachable;     ///< alcanzable desde entry

    bool dominates(IrBlockId T, IrBlockId B) const {
        if (T == B) return true;
        if (T >= N || B >= N || idom[B] == UNDEF) return false;
        IrBlockId cur = B;
        while (idom[cur] != cur) { cur = idom[cur]; if (cur == T) return true; }
        return false;
    }
};

/**
 * @brief Computa CFG + dominadores (Cooper-Harvey-Kennedy) + dominance frontier
 *        (Cytron) + dom-tree para @p fn.  Convencion: bloque 0 = entry.
 */
SrDom sr_compute_dom(const IrFunction &fn) {
    SrDom d;
    const size_t N = fn.blocks.size();
    d.N = N;
    d.UNDEF = static_cast<IrBlockId>(N);
    d.preds.assign(N, {});
    d.succs.assign(N, {});
    d.idom.assign(N, d.UNDEF);
    d.df.assign(N, {});
    d.dom_children.assign(N, {});
    d.reachable.assign(N, 0);
    if (N == 0) return d;

    /* CFG desde los terminadores. */
    for (size_t b = 0; b < N; ++b) {
        const auto &bb = fn.blocks[b];
        if (bb.instrs.empty()) continue;
        const auto &last = bb.instrs.back();
        IrBlockId t1 = IR_NO_BLOCK, t2 = IR_NO_BLOCK;
        if (last.op == IrOp::BR) { t1 = last.target_block; }
        else if (last.op == IrOp::BR_COND) { t1 = last.target_block; t2 = last.false_block; }
        if (t1 != IR_NO_BLOCK && t1 < N) { d.preds[t1].push_back((IrBlockId)b); d.succs[b].push_back(t1); }
        if (t2 != IR_NO_BLOCK && t2 < N) { d.preds[t2].push_back((IrBlockId)b); d.succs[b].push_back(t2); }
    }

    const IrBlockId entry = 0;
    /* Reverse postorder via DFS iterativo (evita stack overflow en CFGs grandes). */
    std::vector<IrBlockId> rpo;
    {
        std::vector<uint8_t> vis(N, 0);
        std::vector<std::pair<IrBlockId,size_t>> st;  /* (bloque, idx_succ) */
        st.push_back({entry, 0});
        vis[entry] = 1; d.reachable[entry] = 1;
        std::vector<IrBlockId> post;
        while (!st.empty()) {
            auto &top = st.back();
            if (top.second < d.succs[top.first].size()) {
                IrBlockId s = d.succs[top.first][top.second++];
                if (s < N && !vis[s]) { vis[s] = 1; d.reachable[s] = 1; st.push_back({s, 0}); }
            } else {
                post.push_back(top.first);
                st.pop_back();
            }
        }
        rpo.assign(post.rbegin(), post.rend());
    }
    std::vector<uint32_t> rpo_pos(N, UINT32_MAX);
    for (size_t i = 0; i < rpo.size(); ++i) rpo_pos[rpo[i]] = (uint32_t)i;

    d.idom[entry] = entry;
    auto intersect = [&](IrBlockId b1, IrBlockId b2) -> IrBlockId {
        while (b1 != b2) {
            while (b1 != d.UNDEF && rpo_pos[b1] > rpo_pos[b2]) b1 = d.idom[b1];
            while (b2 != d.UNDEF && rpo_pos[b2] > rpo_pos[b1]) b2 = d.idom[b2];
            if (b1 == d.UNDEF || b2 == d.UNDEF) return d.UNDEF;
        }
        return b1;
    };
    bool ch = true;
    while (ch) {
        ch = false;
        for (IrBlockId b : rpo) {
            if (b == entry) continue;
            IrBlockId nd = d.UNDEF;
            for (IrBlockId p : d.preds[b]) {
                if (d.idom[p] != d.UNDEF) {
                    nd = (nd == d.UNDEF) ? p : intersect(nd, p);
                    if (nd == d.UNDEF) break;
                }
            }
            if (nd != d.UNDEF && nd != d.idom[b]) { d.idom[b] = nd; ch = true; }
        }
    }

    /* Dom-tree children. */
    for (IrBlockId b = 0; b < N; ++b) {
        if (b != entry && d.idom[b] != d.UNDEF) d.dom_children[d.idom[b]].push_back(b);
    }

    /* Dominance frontier (Cytron): por cada bloque b con >=2 preds, por cada
     * pred p, sube en el dom-tree desde p hasta idom[b] anyadiendo b al DF. */
    for (IrBlockId b = 0; b < N; ++b) {
        if (d.preds[b].size() < 2) continue;
        for (IrBlockId p : d.preds[b]) {
            IrBlockId runner = p;
            while (runner != d.UNDEF && runner != d.idom[b]) {
                d.df[runner].push_back(b);
                if (d.idom[runner] == runner) break;  /* entry */
                runner = d.idom[runner];
            }
        }
    }
    return d;
}

//==============================================================================
//  SROA/mem2reg de los campos de un objeto GC no-escapante.
//
//  Promueve cada campo (offset) del objeto a forma SSA a traves del control de
//  flujo (incluyendo loops): inserta PHIs en el dominance frontier de las
//  definiciones (ctor-init + stores) y renombra (Cytron) reemplazando cada load
//  por la definicion que lo alcanza.  Tras esto el objeto no toca memoria -> el
//  alloc + los stores se borran.
//
//  Precondiciones (el caller las garantiza salvo lo que se revalida aqui):
//    - El objeto NO escapa y TODOS sus usos son field-access (load/store de
//      `obj` o de `add obj, Kconst`), nunca en phi_args/func_ptr/CALL/RET.
//    - El ctor es un inicializador trivial (modelo @p model).
//    - CFG reducible (el frontend Vex lo garantiza).
//
//  Conservador: si cualquier campo accedido no esta en el modelo, no es entero,
//  o los tipos no son consistentes -> bail (no muta nada).
//==============================================================================

bool sr_mem2reg_object(IrFunction &fn,
                       const SrCtorModel &model,
                       size_t call_bi, size_t call_ii, IrValueId obj,
                       const std::vector<IrValueId> &args,
                       const std::unordered_map<IrValueId, uint32_t> &fieldaddr_off,
                       std::string &reason) {
    const size_t N = fn.blocks.size();
    if (N == 0) { reason = "fn vacia"; return false; }

    /* Helper: la instr es un load/store de un campo del objeto?  Devuelve
     * offset + si es store + el valor almacenado. */
    auto classify = [&](const IrInstr &in, uint32_t &off, bool &is_ld,
                        bool &is_st, IrValueId &sval) -> bool {
        is_ld = is_st = false;
        if (in.op == IrOp::LOAD && !in.operands.empty()) {
            IrValueId a = in.operands[0];
            if (a == obj) { off = 0; is_ld = true; return true; }
            auto it = fieldaddr_off.find(a);
            if (it != fieldaddr_off.end()) { off = it->second; is_ld = true; return true; }
        } else if (in.op == IrOp::STORE && in.operands.size() >= 2) {
            IrValueId a = in.operands[1];
            if (a == obj)        { off = 0; is_st = true; sval = in.operands[0]; return true; }
            auto it = fieldaddr_off.find(a);
            if (it != fieldaddr_off.end()) { off = it->second; is_st = true; sval = in.operands[0]; return true; }
        }
        return false;
    };

    /* 1) Recolectar offsets accedidos + tipo por offset + bloques con store. */
    std::unordered_map<uint32_t, IrType> field_type;   /* offset -> tipo */
    std::unordered_map<uint32_t, std::vector<IrBlockId>> store_blocks;
    std::vector<uint32_t> offsets;
    for (size_t bi = 0; bi < N; ++bi) {
        for (const auto &in : fn.blocks[bi].instrs) {
            uint32_t off; bool ld, st; IrValueId sv;
            if (!classify(in, off, ld, st, sv)) continue;
            /* Tipo del campo: para load = in.type; para store = tipo del valor. */
            IrType t = ld ? in.type
                          : (sv < fn.values.size() ? fn.values[sv].type : IrType::I64);
            if (!sr_type_is_int(t)) { reason = "campo no-entero en mem2reg"; return false; }
            auto fit = field_type.find(off);
            if (fit == field_type.end()) { field_type[off] = t; offsets.push_back(off); }
            else if (fit->second != t)   { reason = "tipo inconsistente del campo"; return false; }
            if (st) store_blocks[off].push_back((IrBlockId)bi);
        }
    }
    if (offsets.empty()) { reason = "sin accesos a campos"; return false; }

    /* Todo offset accedido debe estar en el modelo (ctor-init disponible) y su
     * tipo coincidir con el del ctor (default-0 no soportado en mem2reg v1). */
    for (uint32_t off : offsets) {
        const SrFieldInit *fi = model.find(off);
        if (!fi) { reason = "campo no inicializado por el ctor (default-0)"; return false; }
        if (fi->field_type != field_type[off]) { reason = "tipo ctor/acceso difiere"; return false; }
    }

    SrDom dom = sr_compute_dom(fn);
    /* El bloque del alloc debe ser alcanzable (lo es: contiene el call). */
    if (call_bi >= N || !dom.reachable[call_bi]) { reason = "call_bi inalcanzable"; return false; }
    /* Todos los bloques con acceso a campos deben ser alcanzables + dominados
     * por el alloc (garantizado por SSA, pero revalidamos defensivamente). */

    /* 2) Materializar el valor de construccion (init) de cada campo como un
     * SSA value disponible en el sitio del alloc.  Si requiere conversion
     * (trunc) o es const, se insertara una instruccion JUSTO antes del call. */
    std::unordered_map<uint32_t, IrValueId> init_val;     /* offset -> SSA value */
    std::vector<IrInstr> init_instrs;                      /* a insertar antes del call */
    for (uint32_t off : offsets) {
        const SrFieldInit *fi = model.find(off);
        const IrType T = field_type[off];
        if (fi->kind == SrFieldInit::CONST) {
            uint64_t v = fi->const_val;
            int sz = sr_type_size(T);
            if (sz < 8) v &= ((uint64_t{1} << (sz * 8)) - 1);
            IrInstr ci; ci.op = IrOp::CONST; ci.type = T; ci.imm = v;
            IrValue nv; nv.id = (IrValueId)fn.values.size(); nv.type = T;
            nv.is_const = true; nv.const_val = v;
            nv.name = "%m2ri" + std::to_string(nv.id);
            ci.dst = nv.id;
            fn.values.push_back(nv);
            init_val[off] = nv.id;     /* antes del move de ci */
            init_instrs.push_back(std::move(ci));
        } else {
            /* PARAM. */
            if (fi->new_arg_index < 0 || (size_t)fi->new_arg_index >= args.size()) {
                reason = "arg index fuera de rango"; return false;
            }
            IrValueId arg = args[fi->new_arg_index];
            if (arg == IR_NO_VALUE || arg >= fn.values.size()) { reason = "arg invalido"; return false; }
            IrType Ta = fn.values[arg].type;
            if (!sr_type_is_int(Ta)) { reason = "arg no entero"; return false; }
            int szA = sr_type_size(Ta), szT = sr_type_size(T);
            if (szA == szT) {
                init_val[off] = arg;           /* sin conversion */
            } else if (szA > szT) {
                IrInstr ti; ti.op = IrOp::TRUNC; ti.type = T; ti.operands.push_back(arg);
                IrValue nv; nv.id = (IrValueId)fn.values.size(); nv.type = T;
                nv.name = "%m2ri" + std::to_string(nv.id);
                ti.dst = nv.id; fn.values.push_back(nv);
                init_val[off] = nv.id;     /* antes del move de ti */
                init_instrs.push_back(std::move(ti));
            } else {
                reason = "widening arg->campo no soportado"; return false;  /* szA < szT */
            }
        }
    }

    /* 2.5) Deteccion de loops (headers + bloques in-loop) desde back-edges.
     * Necesario para el COST-MODEL: un PHI in-loop que NO esta en un loop
     * header (= escritura condicional de campo dentro de un loop) anyade copies
     * en el path no-tomado por iteracion.  Medido: regresiona el interp (16
     * registros VM -> presion + copies).  Los PHIs de loop-header (acumuladores
     * incondicionales) y los if-merge FUERA de loops (coste unico) SI son win. */
    std::vector<uint8_t> is_loop_header(N, 0), in_loop(N, 0);
    for (IrBlockId b = 0; b < N; ++b) {
        for (IrBlockId h : dom.succs[b]) {
            if (!dom.dominates(h, b)) continue;   /* back-edge b->h */
            is_loop_header[h] = 1; in_loop[h] = 1;
            std::vector<IrBlockId> stk;
            if (!in_loop[b]) { in_loop[b] = 1; stk.push_back(b); }
            while (!stk.empty()) {
                IrBlockId x = stk.back(); stk.pop_back();
                if (x == h) continue;
                for (IrBlockId p : dom.preds[x]) {
                    if (!in_loop[p]) { in_loop[p] = 1; if (p != h) stk.push_back(p); }
                }
            }
        }
    }

    /* 3) Insercion de PHIs: por cada offset, iterated dominance frontier de los
     * def-blocks (= {call_bi} U store_blocks).  Crea SSA values para los phis. */
    /* phi_value[offset][block] = SSA value del phi (IR_NO_VALUE = no hay). */
    std::unordered_map<uint64_t, IrValueId> phi_value;  /* key = (off<<32)|block */
    std::unordered_map<IrValueId, uint32_t> phi_dst_off; /* phi dst -> offset */
    auto pkey = [](uint32_t off, IrBlockId b) -> uint64_t {
        return ((uint64_t)off << 32) | (uint64_t)b;
    };
    /* COST-MODEL (default-on): permitir VESTA_ESCAPE_MEM2REG_FORCE para saltarlo
     * y promover siempre (util para medir / casos JIT-only). */
    const bool force = env_flag_on("VESTA_ESCAPE_MEM2REG_FORCE");
    for (uint32_t off : offsets) {
        std::vector<IrBlockId> worklist;
        std::unordered_set<IrBlockId> on_work, has_phi;
        worklist.push_back((IrBlockId)call_bi); on_work.insert((IrBlockId)call_bi);
        for (IrBlockId b : store_blocks[off]) {
            if (!on_work.count(b)) { worklist.push_back(b); on_work.insert(b); }
        }
        size_t wp = 0;
        while (wp < worklist.size()) {
            IrBlockId b = worklist[wp++];
            if (b >= N) continue;
            for (IrBlockId f : dom.df[b]) {
                if (has_phi.count(f)) continue;
                if (!dom.reachable[f]) continue;
                /* Solo PHIs en bloques DOMINADOS por el alloc: ahi todos los
                 * preds estan dominados por el call -> el objeto existe en cada
                 * pred -> el operando del phi siempre tiene def alcanzante.  Un
                 * merge no-dominado no puede leer el campo (violaria SSA), asi
                 * que su phi seria muerto; lo omitimos. */
                if (!dom.dominates((IrBlockId)call_bi, f)) continue;
                /* COST-MODEL: if-merge DENTRO de un loop = escritura condicional
                 * en el loop -> pessimiza el interp.  Bail (a menos que FORCE). */
                if (!force && in_loop[f] && !is_loop_header[f]) {
                    reason = "escritura condicional de campo en loop (cost-model)";
                    return false;
                }
                has_phi.insert(f);
                /* Crear el SSA value del phi. */
                IrValue nv; nv.id = (IrValueId)fn.values.size(); nv.type = field_type[off];
                nv.name = "%m2rphi" + std::to_string(nv.id);
                fn.values.push_back(nv);
                phi_value[pkey(off, f)] = nv.id;
                phi_dst_off[nv.id] = off;
                if (!on_work.count(f)) { worklist.push_back(f); on_work.insert(f); }
            }
        }
    }

    /* 4) Renaming (Cytron) DFS sobre el dom-tree.  current[off] = def alcanzante.
     * Se construyen: load_repl (load dst -> valor), store_remove (posiciones),
     * y los phi_args de cada phi insertado.  NO se muta el IR todavia. */
    /* Plan de mutacion: */
    std::unordered_map<IrValueId, IrValueId> load_repl;  /* load.dst -> valor reemplazo */
    /* phi_args[phi_dst] = lista de (pred_block, value). */
    std::unordered_map<IrValueId, std::vector<IrPhiArg>> phi_args_plan;

    std::unordered_map<uint32_t, std::vector<IrValueId>> stack;  /* off -> pila de defs */
    auto cur = [&](uint32_t off) -> IrValueId {
        auto it = stack.find(off);
        return (it != stack.end() && !it->second.empty()) ? it->second.back() : IR_NO_VALUE;
    };

    bool rename_ok = true;
    const char *rfail = nullptr;
    std::function<void(IrBlockId, int)> rename = [&](IrBlockId b, int depth) {
        if (!rename_ok) return;
        if (depth > 4096) { rename_ok = false; rfail = "dom-tree demasiado profundo"; return; }
        std::vector<uint32_t> pushed;  /* offsets con un push en este bloque (para pop) */

        /* (a) PHIs planeados para este bloque (aun NO insertados como
         * instrucciones): definen current[off].  Se consultan via phi_value, no
         * escaneando instrucciones (que no existen todavia en esta fase). */
        for (uint32_t off : offsets) {
            auto it = phi_value.find(pkey(off, b));
            if (it == phi_value.end()) continue;
            stack[off].push_back(it->second); pushed.push_back(off);
        }

        /* (b) instrucciones en orden. */
        for (size_t ii = 0; ii < fn.blocks[b].instrs.size(); ++ii) {
            const IrInstr &in = fn.blocks[b].instrs[ii];
            /* El alloc: define todos los campos = init_val. */
            if (b == call_bi && ii == call_ii) {
                for (uint32_t off : offsets) {
                    stack[off].push_back(init_val[off]); pushed.push_back(off);
                }
                continue;
            }
            uint32_t off; bool ld, st; IrValueId sv;
            if (!classify(in, off, ld, st, sv)) continue;
            if (ld) {
                IrValueId rv = cur(off);
                if (rv == IR_NO_VALUE) { rename_ok = false; rfail = "load sin def alcanzante"; return; }
                if (in.dst != IR_NO_VALUE) load_repl[in.dst] = rv;
            } else if (st) {
                stack[off].push_back(sv); pushed.push_back(off);
            }
        }

        /* (c) rellenar operandos de los phis planeados de los sucesores. */
        for (IrBlockId s : dom.succs[b]) {
            for (uint32_t off : offsets) {
                auto it = phi_value.find(pkey(off, s));
                if (it == phi_value.end()) continue;
                IrValueId rv = cur(off);
                if (rv == IR_NO_VALUE) { rename_ok = false; rfail = "phi operand sin def"; return; }
                phi_args_plan[it->second].push_back(IrPhiArg{rv, b});
            }
        }

        /* (d) recursion en hijos del dom-tree. */
        for (IrBlockId c : dom.dom_children[b]) rename(c, depth + 1);

        /* (e) pop. */
        for (auto it = pushed.rbegin(); it != pushed.rend(); ++it) stack[*it].pop_back();
    };
    rename(0, 0);
    if (!rename_ok) { reason = rfail ? rfail : "rename fallo"; return false; }

    /* ====================================================================
     * 5) APLICAR el plan (todas las precondiciones validadas).
     * ==================================================================== */
    /* (a) Reescribir loads -> MOV del valor reemplazo (copy_prop lo limpia). */
    for (auto &bb : fn.blocks) {
        for (auto &in : bb.instrs) {
            if ((in.op == IrOp::LOAD) && in.dst != IR_NO_VALUE) {
                auto it = load_repl.find(in.dst);
                if (it == load_repl.end()) continue;
                /* Confirmar que es un load de un campo del objeto. */
                uint32_t off; bool ld, st; IrValueId sv;
                if (!classify(in, off, ld, st, sv) || !ld) continue;
                in.op = IrOp::MOV; in.operands.clear();
                in.operands.push_back(it->second);
                in.func_name.clear();
                if (in.dst < fn.values.size()) {
                    fn.values[in.dst].is_const = false;
                    fn.values[in.dst].is_host_ptr = false;
                }
            }
        }
    }

    /* (b) Insertar los PHIs al frente de sus bloques con sus operandos. */
    for (const auto &kv : phi_dst_off) {
        IrValueId phidst = kv.first;
        uint32_t  off    = kv.second;
        /* Localizar el bloque (clave inversa: buscar en phi_value). */
        IrBlockId blk = dom.UNDEF;
        for (IrBlockId b = 0; b < N; ++b) {
            auto it = phi_value.find(pkey(off, b));
            if (it != phi_value.end() && it->second == phidst) { blk = b; break; }
        }
        if (blk >= N) continue;
        IrInstr phi; phi.op = IrOp::PHI; phi.type = field_type[off]; phi.dst = phidst;
        auto pit = phi_args_plan.find(phidst);
        if (pit != phi_args_plan.end()) phi.phi_args = pit->second;
        fn.blocks[blk].instrs.insert(fn.blocks[blk].instrs.begin(), std::move(phi));
    }

    /* (c) Eliminar TODOS los stores a campos del objeto (NOP).  Tras mem2reg
     * ningun store es necesario (el valor fluye por SSA).  Se re-localizan por
     * contenido (no por indice) porque (b) inserto phis al frente. */
    for (auto &bb : fn.blocks) {
        for (auto &in : bb.instrs) {
            uint32_t off; bool ld, st; IrValueId sv;
            if (in.op == IrOp::STORE && classify(in, off, ld, st, sv) && st) {
                in.op = IrOp::NOP; in.operands.clear();
                in.dst = IR_NO_VALUE; in.func_name.clear();
            }
        }
    }

    /* (d) Insertar las init_instrs antes del call + NOPear el call + field-addrs.
     * El call sigue identificable por dst==obj. */
    for (auto &bb : fn.blocks) {
        for (size_t ii = 0; ii < bb.instrs.size(); ++ii) {
            IrInstr &in = bb.instrs[ii];
            if (in.op == IrOp::CALL && in.dst == obj
             && is_new_helper_name(in.func_name, nullptr)) {
                /* Insertar init_instrs justo antes. */
                if (!init_instrs.empty()) {
                    bb.instrs.insert(bb.instrs.begin() + ii,
                                     init_instrs.begin(), init_instrs.end());
                    ii += init_instrs.size();
                }
                IrInstr &call = bb.instrs[ii];
                call.op = IrOp::NOP; call.operands.clear();
                call.dst = IR_NO_VALUE; call.func_name.clear();
                goto done_call;
            }
        }
    }
    done_call:;

    /* (e) NOPear las field-addr (add obj, K) -- ahora muertas. */
    for (auto &bb : fn.blocks) {
        for (auto &in : bb.instrs) {
            if (in.op == IrOp::ADD && in.dst != IR_NO_VALUE
             && fieldaddr_off.count(in.dst)) {
                in.op = IrOp::NOP; in.operands.clear();
                in.dst = IR_NO_VALUE; in.func_name.clear();
            }
        }
    }

    /* (f) compactar NOPs sin dst/operandos. */
    for (auto &bb : fn.blocks) {
        auto &is = bb.instrs;
        is.erase(std::remove_if(is.begin(), is.end(), [](const IrInstr &i) {
            return i.op == IrOp::NOP && i.operands.empty() && i.dst == IR_NO_VALUE;
        }), is.end());
    }
    return true;
}

}  // namespace

/**
 * @brief Phase C2.13: Scalar Replacement de objetos GC no-escapantes.
 *
 * Elimina los `new X()` que no escapan y cuyo ctor es un inicializador
 * trivial de campos, reemplazando los field-reads por los valores de
 * construccion.  Resultado: el alloc GC desaparece por completo (tanto en
 * interp como en JIT, porque ambos consumen el mismo IR optimizado).
 *
 * Conservador por diseno: cada sitio se transforma SOLO si todas las
 * precondiciones de seguridad se cumplen; en caso de duda, no se toca.
 *
 * @return true si se transformo algun sitio.
 */
bool ir_pass_scalar_replace_gc(IrFunction &fn, const IrModule &mod) {
    if (fn.is_native || fn.values.empty()) return false;

    auto sites = analyze_gc_escape(fn);
    if (sites.empty()) return false;

    const bool dbg = env_flag_on("VESTA_ESCAPE_DEBUG");
    auto diag = [&](const GcAllocSite &s, const std::string &why) {
        if (dbg) std::fprintf(stderr,
            "[escape] fn '%s': new %s() (dst %%%u) NO transformado: %s\n",
            fn.name.c_str(), s.class_name.c_str(),
            static_cast<unsigned>(s.dst), why.c_str());
    };

    /* Cache de modelos de ctor por clase (validos e invalidos) + razon. */
    struct CachedModel { SrCtorModel m; std::string reason; };
    std::unordered_map<std::string, CachedModel> model_cache;
    auto get_model = [&](const std::string &cls, std::string &out_reason)
            -> const SrCtorModel * {
        auto it = model_cache.find(cls);
        if (it == model_cache.end()) {
            CachedModel cm;
            sr_build_ctor_model(mod, cls, cm.m, &cm.reason);
            it = model_cache.emplace(cls, std::move(cm)).first;
        }
        out_reason = it->second.reason;
        return it->second.m.valid ? &it->second.m : nullptr;
    };

    bool changed = false;

    for (const auto &site : sites) {
        if (site.escapes) continue;
        const IrValueId obj = site.dst;

        std::string mreason;
        const SrCtorModel *model = get_model(site.class_name, mreason);
        if (!model) { diag(site, "ctor: " + mreason); continue; }

        /* La instr del CALL seed (para leer sus args + NOPearla luego). */
        if (site.block_idx >= fn.blocks.size()) continue;
        auto &seed_blk = fn.blocks[site.block_idx];
        if (site.ins_idx >= seed_blk.instrs.size()) continue;
        IrInstr &call_ins = seed_blk.instrs[site.ins_idx];
        if (call_ins.op != IrOp::CALL || call_ins.dst != obj) continue;
        const std::vector<IrValueId> args = call_ins.operands;  /* copia */
        if (args.size() != model->num_new_args) continue;       /* arity mismatch */

        /* --- Recolectar TODOS los usos de obj.  Deben ser solo
         * `add.ptr obj, Kconst` (direccion de campo) o `load obj` (offset 0).
         * Cada field-addr solo puede usarse en LOADs.  Si algo no encaja ->
         * abortar este sitio (no transformar). --- */
        struct LoadRef  { size_t bi; size_t ii; uint32_t off; };
        struct StoreRef { size_t ii; uint32_t off; };
        std::vector<LoadRef>  loads;       /* loads a reescribir */
        std::vector<std::pair<size_t,size_t>> dead_addr;  /* add.ptr a NOPear */
        std::unordered_map<IrValueId, uint32_t> fieldaddr_off;  /* addr_vid -> off */
        bool ok = true;
        bool single_block = true;          /* todos los usos en el bloque del call */
        bool has_writes   = false;         /* algun STORE a un campo del objeto */
        const char *use_reason = "uso no soportado de obj";

        /* Helper: lee el offset const de un `add.ptr obj, K`. */
        auto const_value_of = [&](IrValueId v, uint64_t &out_k) -> bool {
            if (v == IR_NO_VALUE || v >= fn.values.size()) return false;
            if (fn.values[v].is_const) { out_k = fn.values[v].const_val; return true; }
            /* Buscar la CONST que produjo v. */
            for (const auto &b : fn.blocks)
                for (const auto &in : b.instrs)
                    if (in.dst == v && in.op == IrOp::CONST) { out_k = in.imm; return true; }
            return false;
        };

        /* Pasada A: localizar field-addrs derivadas de obj + loads/stores
         * directos (offset 0).  Trackea single_block + has_writes. */
        for (size_t bi = 0; bi < fn.blocks.size() && ok; ++bi) {
            const auto &b = fn.blocks[bi];
            for (size_t ii = 0; ii < b.instrs.size() && ok; ++ii) {
                const auto &in = b.instrs[ii];
                /* obj usado en phi_args o func_ptr -> uso no modelable como
                 * field-access (p.ej. `x = phi(a, b)`).  El escape analysis lo
                 * considera no-escapante pero el transform no sabe materializar
                 * el campo a traves de un merge -> abortar (necesitaria SROA con
                 * PHI de los valores de campo). */
                bool bad_use = false;
                for (const auto &pa : in.phi_args) if (pa.value == obj) { bad_use = true; break; }
                if (in.func_ptr == obj) bad_use = true;
                if (bad_use) {
                    ok = false; use_reason = "obj usado en PHI/func_ptr (necesita SROA con PHI)";
                    break;
                }
                bool uses_obj = false;
                for (auto v : in.operands) if (v == obj) { uses_obj = true; break; }
                if (!uses_obj) continue;

                if (in.op == IrOp::ADD && in.operands.size() == 2
                 && in.dst != IR_NO_VALUE) {
                    /* `add obj, Kconst` o `add Kconst, obj`. */
                    if (in.operands[0] == obj && in.operands[1] == obj) {
                        ok = false; use_reason = "obj en ambos operandos de add"; break;
                    }
                    IrValueId other = (in.operands[0] == obj) ? in.operands[1]
                                                              : in.operands[0];
                    uint64_t k;
                    if (!const_value_of(other, k)) {
                        ok = false; use_reason = "field-addr con offset no-const"; break;
                    }
                    fieldaddr_off[in.dst] = static_cast<uint32_t>(k);
                    dead_addr.push_back({bi, ii});
                    if (bi != site.block_idx) single_block = false;
                } else if (in.op == IrOp::LOAD && !in.operands.empty()
                        && in.operands[0] == obj) {
                    loads.push_back({bi, ii, 0});  /* load directo -> offset 0 */
                    if (bi != site.block_idx) single_block = false;
                } else if (in.op == IrOp::STORE && in.operands.size() >= 2
                        && in.operands[1] == obj && in.operands[0] != obj) {
                    has_writes = true;             /* store directo -> offset 0 */
                    if (bi != site.block_idx) single_block = false;
                } else {
                    ok = false;
                    use_reason = "obj usado fuera de field-access (CMP/callvirt/store-val/GEP/...)";
                    break;
                }
            }
        }
        if (!ok) { diag(site, use_reason); continue; }

        /* Pasada B: cada field-addr solo puede usarse en LOAD o STORE-addr.
         * Recolecta loads + marca has_writes; trackea single_block. */
        for (size_t bi = 0; bi < fn.blocks.size() && ok; ++bi) {
            const auto &b = fn.blocks[bi];
            for (size_t ii = 0; ii < b.instrs.size() && ok; ++ii) {
                const auto &in = b.instrs[ii];
                /* field-addr usada en phi_args/func_ptr -> no soportado. */
                for (const auto &pa : in.phi_args)
                    if (fieldaddr_off.count(pa.value)) { ok = false; break; }
                if (in.func_ptr != IR_NO_VALUE && fieldaddr_off.count(in.func_ptr)) ok = false;
                if (!ok) { use_reason = "field-addr usada en PHI/func_ptr"; break; }
                /* Es field-addr operando de esta instr? */
                bool touches_fa = false; IrValueId fav = IR_NO_VALUE; uint32_t foff = 0;
                for (auto v : in.operands) {
                    auto it = fieldaddr_off.find(v);
                    if (it != fieldaddr_off.end()) { touches_fa = true; fav = v; foff = it->second; break; }
                }
                if (!touches_fa) continue;

                if (in.op == IrOp::LOAD && !in.operands.empty()
                 && in.operands[0] == fav) {
                    loads.push_back({bi, ii, foff});
                    if (bi != site.block_idx) single_block = false;
                } else if (in.op == IrOp::STORE && in.operands.size() >= 2
                        && in.operands[1] == fav && in.operands[0] != fav) {
                    /* field-write: la field-addr es la DIRECCION (operand 1),
                     * no el valor.  Si la field-addr fuera el VALOR -> escape. */
                    has_writes = true;
                    if (bi != site.block_idx) single_block = false;
                } else {
                    ok = false;
                    use_reason = "field-addr usado por op no-LOAD/STORE (o como valor)";
                }
            }
        }
        if (!ok) { diag(site, use_reason); continue; }

        /* ====================================================================
         * Caso A: SIN escrituras -> path read-only (cross-block OK).
         * Reemplaza cada load por el valor de construccion del campo. */
        if (!has_writes) {
            struct PendingRewrite { size_t bi; size_t ii; const SrFieldInit *fi; };
            std::vector<PendingRewrite> pending;
            for (const auto &lr : loads) {
                const SrFieldInit *fi = model->find(lr.off);
                if (!fi) {
                    ok = false;
                    use_reason = "load de campo no inicializado por el ctor (default-0)";
                    break;
                }
                IrInstr &probe = fn.blocks[lr.bi].instrs[lr.ii];
                if (!sr_rewrite_load(probe, *fi, args, fn, /*apply=*/false)) {
                    ok = false;
                    use_reason = "tipo de campo no soportado (float/ptr/handle/widening)";
                    break;
                }
                pending.push_back({lr.bi, lr.ii, fi});
            }
            if (!ok) { diag(site, use_reason); continue; }

            for (const auto &pr : pending) {
                IrInstr &ld = fn.blocks[pr.bi].instrs[pr.ii];
                sr_rewrite_load(ld, *pr.fi, args, fn, /*apply=*/true);
            }
            for (const auto &da : dead_addr) {
                IrInstr &ai = fn.blocks[da.first].instrs[da.second];
                ai.op = IrOp::NOP; ai.operands.clear();
                ai.dst = IR_NO_VALUE; ai.func_name.clear();
            }
            call_ins.op = IrOp::NOP; call_ins.operands.clear();
            call_ins.dst = IR_NO_VALUE; call_ins.func_name.clear();
            changed = true;
            continue;
        }

        /* ====================================================================
         * Caso B: CON escrituras.
         *
         * B.1 single-block -> field versioning LINEAL: un walk lineal hace
         *     store-to-load forwarding, elimina los stores y borra el alloc.
         *
         * B.2 cross-block / loop -> SROA/mem2reg: promueve los campos a SSA con
         *     insercion de PHI (Cytron) + renaming.
         *
         *     Default-on con COST-MODEL: el propio @c sr_mem2reg_object baila si
         *     promover anyadiria un if-merge PHI DENTRO de un loop (= escritura
         *     condicional de campo en el loop), que pessimiza el interp (16
         *     registros VM -> copies + presion).  Los casos que SI promueve son
         *     win (acumuladores incondicionales en loop: +14..41% interp; cross-
         *     block fuera de loops: coste unico + elimina el alloc).
         *     VESTA_NO_ESCAPE_MEM2REG=1 lo desactiva entero;
         *     VESTA_ESCAPE_MEM2REG_FORCE=1 ignora el cost-model. */
        if (!single_block) {
            static const bool mem2reg_off = env_flag_on("VESTA_NO_ESCAPE_MEM2REG");
            if (!mem2reg_off) {
                std::string mr;
                if (sr_mem2reg_object(fn, *model, site.block_idx, site.ins_idx,
                                      obj, args, fieldaddr_off, mr)) {
                    changed = true;
                    continue;
                }
                diag(site, "mem2reg: " + mr);
            } else {
                diag(site, "field-write cross-block (mem2reg off)");
            }
            continue;
        }

        {
            auto &blkv = fn.blocks[site.block_idx].instrs;
            /* DRY-RUN: simular current[offset] en orden de bloque + validar. */
            struct LdPlan { size_t ii; bool from_store; IrValueId stored; const SrFieldInit *fi; };
            std::vector<LdPlan> ld_plan;
            std::vector<size_t> store_iis;
            std::unordered_map<uint32_t, IrValueId> sim;  /* offset -> ultimo valor escrito */
            const char *vreason = "versioning";
            bool vok = true;

            for (size_t ii = 0; ii < blkv.size() && vok; ++ii) {
                const auto &in = blkv[ii];
                uint32_t off = 0; bool is_ld = false, is_st = false; IrValueId sval = IR_NO_VALUE;
                if (in.op == IrOp::LOAD && !in.operands.empty()) {
                    IrValueId addr = in.operands[0];
                    if (addr == obj) { off = 0; is_ld = true; }
                    else { auto it = fieldaddr_off.find(addr);
                           if (it != fieldaddr_off.end()) { off = it->second; is_ld = true; } }
                } else if (in.op == IrOp::STORE && in.operands.size() >= 2) {
                    IrValueId addr = in.operands[1];
                    if (addr == obj) { off = 0; is_st = true; sval = in.operands[0]; }
                    else { auto it = fieldaddr_off.find(addr);
                           if (it != fieldaddr_off.end()) { off = it->second; is_st = true; sval = in.operands[0]; } }
                }
                if (is_ld) {
                    /* Solo enteros: el forwarding via MOV de un valor float/
                     * ptr/handle podria mezclar bancos GP/ZMM en codegen.
                     * Consistente con el path read-only (int-only). */
                    if (!sr_type_is_int(in.type)) {
                        vok = false; vreason = "campo no-entero con field-write (float/ptr/handle)"; break;
                    }
                    auto sit = sim.find(off);
                    if (sit != sim.end()) {
                        /* forward del ultimo store: el tipo del valor debe
                         * coincidir con el tipo leido. */
                        IrValueId v = sit->second;
                        if (v == IR_NO_VALUE || v >= fn.values.size()
                         || fn.values[v].type != in.type) {
                            vok = false; vreason = "tipo store/load no coincide"; break;
                        }
                        ld_plan.push_back({ii, true, v, nullptr});
                    } else {
                        /* primer load del campo: valor de construccion. */
                        const SrFieldInit *fi = model->find(off);
                        if (!fi) { vok = false; vreason = "load de campo no inicializado (default-0)"; break; }
                        IrInstr probe = blkv[ii];   /* copia: apply=false no muta */
                        if (!sr_rewrite_load(probe, *fi, args, fn, /*apply=*/false)) {
                            vok = false; vreason = "tipo de campo no soportado (float/ptr/handle/widening)"; break;
                        }
                        ld_plan.push_back({ii, false, IR_NO_VALUE, fi});
                    }
                } else if (is_st) {
                    sim[off] = sval;
                    store_iis.push_back(ii);
                }
            }
            if (!vok) { diag(site, vreason); continue; }

            /* APLICAR: reescribir loads, NOPear stores + field-addrs + call. */
            for (const auto &lp : ld_plan) {
                IrInstr &ld = blkv[lp.ii];
                if (lp.from_store) {
                    ld.op = IrOp::MOV; ld.operands.clear();
                    ld.operands.push_back(lp.stored);
                    ld.func_name.clear();
                    if (ld.dst != IR_NO_VALUE && ld.dst < fn.values.size()) {
                        fn.values[ld.dst].is_const = false;
                        fn.values[ld.dst].is_host_ptr = false;
                    }
                } else {
                    sr_rewrite_load(ld, *lp.fi, args, fn, /*apply=*/true);
                }
            }
            for (size_t sii : store_iis) {
                IrInstr &st = blkv[sii];
                st.op = IrOp::NOP; st.operands.clear();
                st.dst = IR_NO_VALUE; st.func_name.clear();
            }
            for (const auto &da : dead_addr) {
                IrInstr &ai = fn.blocks[da.first].instrs[da.second];
                ai.op = IrOp::NOP; ai.operands.clear();
                ai.dst = IR_NO_VALUE; ai.func_name.clear();
            }
            call_ins.op = IrOp::NOP; call_ins.operands.clear();
            call_ins.dst = IR_NO_VALUE; call_ins.func_name.clear();
            changed = true;
        }
    }

    /* Compactar NOPs introducidos (mismo patron que promote_local_raw_alloc). */
    if (changed) {
        for (auto &blk : fn.blocks) {
            auto &is = blk.instrs;
            is.erase(std::remove_if(is.begin(), is.end(), [](const IrInstr &i) {
                return i.op == IrOp::NOP && i.operands.empty()
                    && i.dst == IR_NO_VALUE;
            }), is.end());
        }
    }

    return changed;
}

// =========================================================================
//  Pase ir_pass_simplify
// =========================================================================
//
// Aplica identidades algebraicas, plegado de constantes a traves de
// casts, y simplificacion de phis triviales.  Beneficia IGUAL al JIT
// (menos instrucciones emitidas) y a los port targets (codigo C mas
// limpio).
//
// Patrones reconocidos:
//   (A) Algebraic identities:
//       add x, 0     -> x
//       sub x, 0     -> x
//       mul x, 1     -> x
//       mul x, 0     -> 0
//       and x, 0     -> 0
//       and x, -1    -> x
//       or  x, 0     -> x
//       or  x, -1    -> -1
//       xor x, 0     -> x
//       sub x, x     -> 0
//       xor x, x     -> 0
//       shl x, 0, shr x, 0, sar x, 0 -> x
//
//   (B) Cast de constantes:
//       sext.T (const.U K)   -> const.T sign_extend(K)
//       zext.T (const.U K)   -> const.T zero_extend(K)
//       trunc.T (const.U K)  -> const.T K & mask(T)
//       bitcast.T (const.U K) -> const.T K  (mismo ancho ya)
//       cast.T (const.U K)   -> const.T K   (best-effort)
//
//   (C) Phi simplification:
//       %a = phi(b, b, b)   -> %a = mov b   (todos iguales)
//       %a = phi(a, x, y)   -> ignorar self-ref; si solo queda 1 unique -> %a = mov ese
//
// Helpers para Trabajar con CONST + sus valores.

namespace {

bool is_const_with_value(const IrFunction &fn, IrValueId vid, int64_t &out) {
    if (vid == IR_NO_VALUE) return false;
    /* Solo CONST inicializa SSA con un imm conocido.  Buscar la instr
     * que produjo @p vid en cualquier bloque. */
    for (const auto &bb : fn.blocks) {
        for (const auto &ins : bb.instrs) {
            if (ins.dst == vid && ins.op == IrOp::CONST) {
                out = static_cast<int64_t>(ins.imm);
                return true;
            }
        }
    }
    return false;
}

/** @brief Mascara de bits para truncar a @p type. */
uint64_t type_mask(IrType t) {
    switch (t) {
        case IrType::I8:  case IrType::U8:  case IrType::BOOL: return 0xFFu;
        case IrType::I16: case IrType::U16: return 0xFFFFu;
        case IrType::I32: case IrType::U32: case IrType::F32: return 0xFFFFFFFFu;
        default: return ~static_cast<uint64_t>(0u);
    }
}

bool type_is_signed_int(IrType t) {
    return t == IrType::I8 || t == IrType::I16
        || t == IrType::I32 || t == IrType::I64;
}

/** @brief Sign-extiende @p v desde @p from_t a 64-bit. */
int64_t sign_extend_from(int64_t v, IrType from_t) {
    switch (from_t) {
        case IrType::I8:  return static_cast<int8_t>(v);
        case IrType::I16: return static_cast<int16_t>(v);
        case IrType::I32: return static_cast<int32_t>(v);
        default: return v;
    }
}

/** @brief Sustituye @p ins por un MOV de @p src_vid (mantiene dst).
 *  Devuelve true. */
void rewrite_as_mov(IrInstr &ins, IrValueId src_vid) {
    ins.op = IrOp::MOV;
    ins.operands.clear();
    ins.operands.push_back(src_vid);
    ins.phi_args.clear();
    ins.imm = 0;
    ins.func_name.clear();
    ins.func_ptr = IR_NO_VALUE;
    ins.target_block = IR_NO_BLOCK;
    ins.false_block = IR_NO_BLOCK;
}

/** @brief Convierte @p ins en un CONST con valor @p imm.  No cambia tipo. */
void rewrite_as_const(IrInstr &ins, uint64_t imm) {
    ins.op = IrOp::CONST;
    ins.imm = imm;
    ins.operands.clear();
    ins.phi_args.clear();
    ins.func_name.clear();
    ins.func_ptr = IR_NO_VALUE;
    ins.target_block = IR_NO_BLOCK;
    ins.false_block = IR_NO_BLOCK;
}

/** @brief Como @c rewrite_as_const pero ademas marca @c is_const + @c const_val
 *         en el @c IrValue correspondiente para que consumers downstream
 *         (regalloc/selector imm32 fold) detecten la constante. */
void rewrite_as_const_with_value(IrFunction &fn, IrInstr &ins, uint64_t imm) {
    rewrite_as_const(ins, imm);
    if (ins.dst != IR_NO_VALUE && ins.dst < fn.values.size()) {
        fn.values[ins.dst].is_const  = true;
        fn.values[ins.dst].const_val = imm;
    }
}

} // namespace

bool ir_pass_simplify(IrFunction &fn) {
    bool changed = false;

    /* Pre-build: vid -> CONST imm (si lo es).  Evita el escaneo lineal
     * dentro del bucle principal. */
    std::unordered_map<IrValueId, int64_t> const_vids;
    for (const auto &bb : fn.blocks) {
        for (const auto &ins : bb.instrs) {
            if (ins.op == IrOp::CONST && ins.dst != IR_NO_VALUE) {
                const_vids[ins.dst] = static_cast<int64_t>(ins.imm);
            }
        }
    }
    auto get_const = [&](IrValueId v, int64_t &out) -> bool {
        if (v == IR_NO_VALUE) return false;
        auto it = const_vids.find(v);
        if (it == const_vids.end()) return false;
        out = it->second;
        return true;
    };

    for (auto &bb : fn.blocks) {
        for (auto &ins : bb.instrs) {
            switch (ins.op) {
                /* ---- (A) Algebraic identities ---- */
                case IrOp::ADD: {
                    if (ins.operands.size() < 2) break;
                    int64_t c = 0;
                    if (get_const(ins.operands[1], c) && c == 0) {
                        rewrite_as_mov(ins, ins.operands[0]);
                        changed = true;
                    } else if (get_const(ins.operands[0], c) && c == 0) {
                        rewrite_as_mov(ins, ins.operands[1]);
                        changed = true;
                    }
                    break;
                }
                case IrOp::SUB: {
                    if (ins.operands.size() < 2) break;
                    int64_t c = 0;
                    if (get_const(ins.operands[1], c) && c == 0) {
                        rewrite_as_mov(ins, ins.operands[0]);
                        changed = true;
                    } else if (ins.operands[0] == ins.operands[1]) {
                        rewrite_as_const_with_value(fn, ins, 0);
                        const_vids[ins.dst] = 0;
                        changed = true;
                    }
                    break;
                }
                case IrOp::MUL: {
                    if (ins.operands.size() < 2) break;
                    int64_t c = 0;
                    if (get_const(ins.operands[1], c)) {
                        if (c == 1) {
                            rewrite_as_mov(ins, ins.operands[0]);
                            changed = true;
                        } else if (c == 0) {
                            rewrite_as_const_with_value(fn, ins, 0);
                            const_vids[ins.dst] = 0;
                            changed = true;
                        }
                    } else if (get_const(ins.operands[0], c)) {
                        if (c == 1) {
                            rewrite_as_mov(ins, ins.operands[1]);
                            changed = true;
                        } else if (c == 0) {
                            rewrite_as_const_with_value(fn, ins, 0);
                            const_vids[ins.dst] = 0;
                            changed = true;
                        }
                    }
                    break;
                }
                case IrOp::DIV: {
                    if (ins.operands.size() < 2) break;
                    int64_t c = 0;
                    /* x / 1 = x */
                    if (get_const(ins.operands[1], c) && c == 1) {
                        rewrite_as_mov(ins, ins.operands[0]);
                        changed = true;
                    } else if (ins.operands[0] == ins.operands[1]) {
                        /* x / x = 1 (cuidado con x = 0 -> division by zero,
                         * pero el codigo SSA implica que ya se ha dividido,
                         * asi que x != 0; seguro reescribir como const 1) */
                        rewrite_as_const_with_value(fn, ins, 1);
                        const_vids[ins.dst] = 1;
                        changed = true;
                    }
                    break;
                }
                case IrOp::MOD: {
                    if (ins.operands.size() < 2) break;
                    int64_t c = 0;
                    /* x % 1 = 0 */
                    if (get_const(ins.operands[1], c) && c == 1) {
                        rewrite_as_const_with_value(fn, ins, 0);
                        const_vids[ins.dst] = 0;
                        changed = true;
                    } else if (ins.operands[0] == ins.operands[1]) {
                        /* x % x = 0 (x != 0 implicito) */
                        rewrite_as_const_with_value(fn, ins, 0);
                        const_vids[ins.dst] = 0;
                        changed = true;
                    }
                    break;
                }
                case IrOp::AND: {
                    if (ins.operands.size() < 2) break;
                    int64_t c = 0;
                    /* commutative: probar ambos operandos */
                    if (get_const(ins.operands[1], c)) {
                        if (c == 0) {
                            rewrite_as_const_with_value(fn, ins, 0);
                            const_vids[ins.dst] = 0;
                            changed = true;
                        } else if (c == -1) {
                            rewrite_as_mov(ins, ins.operands[0]);
                            changed = true;
                        }
                    } else if (get_const(ins.operands[0], c)) {
                        if (c == 0) {
                            rewrite_as_const_with_value(fn, ins, 0);
                            const_vids[ins.dst] = 0;
                            changed = true;
                        } else if (c == -1) {
                            rewrite_as_mov(ins, ins.operands[1]);
                            changed = true;
                        }
                    } else if (ins.operands[0] == ins.operands[1]) {
                        rewrite_as_mov(ins, ins.operands[0]);
                        changed = true;
                    }
                    break;
                }
                case IrOp::OR: {
                    if (ins.operands.size() < 2) break;
                    int64_t c = 0;
                    if (get_const(ins.operands[1], c)) {
                        if (c == 0) {
                            rewrite_as_mov(ins, ins.operands[0]);
                            changed = true;
                        } else if (c == -1) {
                            rewrite_as_const_with_value(fn, ins, static_cast<uint64_t>(-1));
                            const_vids[ins.dst] = -1;
                            changed = true;
                        }
                    } else if (get_const(ins.operands[0], c)) {
                        if (c == 0) {
                            rewrite_as_mov(ins, ins.operands[1]);
                            changed = true;
                        } else if (c == -1) {
                            rewrite_as_const_with_value(fn, ins, static_cast<uint64_t>(-1));
                            const_vids[ins.dst] = -1;
                            changed = true;
                        }
                    } else if (ins.operands[0] == ins.operands[1]) {
                        rewrite_as_mov(ins, ins.operands[0]);
                        changed = true;
                    }
                    break;
                }
                case IrOp::XOR: {
                    if (ins.operands.size() < 2) break;
                    int64_t c = 0;
                    if (get_const(ins.operands[1], c) && c == 0) {
                        rewrite_as_mov(ins, ins.operands[0]);
                        changed = true;
                    } else if (get_const(ins.operands[0], c) && c == 0) {
                        rewrite_as_mov(ins, ins.operands[1]);
                        changed = true;
                    } else if (ins.operands[0] == ins.operands[1]) {
                        rewrite_as_const_with_value(fn, ins, 0);
                        const_vids[ins.dst] = 0;
                        changed = true;
                    }
                    break;
                }
                case IrOp::SHL:
                case IrOp::SHR:
                case IrOp::SAR: {
                    if (ins.operands.size() < 2) break;
                    int64_t c = 0;
                    if (get_const(ins.operands[1], c) && c == 0) {
                        rewrite_as_mov(ins, ins.operands[0]);
                        changed = true;
                    }
                    break;
                }

                /* ---- (B) Cast de constantes ---- */
                case IrOp::SEXT: {
                    if (ins.operands.empty()) break;
                    /* Identidad: si src_type == dst_type, es un MOV. */
                    IrType from_t = (ins.operands[0] < fn.values.size())
                                    ? fn.values[ins.operands[0]].type
                                    : IrType::I64;
                    if (from_t == ins.type) {
                        rewrite_as_mov(ins, ins.operands[0]);
                        changed = true;
                        break;
                    }
                    int64_t c = 0;
                    if (get_const(ins.operands[0], c)) {
                        int64_t ext = sign_extend_from(c, from_t);
                        rewrite_as_const_with_value(fn, ins, static_cast<uint64_t>(ext));
                        const_vids[ins.dst] = ext;
                        changed = true;
                    }
                    break;
                }
                case IrOp::ZEXT: {
                    if (ins.operands.empty()) break;
                    IrType from_t = (ins.operands[0] < fn.values.size())
                                    ? fn.values[ins.operands[0]].type
                                    : IrType::I64;
                    if (from_t == ins.type) {
                        rewrite_as_mov(ins, ins.operands[0]);
                        changed = true;
                        break;
                    }
                    int64_t c = 0;
                    if (get_const(ins.operands[0], c)) {
                        uint64_t z = static_cast<uint64_t>(c) & type_mask(from_t);
                        rewrite_as_const_with_value(fn, ins, z);
                        const_vids[ins.dst] = static_cast<int64_t>(z);
                        changed = true;
                    }
                    break;
                }
                case IrOp::TRUNC: {
                    if (ins.operands.empty()) break;
                    IrType from_t = (ins.operands[0] < fn.values.size())
                                    ? fn.values[ins.operands[0]].type
                                    : IrType::I64;
                    if (from_t == ins.type) {
                        rewrite_as_mov(ins, ins.operands[0]);
                        changed = true;
                        break;
                    }
                    int64_t c = 0;
                    if (get_const(ins.operands[0], c)) {
                        uint64_t m = type_mask(ins.type);
                        uint64_t v = static_cast<uint64_t>(c) & m;
                        /* Si el tipo destino es signed, sign-extender de
                         * vuelta a i64 para preservar el valor logico. */
                        int64_t out_val;
                        if (type_is_signed_int(ins.type)) {
                            out_val = sign_extend_from(static_cast<int64_t>(v), ins.type);
                        } else {
                            out_val = static_cast<int64_t>(v);
                        }
                        rewrite_as_const_with_value(fn, ins, static_cast<uint64_t>(out_val));
                        const_vids[ins.dst] = out_val;
                        changed = true;
                    }
                    break;
                }
                case IrOp::BITCAST:
                case IrOp::CAST: {
                    if (ins.operands.empty()) break;
                    IrType from_t = (ins.operands[0] < fn.values.size())
                                    ? fn.values[ins.operands[0]].type
                                    : IrType::I64;
                    if (from_t == ins.type) {
                        rewrite_as_mov(ins, ins.operands[0]);
                        changed = true;
                        break;
                    }
                    int64_t c = 0;
                    if (get_const(ins.operands[0], c)) {
                        rewrite_as_const_with_value(fn, ins, static_cast<uint64_t>(c));
                        const_vids[ins.dst] = c;
                        changed = true;
                    }
                    break;
                }

                /* ---- (D) Math IR ops constant folding (sprint v2.2c) ----
                 *
                 * Cuando todos los operandos son CONST conocidos, evaluamos
                 * la operacion en compile-time y reemplazamos por CONST
                 * literal.  Float ops preservan bits IEEE 754 via memcpy.
                 * Habilita propagacion downstream (e.g. `sqrt(25.0) + 3.0`
                 * pliega a CONST 8.0 directamente). */
                case IrOp::FSQRT:
                case IrOp::FABS:
                case IrOp::FNEG:
                case IrOp::FFLOOR:
                case IrOp::FCEIL:
                case IrOp::FROUND:
                case IrOp::FTRUNC: {
                    if (ins.operands.empty()) break;
                    int64_t c0 = 0;
                    if (!get_const(ins.operands[0], c0)) break;
                    const bool is_f32 = (ins.type == IrType::F32);
                    double in_v = is_f32
                        ? static_cast<double>(bits_to_f32(static_cast<uint32_t>(c0)))
                        : bits_to_f64(static_cast<uint64_t>(c0));
                    double out_v = 0.0;
                    switch (ins.op) {
                        case IrOp::FSQRT:  out_v = std::sqrt(in_v); break;
                        case IrOp::FABS:   out_v = std::fabs(in_v); break;
                        case IrOp::FNEG:   out_v = -in_v;           break;
                        case IrOp::FFLOOR: out_v = std::floor(in_v); break;
                        case IrOp::FCEIL:  out_v = std::ceil(in_v);  break;
                        case IrOp::FROUND: out_v = std::nearbyint(in_v); break;
                        case IrOp::FTRUNC: out_v = std::trunc(in_v); break;
                        default: break;
                    }
                    uint64_t out_bits = is_f32
                        ? static_cast<uint64_t>(f32_to_bits(static_cast<float>(out_v)))
                        : f64_to_bits(out_v);
                    rewrite_as_const_with_value(fn, ins, out_bits);
                    const_vids[ins.dst] = static_cast<int64_t>(out_bits);
                    changed = true;
                    break;
                }
                case IrOp::FADD:
                case IrOp::FSUB:
                case IrOp::FMUL:
                case IrOp::FDIV:
                case IrOp::FMIN:
                case IrOp::FMAX: {
                    if (ins.operands.size() < 2) break;
                    int64_t c0 = 0, c1 = 0;
                    if (!get_const(ins.operands[0], c0)) break;
                    if (!get_const(ins.operands[1], c1)) break;
                    const bool is_f32 = (ins.type == IrType::F32);
                    auto bits_to_dbl = [&](int64_t v) -> double {
                        return is_f32
                            ? static_cast<double>(bits_to_f32(static_cast<uint32_t>(v)))
                            : bits_to_f64(static_cast<uint64_t>(v));
                    };
                    double a = bits_to_dbl(c0);
                    double b = bits_to_dbl(c1);
                    double r = 0.0;
                    bool ok = true;
                    switch (ins.op) {
                        case IrOp::FADD: r = a + b; break;
                        case IrOp::FSUB: r = a - b; break;
                        case IrOp::FMUL: r = a * b; break;
                        case IrOp::FDIV:
                            /* Folding de FDIV por 0 produciria NaN/Inf
                             * deterministico en IEEE 754; aceptable. */
                            r = a / b; break;
                        case IrOp::FMIN: r = std::fmin(a, b); break;
                        case IrOp::FMAX: r = std::fmax(a, b); break;
                        default: ok = false; break;
                    }
                    if (!ok) break;
                    uint64_t out_bits = is_f32
                        ? static_cast<uint64_t>(f32_to_bits(static_cast<float>(r)))
                        : f64_to_bits(r);
                    rewrite_as_const_with_value(fn, ins, out_bits);
                    const_vids[ins.dst] = static_cast<int64_t>(out_bits);
                    changed = true;
                    break;
                }
                case IrOp::IABS: {
                    if (ins.operands.empty()) break;
                    int64_t c0 = 0;
                    if (!get_const(ins.operands[0], c0)) break;
                    /* INT_MIN -> UB en C; el IR doc dice "undef".  Para no
                     * crashear, dejar como esta (no foldear). */
                    if (c0 == std::numeric_limits<int64_t>::min()) break;
                    int64_t r = c0 < 0 ? -c0 : c0;
                    rewrite_as_const_with_value(fn, ins, static_cast<uint64_t>(r));
                    const_vids[ins.dst] = r;
                    changed = true;
                    break;
                }
                case IrOp::IMIN:
                case IrOp::IMAX: {
                    if (ins.operands.size() < 2) break;
                    int64_t c0 = 0, c1 = 0;
                    if (!get_const(ins.operands[0], c0)) break;
                    if (!get_const(ins.operands[1], c1)) break;
                    int64_t r = (ins.op == IrOp::IMIN)
                        ? (c0 < c1 ? c0 : c1)
                        : (c0 > c1 ? c0 : c1);
                    rewrite_as_const_with_value(fn, ins, static_cast<uint64_t>(r));
                    const_vids[ins.dst] = r;
                    changed = true;
                    break;
                }
                case IrOp::IMINU:
                case IrOp::IMAXU: {
                    if (ins.operands.size() < 2) break;
                    int64_t c0 = 0, c1 = 0;
                    if (!get_const(ins.operands[0], c0)) break;
                    if (!get_const(ins.operands[1], c1)) break;
                    uint64_t u0 = static_cast<uint64_t>(c0);
                    uint64_t u1 = static_cast<uint64_t>(c1);
                    uint64_t r = (ins.op == IrOp::IMINU)
                        ? (u0 < u1 ? u0 : u1)
                        : (u0 > u1 ? u0 : u1);
                    rewrite_as_const_with_value(fn, ins, r);
                    const_vids[ins.dst] = static_cast<int64_t>(r);
                    changed = true;
                    break;
                }
                case IrOp::ILOG2: {
                    if (ins.operands.empty()) break;
                    int64_t c0 = 0;
                    if (!get_const(ins.operands[0], c0)) break;
                    uint64_t u = static_cast<uint64_t>(c0);
                    if (u == 0) break; /* undef segun doc IR; no foldear. */
                    int r = 63;
                #if defined(__GNUC__) || defined(__clang__)
                    r = 63 - __builtin_clzll(u);
                #else
                    for (r = 63; r >= 0 && ((u >> r) & 1ULL) == 0; --r) {}
                #endif
                    rewrite_as_const_with_value(fn, ins, static_cast<uint64_t>(r));
                    const_vids[ins.dst] = r;
                    changed = true;
                    break;
                }
                case IrOp::POPCNT: {
                    if (ins.operands.empty()) break;
                    int64_t c0 = 0;
                    if (!get_const(ins.operands[0], c0)) break;
                    uint64_t u = static_cast<uint64_t>(c0);
                    int r;
                #if defined(__GNUC__) || defined(__clang__)
                    r = __builtin_popcountll(u);
                #else
                    r = 0; for (; u; u &= u - 1) ++r;
                #endif
                    rewrite_as_const_with_value(fn, ins, static_cast<uint64_t>(r));
                    const_vids[ins.dst] = r;
                    changed = true;
                    break;
                }
                case IrOp::CLZ: {
                    if (ins.operands.empty()) break;
                    int64_t c0 = 0;
                    if (!get_const(ins.operands[0], c0)) break;
                    uint64_t u = static_cast<uint64_t>(c0);
                    if (u == 0) break; /* clz(0) undef en x86 lzcnt; no foldear. */
                    int r;
                #if defined(__GNUC__) || defined(__clang__)
                    r = __builtin_clzll(u);
                #else
                    r = 0; while (((u >> 63) & 1ULL) == 0) { u <<= 1; ++r; }
                #endif
                    rewrite_as_const_with_value(fn, ins, static_cast<uint64_t>(r));
                    const_vids[ins.dst] = r;
                    changed = true;
                    break;
                }
                case IrOp::CTZ: {
                    if (ins.operands.empty()) break;
                    int64_t c0 = 0;
                    if (!get_const(ins.operands[0], c0)) break;
                    uint64_t u = static_cast<uint64_t>(c0);
                    if (u == 0) break; /* ctz(0) undef. */
                    int r;
                #if defined(__GNUC__) || defined(__clang__)
                    r = __builtin_ctzll(u);
                #else
                    r = 0; while ((u & 1ULL) == 0) { u >>= 1; ++r; }
                #endif
                    rewrite_as_const_with_value(fn, ins, static_cast<uint64_t>(r));
                    const_vids[ins.dst] = r;
                    changed = true;
                    break;
                }
                case IrOp::BYTESWAP: {
                    if (ins.operands.empty()) break;
                    int64_t c0 = 0;
                    if (!get_const(ins.operands[0], c0)) break;
                    uint64_t u = static_cast<uint64_t>(c0);
                #if defined(__GNUC__) || defined(__clang__)
                    u = __builtin_bswap64(u);
                #else
                    u = ((u & 0xFF00000000000000ULL) >> 56)
                      | ((u & 0x00FF000000000000ULL) >> 40)
                      | ((u & 0x0000FF0000000000ULL) >> 24)
                      | ((u & 0x000000FF00000000ULL) >> 8)
                      | ((u & 0x00000000FF000000ULL) << 8)
                      | ((u & 0x0000000000FF0000ULL) << 24)
                      | ((u & 0x000000000000FF00ULL) << 40)
                      | ((u & 0x00000000000000FFULL) << 56);
                #endif
                    rewrite_as_const_with_value(fn, ins, u);
                    const_vids[ins.dst] = static_cast<int64_t>(u);
                    changed = true;
                    break;
                }
                case IrOp::ROTL:
                case IrOp::ROTR: {
                    if (ins.operands.size() < 2) break;
                    int64_t c0 = 0, c1 = 0;
                    if (!get_const(ins.operands[0], c0)) break;
                    if (!get_const(ins.operands[1], c1)) break;
                    uint64_t u = static_cast<uint64_t>(c0);
                    unsigned n = static_cast<unsigned>(c1) & 63u;
                    uint64_t r = (ins.op == IrOp::ROTL)
                        ? ((u << n) | (n ? (u >> (64 - n)) : 0))
                        : ((u >> n) | (n ? (u << (64 - n)) : 0));
                    rewrite_as_const_with_value(fn, ins, r);
                    const_vids[ins.dst] = static_cast<int64_t>(r);
                    changed = true;
                    break;
                }
                case IrOp::ITOF: {
                    if (ins.operands.empty()) break;
                    int64_t c0 = 0;
                    if (!get_const(ins.operands[0], c0)) break;
                    const bool is_f32 = (ins.type == IrType::F32);
                    double d = static_cast<double>(c0);
                    uint64_t b = is_f32
                        ? static_cast<uint64_t>(f32_to_bits(static_cast<float>(d)))
                        : f64_to_bits(d);
                    rewrite_as_const_with_value(fn, ins, b);
                    const_vids[ins.dst] = static_cast<int64_t>(b);
                    changed = true;
                    break;
                }
                case IrOp::UITOF: {
                    if (ins.operands.empty()) break;
                    int64_t c0 = 0;
                    if (!get_const(ins.operands[0], c0)) break;
                    const bool is_f32 = (ins.type == IrType::F32);
                    double d = static_cast<double>(static_cast<uint64_t>(c0));
                    uint64_t b = is_f32
                        ? static_cast<uint64_t>(f32_to_bits(static_cast<float>(d)))
                        : f64_to_bits(d);
                    rewrite_as_const_with_value(fn, ins, b);
                    const_vids[ins.dst] = static_cast<int64_t>(b);
                    changed = true;
                    break;
                }
                case IrOp::FTOI:
                case IrOp::FTOUI: {
                    if (ins.operands.empty()) break;
                    int64_t c0 = 0;
                    if (!get_const(ins.operands[0], c0)) break;
                    /* El tipo de la SOURCE (no del dst) decide ancho.  Para
                     * conservar correctness, leemos el tipo del operando. */
                    IrType src_t = (ins.operands[0] < fn.values.size())
                                   ? fn.values[ins.operands[0]].type
                                   : IrType::F64;
                    double v = (src_t == IrType::F32)
                        ? static_cast<double>(bits_to_f32(static_cast<uint32_t>(c0)))
                        : bits_to_f64(static_cast<uint64_t>(c0));
                    /* Out-of-range -> UB en C; saltar fold para NaN/Inf y
                     * valores que no entran en int64.  Conservador. */
                    if (!std::isfinite(v)) break;
                    if (ins.op == IrOp::FTOI) {
                        if (v < -9.2233720368547758e18 || v > 9.2233720368547758e18) break;
                        int64_t r = static_cast<int64_t>(v);
                        rewrite_as_const_with_value(fn, ins, static_cast<uint64_t>(r));
                        const_vids[ins.dst] = r;
                    } else {
                        if (v < 0.0 || v > 1.8446744073709552e19) break;
                        uint64_t r = static_cast<uint64_t>(v);
                        rewrite_as_const_with_value(fn, ins, r);
                        const_vids[ins.dst] = static_cast<int64_t>(r);
                    }
                    changed = true;
                    break;
                }
                case IrOp::F32TOF64: {
                    if (ins.operands.empty()) break;
                    int64_t c0 = 0;
                    if (!get_const(ins.operands[0], c0)) break;
                    double d = static_cast<double>(bits_to_f32(static_cast<uint32_t>(c0)));
                    uint64_t b = f64_to_bits(d);
                    rewrite_as_const_with_value(fn, ins, b);
                    const_vids[ins.dst] = static_cast<int64_t>(b);
                    changed = true;
                    break;
                }
                case IrOp::F64TOF32: {
                    if (ins.operands.empty()) break;
                    int64_t c0 = 0;
                    if (!get_const(ins.operands[0], c0)) break;
                    float f = static_cast<float>(bits_to_f64(static_cast<uint64_t>(c0)));
                    uint64_t b = static_cast<uint64_t>(f32_to_bits(f));
                    rewrite_as_const_with_value(fn, ins, b);
                    const_vids[ins.dst] = static_cast<int64_t>(b);
                    changed = true;
                    break;
                }

                /* ---- (C) Phi simplification ---- */
                case IrOp::PHI: {
                    if (ins.phi_args.empty()) break;
                    /* Recolectar VIDs unicos (excluyendo self-ref). */
                    IrValueId unique = IR_NO_VALUE;
                    bool multi = false;
                    for (const auto &arg : ins.phi_args) {
                        if (arg.value == IR_NO_VALUE) continue;
                        if (arg.value == ins.dst) continue;  /* self-ref */
                        if (unique == IR_NO_VALUE) {
                            unique = arg.value;
                        } else if (unique != arg.value) {
                            multi = true;
                            break;
                        }
                    }
                    if (!multi && unique != IR_NO_VALUE) {
                        /* Todos los args no-self apuntan al mismo VID. */
                        rewrite_as_mov(ins, unique);
                        changed = true;
                    }
                    break;
                }

                default:
                    break;
            }
        }
    }

    return changed;
}

// =========================================================================
//  Pase ir_pass_strength_reduction (Phase D.7.opt)
// =========================================================================
//
// Reemplaza operaciones MUL/DIV/MOD por constante potencia-de-2 con
// shifts/AND, que son significativamente mas baratas:
//   x * 2^k        -> x << k
//   x / 2^k (uN)   -> x >> k       (unsigned: shift logico)
//   x / 2^k (iN)   -> x >> k       (sar para preservar signo)
//   x % 2^k (uN)   -> x & (2^k-1)  (unsigned solo, signed tiene round-bias)
//
// Beneficios:
//   - JIT: skip IMUL (3-5 ciclos) / IDIV (~20-30 ciclos) -> SHL/SHR (1 ciclo).
//   - port-C: el compilador C tambien hace esto pero IR limpio ayuda.
//
// La identificacion de "power of 2" usa popcount (== 1).

namespace {

/** @brief Si @p v es potencia de 2 positiva, devuelve log2(v).  Sino -1. */
int log2_if_power_of_two(uint64_t v) {
    if (v == 0) return -1;
    if ((v & (v - 1)) != 0) return -1;  /* mas de un bit set */
    /* Encontrar el bit set.  Usar __builtin_ctzll si disponible. */
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_ctzll(v);
#else
    int k = 0;
    while ((v & 1) == 0) { v >>= 1; ++k; }
    return k;
#endif
}

} // namespace

bool ir_pass_strength_reduction(IrFunction &fn) {
    bool changed = false;

    /* Pre-build vid -> CONST imm. */
    std::unordered_map<IrValueId, int64_t> const_vids;
    for (const auto &bb : fn.blocks) {
        for (const auto &ins : bb.instrs) {
            if (ins.op == IrOp::CONST && ins.dst != IR_NO_VALUE) {
                const_vids[ins.dst] = static_cast<int64_t>(ins.imm);
            }
        }
    }
    auto get_const_pos = [&](IrValueId v, uint64_t &out) -> bool {
        if (v == IR_NO_VALUE) return false;
        auto it = const_vids.find(v);
        if (it == const_vids.end()) return false;
        if (it->second <= 0) return false;  /* solo positivos para SR */
        out = static_cast<uint64_t>(it->second);
        return true;
    };

    /* Crea un nuevo SSA value de tipo CONST y devuelve (new_vid,
     * const_instr).  El caller debe INSERTAR la instr en algun bloque
     * para que el IR sea bien-formado.  Tipicamente justo antes del
     * uso (en el mismo bloque). */
    auto make_new_const = [&](IrType type, uint64_t imm)
                          -> std::pair<IrValueId, IrInstr> {
        const IrValueId new_id = static_cast<IrValueId>(fn.values.size());
        IrValue v{};
        v.id   = new_id;
        v.type = type;
        v.name = "%sr" + std::to_string(new_id);
        v.is_const = true;
        v.const_val = imm;
        fn.values.push_back(v);
        const_vids[new_id] = static_cast<int64_t>(imm);

        IrInstr ci{};
        ci.op   = IrOp::CONST;
        ci.type = type;
        ci.dst  = new_id;
        ci.imm  = imm;
        return {new_id, ci};
    };

    /* Recolectar (bb, pos) -> [list of CONST instrs a insertar ANTES].
     * Lo hacemos en una segunda pasada para no invalidar indices durante
     * el iteracion principal. */
    struct Insertion { size_t bb_idx; size_t pos; IrInstr instr; };
    std::vector<Insertion> pending_insertions;

    for (size_t bi = 0; bi < fn.blocks.size(); ++bi) {
        auto &bb = fn.blocks[bi];
        for (size_t i = 0; i < bb.instrs.size(); ++i) {
            IrInstr &ins = bb.instrs[i];
            if (ins.operands.size() < 2) continue;

            switch (ins.op) {
                case IrOp::MUL: {
                    uint64_t cv = 0;
                    IrValueId rhs_const = IR_NO_VALUE;
                    IrValueId other = IR_NO_VALUE;
                    if (get_const_pos(ins.operands[1], cv)) {
                        rhs_const = ins.operands[1];
                        other     = ins.operands[0];
                    } else if (get_const_pos(ins.operands[0], cv)) {
                        rhs_const = ins.operands[0];
                        other     = ins.operands[1];
                    }
                    if (rhs_const == IR_NO_VALUE) break;
                    int k = log2_if_power_of_two(cv);
                    if (k <= 0) break;
                    auto p = make_new_const(IrType::I64,
                        static_cast<uint64_t>(k));
                    pending_insertions.push_back({bi, i, p.second});
                    ins.op = IrOp::SHL;
                    ins.operands = {other, p.first};
                    changed = true;
                    break;
                }
                case IrOp::DIV: {
                    if (ins.type != IrType::U8 && ins.type != IrType::U16
                     && ins.type != IrType::U32 && ins.type != IrType::U64) {
                        break;
                    }
                    uint64_t cv = 0;
                    if (!get_const_pos(ins.operands[1], cv)) break;
                    int k = log2_if_power_of_two(cv);
                    if (k <= 0) break;
                    auto p = make_new_const(IrType::I64,
                        static_cast<uint64_t>(k));
                    pending_insertions.push_back({bi, i, p.second});
                    ins.op = IrOp::SHR;
                    ins.operands = {ins.operands[0], p.first};
                    changed = true;
                    break;
                }
                case IrOp::MOD: {
                    if (ins.type != IrType::U8 && ins.type != IrType::U16
                     && ins.type != IrType::U32 && ins.type != IrType::U64) {
                        break;
                    }
                    uint64_t cv = 0;
                    if (!get_const_pos(ins.operands[1], cv)) break;
                    int k = log2_if_power_of_two(cv);
                    if (k <= 0) break;
                    const uint64_t mask = cv - 1;
                    auto p = make_new_const(ins.type, mask);
                    pending_insertions.push_back({bi, i, p.second});
                    ins.op = IrOp::AND;
                    ins.operands = {ins.operands[0], p.first};
                    changed = true;
                    break;
                }
                default:
                    break;
            }
        }
    }

    /* Aplicar inserciones en orden inverso para no invalidar pos. */
    std::sort(pending_insertions.begin(), pending_insertions.end(),
        [](const Insertion &a, const Insertion &b) {
            if (a.bb_idx != b.bb_idx) return a.bb_idx > b.bb_idx;
            return a.pos > b.pos;
        });
    for (const auto &ins_req : pending_insertions) {
        auto &bb = fn.blocks[ins_req.bb_idx];
        bb.instrs.insert(bb.instrs.begin() + ins_req.pos, ins_req.instr);
    }

    return changed;
}

// =========================================================================
//  Pase ir_pass_reassoc
// =========================================================================
//
// Reasocia operaciones binarias asociativas para combinar constantes:
//   (x + c1) + c2  ->  x + (c1+c2)        donde c1+c2 se folda
//   (x * c1) * c2  ->  x * (c1*c2)
//   Idem para AND/OR/XOR.
//
// Permite que const-fold colapse cadenas de operaciones que el const-fold
// local no veria (por no estar en la misma instruccion).
//
// Implementacion: para cada binop con rhs CONST, mira si el lhs es la
// MISMA op con un CONST rhs.  Si si, fusiona y deja la nueva instr.

bool ir_pass_reassoc(IrFunction &fn) {
    bool changed = false;

    /* Build vid -> instr (defining instr) para reassoc lookups. */
    struct DefInfo { IrBlockId bb; size_t idx; };
    std::unordered_map<IrValueId, DefInfo> defs;
    for (size_t bi = 0; bi < fn.blocks.size(); ++bi) {
        const auto &bb = fn.blocks[bi];
        for (size_t i = 0; i < bb.instrs.size(); ++i) {
            const auto &ins = bb.instrs[i];
            if (ins.dst != IR_NO_VALUE) {
                defs[ins.dst] = {static_cast<IrBlockId>(bi), i};
            }
        }
    }
    std::unordered_map<IrValueId, int64_t> const_vids;
    for (const auto &bb : fn.blocks) {
        for (const auto &ins : bb.instrs) {
            if (ins.op == IrOp::CONST && ins.dst != IR_NO_VALUE) {
                const_vids[ins.dst] = static_cast<int64_t>(ins.imm);
            }
        }
    }

    auto is_const = [&](IrValueId v, int64_t &out) -> bool {
        auto it = const_vids.find(v);
        if (it == const_vids.end()) return false;
        out = it->second;
        return true;
    };

    auto make_new_const = [&](IrType type, uint64_t imm)
                          -> std::pair<IrValueId, IrInstr> {
        const IrValueId new_id = static_cast<IrValueId>(fn.values.size());
        IrValue v{};
        v.id = new_id; v.type = type;
        v.name = "%ra" + std::to_string(new_id);
        v.is_const = true; v.const_val = imm;
        fn.values.push_back(v);
        const_vids[new_id] = static_cast<int64_t>(imm);

        IrInstr ci{};
        ci.op   = IrOp::CONST;
        ci.type = type;
        ci.dst  = new_id;
        ci.imm  = imm;
        return {new_id, ci};
    };

    auto is_assoc = [](IrOp op) {
        return op == IrOp::ADD || op == IrOp::MUL
            || op == IrOp::AND || op == IrOp::OR || op == IrOp::XOR;
    };

    auto fold_consts = [](IrOp op, int64_t a, int64_t b, int64_t &out) -> bool {
        switch (op) {
            case IrOp::ADD: out = a + b; return true;
            case IrOp::MUL: out = a * b; return true;
            case IrOp::AND: out = a & b; return true;
            case IrOp::OR:  out = a | b; return true;
            case IrOp::XOR: out = a ^ b; return true;
            default: return false;
        }
    };

    struct ReassocInsert { size_t bb_idx; size_t pos; IrInstr instr; };
    std::vector<ReassocInsert> pending;

    for (size_t bi = 0; bi < fn.blocks.size(); ++bi) {
        auto &bb = fn.blocks[bi];
        for (size_t i = 0; i < bb.instrs.size(); ++i) {
            IrInstr &ins = bb.instrs[i];
            if (!is_assoc(ins.op) || ins.operands.size() < 2) continue;
            int64_t c2 = 0;
            IrValueId lhs = ins.operands[0];
            IrValueId rhs = ins.operands[1];
            /* Normalizar: queremos `(x op c1) op c2` con c2 en RHS. */
            if (is_const(lhs, c2) && !is_const(rhs, c2)) {
                std::swap(lhs, rhs);
            }
            if (!is_const(rhs, c2)) continue;
            auto dit = defs.find(lhs);
            if (dit == defs.end()) continue;
            if (dit->second.bb >= fn.blocks.size()) continue;
            const auto &inner_bb = fn.blocks[dit->second.bb];
            if (dit->second.idx >= inner_bb.instrs.size()) continue;
            const IrInstr &inner = inner_bb.instrs[dit->second.idx];
            if (inner.op != ins.op || inner.operands.size() < 2) continue;
            int64_t c1 = 0;
            IrValueId x = inner.operands[0];
            IrValueId rhs_inner = inner.operands[1];
            if (is_const(x, c1) && !is_const(rhs_inner, c1)) {
                std::swap(x, rhs_inner);
            }
            if (!is_const(rhs_inner, c1)) continue;
            int64_t combined = 0;
            if (!fold_consts(ins.op, c1, c2, combined)) continue;
            auto p = make_new_const(ins.type, static_cast<uint64_t>(combined));
            pending.push_back({bi, i, p.second});
            ins.operands = {x, p.first};
            changed = true;
        }
    }
    std::sort(pending.begin(), pending.end(),
        [](const ReassocInsert &a, const ReassocInsert &b) {
            if (a.bb_idx != b.bb_idx) return a.bb_idx > b.bb_idx;
            return a.pos > b.pos;
        });
    for (const auto &ins_req : pending) {
        auto &bb = fn.blocks[ins_req.bb_idx];
        bb.instrs.insert(bb.instrs.begin() + ins_req.pos, ins_req.instr);
    }
    return changed;
}

bool ir_pass_dce(IrFunction &fn) {
    // Construir conjunto de valores que son usados en algun operando
    std::unordered_set<IrValueId> used;
    for (const auto &bb : fn.blocks) {
        for (const auto &ins : bb.instrs) {
            for (IrValueId op : ins.operands) {
                if (op != IR_NO_VALUE) used.insert(op);
            }
            // CALLIND y CALLCLOSURE referencian el callee via func_ptr (no
            // via operands), asi que DCE debe contarlo como uso para que el
            // SSA value que produjo el puntero (e.g. RAW_ASM `mov rN, @Abs(...)`
            // o LOAD del slot del function value) NO sea eliminado.  Sin
            // esto, las closures se rompen: el optimizer purga la instr que
            // materializa fn_addr y el regalloc deja r14 (asignado a
            // fn_addr_v) sin inicializar -> callvmr salta a 0 y crash.
            if ((ins.op == IrOp::CALLIND || ins.op == IrOp::CALLCLOSURE)
             && ins.func_ptr != IR_NO_VALUE) {
                used.insert(ins.func_ptr);
            }
            for (const auto &pa : ins.phi_args) {
                if (pa.value != IR_NO_VALUE) used.insert(pa.value);
            }
        }
    }

    bool changed = false;
    for (auto &bb : fn.blocks) {
        auto &instrs = bb.instrs;
        size_t write = 0;
        for (size_t i = 0; i < instrs.size(); ++i) {
            const IrInstr &ins = instrs[i];
            bool keep = true;
            // Una instruccion con resultado no usado y sin efectos laterales se elimina,
            // EXCEPTO si lleva el flag @c preserve (barreras del codegen).
            if (ins.dst != IR_NO_VALUE
                && !used.count(ins.dst)
                && !is_side_effecting(ins.op)
                && !ins.preserve) {
                keep  = false;
                changed = true;
            }
            if (keep) {
                if (write != i) instrs[write] = std::move(instrs[i]);
                ++write;
            }
        }
        instrs.resize(write);
    }
    return changed;
}

// =========================================================================
//  Pase de propagacion de copias
// =========================================================================

bool ir_pass_copy_prop(IrFunction &fn) {
    // Construir mapa de sustituciones: %b -> %a para cada "%b = mov %a"
    std::unordered_map<IrValueId, IrValueId> subst;
    for (const auto &bb : fn.blocks) {
        for (const auto &ins : bb.instrs) {
            if (ins.op == IrOp::MOV
                && !ins.preserve
                && ins.dst != IR_NO_VALUE
                && ins.operands.size() == 1
                && ins.operands[0] != IR_NO_VALUE) {
                // seguir la cadena de sustituciones
                IrValueId src = ins.operands[0];
                while (subst.count(src)) src = subst[src];
                subst[ins.dst] = src;
            }
        }
    }
    if (subst.empty()) return false;

    // Aplicar sustituciones en todos los operandos
    bool changed = false;
    for (auto &bb : fn.blocks) {
        for (auto &ins : bb.instrs) {
            // sustituir operandos normales
            for (auto &op : ins.operands) {
                auto it = subst.find(op);
                if (it != subst.end() && it->second != op) {
                    op      = it->second;
                    changed = true;
                }
            }
            // sustituir func_ptr en CALLIND
            if (ins.func_ptr != IR_NO_VALUE) {
                auto it = subst.find(ins.func_ptr);
                if (it != subst.end() && it->second != ins.func_ptr) {
                    ins.func_ptr = it->second;
                    changed      = true;
                }
            }
            // sustituir argumentos phi
            for (auto &pa : ins.phi_args) {
                auto it = subst.find(pa.value);
                if (it != subst.end() && it->second != pa.value) {
                    pa.value = it->second;
                    changed  = true;
                }
            }
        }
    }
    // Eliminar los MOV que ahora son copias triviales (%a = mov %a)
    if (changed) ir_pass_dce(fn);
    return changed;
}

// =========================================================================
//  Pase de plegado de constantes
// =========================================================================

/** @brief Devuelve el valor constante de un IrValue, o UNDEF si no es const. */
static bool get_const(const IrFunction &fn, IrValueId id, uint64_t &val) {
    if (id == IR_NO_VALUE || id >= static_cast<IrValueId>(fn.values.size()))
        return false;
    const IrValue &v = fn.values[id];
    if (!v.is_const) return false;
    val = v.const_val;
    return true;
}

bool ir_pass_const_fold(IrFunction &fn) {
    bool changed = false;

    for (auto &bb : fn.blocks) {
        for (auto &ins : bb.instrs) {
            if (ins.dst == IR_NO_VALUE) continue;
            if (!is_pure(ins.op))       continue;

            // --- Operaciones binarias enteras ---
            if (ins.operands.size() == 2) {
                uint64_t a, b;
                bool ca = get_const(fn, ins.operands[0], a);
                bool cb = get_const(fn, ins.operands[1], b);
                if (!ca || !cb) continue;

                uint64_t res = 0;
                bool folded  = true;
                int64_t sa = static_cast<int64_t>(a);
                int64_t sb = static_cast<int64_t>(b);

                switch (ins.op) {
                    case IrOp::ADD:     res = a + b;                        break;
                    case IrOp::SUB:     res = a - b;                        break;
                    case IrOp::MUL:     res = a * b;                        break;
                    /* Sprint edge-bugs (2026-06-02): si el divisor es CONST 0,
                     * NO foldear -- dejar la operacion en runtime para que el
                     * interp/JIT lance FatalError capturable.  Antes esto se
                     * foldeaba a 0 silencioso ocultando el bug del programa. */
                    case IrOp::DIV:     if (sb == 0) { folded = false; break; }
                                        res = (uint64_t)(sa / sb); break;
                    case IrOp::MOD:     if (sb == 0) { folded = false; break; }
                                        res = (uint64_t)(sa % sb); break;
                    case IrOp::AND:     res = a & b;                        break;
                    case IrOp::OR:      res = a | b;                        break;
                    case IrOp::XOR:     res = a ^ b;                        break;
                    case IrOp::SHL:     res = a << (b & 63);                break;
                    case IrOp::SHR:     res = a >> (b & 63);                break;
                    case IrOp::SAR:     res = (uint64_t)(sa >> (b & 63));   break;
                    // comparaciones enteras -> resultado bool (0 o 1)
                    case IrOp::CMP_EQ:  res = (a == b)           ? 1 : 0;  break;
                    case IrOp::CMP_NE:  res = (a != b)           ? 1 : 0;  break;
                    case IrOp::CMP_LT:  res = (sa <  sb)         ? 1 : 0;  break;
                    case IrOp::CMP_GT:  res = (sa >  sb)         ? 1 : 0;  break;
                    case IrOp::CMP_LE:  res = (sa <= sb)         ? 1 : 0;  break;
                    case IrOp::CMP_GE:  res = (sa >= sb)         ? 1 : 0;  break;
                    case IrOp::CMP_ULT: res = (a <  b)           ? 1 : 0;  break;
                    case IrOp::CMP_UGT: res = (a >  b)           ? 1 : 0;  break;
                    case IrOp::CMP_ULE: res = (a <= b)           ? 1 : 0;  break;
                    case IrOp::CMP_UGE: res = (a >= b)           ? 1 : 0;  break;
                    default: folded = false; break;
                }

                if (folded) {
                    ins.op         = IrOp::CONST;
                    ins.imm        = res;
                    ins.operands.clear();
                    ins.type       = (ins.op == IrOp::CMP_EQ) ? IrType::BOOL : ins.type;
                    // marcar el valor destino como constante
                    if (ins.dst < static_cast<IrValueId>(fn.values.size())) {
                        fn.values[ins.dst].is_const  = true;
                        fn.values[ins.dst].const_val = res;
                    }
                    changed = true;
                }
            }

            // --- Operaciones unarias enteras ---
            if (ins.operands.size() == 1) {
                uint64_t a;
                if (!get_const(fn, ins.operands[0], a)) continue;

                uint64_t res  = 0;
                bool folded   = true;
                int64_t sa = static_cast<int64_t>(a);

                switch (ins.op) {
                    case IrOp::NEG:     res = (uint64_t)(-sa);  break;
                    case IrOp::NOT:     res = ~a;                break;
                    case IrOp::ZEXT:    res = a;                 break;
                    case IrOp::TRUNC: {
                        /* Sprint edge-bugs (2026-06-02): TRUNC debe
                         * preservar el signo si el tipo destino es
                         * signed (i8/i16/i32).  Antes hacia
                         * `a & 0xFFFFFFFF` lo que para `-7` (signed i32)
                         * truncado dejaba `0x00000000FFFFFFF9` -- valor
                         * UNSIGNED 4294967289, no -7.  Operaciones
                         * downstream (e.g. const-fold de `mod.i32`) lo
                         * interpretaban como positivo -> resultado
                         * equivocado del modulo signed.
                         *
                         * Fix: para tipos signed, sign-extender el bit
                         * mas alto del ancho destino al resto del u64.
                         * Para unsigned, mask simple. */
                        uint64_t mask;
                        bool sign_extend = false;
                        switch (ins.type) {
                            case IrType::I8:
                                mask = 0xFFULL; sign_extend = true; break;
                            case IrType::I16:
                                mask = 0xFFFFULL; sign_extend = true; break;
                            case IrType::I32:
                                mask = 0xFFFFFFFFULL; sign_extend = true; break;
                            case IrType::U8:  mask = 0xFFULL; break;
                            case IrType::U16: mask = 0xFFFFULL; break;
                            case IrType::U32: mask = 0xFFFFFFFFULL; break;
                            case IrType::BOOL: mask = 0x1ULL; break;
                            default:
                                /* Sin truncacion real (mismo ancho). */
                                mask = 0xFFFFFFFFFFFFFFFFULL;
                                break;
                        }
                        res = a & mask;
                        if (sign_extend) {
                            const uint64_t sign_bit = (mask >> 1) + 1;
                            if (res & sign_bit) {
                                /* Bit alto del ancho destino set -> negativo.
                                 * Or-ear los bits altos para sign-extend a u64. */
                                res |= ~mask;
                            }
                        }
                        break;
                    }
                    case IrOp::MOV:     res = a;                 break;
                    default: folded = false; break;
                }

                if (folded) {
                    ins.op  = IrOp::CONST;
                    ins.imm = res;
                    ins.operands.clear();
                    if (ins.dst < static_cast<IrValueId>(fn.values.size())) {
                        fn.values[ins.dst].is_const  = true;
                        fn.values[ins.dst].const_val = res;
                    }
                    changed = true;
                }
            }
        }
    }
    return changed;
}

// =========================================================================
//  Pase de eliminacion de bloques inalcanzables
// =========================================================================

// =========================================================================
//  Pase Dead Store Elimination
// =========================================================================
//
// Para cada bloque basico, detecta STOREs sucesivos a la MISMA direccion
// sin lecturas intermedias.  El primer STORE es muerto: su valor sera
// sobrescrito sin ser leido.
//
// Es conservador con side-effects: cualquier CALL/RAW_ASM/CALLN limpia el
// estado (no podemos garantizar que el callee no lea la memoria).
//
// Patron comun en frontend Vex:
//   %a = const.i64 0
//   store %a, %slot
//   ...   (sin LOAD de %slot, sin CALL)
//   %b = const.i64 42
//   store %b, %slot   <-- valor que persiste; el store anterior es dead
//
// Bajo esto a IR:
//   - El primer STORE se marca para eliminar
//   - La CONST que solo alimentaba al store dead queda dead -> DCE la quita
//
// Ahorro: en codigo generado por frontend Vex se ven STOREs de zero seguidos
// de STOREs reales (init list, alloca cleared, etc).  ~10-15% reduccion.
bool ir_pass_dse(IrFunction &fn) {
    bool changed = false;

    for (auto &bb : fn.blocks) {
        // Mapa: ptr_vid -> indice del ultimo STORE a ese ptr (en este bloque)
        std::unordered_map<IrValueId, size_t> last_store_idx;
        // Phase D.7.opt: STORE-TO-LOAD FORWARDING.
        // Mapa paralelo: ptr_vid -> (stored_value_vid, store_type) del
        // ultimo STORE.  Cuando un LOAD lee de ese mismo ptr CON EL MISMO
        // tipo, podemos reemplazar el LOAD por MOV del valor almacenado
        // (ahorra la lectura de memoria + cualquier conversion).
        std::unordered_map<IrValueId, std::pair<IrValueId, IrType>> last_store_val;
        // Set de indices marcados como dead
        std::vector<bool> dead(bb.instrs.size(), false);

        for (size_t i = 0; i < bb.instrs.size(); ++i) {
            auto &ins = bb.instrs[i];
            switch (ins.op) {
                case IrOp::STORE: {
                    if (ins.operands.size() < 2) break;
                    IrValueId ptr = ins.operands[1];
                    IrValueId val = ins.operands[0];
                    if (ptr == IR_NO_VALUE) break;
                    auto it = last_store_idx.find(ptr);
                    if (it != last_store_idx.end()) {
                        // STORE anterior al mismo ptr SIN reads intermedios.
                        // El anterior es DEAD.
                        dead[it->second] = true;
                        changed = true;
                    }
                    last_store_idx[ptr] = i;
                    if (val != IR_NO_VALUE) {
                        last_store_val[ptr] = {val, ins.type};
                    }
                    break;
                }
                case IrOp::LOAD: {
                    if (ins.operands.empty()) break;
                    IrValueId ptr = ins.operands[0];
                    if (ptr == IR_NO_VALUE) break;
                    auto it = last_store_val.find(ptr);
                    if (it != last_store_val.end() && it->second.second == ins.type) {
                        // SLF: reemplazar LOAD por MOV del valor almacenado.
                        // copy_prop posterior substituye los usos del LOAD dst
                        // por el stored value y DCE elimina el MOV.
                        //
                        // Bug fix is_host_ptr propagation: el LOAD dst pudo
                        // estar marcado is_host_ptr=true por el frontend (e.g.
                        // lend() lowering marca el load del slot[0] de un
                        // unique<T> como host_ptr).  Al substituir uses con
                        // el stored value, ESE value debe tambien tener
                        // is_host_ptr=true para que LOAD/STORE posteriores
                        // emitan movh (host mem) en vez de mov (VM mem).
                        // Semanticamente: load(store(v, p)) == v, asi que
                        // el LOAD result Y el stored value son el MISMO
                        // value runtime y deben compartir flags.
                        if (ins.dst < fn.values.size()
                         && it->second.first < fn.values.size()) {
                            const auto &dst_v = fn.values[ins.dst];
                            auto &val_v = fn.values[it->second.first];
                            if (dst_v.is_host_ptr && !val_v.is_host_ptr) {
                                val_v.is_host_ptr = true;
                            }
                            if (dst_v.is_gc_object && !val_v.is_gc_object) {
                                val_v.is_gc_object = true;
                            }
                            if (dst_v.pointee_is_host_ptr && !val_v.pointee_is_host_ptr) {
                                val_v.pointee_is_host_ptr = true;
                            }
                        }
                        ins.op = IrOp::MOV;
                        ins.operands = {it->second.first};
                        changed = true;
                    }
                    // No invalidar last_store_idx: el STORE no es dead
                    // (acabamos de demostrar que ESTE LOAD lo lee).
                    last_store_idx.erase(ptr);
                    break;
                }
                case IrOp::ARRAY_LOAD:
                case IrOp::GETFIELD:
                case IrOp::ARRAY_LEN:
                    // Cualquier LOAD desde un ptr que tenemos seguido invalida
                    // la posibilidad de eliminar el STORE previo (no sabemos
                    // alias).  Conservador: limpiar todo el mapa.
                    last_store_idx.clear();
                    last_store_val.clear();
                    break;
                // Side-effects/calls: limpiar (memoria puede cambiar dentro).
                case IrOp::CALL: case IrOp::CALLN: case IrOp::CALLVIRT:
                case IrOp::CALLIND: case IrOp::CALLM: case IrOp::CALLCLOSURE:
                case IrOp::TAILCALL:
                case IrOp::RAW_ASM:
                case IrOp::MEMCPY: case IrOp::SETFIELD: case IrOp::ARRAY_STORE:
                case IrOp::STRFINALIZE: case IrOp::GCWB_IR:
                case IrOp::NEWOBJ: case IrOp::GC_ALLOC:
                case IrOp::RAW_ALLOC: case IrOp::RAW_FREE:
                case IrOp::THROW: case IrOp::TRYENTER: case IrOp::TRYLEAVE:
                // Sprint string-perf-2 bug fix (2026-06-02): STRMAKE/STRCAT/
                // STRCONV LEEN memoria en runtime (STRMAKE lee vm_mem para
                // construir el StringObject; STRCAT puede materializar bytes;
                // STRCONV lee el src y escribe el converted).  Sin invalidar
                // last_store_val, DSE elimina STOREs previos pensando que
                // nadie los lee, pero STRMAKE LOS LEE en runtime.
                // Bug reproducido en patron `buf[i]=X; STRMAKE(buf); buf[i]=Y;
                // STRMAKE(buf)`: el primer set de stores se eliminaba.
                case IrOp::STRMAKE: case IrOp::STRCAT: case IrOp::STRCONV:
                case IrOp::STRFLAT: case IrOp::STRINTERN: case IrOp::STRRESERVE:
                // Sprint edge-bugs (2026-06-02): MVTAKE_IR muta memoria
                // (copia src->dst + zerifica src), por lo que invalida
                // last_store_val para ambos punteros.  Conservadoramente
                // clearamos todo el mapa.  Sin esto, ptr_of() despues de
                // move() retornaba el host_ptr ORIGINAL (SLF leia el store
                // anterior al mvtake) en vez del 0 que el runtime escribio.
                case IrOp::MVTAKE_IR:
                // GC_PROMOTE/GC_DEMOTE copian memoria cross-heap.
                case IrOp::GC_PROMOTE: case IrOp::GC_DEMOTE:
                // ATOMIC_ST_I64 / ATOMIC_CAS_I64 / ATOMIC_ADD_I64 escriben
                // memoria; ATOMIC_LD_I64 lee (que es OK para SLF si no hay
                // store intermedio, pero conservativo clearamos).
                case IrOp::ATOMIC_LD_I64: case IrOp::ATOMIC_ST_I64:
                case IrOp::ATOMIC_CAS_I64: case IrOp::ATOMIC_ADD_I64:
                // SMARTPTR_FREE invoca deleter que puede tocar memoria.
                case IrOp::SMARTPTR_FREE:
                // FFI runtime puede mutar cualquier cosa.
                case IrOp::DLOPEN: case IrOp::DLSYM:
                case IrOp::MOD_LOAD:
                    last_store_idx.clear();
                    last_store_val.clear();
                    break;
                default:
                    break;
            }
        }

        // Compactar: eliminar instrucciones marcadas dead.
        if (changed) {
            std::vector<IrInstr> kept;
            kept.reserve(bb.instrs.size());
            for (size_t i = 0; i < bb.instrs.size(); ++i) {
                if (!dead[i]) kept.push_back(std::move(bb.instrs[i]));
            }
            bb.instrs = std::move(kept);
        }
    }

    // DCE para limpiar valores que solo alimentaban los stores eliminados,
    // y copy_prop para resolver los MOVs generados por SLF.
    if (changed) {
        ir_pass_copy_prop(fn);
        ir_pass_dce(fn);
    }
    return changed;
}

bool ir_pass_unreachable(IrFunction &fn) {
    if (fn.blocks.empty()) return false;

    const size_t nblocks = fn.blocks.size();
    std::vector<bool> reachable(nblocks, false);

    // BFS desde el bloque de entrada (bloque 0)
    std::queue<IrBlockId> worklist;
    worklist.push(0);
    reachable[0] = true;

    while (!worklist.empty()) {
        IrBlockId bid = worklist.front();
        worklist.pop();

        if (bid >= nblocks) continue;
        const IrBlock &bb = fn.blocks[bid];

        // Encontrar el terminador del bloque y sus sucesores
        for (const auto &ins : bb.instrs) {
            if (ins.op == IrOp::BR) {
                IrBlockId s = ins.target_block;
                if (s < nblocks && !reachable[s]) {
                    reachable[s] = true;
                    worklist.push(s);
                }
            } else if (ins.op == IrOp::BR_COND) {
                for (IrBlockId s : {ins.target_block, ins.false_block}) {
                    if (s < nblocks && !reachable[s]) {
                        reachable[s] = true;
                        worklist.push(s);
                    }
                }
            }
        }
        // Usar sucesores precalculados si existen
        for (IrBlockId s : bb.succs) {
            if (s < nblocks && !reachable[s]) {
                reachable[s] = true;
                worklist.push(s);
            }
        }
    }

    // Verificar si hay bloques inalcanzables
    bool any_unreachable = false;
    for (size_t b = 0; b < nblocks; ++b) {
        if (!reachable[b]) { any_unreachable = true; break; }
    }
    if (!any_unreachable) return false;

    // Construir mapa de reindexacion: viejo_id -> nuevo_id
    std::vector<IrBlockId> remap(nblocks, IR_NO_BLOCK);
    IrBlockId new_id = 0;
    for (size_t b = 0; b < nblocks; ++b) {
        if (reachable[b]) remap[b] = new_id++;
    }

    // Reescribir los bloques: eliminar los inalcanzables y actualizar referencias
    std::vector<IrBlock> new_blocks;
    new_blocks.reserve(new_id);
    for (size_t b = 0; b < nblocks; ++b) {
        if (!reachable[b]) continue;
        IrBlock bb = fn.blocks[b];
        bb.id = remap[b];

        // Actualizar predecesores y sucesores
        std::vector<IrBlockId> new_preds, new_succs;
        for (IrBlockId p : bb.preds) {
            if (p < nblocks && reachable[p]) new_preds.push_back(remap[p]);
        }
        for (IrBlockId s : bb.succs) {
            if (s < nblocks && reachable[s]) new_succs.push_back(remap[s]);
        }
        bb.preds = std::move(new_preds);
        bb.succs = std::move(new_succs);

        // Actualizar referencias de bloques en instrucciones
        for (auto &ins : bb.instrs) {
            if (ins.target_block != IR_NO_BLOCK && ins.target_block < nblocks)
                ins.target_block = remap[ins.target_block];
            if (ins.false_block != IR_NO_BLOCK && ins.false_block < nblocks)
                ins.false_block = remap[ins.false_block];
            // Eliminar argumentos phi que vienen de bloques inalcanzables
            std::vector<IrPhiArg> new_phi;
            for (const auto &pa : ins.phi_args) {
                if (pa.block < nblocks && reachable[pa.block]) {
                    IrPhiArg na = pa;
                    na.block = remap[pa.block];
                    new_phi.push_back(na);
                }
            }
            ins.phi_args = std::move(new_phi);
        }
        new_blocks.push_back(std::move(bb));
    }
    fn.blocks = std::move(new_blocks);
    return true;
}

// =========================================================================
//  Pase CSE (Common Subexpression Elimination, local)
// =========================================================================

/* Global const CSE: deduplicar CONSTs en bloques no-entry contra los
 * de entry (que dominan a todos los demas).  Pase separado, seguro a O2.
 *
 * Implementacion: en vez de rewrite-CONST-to-MOV-then-copy-prop (que
 * dejaba el is_const flag stale en fn.values y causaba interaccion mala
 * con simplify/const_fold), hacemos un DIRECT REWRITE:
 *
 *   1. Recolectar mapa {(type, imm) -> entry_vid} de entry.
 *   2. Recolectar mapa {dup_vid -> canon_vid} para CONSTs duplicados en
 *      bloques no-entry.
 *   3. Aplicar sustitucion en TODOS los operandos / func_ptr / phi_args.
 *   4. Eliminar las instrucciones CONST duplicadas (no las convertimos a
 *      MOV; las quitamos del bloque directamente -- DCE no es necesario).
 *
 * Esto evita el camino MOV+copy_prop que dejaba is_const stale.  La
 * IrValue del dup_vid queda huerfana (sin instr definidora), pero como
 * todos sus usos se substituyen, no se referencia mas. */
bool ir_pass_const_cse_entry(IrFunction &fn) {
    if (fn.blocks.empty()) return false;
    /* Pase 1: en entry, recolectar el PRIMER vid por (type,imm) y registrar
     * duplicados subsiguientes -> subst (apuntan al primer vid). */
    std::unordered_map<std::string, IrValueId> entry_const_table;
    std::unordered_map<IrValueId, IrValueId> subst;
    for (const auto &ins : fn.blocks[0].instrs) {
        if (ins.op != IrOp::CONST || ins.dst == IR_NO_VALUE) continue;
        std::ostringstream key;
        key << static_cast<int>(ins.type) << ":" << ins.imm;
        std::string k = key.str();
        auto it = entry_const_table.find(k);
        if (it == entry_const_table.end()) {
            entry_const_table[k] = ins.dst;
        } else if (it->second != ins.dst) {
            /* Duplicado dentro de entry -- redirige al primero. */
            subst[ins.dst] = it->second;
        }
    }
    if (entry_const_table.empty()) return false;

    /* Pase 2: en bloques no-entry, identificar duplicados y agregar subst. */
    for (size_t bi = 1; bi < fn.blocks.size(); ++bi) {
        for (const auto &ins : fn.blocks[bi].instrs) {
            if (ins.op != IrOp::CONST || ins.dst == IR_NO_VALUE) continue;
            std::ostringstream key;
            key << static_cast<int>(ins.type) << ":" << ins.imm;
            auto it = entry_const_table.find(key.str());
            if (it != entry_const_table.end() && it->second != ins.dst) {
                subst[ins.dst] = it->second;
            }
        }
    }
    if (subst.empty()) return false;

    /* Aplicar sustitucion en operandos / func_ptr / phi_args. */
    auto canon = [&](IrValueId v) -> IrValueId {
        IrValueId cur = v;
        int hops = 0;
        while (subst.count(cur) && hops++ < 8) cur = subst[cur];
        return cur;
    };
    bool changed = false;
    for (auto &bb : fn.blocks) {
        for (auto &ins : bb.instrs) {
            for (auto &op : ins.operands) {
                IrValueId c = canon(op);
                if (c != op) { op = c; changed = true; }
            }
            if (ins.func_ptr != IR_NO_VALUE) {
                IrValueId c = canon(ins.func_ptr);
                if (c != ins.func_ptr) { ins.func_ptr = c; changed = true; }
            }
            for (auto &pa : ins.phi_args) {
                IrValueId c = canon(pa.value);
                if (c != pa.value) { pa.value = c; changed = true; }
            }
        }
    }

    /* Eliminar las instrucciones CONST duplicadas EN TODOS LOS BLOQUES
     * (incluyendo entry: si entry tiene dups internos, los quitamos). */
    for (size_t bi = 0; bi < fn.blocks.size(); ++bi) {
        auto &instrs = fn.blocks[bi].instrs;
        std::vector<IrInstr> kept;
        kept.reserve(instrs.size());
        for (auto &ins : instrs) {
            if (ins.op == IrOp::CONST && ins.dst != IR_NO_VALUE
             && subst.count(ins.dst)) {
                /* skip: duplicado eliminado */
                changed = true;
            } else {
                kept.push_back(std::move(ins));
            }
        }
        instrs = std::move(kept);
    }
    return changed;
}

bool ir_pass_cse(IrFunction &fn) {
    bool changed = false;

    /* ============================================================
     * GLOBAL const CSE.
     *
     * Antes del CSE local per-block, deduplicar CONSTs cross-block
     * recolectando el PRIMER CONST por (type, imm) en orden lineal de
     * bloques y registrando un mapa global de sustitucion.  Como el
     * bloque 0 (entry) domina a todos los demas, si el primer CONST
     * esta en entry, todos los duplicados en bloques posteriores
     * pueden referirse a el sin violar SSA dominance.
     *
     * Para CONSTs no en entry: solo deduplicamos si el primer CONST
     * encontrado en orden lineal de bloques domina a los duplicados.
     * Aproximacion conservadora: solo dedupe si el primer CONST esta
     * en el bloque 0 (entry).  Mas casos requeririan dominator analysis,
     * pero como SR/reassoc/LICM insertan CONSTs en entry por construccion,
     * esta heuristica cubre el 95% de los casos reales.
     * ============================================================ */
    {
        std::unordered_map<std::string, IrValueId> entry_const_table;
        /* Pase 1: recolectar CONSTs en entry (bloque 0). */
        if (!fn.blocks.empty()) {
            for (const auto &ins : fn.blocks[0].instrs) {
                if (ins.op != IrOp::CONST) continue;
                if (ins.dst == IR_NO_VALUE) continue;
                std::ostringstream key;
                key << static_cast<int>(ins.type) << ":" << ins.imm;
                std::string k = key.str();
                if (!entry_const_table.count(k)) {
                    entry_const_table[k] = ins.dst;
                }
            }
        }
        /* Pase 2: en otros bloques, deduplicar CONSTs cuyo (type, imm)
         * matchea uno de entry.  Convertir a MOV. */
        if (!entry_const_table.empty()) {
            for (size_t bi = 1; bi < fn.blocks.size(); ++bi) {
                for (auto &ins : fn.blocks[bi].instrs) {
                    if (ins.op != IrOp::CONST) continue;
                    if (ins.dst == IR_NO_VALUE) continue;
                    std::ostringstream key;
                    key << static_cast<int>(ins.type) << ":" << ins.imm;
                    auto it = entry_const_table.find(key.str());
                    if (it != entry_const_table.end() && it->second != ins.dst) {
                        ins.op = IrOp::MOV;
                        ins.operands = {it->second};
                        ins.imm = 0;
                        changed = true;
                    }
                }
            }
        }
    }
    /* Tras el global const CSE, copy_prop limpiara los MOVs. */

    auto is_mem_read = [](IrOp op) -> bool {
        return op == IrOp::LOAD || op == IrOp::ARRAY_LOAD
            || op == IrOp::GETFIELD || op == IrOp::ARRAY_LEN;
    };

    for (auto &bb : fn.blocks) {
        // Tabla: hash de (op, type, operands) -> IrValueId del primer calculo
        std::unordered_map<std::string, IrValueId> expr_table;
        // Mapa paralelo: clave -> bool (es memory-read?) para invalidar
        // rapido al ver side-effects.
        std::unordered_set<std::string> mem_read_keys;
        // Mapa de sustituciones para aplicar
        std::unordered_map<IrValueId, IrValueId> subst;

        for (auto &ins : bb.instrs) {
            /* Bug fix: instrucciones SIN dst (STORE/BR/RET/CALL-void)
             * pueden tener side-effects que invalidan memory-read entries.
             * Procesar invalidacion ANTES del `continue` por dst==NO_VALUE. */
            if (!is_pure(ins.op)) {
                for (const auto &mk : mem_read_keys) expr_table.erase(mk);
                mem_read_keys.clear();
                continue;
            }
            if (ins.dst == IR_NO_VALUE) continue;
            if (ins.op == IrOp::PHI) continue;  /* phi no se dedupea */

            /* dedupe CONSTs por (type, imm).  Antes el
             * pase ignoraba CONSTs explicitamente; los port targets
             * (e.g. port-C) y el JIT se beneficiaban de tener un solo
             * SSA value por (type, imm).  Reduce el numero de slots
             * stack alocados y el output destino es mas limpio. */
            if (ins.op == IrOp::CONST) {
                std::ostringstream key;
                key << "C:" << static_cast<int>(ins.type) << ":" << ins.imm;
                std::string k = key.str();
                auto it = expr_table.find(k);
                if (it != expr_table.end()) {
                    subst[ins.dst] = it->second;
                    ins.op = IrOp::MOV;
                    ins.operands = {it->second};
                    ins.imm = 0;
                    changed = true;
                } else {
                    expr_table[k] = ins.dst;
                }
                continue;
            }

            // Construir clave canonica: "op:type:imm:func_name:op0:op1:..."
            //
            // Bug fix: imm es semanticamente significativo para STR_LIT_ADDR
            // (indice del string), GETFIELD (offset del campo), ALLOCA (size),
            // y posiblemente otros.  Incluirlo en la clave evita dedupe falso
            // (e.g., dos str_lit_addr con strings distintos parecian iguales).
            //
            // Bug fix fase B: @c func_name es CRITICO para LABEL_ADDR y los
            // CALL-like ops (CALL, CALLN, etc).  Sin esto, dos LABEL_ADDR con
            // labels distintos se deduplican incorrectamente (handler_pc del
            // tryenter se mezcla con el name_addr del findclass, p.ej.).
            std::ostringstream key;
            key << static_cast<int>(ins.op) << ":"
                << static_cast<int>(ins.type) << ":"
                << ins.imm << ":" << ins.func_name;
            for (IrValueId op : ins.operands) {
                // Resolver sustituciones previas en los operandos
                IrValueId canonical = op;
                while (subst.count(canonical)) canonical = subst[canonical];
                key << ":" << canonical;
            }
            std::string k = key.str();

            auto it = expr_table.find(k);
            if (it != expr_table.end()) {
                // Expresion ya calculada: sustituir dst con el valor anterior
                subst[ins.dst] = it->second;
                ins.op  = IrOp::MOV;
                ins.operands = {it->second};
                changed = true;
            } else {
                // Primera ocurrencia: registrarla
                expr_table[k] = ins.dst;
                if (is_mem_read(ins.op)) mem_read_keys.insert(k);
                // Aplicar sustituciones previas a los operandos de esta instruccion
                for (auto &op : ins.operands) {
                    auto sit = subst.find(op);
                    if (sit != subst.end()) op = sit->second;
                }
            }
        }
    }
    if (changed) ir_pass_copy_prop(fn); // limpiar los MOV generados por CSE
    return changed;
}

// =========================================================================
//  Pase TCO (Tail Call Optimization)
// =========================================================================

// =========================================================================
//  Helper: detectar si un IrValueId deriva (transitivamente) de una ALLOCA
//
//  Usado por TCO para descartar la transformacion CALL->TAILCALL cuando
//  algun argumento referencia memoria asignada en el frame del caller.
//  Razon: TAILCALL emite `leave` antes del salto al callee, lo que
//  restaura RSP=RBP y libera el bloque ALLOCA.  Si el callee dereferencia
//  un puntero que apuntaba a esa region, lee basura (o memoria del
//  callee).  Demo regresion: 17_ecs_basico.vex pasaba arrays
//  i32[8] (ALLOCA) por valor a system_sum_positions y obtenia R0=0
//  en vez de 100 con TCO activo.
//
//  La deteccion es conservadora: marcamos un valor como "alloca-derived"
//  si su def es ALLOCA, MEMBER (que devuelve direccion en frame), o
//  cualquier ADD/SUB/MOV/STORE/STR_LIT_ADDR cuyo operando ya este marcado.
//  No intenta tracking flow-sensitive: una sobreestimacion implica
//  perder TCO en ese caller, no incorrectness.
// =========================================================================
static bool collect_alloca_derived(const IrFunction &fn,
                                    std::unordered_set<IrValueId> &out) {
    out.clear();
    // Pase 1: identificar las definiciones ALLOCA directas.
    for (const auto &bb : fn.blocks) {
        for (const auto &ins : bb.instrs) {
            if (ins.op == IrOp::ALLOCA && ins.dst != IR_NO_VALUE) {
                out.insert(ins.dst);
            }
        }
    }
    if (out.empty()) return false;

    // Pase 2: propagar la marca por aritmetica de punteros y MOV/PHI.
    // Iteramos hasta punto fijo (cota: numero de blocks * instrs por block).
    bool changed = true;
    int  guard   = 1024;
    while (changed && guard-- > 0) {
        changed = false;
        for (const auto &bb : fn.blocks) {
            for (const auto &ins : bb.instrs) {
                if (ins.dst == IR_NO_VALUE) continue;
                if (out.count(ins.dst)) continue;
                bool any_op_tainted = false;
                for (auto op : ins.operands) {
                    if (op != IR_NO_VALUE && out.count(op)) {
                        any_op_tainted = true;
                        break;
                    }
                }
                if (!any_op_tainted) {
                    for (const auto &pa : ins.phi_args) {
                        if (out.count(pa.value)) {
                            any_op_tainted = true;
                            break;
                        }
                    }
                }
                if (any_op_tainted) {
                    // Solo propagamos por ops que SI pueden producir una
                    // direccion derivada: aritmetica (ADD/SUB), copias
                    // (MOV, PHI), o casts/cargas/loads que conserven el
                    // puntero.  Para ALU "verdadera" sobre escalares no
                    // hay riesgo, asi que la lista blanca es restrictiva
                    // pero suficiente para el patron observado.
                    switch (ins.op) {
                        case IrOp::ADD: case IrOp::SUB:
                        case IrOp::MOV: case IrOp::PHI:
                            out.insert(ins.dst);
                            changed = true;
                            break;
                        default:
                            break;
                    }
                }
            }
        }
    }
    return true;
}

bool ir_pass_tailcall(IrFunction &fn) {
    // Detecta el patron: CALL @f(args) seguido inmediatamente de RET %resultado
    // y convierte el CALL en TAILCALL (elimina la RET subsiguiente).
    // Tambien maneja RET void inmediatamente despues de CALL void.
    //
    // SAFETY: NO se promueve a TAILCALL si algun argumento es derivado
    // de una ALLOCA del caller.  TAILCALL emite `leave` (que restaura
    // RSP=RBP y libera el frame), invalidando los punteros que apuntan
    // al area de allocas.  El callee leeria basura.  La deteccion se
    // hace una vez por funcion y la cache se reutiliza para todos los
    // CALLs candidatos.
    bool changed = false;

    std::unordered_set<IrValueId> alloca_derived;
    const bool fn_has_alloca = collect_alloca_derived(fn, alloca_derived);

    for (auto &bb : fn.blocks) {
        auto &instrs = bb.instrs;
        for (size_t i = 0; i + 1 < instrs.size(); ) {
            IrInstr &call = instrs[i];
            IrInstr &ret  = instrs[i + 1];

            // Solo CALL directo (no CALLVIRT, CALLN, CALLIND)
            if (call.op != IrOp::CALL) { ++i; continue; }
            if (ret.op  != IrOp::RET)  { ++i; continue; }

            // Verificar que RET usa directamente el resultado del CALL (o es void)
            bool ret_uses_call = (!ret.operands.empty()
                                  && ret.operands[0] == call.dst);
            bool ret_is_void   = ret.operands.empty();

            if (!ret_uses_call && !ret_is_void) { ++i; continue; }

            // Bloqueo de seguridad: si CUALQUIER arg es derivado de
            // ALLOCA del caller, NO promover a TAILCALL.  El leave
            // posterior liberaria la memoria todavia referenciada.
            if (fn_has_alloca) {
                bool unsafe = false;
                for (auto op : call.operands) {
                    if (op != IR_NO_VALUE && alloca_derived.count(op)) {
                        unsafe = true;
                        break;
                    }
                }
                if (unsafe) { ++i; continue; }
            }

            // Convertir: CALL -> TAILCALL, eliminar RET
            call.op  = IrOp::TAILCALL;
            call.dst = IR_NO_VALUE; // TAILCALL no tiene destino
            instrs.erase(instrs.begin() + static_cast<ptrdiff_t>(i + 1));
            changed = true;
            // No avanzar 'i': re-examinar la misma posicion por si hay otro patron
        }
    }
    return changed;
}

// =========================================================================
//  Pase inline_loop_header (peephole pre-codegen)
// =========================================================================

/**
 * @brief Inline de header trivial de loop para habilitar fusion decjnz.
 *
 * Detecta el patron:
 *   B: ...; <SUB>; br H
 *   H: %cmp = CMP_X(...); br.cond %cmp, T, F
 * con H teniendo UN SOLO predecesor (B).  Mueve las 2 instrs de H al
 * final de B (reemplazando el br) y vacia H.  Despues, B queda con
 * SUB+CMP+BR_COND consecutivos -> el peephole de same-block del IR
 * emitter aplica decjnz fusion.
 *
 * Generaliza a cualquier patron "header trivial con un predecesor", no
 * solo a decjnz.  Otros peepholes (cmpjmp) tambien se benefician.
 *
 * Coste: O(N_blocks * N_predecessors).  El check de predecesores es
 * lineal pero solo se ejecuta cuando el header tiene exactamente 2
 * instr (raro fuera de loop headers).
 *
 * @return true si se hizo al menos una fusion (puede dispararse otro DCE).
 */
bool ir_pass_inline_loop_header(IrFunction &fn) {
    bool changed = false;
    for (size_t bi = 0; bi < fn.blocks.size(); ++bi) {
        IrBlock &B = fn.blocks[bi];
        if (B.instrs.empty()) continue;
        IrInstr &term = B.instrs.back();
        if (term.op != IrOp::BR) continue; // solo BR incondicional
        IrBlockId hid = term.target_block;
        if (hid >= fn.blocks.size() || hid == static_cast<IrBlockId>(bi)) continue;
        IrBlock &H = fn.blocks[hid];
        // H debe terminar en BR_COND.  Las instrs pueden incluir CONSTs
        // literales que materializan valores usados solo por el CMP final.
        // Patron tipico: %k = const.i64 0; %z = cmp.ne.bool %x, %k; br.cond %z, T, F.
        // Tambien permitimos el CMP en penultima posicion con 0..N CONSTs antes.
        if (H.instrs.size() < 2) continue;
        const IrInstr &h_last = H.instrs.back();
        if (h_last.op != IrOp::BR_COND) continue;
        // Buscar el CMP penultimo (h_last - 1) o anterior.  Las instrs
        // entre el CMP y el BR_COND deben ser CONST puros (sin side effects).
        size_t cmp_idx = H.instrs.size() - 2;
        const IrInstr &h_cmp = H.instrs[cmp_idx];
        bool is_cmp = (h_cmp.op == IrOp::CMP_EQ  || h_cmp.op == IrOp::CMP_NE
                    || h_cmp.op == IrOp::CMP_LT  || h_cmp.op == IrOp::CMP_GT
                    || h_cmp.op == IrOp::CMP_LE  || h_cmp.op == IrOp::CMP_GE
                    || h_cmp.op == IrOp::CMP_ULT || h_cmp.op == IrOp::CMP_UGT
                    || h_cmp.op == IrOp::CMP_ULE || h_cmp.op == IrOp::CMP_UGE);
        if (!is_cmp) continue;
        // Verificar que las instrs antes del CMP sean todas CONST (puras).
        bool only_consts = true;
        for (size_t k = 0; k < cmp_idx; ++k) {
            if (H.instrs[k].op != IrOp::CONST) { only_consts = false; break; }
        }
        if (!only_consts) continue;
        // El cmp.dst debe ser el unico operand del BR_COND.
        if (h_last.operands.empty() || h_last.operands[0] != h_cmp.dst) continue;
        // Contar predecesores de H.
        int preds = 0;
        for (size_t pi = 0; pi < fn.blocks.size(); ++pi) {
            if (fn.blocks[pi].instrs.empty()) continue;
            const IrInstr &pterm = fn.blocks[pi].instrs.back();
            if (pterm.op == IrOp::BR && pterm.target_block == hid) ++preds;
            else if (pterm.op == IrOp::BR_COND
                  && (pterm.target_block == hid || pterm.false_block == hid))
                ++preds;
        }
        if (preds != 1) continue; // mas de 1 pred o 0 -> no fusionar
        // No tocar entry block: si H es entry, no podemos fusionarlo
        // (entry no tiene predecesores; ya filtrado por preds!=1, pero
        // doble check defensivo).
        if (hid == 0) continue;
        // El header NO debe tener PHI nodes (el primer instr debe ser CMP, no PHI).
        // Ya implicito en H.instrs.size() == 2 con h0 = CMP.

        // Aplicar la fusion:
        //   1. Eliminar BR de B.
        //   2. Append todas las instrs de H (CONSTs + CMP + BR_COND) al final de B.
        //   3. Hoist de CONSTs: mover todas las IrOp::CONST justo despues de
        //      las PHI nodes (CONSTs son puros, su orden es irrelevante para
        //      la semantica).  Esto agrupa los CONSTs y deja a SUB+CMP+BR_COND
        //      consecutivos en el final, habilitando peepholes como decjnz.
        //   4. Limpiar H (queda inalcanzable, lo barren los demas pases).
        //   5. Reescribir phi_args en sucesores de BR_COND: ref a H debe pasar a B.
        IrBlockId t_true  = h_last.target_block;
        IrBlockId t_false = h_last.false_block;

        std::vector<IrInstr> moved;
        moved.reserve(H.instrs.size());
        for (auto &ins : H.instrs) moved.push_back(std::move(ins));
        B.instrs.pop_back(); // remover BR de B
        for (auto &ins : moved) B.instrs.push_back(std::move(ins));
        H.instrs.clear();

        // Hoist de CONSTs: estabilizamos el orden en B asi:
        //   [PHIs...] [CONSTs...] [resto en orden original]
        // Stable_partition mantiene el orden relativo de cada grupo.  Los
        // CONSTs no tienen side effects ni dependen de instrucciones
        // anteriores (su unico operand_id es @c imm), asi que moverlos
        // hacia adelante no rompe SSA dominance: si un CONST se usaba a
        // X, ahora esta definido aun antes que X.
        {
            std::vector<IrInstr> phis, consts, rest;
            phis.reserve(4); consts.reserve(8); rest.reserve(B.instrs.size());
            for (auto &ins : B.instrs) {
                if (ins.op == IrOp::PHI)        phis.push_back(std::move(ins));
                else if (ins.op == IrOp::CONST) consts.push_back(std::move(ins));
                else                            rest.push_back(std::move(ins));
            }
            B.instrs.clear();
            B.instrs.reserve(phis.size() + consts.size() + rest.size());
            for (auto &ins : phis)   B.instrs.push_back(std::move(ins));
            for (auto &ins : consts) B.instrs.push_back(std::move(ins));
            for (auto &ins : rest)   B.instrs.push_back(std::move(ins));
        }

        auto rewrite_phi_block_ref = [&](IrBlockId target_id) {
            if (target_id >= fn.blocks.size()) return;
            IrBlock &T = fn.blocks[target_id];
            for (auto &ins : T.instrs) {
                if (ins.op != IrOp::PHI) break;
                for (auto &pa : ins.phi_args) {
                    if (pa.block == hid) pa.block = static_cast<IrBlockId>(bi);
                }
            }
        };
        rewrite_phi_block_ref(t_true);
        rewrite_phi_block_ref(t_false);

        changed = true;
        // No avanzar bi: re-procesar B porque puede haber cadena
        // (B -> H1 -> H2 inlinable transitivamente).
        --bi;
    }
    return changed;
}

// =========================================================================
//  Pase ir_pass_inline
// =========================================================================
//
// Function inlining a nivel modulo.  Para cada CALL site, si el callee
// cumple las heuristicas de inlineabilidad, sustituye la CALL con el
// cuerpo del callee, renombrando SSA values + bloques.
//
// = v1: Single-block callees =
//
// El callee debe tener exactamente 1 bloque que termine con RET (o no
// terminar si vacio).  Esto cubre casos muy comunes:
//   - Wrappers triviales: `i32 f() { return 0; }`
//   - Getters: `i32 get_x(T this) { return this.x; }`
//   - Helpers pequenos sin control de flujo.
//
// Multi-block callees y recursion requieren CFG manipulation mas
// compleja (CFG merge + phi insertion) -- deferred a v2.
//
// = Heuristica =
//
// Inlineable si:
//   - Callee esta definido en nuestro IrModule (no @c is_native).
//   - Callee tiene 1 bloque exactamente y termina con RET.
//   - Body del callee tiene < INLINE_THRESHOLD instrucciones.
//   - Callee NO es el caller (no self-inline).
//
// = Beneficios =
//
// JIT: skip CALL/RET overhead, regalloc puede ver mas contexto,
// const-fold se propaga a traves de la call.  Ejemplo: pruebas() en
// 100_reflection_full despues de dead-alloc elim queda como
// `return 0`.  Si se inline en main, la asignacion a val es trivial
// y el loop body se reduce a la comparacion + incremento.
//
// port-C: codigo destino mas legible, sin auxiliary functions ni
// goto-style returns.

bool ir_pass_inline(IrModule &mod, size_t threshold) {
    /* Threshold de tamano del body del callee para inlinar.
     *
     * Por que 12 por defecto (en lugar de 8 o 16): el overhead del CALLVM (push
     * regs vivos, mov r1..rN args, callvm, pop regs, mov dst r0) en el peor caso
     * son ~24 instrucciones VM.  Cualquier callee cuyo cuerpo cabe en menos de
     * eso es candidato directo a inline ya que ahorramos mas de lo que crece el
     * caller.  12 es el balance que captura getters, setters y helpers
     * aritmeticos pequenos sin causar bloat material en el .velb de programas
     * tipicos.  El C2/OSR pasa un threshold mayor para inlinear las CALLs de un
     * loop CALIENTE (el code-size no importa cuando el loop domina el tiempo). */
    const size_t INLINE_THRESHOLD = threshold;
    bool changed = false;

    /* Build name -> index map. */
    std::unordered_map<std::string, size_t> name_to_idx;
    for (size_t i = 0; i < mod.functions.size(); ++i) {
        name_to_idx[mod.functions[i].name] = i;
    }

    /* Funciones que NUNCA se deben inlinear (entry points alternativos
     * o sintetic functions con calling conventions especiales). */
    auto is_blacklisted = [](const std::string &name) -> bool {
        /* __module_init: el Loader lo invoca via init_pc, no es un CALL
         * normal.  Inlinearlo en main duplica el defclass + deffield
         * + defmethod del bytecode. */
        if (name == "__module_init") return true;
        /* Lambda helpers: invocados via function pointer en CALLCLOSURE.
         * Sus IR son single-block + RET pero el calling convention es
         * distinta (env_addr en r14, etc). */
        if (name.size() > 9 && name.compare(0, 9, "__lambda_") == 0) return true;
        /* Spawn helpers: invocados por SPAWN op, no por CALL. */
        if (name.size() > 8 && name.compare(0, 8, "__spawn_") == 0) return true;
        /* Async helpers: invocados por @Async machinery. */
        if (name.size() > 8 && name.compare(0, 8, "__async_") == 0) return true;
        /* rspawn body helpers. */
        if (name.size() > 9 && name.compare(0, 9, "__rspawn_") == 0) return true;
        return false;
    };

    /* Pre-classify cada function: es inlineable? */
    auto is_inlineable = [&](const IrFunction &fn) -> bool {
        if (fn.is_native) return false;
        if (is_blacklisted(fn.name)) return false;
        if (fn.blocks.size() != 1) return false;
        if (fn.blocks[0].instrs.empty()) return false;
        /* Ultima instr debe ser RET. */
        const auto &last = fn.blocks[0].instrs.back();
        if (last.op != IrOp::RET) return false;
        if (fn.blocks[0].instrs.size() > INLINE_THRESHOLD) return false;
        /* No inlinear funciones que contengan CALLs recursivas a si mismas. */
        for (const auto &ins : fn.blocks[0].instrs) {
            if ((ins.op == IrOp::CALL || ins.op == IrOp::TAILCALL)
             && ins.func_name == fn.name) {
                return false;
            }
        }
        /* No inlinear funciones que tengan @c RAW_ASM en su body cuando
         * el RAW_ASM podria depender del calling convention especifico
         * de la callee.  Conservadoramente: skip si hay raw_asm. */
        for (const auto &ins : fn.blocks[0].instrs) {
            if (ins.op == IrOp::RAW_ASM) return false;
        }
        return true;
    };

    /* Cache de classification. */
    std::vector<bool> can_inline(mod.functions.size(), false);
    for (size_t i = 0; i < mod.functions.size(); ++i) {
        can_inline[i] = is_inlineable(mod.functions[i]);
    }

    for (size_t fi = 0; fi < mod.functions.size(); ++fi) {
        IrFunction &caller = mod.functions[fi];
        if (caller.is_native) continue;

        for (auto &bb : caller.blocks) {
            /* Procesar in-place; recolectar lista de cambios primero
             * para no invalidar iterators. */
            std::vector<IrInstr> new_instrs;
            new_instrs.reserve(bb.instrs.size());

            for (size_t i = 0; i < bb.instrs.size(); ++i) {
                IrInstr &ins = bb.instrs[i];
                if (ins.op != IrOp::CALL) {
                    new_instrs.push_back(std::move(ins));
                    continue;
                }
                /* CALL a function user.  Verificar si el callee esta
                 * en el modulo y es inlineable. */
                auto it = name_to_idx.find(ins.func_name);
                if (it == name_to_idx.end() || it->second == fi
                 || !can_inline[it->second]) {
                    new_instrs.push_back(std::move(ins));
                    continue;
                }
                const IrFunction &callee = mod.functions[it->second];
                const IrBlock &cbody = callee.blocks[0];
                /* Aridad must match: params.size() == operands.size(). */
                if (callee.params.size() != ins.operands.size()) {
                    new_instrs.push_back(std::move(ins));
                    continue;
                }

                /* Mapeo callee_vid -> caller_vid. */
                std::unordered_map<IrValueId, IrValueId> vmap;
                /* Params del callee se mapean a operandos del CALL. */
                for (size_t pi = 0; pi < callee.params.size(); ++pi) {
                    vmap[callee.params[pi]] = ins.operands[pi];
                }

                /* Helper: para cada SSA value que el callee DEFINE,
                 * allocar fresh en el caller. */
                auto remap_dst = [&](IrValueId cvid, IrType type,
                                     const std::string &name_hint) -> IrValueId {
                    if (cvid == IR_NO_VALUE) return IR_NO_VALUE;
                    auto vit = vmap.find(cvid);
                    if (vit != vmap.end()) return vit->second;
                    const IrValueId new_vid = static_cast<IrValueId>(caller.values.size());
                    IrValue nv{};
                    nv.id   = new_vid;
                    nv.type = type;
                    nv.name = "%inl_" + std::to_string(new_vid);
                    (void)name_hint;
                    if (cvid < callee.values.size()) {
                        const auto &cv = callee.values[cvid];
                        nv.is_const          = cv.is_const;
                        nv.const_val         = cv.const_val;
                        nv.is_host_ptr       = cv.is_host_ptr;
                        nv.pointee_is_host_ptr = cv.pointee_is_host_ptr;
                        nv.is_gc_object      = cv.is_gc_object;
                        nv.narrow_only       = cv.narrow_only;
                    }
                    caller.values.push_back(nv);
                    vmap[cvid] = new_vid;
                    return new_vid;
                };

                auto remap_op = [&](IrValueId cvid) -> IrValueId {
                    if (cvid == IR_NO_VALUE) return IR_NO_VALUE;
                    auto vit = vmap.find(cvid);
                    if (vit != vmap.end()) return vit->second;
                    /* Valor del callee que no fue param ni dst previo.
                     * Esto no deberia pasar si procesamos en orden. */
                    return IR_NO_VALUE;
                };

                /* Replicar todas las instrs del callee EXCEPTO el RET final. */
                IrValueId ret_value = IR_NO_VALUE;
                bool inline_ok = true;
                for (const auto &c_ins : cbody.instrs) {
                    if (c_ins.op == IrOp::RET) {
                        if (!c_ins.operands.empty()) {
                            ret_value = remap_op(c_ins.operands[0]);
                        }
                        continue;  /* skip RET; ret value resolved */
                    }
                    /* Clonar c_ins y remap operandos + dst. */
                    IrInstr ni = c_ins;
                    /* dst: si tiene resultado, mapear a nuevo VID en caller. */
                    if (ni.dst != IR_NO_VALUE) {
                        const IrType dst_type = (ni.dst < callee.values.size())
                            ? callee.values[ni.dst].type : ni.type;
                        ni.dst = remap_dst(ni.dst, dst_type, "");
                    }
                    /* operands. */
                    for (auto &op : ni.operands) {
                        op = remap_op(op);
                    }
                    /* func_ptr (CALLIND/CALLCLOSURE). */
                    if (ni.func_ptr != IR_NO_VALUE) {
                        ni.func_ptr = remap_op(ni.func_ptr);
                    }
                    /* phi_args: callee es single-block, no phis razonables.
                     * Si los hay, skip inline. */
                    if (!ni.phi_args.empty()) {
                        inline_ok = false;
                        break;
                    }
                    new_instrs.push_back(std::move(ni));
                }

                if (!inline_ok) {
                    /* Cancelar el inline: revertir lo que anyadimos.
                     * Conservativo: push el CALL original. */
                    /* Quitar las instrs recien anyadidas relacionadas con inline.
                     * Para simplicidad: NO retroceder; quedaria un mix
                     * incorrecto.  Marcar y emit CALL original al final. */
                    /* Reset estrategia: para evitar IR corrupto, hacemos un
                     * passthrough simple: no haber comenzado a anyadir.
                     * Como ya empezamos, no podemos limpiar facilmente.
                     * Por seguridad: rebuild new_instrs desde scratch
                     * usando bb.instrs[0..i]. */
                    new_instrs.clear();
                    for (size_t k = 0; k <= i; ++k) {
                        new_instrs.push_back(bb.instrs[k]);
                    }
                    continue;
                }

                /* Emitir el "resultado" del inline: si el CALL tenia dst,
                 * MOV dst <- ret_value. */
                if (ins.dst != IR_NO_VALUE && ret_value != IR_NO_VALUE) {
                    IrInstr mv{};
                    mv.op = IrOp::MOV;
                    mv.type = ins.type;
                    mv.dst = ins.dst;
                    mv.operands = {ret_value};
                    new_instrs.push_back(std::move(mv));
                } else if (ins.dst != IR_NO_VALUE) {
                    /* Callee no retorno valor pero CALL expected uno.
                     * Conservativo: CONST 0 placeholder. */
                    IrInstr cz{};
                    cz.op = IrOp::CONST;
                    cz.type = ins.type;
                    cz.dst = ins.dst;
                    cz.imm = 0;
                    new_instrs.push_back(std::move(cz));
                }

                changed = true;
            }
            bb.instrs = std::move(new_instrs);
        }
    }
    return changed;
}

// =========================================================================
//  Pase ir_pass_licm
// =========================================================================
//
// Loop-Invariant Code Motion.  Para cada loop simple detectado, mueve
// instrucciones cuyos operandos son TODOS invariantes (definidos fuera
// del loop O constantes) al predecesor del loop header.
//
// Beneficio: las invariantes se calculan UNA vez (en el predecesor) en
// vez de N veces (en cada iteracion del loop).  Tipico para patrones
// como `for(i; i<size; i++)` donde `size` es invariant.
//
// = v1: simple algorithm =
//
// 1. Detectar loops via back-edges (JMP/BR_COND a un bloque ANTERIOR
//    en orden lineal o via dataflow simple).
// 2. Para cada loop header H y back-edge from B:
//    - Conjunto de bloques en el loop: { H, ..., B } (BFS desde B
//      hasta H siguiendo predecesores).
//    - Pre-header: bloque que cae a H pero NO esta en el loop.
//      Si hay multiples, abortar (necesita split, deferred).
// 3. Para cada instr en el loop:
//    - Si pure_op AND todos los operands son CONSTs o definidos fuera
//      del loop AND la instr no es phi AND no tiene side effects:
//      Mover al pre-header.
//
// v1 conservativo: solo loops simples con UN solo back-edge y UN pre-
// header (case clasico while/for).

bool ir_pass_licm(IrFunction &fn) {
    if (fn.blocks.size() < 3) return false;  /* necesita pre-header + body + header */
    const size_t N = fn.blocks.size();

    /* Construir CFG: para cada bloque, sus sucesores y predecesores. */
    std::vector<std::vector<IrBlockId>> preds(N);
    std::vector<std::vector<IrBlockId>> succs(N);
    for (size_t b = 0; b < N; ++b) {
        const auto &bb = fn.blocks[b];
        if (bb.instrs.empty()) continue;
        const auto &last = bb.instrs.back();
        IrBlockId t1 = IR_NO_BLOCK, t2 = IR_NO_BLOCK;
        if (last.op == IrOp::BR) {
            t1 = last.target_block;
        } else if (last.op == IrOp::BR_COND) {
            t1 = last.target_block;
            t2 = last.false_block;
        }
        if (t1 != IR_NO_BLOCK && t1 < N) {
            preds[t1].push_back(static_cast<IrBlockId>(b));
            succs[b].push_back(t1);
        }
        if (t2 != IR_NO_BLOCK && t2 < N) {
            preds[t2].push_back(static_cast<IrBlockId>(b));
            succs[b].push_back(t2);
        }
    }

    /* Dominadores via Cooper-Harvey-Kennedy iterativo.
     * dom[entry] = entry, otros = UNDEF.  Procesar en reverse postorder
     * hasta punto fijo.  intersect(b1, b2) sube en el dom-tree hasta
     * encontrar el ancestro comun mas cercano. */
    const IrBlockId UNDEF = static_cast<IrBlockId>(N);
    const IrBlockId entry = 0;  /* convencion: bloque 0 es entry */

    /* DFS para reverse postorder. */
    std::vector<IrBlockId> rpo;
    rpo.reserve(N);
    {
        std::vector<bool> visited(N, false);
        std::function<void(IrBlockId)> dfs = [&](IrBlockId b) {
            if (b >= N || visited[b]) return;
            visited[b] = true;
            for (IrBlockId s : succs[b]) dfs(s);
            rpo.push_back(b);
        };
        dfs(entry);
        std::reverse(rpo.begin(), rpo.end());
    }
    /* rpo_pos[b] = posicion de b en rpo (mayor = mas adelante = mas alto).
     * Usado por intersect_dom.  Bloques no alcanzables tienen UNDEF rpo_pos. */
    std::vector<uint32_t> rpo_pos(N, UINT32_MAX);
    for (size_t i = 0; i < rpo.size(); ++i) rpo_pos[rpo[i]] = static_cast<uint32_t>(i);

    /* idom[b] = inmediato dominador.  UNDEF = no computado todavia. */
    std::vector<IrBlockId> idom(N, UNDEF);
    idom[entry] = entry;

    auto intersect_dom = [&](IrBlockId b1, IrBlockId b2) -> IrBlockId {
        while (b1 != b2) {
            while (b1 != UNDEF && rpo_pos[b1] > rpo_pos[b2]) b1 = idom[b1];
            while (b2 != UNDEF && rpo_pos[b2] > rpo_pos[b1]) b2 = idom[b2];
            if (b1 == UNDEF || b2 == UNDEF) return UNDEF;
        }
        return b1;
    };

    bool dom_changed = true;
    while (dom_changed) {
        dom_changed = false;
        for (IrBlockId b : rpo) {
            if (b == entry) continue;
            IrBlockId new_idom = UNDEF;
            for (IrBlockId p : preds[b]) {
                if (idom[p] != UNDEF) {
                    new_idom = (new_idom == UNDEF) ? p : intersect_dom(new_idom, p);
                    if (new_idom == UNDEF) break;
                }
            }
            if (new_idom != UNDEF && new_idom != idom[b]) {
                idom[b] = new_idom;
                dom_changed = true;
            }
        }
    }

    /* Helper: T domina B?  Camina la cadena idom desde B hasta entry o T. */
    auto dominates = [&](IrBlockId T, IrBlockId B) -> bool {
        if (T == B) return true;
        if (T >= N || B >= N || idom[B] == UNDEF) return false;
        IrBlockId cur = B;
        while (idom[cur] != cur) {
            cur = idom[cur];
            if (cur == T) return true;
        }
        return false;
    };

    /* Back-edge real: arista B->T donde T domina a B. */
    struct BackEdge { IrBlockId pred; IrBlockId header; };
    std::vector<BackEdge> backs;
    for (size_t b = 0; b < N; ++b) {
        for (IrBlockId s : succs[b]) {
            if (dominates(s, static_cast<IrBlockId>(b))) {
                backs.push_back({static_cast<IrBlockId>(b), s});
            }
        }
    }

    if (backs.empty()) return false;

    bool changed = false;

    /* Procesar cada back-edge (= 1 loop). */
    for (const auto &be : backs) {
        const IrBlockId header = be.header;
        const IrBlockId back   = be.pred;

        /* Bloques en el loop: BFS reverse desde back hasta header via
         * preds (en CFG reducible). */
        std::unordered_set<IrBlockId> loop_set;
        loop_set.insert(header);
        loop_set.insert(back);
        std::vector<IrBlockId> stack{back};
        while (!stack.empty()) {
            IrBlockId b = stack.back();
            stack.pop_back();
            if (b == header) continue;
            for (IrBlockId p : preds[b]) {
                if (!loop_set.count(p)) {
                    loop_set.insert(p);
                    stack.push_back(p);
                }
            }
        }

        /* Pre-header: predecesor de @c header que NO esta en el loop.
         * Tipicamente entry o un bloque previo al while. */
        IrBlockId pre_header = IR_NO_BLOCK;
        for (IrBlockId p : preds[header]) {
            if (!loop_set.count(p)) {
                if (pre_header == IR_NO_BLOCK) {
                    pre_header = p;
                } else {
                    /* Multiples pre-headers: skip (necesita split). */
                    pre_header = IR_NO_BLOCK;
                    break;
                }
            }
        }
        if (pre_header == IR_NO_BLOCK) continue;

        /* Conjunto de VIDs definidos DENTRO del loop. */
        std::unordered_set<IrValueId> defined_in_loop;
        for (IrBlockId b : loop_set) {
            for (const auto &ins : fn.blocks[b].instrs) {
                if (ins.dst != IR_NO_VALUE) {
                    defined_in_loop.insert(ins.dst);
                }
            }
        }

        /* Detectar si hay STORE/MEMCPY/SETFIELD/ARRAY_STORE/RAW_ASM/CALL
         * dentro del loop.  Si los hay, los LOADs no son hoistables sin
         * alias analysis (la memoria pudo cambiar entre iteraciones). */
        bool loop_has_memory_writes = false;
        for (IrBlockId b : loop_set) {
            for (const auto &ins : fn.blocks[b].instrs) {
                switch (ins.op) {
                    case IrOp::STORE: case IrOp::MEMCPY: case IrOp::SETFIELD:
                    case IrOp::ARRAY_STORE: case IrOp::STRFINALIZE:
                    case IrOp::RAW_ASM:
                    case IrOp::CALL: case IrOp::CALLN: case IrOp::CALLVIRT:
                    case IrOp::CALLIND: case IrOp::CALLM: case IrOp::CALLCLOSURE:
                    case IrOp::TAILCALL:
                        loop_has_memory_writes = true;
                        break;
                    default: break;
                }
                if (loop_has_memory_writes) break;
            }
            if (loop_has_memory_writes) break;
        }

        /* Helper: instr es candidato para mover? */
        auto is_invariant_candidate = [&](const IrInstr &ins) -> bool {
            /* Sprint mem-perf string_hot: anyadir STRMAKE/STRCAT/STRINTERN/
             * STRCONV/STRRESERVE como hoistables a pesar de side-effecting.
             * El alloc-identity no es observable; el contenido si.  Ver
             * @c is_licm_hoistable_alloc. */
            if (!is_pure(ins.op) && !is_licm_hoistable_alloc(ins.op)) return false;
            if (ins.op == IrOp::PHI) return false;
            if (ins.preserve) return false;
            if (ins.dst == IR_NO_VALUE) return false;
            /* Sprint string-perf-2 bug fix: STRMAKE solo es seguro hoistar
             * si su vm_addr operand apunta a memoria immutable (literal
             * en static_data via STR_LIT_ADDR).  Para mutable buffers
             * (ALLOCA + stores), hoistar = capturar bytes en momento
             * incorrecto. */
            if (ins.op == IrOp::STRMAKE) {
                if (ins.operands.empty()) return false;
                if (!strmake_reads_immutable(fn, ins.operands[0])) return false;
            }
            /* Ops que LEEN memoria: solo hoistables si NO hay writes en el
             * loop (v1 sin alias analysis).  Conservador pero correcto. */
            switch (ins.op) {
                case IrOp::LOAD: case IrOp::ARRAY_LOAD: case IrOp::GETFIELD:
                case IrOp::ARRAY_LEN:
                    if (loop_has_memory_writes) return false;
                    break;
                default: break;
            }
            /* Todos los operands deben ser invariant (CONST o definidos fuera). */
            for (IrValueId op : ins.operands) {
                if (op == IR_NO_VALUE) continue;
                if (defined_in_loop.count(op)) return false;
            }
            return true;
        };

        /* Recolectar instrucciones a mover (en orden) + remover de sus
         * bloques originales. */
        std::vector<IrInstr> moved;
        for (IrBlockId b : loop_set) {
            auto &bb = fn.blocks[b];
            std::vector<IrInstr> remaining;
            remaining.reserve(bb.instrs.size());
            for (auto &ins : bb.instrs) {
                if (is_invariant_candidate(ins)) {
                    /* Mover.  Despues de mover, el VID ya no esta
                     * definido en el loop -> otros usos del mismo
                     * VID dentro del loop necesitan que la def este
                     * en pre-header (ok, sera invariant alli). */
                    moved.push_back(std::move(ins));
                    defined_in_loop.erase(moved.back().dst);
                    changed = true;
                } else {
                    remaining.push_back(std::move(ins));
                }
            }
            bb.instrs = std::move(remaining);
        }

        if (moved.empty()) continue;

        /* Insertar las moved instrs al final del pre-header, ANTES de
         * su terminador (JMP a header). */
        auto &ph_bb = fn.blocks[pre_header];
        size_t insert_at = ph_bb.instrs.size();
        if (!ph_bb.instrs.empty()) {
            const auto &term = ph_bb.instrs.back();
            if (term.op == IrOp::BR || term.op == IrOp::BR_COND
             || term.op == IrOp::RET || term.op == IrOp::THROW) {
                insert_at = ph_bb.instrs.size() - 1;
            }
        }
        ph_bb.instrs.insert(ph_bb.instrs.begin() + insert_at,
            std::make_move_iterator(moved.begin()),
            std::make_move_iterator(moved.end()));
    }

    return changed;
}

// =========================================================================
//  Pase ir_pass_devirt_monomorphic (Phase D.7.opt)
// =========================================================================

/**
 * @brief Detecta si el modulo usa AOP escaneando raw_asm por "addadvice".
 *
 * Conservador: si encuentra cualquier raw_asm con texto "addadvice", el
 * modulo se considera AOP-enabled y se skip-ea el devirt.  Sin esto las
 * callvirt convertidas a call directo saltarian el advice chain runtime.
 */
static bool module_uses_aop(const IrModule &mod) {
    for (const auto &cls : mod.classes) {
        if (cls.is_aspect) return true;
    }
    for (const auto &fn : mod.functions) {
        for (const auto &bb : fn.blocks) {
            for (const auto &ins : bb.instrs) {
                if (ins.op == IrOp::RAW_ASM
                 && ins.func_name.find("addadvice") != std::string::npos) {
                    return true;
                }
            }
        }
    }
    return false;
}

bool ir_pass_devirt_monomorphic(IrModule &mod) {
    if (module_uses_aop(mod)) return false;

    /* Indice por nombre de clase. */
    std::unordered_map<std::string, const IrClass *> class_by_name;
    for (const auto &c : mod.classes) class_by_name[c.name] = &c;
    if (class_by_name.empty()) return false;

    /* "Effectively final": clase sin subclases dentro del modulo.
     * Conservador (closed world).  El loadmodule dinamico podria
     * cargar una subclase, pero los tests/e2e no usan ese patron. */
    std::unordered_set<std::string> has_subclass;
    for (const auto &c : mod.classes) {
        if (!c.super_name.empty() && c.super_name != "Object") {
            has_subclass.insert(c.super_name);
        }
    }

    bool changed = false;
    for (auto &fn : mod.functions) {
        if (fn.is_native) continue;
        if (fn.blocks.empty()) continue;

        /* class_of: para cada SSA value cuyo origen es conocido como una
         * clase concreta, mapea vid -> class_name.  Se construye iterando
         * en orden lineal de bloques con propagacion a traves de MOV y
         * PHI (si todos los inputs comparten clase). */
        std::unordered_map<IrValueId, std::string> class_of;

        /* Multiples pasadas para propagar a traves de PHIs (loops). */
        const int MAX_ITERS = 4;
        for (int iter = 0; iter < MAX_ITERS; ++iter) {
            bool grew = false;
            for (auto &bb : fn.blocks) {
                for (auto &ins : bb.instrs) {
                    if (ins.dst == IR_NO_VALUE) continue;
                    if (class_of.count(ins.dst)) continue;

                    /* Origen: call @__new_<X> */
                    if (ins.op == IrOp::CALL
                     && ins.func_name.rfind("__new_", 0) == 0) {
                        std::string cn = ins.func_name.substr(6);
                        if (class_by_name.count(cn)) {
                            class_of[ins.dst] = cn;
                            grew = true;
                        }
                        continue;
                    }
                    /* NEWOBJ no carga class_name directamente; ignorar. */
                    /* MOV: hereda clase del source */
                    if (ins.op == IrOp::MOV && !ins.operands.empty()) {
                        auto it = class_of.find(ins.operands[0]);
                        if (it != class_of.end()) {
                            class_of[ins.dst] = it->second;
                            grew = true;
                        }
                        continue;
                    }
                    /* PHI: si TODOS los inputs comparten clase, hereda. */
                    if (ins.op == IrOp::PHI && !ins.phi_args.empty()) {
                        std::string c;
                        bool ok = true;
                        for (const auto &pa : ins.phi_args) {
                            if (pa.value == IR_NO_VALUE
                             || pa.value == ins.dst) continue;
                            auto it = class_of.find(pa.value);
                            if (it == class_of.end()) { ok = false; break; }
                            if (c.empty()) c = it->second;
                            else if (c != it->second) { ok = false; break; }
                        }
                        if (ok && !c.empty()) {
                            class_of[ins.dst] = c;
                            grew = true;
                        }
                        continue;
                    }
                }
            }
            if (!grew) break;
        }

        /* Aplicar devirt. */
        for (auto &bb : fn.blocks) {
            for (auto &ins : bb.instrs) {
                if (ins.op != IrOp::CALLVIRT) continue;
                if (ins.operands.empty()) continue;
                auto it = class_of.find(ins.operands[0]);
                if (it == class_of.end()) continue;
                const std::string &cn = it->second;
                auto ct = class_by_name.find(cn);
                if (ct == class_by_name.end()) continue;
                const IrClass *cls = ct->second;
                /* Safe: clase final O sin subclases EN ESTE MODULO. */
                const bool safe_class = cls->is_final
                                     || !has_subclass.count(cn);
                /* Buscar el metodo en el vtable_index. */
                const IrMethod *mtd = nullptr;
                for (const auto &m : cls->methods) {
                    if (m.vtable_index == static_cast<int32_t>(ins.imm)) {
                        mtd = &m;
                        break;
                    }
                }
                if (!mtd) continue;
                if (mtd->ir_fn_name.empty()) continue;
                const bool safe_method = mtd->is_final || safe_class;
                if (!safe_method) continue;

                /* Rewrite CALLVIRT -> CALL */
                ins.op        = IrOp::CALL;
                ins.func_name = mtd->ir_fn_name;
                ins.imm       = 0;
                /* operands sin cambio: [obj, args...] */
                changed = true;
            }
        }
    }
    return changed;
}

// =========================================================================
//  Pase ir_pass_speculative_devirt (C2): devirt especulativa guiada por IC
// =========================================================================

bool ir_pass_speculative_devirt(IrFunction &fn,
                                const std::vector<SpecDevirtSite> &sites) {
    if (fn.blocks.empty() || sites.empty()) return false;
    bool changed = false;

    /* Reservar de antemano para evitar realocaciones de fn.blocks durante la
     * cirugia (3 bloques nuevos por site).  Aun asi accedemos por INDICE, no
     * por referencia, por seguridad. */
    fn.blocks.reserve(fn.blocks.size() + sites.size() * 3 + 4);

    for (const auto &site : sites) {
        if (site.callvirt_dst == IR_NO_VALUE) continue;  /* void: sin PHI */

        /* Localizar el CALLVIRT objetivo por su dst (unico + estable). */
        IrBlockId bidx = IR_NO_BLOCK;
        size_t    i    = 0;
        for (size_t b = 0; b < fn.blocks.size() && bidx == IR_NO_BLOCK; ++b) {
            auto &bb = fn.blocks[b];
            for (size_t k = 0; k < bb.instrs.size(); ++k) {
                if (bb.instrs[k].op == IrOp::CALLVIRT
                 && bb.instrs[k].dst == site.callvirt_dst) {
                    bidx = static_cast<IrBlockId>(b);
                    i    = k;
                    break;
                }
            }
        }
        if (bidx == IR_NO_BLOCK) continue;  /* no encontrado: skip */

        /* Capturar datos del CALLVIRT (copia) antes de la cirugia. */
        const IrInstr cv             = fn.blocks[bidx].instrs[i];
        const IrType  rtype          = cv.type;
        const IrValueId orig_dst     = cv.dst;
        const std::vector<IrValueId> ops = cv.operands;  /* [obj, args...] */
        const uint32_t srcline       = cv.source_line;
        if (ops.empty()) continue;  /* sin receptor: no especulable */

        /* Capturar los sucesores ORIGINALES de B (los del terminador que va
         * en el tail) antes de sobreescribir B.succs. */
        const std::vector<IrBlockId> orig_succs = fn.blocks[bidx].succs;

        /* Crear los 3 bloques (append; los indices existentes no se mueven). */
        const IrBlockId fastb  = fn.new_block("spec_fast");
        const IrBlockId slowb  = fn.new_block("spec_slow");
        const IrBlockId mergeb = fn.new_block("spec_merge");

        /* Mover el tail [i+1 ..] al merge; truncar B a [0 .. i-1]. */
        {
            auto &Binstrs = fn.blocks[bidx].instrs;
            std::vector<IrInstr> tail(Binstrs.begin() + static_cast<long>(i) + 1,
                                      Binstrs.end());
            fn.blocks[mergeb].instrs = std::move(tail);
            Binstrs.resize(i);  /* descarta el CALLVIRT en i + el tail */
        }

        /* --- Guard en B: cls = load[obj]; cmp cls, T; br_cond fast/slow --- */
        const IrValueId vcls = fn.new_value(IrType::I64, "spec_cls");
        {
            IrInstr ld; ld.op = IrOp::LOAD; ld.type = IrType::I64;
            ld.dst = vcls; ld.operands = {ops[0]}; ld.source_line = srcline;
            fn.blocks[bidx].instrs.push_back(ld);
        }
        const IrValueId vt = fn.new_value(IrType::I64, "spec_T");
        {
            IrInstr c; c.op = IrOp::CONST; c.type = IrType::I64;
            c.dst = vt; c.imm = site.class_ptr; c.source_line = srcline;
            fn.blocks[bidx].instrs.push_back(c);
        }
        const IrValueId vg = fn.new_value(IrType::BOOL, "spec_g");
        {
            IrInstr cm; cm.op = IrOp::CMP_EQ; cm.type = IrType::BOOL;
            cm.dst = vg; cm.operands = {vcls, vt}; cm.source_line = srcline;
            fn.blocks[bidx].instrs.push_back(cm);
        }
        {
            IrInstr br; br.op = IrOp::BR_COND; br.operands = {vg};
            br.target_block = fastb; br.false_block = slowb;
            br.source_line = srcline;
            fn.blocks[bidx].instrs.push_back(br);
        }
        fn.blocks[bidx].succs = {fastb, slowb};

        /* --- Fast: CALL directo al callee (ir_pass_inline lo inlinea). --- */
        const IrValueId rfast = fn.new_value(rtype, "spec_rfast");
        {
            IrInstr call; call.op = IrOp::CALL; call.type = rtype;
            call.dst = rfast; call.func_name = site.callee_ir_name;
            call.operands = ops; call.source_line = srcline;
            fn.blocks[fastb].instrs.push_back(call);
        }
        {
            IrInstr br; br.op = IrOp::BR; br.target_block = mergeb;
            fn.blocks[fastb].instrs.push_back(br);
        }
        fn.blocks[fastb].preds = {bidx};
        fn.blocks[fastb].succs = {mergeb};

        /* --- Slow: CALLVIRT original (copia) -> r_slow. --- */
        const IrValueId rslow = fn.new_value(rtype, "spec_rslow");
        {
            IrInstr cv2 = cv; cv2.dst = rslow;
            fn.blocks[slowb].instrs.push_back(cv2);
        }
        {
            IrInstr br; br.op = IrOp::BR; br.target_block = mergeb;
            fn.blocks[slowb].instrs.push_back(br);
        }
        fn.blocks[slowb].preds = {bidx};
        fn.blocks[slowb].succs = {mergeb};

        /* --- Merge: PHI(orig_dst) = [r_fast@fast, r_slow@slow] + tail. --- */
        {
            IrInstr phi; phi.op = IrOp::PHI; phi.type = rtype; phi.dst = orig_dst;
            phi.phi_args = { IrPhiArg{rfast, fastb}, IrPhiArg{rslow, slowb} };
            phi.source_line = srcline;
            fn.blocks[mergeb].instrs.insert(fn.blocks[mergeb].instrs.begin(), phi);
        }
        fn.blocks[mergeb].preds = {fastb, slowb};
        fn.blocks[mergeb].succs = orig_succs;

        /* Repuntar los sucesores originales de B: ahora su predecesor es
         * merge (el terminador del tail vive ahi).  Tambien sus PHIs. */
        for (IrBlockId s : orig_succs) {
            if (s == IR_NO_BLOCK || s >= fn.blocks.size()) continue;
            auto &sb = fn.blocks[s];
            for (auto &p : sb.preds) if (p == bidx) p = mergeb;
            for (auto &ins : sb.instrs) {
                if (ins.op != IrOp::PHI) continue;
                for (auto &pa : ins.phi_args) if (pa.block == bidx) pa.block = mergeb;
            }
        }

        changed = true;
    }

    return changed;
}

// =========================================================================
//  Pase Load Narrow: elide SEXT redundante tras LOAD i8/i16/i32
// =========================================================================

bool ir_pass_load_narrow(IrFunction &fn) {
    if (fn.blocks.empty()) return false;

    /* Ops "narrow-safe": dado inputs con bits bajos correctos (sin importar
     * bits altos), producen un resultado cuyos bits bajos siguen siendo
     * correctos.  ADD/SUB/MUL/AND/OR/XOR son bit-parallel en los bits bajos. */
    auto is_narrow_safe_arith = [](IrOp op) -> bool {
        switch (op) {
            case IrOp::ADD:
            case IrOp::SUB:
            case IrOp::MUL:
            case IrOp::AND:
            case IrOp::OR:
            case IrOp::XOR:
                return true;
            default:
                return false;
        }
    };

    /* Construir lista de usos (vid -> [(block_idx, instr_idx, kind)]).
     * kind: 0=operands, 1=phi_args.  Solo necesitamos saber QUE instrucciones
     * referencian cada valor para inspeccionar su op. */
    struct UseRef {
        size_t  bi;
        size_t  ii;
    };
    std::unordered_map<IrValueId, std::vector<UseRef>> uses;
    for (size_t bi = 0; bi < fn.blocks.size(); ++bi) {
        const auto &bb = fn.blocks[bi];
        for (size_t ii = 0; ii < bb.instrs.size(); ++ii) {
            const auto &ins = bb.instrs[ii];
            for (IrValueId op : ins.operands) {
                if (op != IR_NO_VALUE) uses[op].push_back({bi, ii});
            }
            for (const auto &pa : ins.phi_args) {
                if (pa.value != IR_NO_VALUE) uses[pa.value].push_back({bi, ii});
            }
        }
    }

    /* Para cada LOAD i8/i16/i32 (signed), computar el cierre transitivo
     * de valores derivados via ops narrow-safe.  Si TODOS los usos terminales
     * son STORE/RET del mismo tipo y todos los usos intermedios son ops
     * narrow-safe o STORE/RET, marcar el LOAD como narrow_only. */
    /* Re-analizar en cada invocacion: el flag puede REVOCARSE si pasos
     * posteriores (CSE/copy_prop) exponen usos que antes no eran visibles
     * (e.g. %42 = add %38, %41 cuya operand %41 luego se rewrite a %29). */
    bool changed = false;
    for (auto &bb : fn.blocks) {
        for (auto &ins : bb.instrs) {
            if (ins.op != IrOp::LOAD) continue;
            if (ins.dst == IR_NO_VALUE) continue;
            if (ins.type != IrType::I8 && ins.type != IrType::I16
             && ins.type != IrType::I32) continue;

            const IrType narrow_type = ins.type;

            /* Cierre transitivo via BFS. */
            std::unordered_set<IrValueId> closure;
            std::vector<IrValueId> worklist;
            closure.insert(ins.dst);
            worklist.push_back(ins.dst);

            bool safe = true;
            while (safe && !worklist.empty()) {
                IrValueId v = worklist.back();
                worklist.pop_back();

                auto it = uses.find(v);
                if (it == uses.end()) continue; // no uses -> trivially safe

                for (const UseRef &u : it->second) {
                    const IrInstr &user = fn.blocks[u.bi].instrs[u.ii];

                    /* STORE del mismo tipo: el valor solo se usa como
                     * operand[0] (val).  Truncar al ancho de tipo es seguro. */
                    if (user.op == IrOp::STORE) {
                        if (user.type != narrow_type) { safe = false; break; }
                        /* El valor solo es seguro si esta en operand[0] (val);
                         * si esta en operand[1] (ptr), eso seria un puntero
                         * derivado del LOAD lo cual es UNSAFE (no es nuestro
                         * caso esperado pero por seguridad). */
                        if (user.operands.size() < 2) { safe = false; break; }
                        if (user.operands[0] != v) { safe = false; break; }
                        continue;
                    }

                    /* RET del mismo tipo: el caller espera el ancho declarado,
                     * el VM trunca al hacer return.  Conservadoramente solo
                     * permitimos cuando fn.ret_type coincide. */
                    if (user.op == IrOp::RET) {
                        if (fn.ret_type != narrow_type) { safe = false; break; }
                        continue;
                    }

                    /* Op narrow-safe del mismo tipo: propagar al closure. */
                    if (is_narrow_safe_arith(user.op) && user.type == narrow_type) {
                        if (user.dst != IR_NO_VALUE && closure.insert(user.dst).second) {
                            worklist.push_back(user.dst);
                        }
                        continue;
                    }

                    /* PHI del mismo tipo: el PHI mismo produce un valor i32
                     * cuyos bits altos pueden ser garbage si NUESTRO valor
                     * llega.  Pero si TODOS los usos del PHI son seguros, el
                     * garbage no importa.  Propagar el dst del PHI al closure
                     * para que la BFS verifique sus usos transitivamente.
                     * Los ciclos en PHIs de loops se manejan via el set
                     * @c closure (no se re-procesa lo ya visitado).
                     * Otros inputs del PHI no nos importan: solo nos
                     * preocupa como nuestro valor se propaga a partir del
                     * PHI hacia adelante. */
                    if (user.op == IrOp::PHI && user.type == narrow_type) {
                        if (user.dst != IR_NO_VALUE && closure.insert(user.dst).second) {
                            worklist.push_back(user.dst);
                        }
                        continue;
                    }

                    /* MOV del mismo tipo (e.g. copy_prop residual): propagar. */
                    if (user.op == IrOp::MOV && user.type == narrow_type) {
                        if (user.dst != IR_NO_VALUE && closure.insert(user.dst).second) {
                            worklist.push_back(user.dst);
                        }
                        continue;
                    }

                    /* Cualquier otro uso (CMP, SEXT, ZEXT, CAST, BITCAST,
                     * TRUNC, SHL/SHR/SAR, NEG/NOT, SDIV/UDIV/SMOD/UMOD,
                     * CALL, STORE/LOAD/RET de tipo distinto, etc.) aborta
                     * la elision -- los bits altos pueden ser necesarios. */
                    safe = false;
                    break;
                }
            }

            /* Establecer/revocar el flag segun analisis actual. */
            const bool prev = fn.values[ins.dst].narrow_only;
            if (prev != safe) {
                fn.values[ins.dst].narrow_only = safe;
                changed = true;
            }
        }
    }

    return changed;
}

// =========================================================================
//  Pase List Scheduling: reordena para exponer ILP
// =========================================================================

/* Determina si la instruccion es una "barrera" (no se puede reordenar
 * a traves de ella en NINGUNA direccion: ni mover instrucciones hacia
 * arriba de la barrera, ni hacia abajo).  Usado para CALLs, RAW_ASM,
 * NEWOBJ, etc. donde el side-effect es opaco al scheduler. */
static bool is_sched_barrier(IrOp op) {
    switch (op) {
        case IrOp::CALL: case IrOp::CALLN: case IrOp::CALLVIRT:
        case IrOp::CALLIND: case IrOp::CALLM: case IrOp::CALLCLOSURE:
        case IrOp::TAILCALL: case IrOp::CALLSUPER:
        case IrOp::RAW_ASM:
        case IrOp::NEWOBJ: case IrOp::GC_ALLOC: case IrOp::GC_ALLOCP:
        case IrOp::RAW_ALLOC: case IrOp::RAW_FREE:
        case IrOp::THROW: case IrOp::TRYENTER: case IrOp::TRYLEAVE:
        case IrOp::SETFIELD: case IrOp::ARRAY_STORE:
        case IrOp::MEMCPY:
        case IrOp::STRFINALIZE: case IrOp::GCWB_IR:
        // Sprint string-perf-2 bug fix (2026-06-02): STRMAKE LEE
        // bytes desde vm_mem en runtime (via vm_addr).  Sin marcarla
        // como barrera, el scheduler podia reordenar STOREs a vm_mem
        // PASADO la STRMAKE, leyendo bytes stale.  Bug capturado en
        // patron `buf[i]=X; STRMAKE(buf); buf[i]=Y; STRMAKE(buf)`
        // donde el segundo STORE quedaba post-STRMAKE.
        // STRCAT/STRCONV/STRFLAT NO leen vm_mem (operan sobre handles),
        // pero pueden disparar alloc -> GC -> rearrange objects.  Mas
        // seguro tratar TODAS las str ops alloc-side como barreras
        // hasta confirmar safety por op.
        case IrOp::STRMAKE: case IrOp::STRCAT: case IrOp::STRCONV:
        case IrOp::STRFLAT: case IrOp::STRINTERN: case IrOp::STRRESERVE:
        case IrOp::FUTURE: case IrOp::AWAIT: case IrOp::FULFILL: case IrOp::REJECT:
        case IrOp::FULFILL_HLT:
        case IrOp::MSGSEND: case IrOp::MSGRECV:
        // Recuperados fase B: instrucciones que LEEN structs de params
        // construidos por STOREs previos.  Sin barrera, el scheduler puede
        // moverlas antes de los STOREs y leer basura.  Cubre tambien las
        // operaciones GC/atomic/static que mutan estado global.
        case IrOp::MVTAKE_IR:
        case IrOp::GC_PROMOTE: case IrOp::GC_DEMOTE:
        case IrOp::GC_HANDLE_FOR_PTR:
        case IrOp::ATOMIC_LD_I64: case IrOp::ATOMIC_ST_I64:
        case IrOp::ATOMIC_CAS_I64: case IrOp::ATOMIC_ADD_I64:
        case IrOp::GETSTATIC: case IrOp::SETSTATIC:
        case IrOp::FINDCLASS: case IrOp::DEFCLASS:
        case IrOp::DEFFIELD:  case IrOp::DEFMETHOD:  case IrOp::ADDADVICE:
        case IrOp::FINDMETHOD: case IrOp::FINDFIELD:
        case IrOp::PROCEED:
        case IrOp::SPAWN_ON: case IrOp::HLT: case IrOp::PANIC:
        case IrOp::GETPID:  case IrOp::GETARGC: case IrOp::GETARG:
        case IrOp::STRGETBYTES:
            return true;
        default:
            return false;
    }
}

/* STORE no es barrera total pero sirve de "memory barrier" suave:
 * LOADs posteriores podrian alias, asi que LOAD depende de todos los
 * STOREs previos del mismo bloque (conservativo).  Otros STOREs tambien
 * dependen del previo (orden de escritura es observable). */
static bool is_store_like(IrOp op) {
    return op == IrOp::STORE || op == IrOp::SETFIELD
        || op == IrOp::ARRAY_STORE || op == IrOp::MEMCPY;
}

static bool is_load_like(IrOp op) {
    return op == IrOp::LOAD || op == IrOp::GETFIELD
        || op == IrOp::ARRAY_LOAD || op == IrOp::ARRAY_LEN;
}

/* Terminadores: deben quedar al final del bloque. */
static bool is_sched_terminator(IrOp op) {
    return op == IrOp::BR || op == IrOp::BR_COND
        || op == IrOp::RET || op == IrOp::THROW;
}

bool ir_pass_schedule(IrFunction &fn) {
    bool changed = false;

    for (auto &bb : fn.blocks) {
        const size_t N = bb.instrs.size();
        if (N <= 2) continue; // nada que reordenar

        /* Identificar prefijo de PHIs (fijo al inicio) y terminador. */
        size_t first_movable = 0;
        while (first_movable < N && bb.instrs[first_movable].op == IrOp::PHI) {
            ++first_movable;
        }
        size_t last_movable = N;
        if (last_movable > 0 && is_sched_terminator(bb.instrs[last_movable - 1].op)) {
            --last_movable;
        }
        if (last_movable - first_movable < 2) continue; // <2 instrucciones movibles

        const size_t M = last_movable - first_movable;

        /* Construir DAG de dependencias.  Nodos = indices [0..M) en el
         * rango movible.  Edges: pred[i] = lista de nodos que i depende.
         * succ[i] = lista de nodos que dependen de i. */
        std::vector<std::vector<size_t>> preds(M);
        std::vector<std::vector<size_t>> succs(M);
        std::vector<size_t> in_degree(M, 0);

        /* Map: def_vid -> index dentro de [0..M) que lo define. */
        std::unordered_map<IrValueId, size_t> def_of;
        for (size_t i = 0; i < M; ++i) {
            const auto &ins = bb.instrs[first_movable + i];
            if (ins.dst != IR_NO_VALUE) def_of[ins.dst] = i;
        }

        /* Tracking de "ultima barrera/store/load" para deps de memoria. */
        long last_barrier = -1;
        long last_store   = -1;
        std::vector<size_t> loads_after_last_store; // LOADs posteriores al ultimo store

        auto add_edge = [&](size_t from, size_t to) {
            /* Evitar duplicados.  Sanity: from != to. */
            if (from == to) return;
            for (size_t p : preds[to]) if (p == from) return;
            preds[to].push_back(from);
            succs[from].push_back(to);
        };

        for (size_t i = 0; i < M; ++i) {
            const auto &ins = bb.instrs[first_movable + i];

            /* Data deps: para cada operando con def en este bloque, edge def->i. */
            for (IrValueId op : ins.operands) {
                if (op == IR_NO_VALUE) continue;
                auto it = def_of.find(op);
                if (it != def_of.end() && it->second < i) {
                    add_edge(it->second, i);
                }
            }
            for (const auto &pa : ins.phi_args) {
                if (pa.value == IR_NO_VALUE) continue;
                auto it = def_of.find(pa.value);
                if (it != def_of.end() && it->second < i) {
                    add_edge(it->second, i);
                }
            }
            if (ins.func_ptr != IR_NO_VALUE) {
                auto it = def_of.find(ins.func_ptr);
                if (it != def_of.end() && it->second < i) {
                    add_edge(it->second, i);
                }
            }

            /* Memory/side-effect deps. */
            const bool is_barr = is_sched_barrier(ins.op);
            const bool is_st   = is_store_like(ins.op);
            const bool is_ld   = is_load_like(ins.op);

            if (is_barr) {
                /* Barrera: depende de todo lo previo, bloquea todo lo posterior.
                 * Conservador: anyadir edge desde TODOS los nodos previos. */
                for (size_t j = 0; j < i; ++j) add_edge(j, i);
                last_barrier = static_cast<long>(i);
                last_store   = static_cast<long>(i);
                loads_after_last_store.clear();
            } else if (is_st) {
                /* STORE depende de la ultima barrera, del ultimo store, y de
                 * todos los LOADs posteriores al ultimo store (orden W-after-R). */
                if (last_barrier >= 0) add_edge(static_cast<size_t>(last_barrier), i);
                if (last_store >= 0)   add_edge(static_cast<size_t>(last_store), i);
                for (size_t ld_idx : loads_after_last_store) add_edge(ld_idx, i);
                last_store = static_cast<long>(i);
                loads_after_last_store.clear();
            } else if (is_ld) {
                /* LOAD depende de la ultima barrera y del ultimo store. */
                if (last_barrier >= 0) add_edge(static_cast<size_t>(last_barrier), i);
                if (last_store >= 0)   add_edge(static_cast<size_t>(last_store), i);
                loads_after_last_store.push_back(i);
            } else {
                /* Pure ops: data deps + dep contra ultima barrera para
                 * evitar hoist sobre CALL/etc.  Sin esto, un @c const.i32
                 * declarado DESPUES de un CALL podria moverse ANTES del
                 * CALL, aumentando register pressure (el const queda vivo
                 * across el call y debe salvarse en push/pop).  El bench
                 * fib regresiono al hoistar @c const.i32 2 sobre el call
                 * recursivo.  La barrera actua como "muro de scheduling"
                 * en ambas direcciones (no entran ni salen ops a traves). */
                if (last_barrier >= 0) add_edge(static_cast<size_t>(last_barrier), i);
            }
        }

        /* Computar in_degree desde preds. */
        for (size_t i = 0; i < M; ++i) in_degree[i] = preds[i].size();

        /* Computar critical path length (CPL): CPL[i] = 1 + max(CPL[succ]).
         * Calculado en orden topologico inverso (de hojas a raices).
         * Hacemos topo sort primero. */
        std::vector<size_t> topo;
        topo.reserve(M);
        std::vector<size_t> in_deg_copy = in_degree;
        std::vector<size_t> q;
        for (size_t i = 0; i < M; ++i) {
            if (in_deg_copy[i] == 0) q.push_back(i);
        }
        while (!q.empty()) {
            size_t n = q.back(); q.pop_back();
            topo.push_back(n);
            for (size_t s : succs[n]) {
                if (--in_deg_copy[s] == 0) q.push_back(s);
            }
        }
        if (topo.size() != M) continue; // ciclo detectado (no deberia pasar en SSA + DAG)

        std::vector<uint32_t> cpl(M, 0);
        for (auto it = topo.rbegin(); it != topo.rend(); ++it) {
            size_t n = *it;
            uint32_t best = 0;
            for (size_t s : succs[n]) {
                if (cpl[s] > best) best = cpl[s];
            }
            cpl[n] = best + 1;
        }

        /* List scheduling: ready set ordenado por (CPL desc, indice asc para estable). */
        std::vector<size_t> new_order;
        new_order.reserve(M);
        std::vector<size_t> ready;
        ready.reserve(M);
        std::vector<size_t> rem_in = in_degree;
        for (size_t i = 0; i < M; ++i) {
            if (rem_in[i] == 0) ready.push_back(i);
        }

        while (!ready.empty()) {
            /* Elegir el de mayor CPL (criterio clasico de Sethi-Ullman /
             * critical-path scheduling).  Tie-break:
             *   (a) menos sucesores primero -- "leaf" nodes que rellenan
             *       latencia sin extender el chain critico;
             *   (b) indice original menor para estabilidad. */
            size_t best_idx = 0;
            for (size_t j = 1; j < ready.size(); ++j) {
                const size_t a = ready[best_idx];
                const size_t b = ready[j];
                if (cpl[b] > cpl[a]) {
                    best_idx = j;
                } else if (cpl[b] == cpl[a]) {
                    if (succs[b].size() < succs[a].size()) {
                        best_idx = j;
                    } else if (succs[b].size() == succs[a].size() && b < a) {
                        best_idx = j;
                    }
                }
            }
            size_t pick = ready[best_idx];
            ready[best_idx] = ready.back();
            ready.pop_back();
            new_order.push_back(pick);
            for (size_t s : succs[pick]) {
                if (--rem_in[s] == 0) ready.push_back(s);
            }
        }

        if (new_order.size() != M) continue; // sanity

        /* Detectar si el orden cambio.  Si no, skip. */
        bool different = false;
        for (size_t i = 0; i < M; ++i) {
            if (new_order[i] != i) { different = true; break; }
        }
        if (!different) continue;

        /* Aplicar reordenamiento: mover bb.instrs[first_movable + new_order[i]]
         * a posicion first_movable + i.  Hacer una copia temporal porque las
         * indices originales se invalidan al mover. */
        std::vector<IrInstr> reordered;
        reordered.reserve(M);
        for (size_t i = 0; i < M; ++i) {
            reordered.push_back(std::move(bb.instrs[first_movable + new_order[i]]));
        }
        for (size_t i = 0; i < M; ++i) {
            bb.instrs[first_movable + i] = std::move(reordered[i]);
        }
        changed = true;
    }

    return changed;
}

// =========================================================================
//  Pase ir_pass_loop_memcpy_idiom (Sprint mem-perf 2026-06-02)
// =========================================================================
//
// Reconocimiento de loop-idiom byte-a-byte de la forma:
//
//   while_header:
//       %i_phi = phi.u64 [%i_init, pred]  [%i_next, body]
//       %cond  = cmp.ult.bool %i_phi, %N_ub
//       br.cond %cond, body, exit
//   body:
//       (%i_or_cast = bitcast %i_phi)?
//       %src_p = add.ptr %src_base, %i_or_cast
//       %dst_p = add.ptr %dst_base, %i_or_cast
//       %v     = load.u8 %src_p
//       store %v, %dst_p
//       %i_next = add %i_phi, %1_const
//       br while_header
//
// Lo reemplaza por una sola @c CALLN a @c vio_memcpy.  La libc nativa
// vectoriza con SSE/AVX/AVX-512 segun la CPU, asi el copy loop se
// acelera ~50-100x sin necesidad de SIMD codegen explicito.
//
// Pre-condiciones:
//   - El bloque body tiene EXACTAMENTE los 6-7 instrs del patron.
//   - El PHI del header solo tiene un valor de loop-carry (no PHIs
//     multiples).
//   - %i_init es CONST 0 (o cualquier const; usado como offset inicial
//     que ignoramos -- el memcpy copia [0, N)).  Por simplicidad
//     exigimos 0.
//   - %i_next = add %i_phi, 1 (step de 1).
//
// Tras el match: el body se reemplaza por `CALLN vio_memcpy(dst, src,
// N) + br exit`.  El header sigue invocando body solo la primera vez;
// la siguiente iteracion el cond falla porque body cambio el flow.
// Mas correcto: el body NO retorna a header (br exit directo), asi
// el loop nunca itera.
bool ir_pass_loop_memcpy_idiom(IrFunction &fn) {
    bool changed = false;
    if (fn.is_native) return false;

    for (size_t hi = 0; hi < fn.blocks.size(); ++hi) {
        IrBlock &header = fn.blocks[hi];
        // Header debe tener exactamente: 1 phi, 1 cmp, 1 br.cond.
        if (header.instrs.size() != 3) continue;
        const IrInstr &phi    = header.instrs[0];
        const IrInstr &cmp_in = header.instrs[1];
        const IrInstr &brc    = header.instrs[2];
        if (phi.op != IrOp::PHI) continue;
        if (phi.phi_args.size() != 2) continue;
        if (cmp_in.op != IrOp::CMP_ULT && cmp_in.op != IrOp::CMP_LT) continue;
        if (brc.op != IrOp::BR_COND) continue;
        if (brc.operands.empty() || brc.operands[0] != cmp_in.dst) continue;
        if (cmp_in.operands.size() != 2 || cmp_in.operands[0] != phi.dst) continue;
        IrValueId v_N = cmp_in.operands[1];
        IrBlockId body_id = brc.target_block;
        IrBlockId exit_id = brc.false_block;
        if (body_id >= fn.blocks.size() || exit_id >= fn.blocks.size()) continue;

        // Identificar el predecessor (entry) y el body en los phi_args.
        IrValueId v_init = IR_NO_VALUE;
        IrBlockId pred_id = IR_NO_VALUE;
        IrValueId v_next = IR_NO_VALUE;
        IrBlockId loop_pred = IR_NO_VALUE;
        for (const auto &pa : phi.phi_args) {
            if (pa.block == body_id) { v_next = pa.value; loop_pred = pa.block; }
            else                      { v_init = pa.value; pred_id   = pa.block; }
        }
        if (v_init == IR_NO_VALUE || v_next == IR_NO_VALUE) continue;

        // %i_init debe ser CONST 0.
        if (v_init >= fn.values.size() || !fn.values[v_init].is_const) continue;
        if (fn.values[v_init].const_val != 0) continue;

        // Body matching.
        IrBlock &body = fn.blocks[body_id];
        // Patron flexible: 5 o 6 instrs (con o sin bitcast).
        if (body.instrs.size() < 5 || body.instrs.size() > 7) continue;
        // Ultima instr debe ser br header.
        const IrInstr &body_term = body.instrs.back();
        if (body_term.op != IrOp::BR) continue;
        if (body_term.target_block != header.id) continue;

        // Buscar: load.u8, store, add (= i+1).
        IrValueId v_src_p = IR_NO_VALUE, v_dst_p = IR_NO_VALUE;
        IrValueId v_loaded = IR_NO_VALUE;
        IrValueId v_inc_step = IR_NO_VALUE;
        IrValueId v_index_used = IR_NO_VALUE;
        bool found_load = false, found_store = false, found_inc = false;
        IrValueId v_src_base = IR_NO_VALUE, v_dst_base = IR_NO_VALUE;
        for (const auto &ins : body.instrs) {
            if (ins.op == IrOp::LOAD && ins.type == IrType::U8
             && ins.operands.size() == 1) {
                v_src_p  = ins.operands[0];
                v_loaded = ins.dst;
                found_load = true;
            } else if (ins.op == IrOp::STORE && ins.operands.size() >= 2) {
                if (ins.operands[0] != v_loaded) { found_store = false; break; }
                v_dst_p = ins.operands[1];
                found_store = true;
            } else if (ins.op == IrOp::ADD && ins.operands.size() == 2
                    && ins.dst == v_next) {
                if (ins.operands[0] != phi.dst) continue;
                v_inc_step = ins.operands[1];
                found_inc = true;
            } else if (ins.op == IrOp::ADD && ins.operands.size() == 2) {
                // posible add.ptr base + index -- lo procesamos despues.
            } else if (ins.op == IrOp::BITCAST && ins.operands.size() == 1
                    && ins.operands[0] == phi.dst) {
                // ok, lo trataremos como un alias del index.
            } else if (ins.op == IrOp::BR) {
                // terminator OK
            } else {
                // instr inesperada -> rechazar.
                found_load = false;
                break;
            }
        }
        if (!found_load || !found_store || !found_inc) continue;

        // v_inc_step debe ser const 1.
        if (v_inc_step >= fn.values.size() || !fn.values[v_inc_step].is_const) continue;
        if (fn.values[v_inc_step].const_val != 1) continue;

        // Identificar bases via los ADDs.  Cada add.ptr produce v_src_p o v_dst_p.
        // operands son (base, index).  Index debe ser phi.dst o un bitcast de phi.dst.
        auto resolve_base = [&](IrValueId pv) -> IrValueId {
            for (const auto &ins : body.instrs) {
                if (ins.op == IrOp::ADD && ins.dst == pv
                 && ins.operands.size() == 2) {
                    IrValueId idx = ins.operands[1];
                    bool ok = (idx == phi.dst);
                    if (!ok) {
                        // chequear bitcast
                        for (const auto &b2 : body.instrs) {
                            if (b2.op == IrOp::BITCAST && b2.dst == idx
                             && !b2.operands.empty() && b2.operands[0] == phi.dst) {
                                ok = true; break;
                            }
                        }
                    }
                    if (ok) return ins.operands[0];
                }
            }
            return IR_NO_VALUE;
        };
        v_src_base = resolve_base(v_src_p);
        v_dst_base = resolve_base(v_dst_p);
        if (v_src_base == IR_NO_VALUE || v_dst_base == IR_NO_VALUE) continue;

        // OK match completo.  Reemplazar el body por:
        //   CALLN vio_memcpy(dst, src, N) + br exit
        IrInstr call_ins;
        call_ins.op = IrOp::CALLN;
        call_ins.type = IrType::I64;
        call_ins.dst = IR_NO_VALUE;
        call_ins.func_name = "stdlib/native/io/vesta_io:vio_memcpy";
        call_ins.operands = { v_dst_base, v_src_base, v_N };
        call_ins.source_line = body.instrs.front().source_line;

        IrInstr br_exit;
        br_exit.op = IrOp::BR;
        br_exit.target_block = exit_id;
        br_exit.dst = IR_NO_VALUE;

        body.instrs.clear();
        body.instrs.push_back(call_ins);
        body.instrs.push_back(br_exit);

        // Fix succs/preds: body ya no apunta a header.
        body.succs.clear();
        body.succs.push_back(exit_id);
        // Quitar body de header.preds (mantener el original pred).
        auto &hpreds = header.preds;
        hpreds.erase(std::remove(hpreds.begin(), hpreds.end(), body_id),
                     hpreds.end());
        // Anyadir body a exit.preds si no esta.
        IrBlock &exit_blk = fn.blocks[exit_id];
        if (std::find(exit_blk.preds.begin(), exit_blk.preds.end(), body_id)
            == exit_blk.preds.end()) {
            exit_blk.preds.push_back(body_id);
        }
        // Quitar el phi_arg de body en el PHI del header (ahora 1-arg).
        IrInstr &header_phi = fn.blocks[hi].instrs[0];
        header_phi.phi_args.erase(
            std::remove_if(header_phi.phi_args.begin(), header_phi.phi_args.end(),
                [body_id](const IrPhiArg &pa) { return pa.block == body_id; }),
            header_phi.phi_args.end());

        changed = true;
    }
    return changed;
}

// =========================================================================
//  Punto de entrada principal
// =========================================================================

void ir_optimize(IrModule &mod, OptLevel level) {
    if (level == OptLevel::O0) return; // sin optimizacion

    /* Phase D.7.opt: inline a nivel modulo ANTES del fix-point loop.
     * Despues del inline, los passes per-function se re-aplican sobre
     * el codigo expandido. */
    if (level >= OptLevel::O1) {
        ir_pass_inline(mod);
    }

    /* Phase D.jit-mem-model AUTO-PROMOTE: marca ALLOCAs que fluyen a
     * CALLN como is_host_ptr=true.  El JIT selector las emite en host
     * stack; el ptr resultante es directamente dereferenciable por
     * funciones nativas (Win API, libc, etc.).  Sin esto, `&local`
     * pasado a CALLN seria VM-addr -> garbage.  Cero anotaciones del
     * usuario: el analisis es backward-flow desde args PTR de CALLN.
     * UNA pasada (no se itera con el resto). */
    if (level >= OptLevel::O1) {
        for (auto &fn : mod.functions) {
            if (!fn.is_native) ir_pass_promote_callned_allocas(fn);
        }
    }

    /* Sprint string-perf-8 (2026-06-02): promueve ALLOCAs LOCALES (no
     * escapan a CALL*, RET, THROW, etc.) a `host_alloca=true`.  El JIT
     * emite `sub rsp, N` en host stack y los LOAD/STORE usan native mov
     * directo (1 instr) en lugar del inline cache check (~10 instr).
     * Skippable via VESTA_NO_PROMOTE_LOCAL_ALLOCAS=1 para A/B testing. */
    if (level >= OptLevel::O1) {
        const char *skip = std::getenv("VESTA_NO_PROMOTE_LOCAL_ALLOCAS");
        if (!skip || skip[0] == '\0' || skip[0] == '0') {
            for (auto &fn : mod.functions) {
                if (!fn.is_native) ir_pass_promote_local_allocas(fn);
            }
        }
    }

    /* Promocionar malloc(N_const)+free(p) locales sin escape a
     * ALLOCA host_alloca.  Convierte ~200-500 ns por alloc/free en
     * loops a ~1 ns (sub/add rsp del host stack).  UNA pasada.
     * Skippable via VESTA_NO_PROMOTE_RAW_ALLOC=1 para A/B testing. */
    if (level >= OptLevel::O1) {
        const char *skip = std::getenv("VESTA_NO_PROMOTE_RAW_ALLOC");
        const bool do_promote = !(skip && skip[0] != '\0' && skip[0] != '0');
        if (do_promote) {
            for (auto &fn : mod.functions) {
                if (!fn.is_native) ir_pass_promote_local_raw_alloc(fn);
            }
        }
    }

    // Iterar hasta punto fijo o maximo 8 pasadas
    for (int pass = 0; pass < 8; ++pass) {
        bool any = false;

        for (auto &fn : mod.functions) {
            if (fn.is_native) continue; // no optimizar stubs nativos

            // O1: copy + simplify + SR + reassoc + dead-alloc + DCE
            any |= ir_pass_copy_prop(fn);
            any |= ir_pass_simplify(fn);            /* algebraic + cast fold + phi simp */
            any |= ir_pass_strength_reduction(fn);  /* mul/div/mod power-of-2 -> shifts */
            any |= ir_pass_reassoc(fn);             /* (x op c1) op c2 -> x op (c1 op c2) */
            any |= ir_pass_licm(fn);                /* LICM con dominators reales */
            any |= ir_pass_dead_alloc_elim(fn);
            any |= ir_pass_dce(fn);

            if (level >= OptLevel::O2) {
                // O2: plegado de constantes + bloques inalcanzables + TCO.
                any |= ir_pass_const_fold(fn);
                any |= ir_pass_unreachable(fn);
                any |= ir_pass_tailcall(fn);
                // Inline de header trivial de loop -> habilita decjnz fusion.
                any |= ir_pass_inline_loop_header(fn);
                // Dead store elimination: limpia STOREs muertos consecutivos.
                any |= ir_pass_dse(fn);
                // Global const CSE solamente (safer than full CSE).
                // El full CSE local tiene bugs sutiles con LOAD/STORE alias
                // que necesitan alias analysis (deferido a O3+).
                // Global const dedup via DIRECT rewrite (no MOV+copy_prop
                // intermedio).  La version vieja con MOV dejaba is_const
                // stale en fn.values causando fallos no-deterministicos en
                // test 110 (smart pointers SRET).  Esta version sustituye
                // operandos directamente y elimina las CONSTs duplicadas.
                any |= ir_pass_const_cse_entry(fn);
                // CSE local de aritmetica pura (ADD/SUB/MUL/etc.) -- dedupea
                // `add.ptr this, off` triplicados en getters/setters.  Tiene
                // invalidacion correcta para LOAD via side-effects.  Habilita
                // store-to-load forwarding al unificar punteros equivalentes.
                any |= ir_pass_cse(fn);
                // Load Narrow: elide SEXT redundante tras LOAD i8/i16/i32
                // cuando todos los usos son arith narrow-safe (ADD/SUB/MUL/
                // AND/OR/XOR) + STORE/RET del mismo ancho.  Ahorra 3 instr VM
                // por LOAD elidido.  Bench struct_field: ~270M instr ahorradas.
                any |= ir_pass_load_narrow(fn);
                // Segunda ronda de DCE tras plegado/TCO/loop header inline/CSE.
                any |= ir_pass_dce(fn);
            }

            if (level >= OptLevel::O3) {
                // O3: pasadas mas costosas (GVN, scheduling, etc.) -- TBD.
            }
        }

        /* Devirt + inline @ O2 al final de cada iteracion del fix-point.
         * Importante hacerlo despues de las per-function passes para que
         * la inline pass vea las callees OPTIMIZADAS (e.g. Counter.inc
         * con 6 instrs en vez de 12), aprobando inline bajo el threshold. */
        if (level >= OptLevel::O2) {
            if (ir_pass_devirt_monomorphic(mod)) any = true;
            if (ir_pass_inline(mod))             any = true;

            /* Phase C2.13: Scalar Replacement de objetos GC no-escapantes.
             * Corre DESPUES del inline (que junta el alloc + los field-access
             * en la misma fn).  Sus reescrituras (loads -> trunc/mov/const)
             * las limpia el const_fold/dce de la siguiente iteracion del
             * fix-point; el alloc GC desaparece por completo.
             * Skippable via VESTA_NO_ESCAPE_SCALAR=1 para A/B testing. */
            {
                const char *skip = std::getenv("VESTA_NO_ESCAPE_SCALAR");
                const bool do_sr = !(skip && skip[0] != '\0' && skip[0] != '0');
                if (do_sr) {
                    for (auto &fn : mod.functions) {
                        if (fn.is_native) continue;
                        if (ir_pass_scalar_replace_gc(fn, mod)) any = true;
                    }
                }
            }
        }

        if (!any) break; // punto fijo alcanzado
    }

    /* Phase C2.13: DETECCION (log-only) de objetos GC no-escapantes.  Corre
     * tras el fix-point (con el IR ya inlineado + optimizado, que es donde el
     * escape es visible: el alloc + los field-access estan en la misma fn).
     * No transforma el IR; solo loguea bajo VESTA_ESCAPE_DEBUG. */
    if (level >= OptLevel::O2) {
        for (auto &fn : mod.functions) {
            if (fn.is_native) continue;
            ir_pass_escape_detect_gc(fn);
        }
    }

    /* Final pass: list scheduling para ILP.  Una sola pasada despues del
     * fix-point porque el reordenamiento NO produce mas oportunidades de
     * optimizacion (es semanticamente neutro -- mismo DAG, distinto orden).
     * Solo aplica @ O2+ por seguridad. */
    if (level >= OptLevel::O2) {
        for (auto &fn : mod.functions) {
            if (fn.is_native) continue;
            ir_pass_schedule(fn);
        }
    }
}

// Set global de helpers @c __new_<X> marcados como puros por el frontend.
// El frontend lo invoca cuando detecta un ctor trivial (sin @c callvirt al
// ctor user-defined); el DCE puede eliminar la llamada si el handle no se
// usa.  Coste: lookup O(1) amortizado en una sola posicion del pipeline.
static std::unordered_set<std::string> g_pure_new_helpers;

void register_pure_new_helper(const std::string &fn_name) {
    g_pure_new_helpers.insert(fn_name);
}

bool is_pure_new_helper(const std::string &fn_name) {
    return g_pure_new_helpers.count(fn_name) != 0;
}

} // namespace ir
