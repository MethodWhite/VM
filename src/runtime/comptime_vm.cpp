/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file comptime_vm.cpp
 * @brief Implementacion del scaffolding de @c ComptimeRuntime (Phase  ).
 *
 * En este sprint la clase es funcional pero la ejecucion via VM esta
 * stub-eada -- el @c try_invoke siempre retorna @c false (caller
 * fallback al AST evaluator).  Esto permite construir, registrar
 * macros, contar invocaciones, sin necesitar la VM operativa.
 *
 *   implementara la VM real (bootstrap del runtime::VM con un
 * .velb in-memory que contenga los `__macro_*` lowered).
 */

#include "vex/comptime_vm.h"

/* Headers runtime full: estos pueden cascade-incluir openssl/capstone
 * pero solo se compilan en este TU (no se filtran al header publico
 * de @c ComptimeRuntime gracias al pimpl). */
#include "runtime/manager_runtime.h"
#include "runtime/runtime.h"
#include "runtime/string_runtime.h"  /*   : make_string_flat */
#include "loader/loader.h"
#include "distrib/dist_runtime.h"   /* dtor de VM destruye DistRuntime via unique_ptr */
#include "jit/auto_jit.h"           /*   : eager-compile macros */
#include "jit/jit_compiler.h"       /* CompileResult */

#include <chrono>
#include <cstdio>     //   : snprintf para hex-encode del hash
#include <cstring>
#include <thread>
#include <vector>

namespace vex {

    /**
     * @brief Pimpl interno de @c ComptimeRuntime.  Contiene todo el
     * estado runtime necesario para ejecutar @Macros lowered al IR.
     *
     *   (este sprint): bootstrap del runtime::VM + ManageVM in-process,
     * sin file I/O.  Lazy-init en el primer @c try_invoke().
     *
     *   (proximo): @c load_macros_from_bytes() cargara el bytecode
     * de los @Macros via @c Loader::load_executable(vm, bytes) y
     * registrara los entry PCs reales por nombre.
     *
     * @note Una sola VM cross-macro per-compile: no se reinstancia.
     */
    struct ComptimeVmImpl {
        /**
         * @brief Manager-VM minimo (listener=nullptr -> modo local sin
         * red).  Owner del Loader, que se usa para load_executable
         * desde un buffer in-memory de bytes.
         */
        runtime::ManageVM mgr;

        /**
         * @brief VM dedicada a ejecucion comptime.  1 scheduler (single
         * threaded) suficiente para evaluacion sequential de macros.
         */
        runtime::VM vm;

        /**
         * @brief Bytecode de los @Macros cargados.  Sera populado en
         *   con los bytes resultantes del linker sobre las
         * IrFunction __macro_*.  En   queda vacio (no se carga nada
         * todavia, solo se valida que el VM ciclo de vida funciona).
         */
        std::vector<uint8_t> macros_bytecode;

        /**
         * @brief Proceso recien creado por @c Loader::load_executable.
         * Reusado entre invocaciones ( ): tras HALT, lo reactivamos
         * con make_ready + nuevo entry_pc + nuevos args.  Permite N
         * invocaciones por @c ComptimeRuntime sin overhead de spawning
         * fresco ni file I/O.
         */
        runtime::ProcessVM *proc = nullptr;

        /**
         * @brief PID global del proceso, requerido por @c make_ready.
         */
        ::GlobalPID proc_pid{};

        /**
         * @brief RSP inicial (= stack base - 8) populado por Loader.
         *   lo re-aplica al inicio de cada call para que el stack
         * empiece limpio con la direccion del sentinel HLT pre-pushed.
         */
        uint64_t initial_rsp = 0;

        /**
         * @brief VA donde Loader escribio los bytes HLT (0x00 0x03) que
         * actuan como direccion de retorno final del macro.    lo
         * re-pushea en cada call para garantizar HLT clean al ret.
         */
        uint64_t hlt_sentinel_va = 0;

        /**
         * @brief Flag: ¿hemos llamado @c vm.start() ya?  Lazy: la primera
         * @c invoke_simple_macro lo activa.  Schedulers se quedan vivos
         * mientras el proceso este en estado HALT (no DEAD), permitiendo
         * re-activarlo con make_ready en subsequent calls.
         */
        bool vm_started = false;

        /**
         * @brief   : cache `entry_pc -> jit_code_ptr`.  Populado
         * por @c load_macros_from_bytes con eager-compile de cada macro
         * registrado.  Si la compile JIT falla (unsupported IR ops, etc.)
         * la entrada queda en @c nullptr y la macro corre via interp.
         *
         * En @c invoke_simple_macro consultamos este mapa con el entry_pc
         * del macro a invocar; si hay fn_ptr, lo asignamos a
         * @c proc->jit_entry_fn antes de @c vm.start() asi el scheduler
         * salta directo al codigo nativo (sin overhead de dispatch interp).
         */
        std::unordered_map<uint64_t, void *> jit_code_by_pc;

        /**
         * @brief    (A: Lazy GC): contador de invocaciones desde
         * el ultimo @c major_gc().  El GC sweep se ejecuta solo cuando
         * supera el threshold (default 64), no en CADA invocacion.
         *
         * Costo amortizado: ~10us / 64 calls = ~0.15us por call vs ~10us
         * en el modelo per-call.  Para macros invocados muchas veces
         * (caso de cache memo o composicion), reduce overhead GC en
         * ~98%.  El peak memory queda bounded a ~64 * 128 B = 8 KB lo
         * cual es perfectamente aceptable.
         *
         * Override via env var @c VESTA_ GC_INTERVAL.  Valor 0 = GC
         * cada call (modo original); valor grande = menos GCs pero
         * mayor peak memory.
         */
        uint64_t invocations_since_gc = 0;
        uint64_t gc_interval          = 64;   /* Default threshold. */

        /**
         * @brief Construye la VM con configuracion minimal: 1 scheduler,
         * sin TCP listener.  Maneja la cadena de inicializacion en el
         * orden correcto (mgr antes que vm).
         *
         * @param mgr_id   ID arbitrario para el manager (cualquier valor).
         * @param vm_id    ID arbitrario para la VM.
         * @param n_sched  Numero de schedulers (1 = single threaded, lo
         *                 mas ligero posible).
         */
        ComptimeVmImpl()
            : mgr(/*listener=*/nullptr, /*id=*/0xC03171EULL),
              vm(mgr, /*id_vm=*/0xC03171EULL, /*num_schedulers=*/1) {
            /*   : configurar GC interval desde env var.
             * Default = 64 (good tradeoff entre peak memory y overhead). */
            if (const char *env = std::getenv("VESTA_ GC_INTERVAL")) {
                char *end = nullptr;
                const unsigned long v = std::strtoul(env, &end, 10);
                if (end != env) gc_interval = v;
            }
        }
    };

    ComptimeRuntime::ComptimeRuntime() noexcept = default;

    ComptimeRuntime::~ComptimeRuntime() = default;

    void ComptimeRuntime::register_macro(const std::string &macro_name,
                                          uint64_t entry_pc) {
        macro_entry_pc_[macro_name] = entry_pc;
    }

    bool ComptimeRuntime::try_invoke(const std::string &macro_name) noexcept {
        /*   : VM lifecycle bootstrap funcional pero la ejecucion
         * real todavia no esta wired (  anyade load + marshalling +
         * call).  El caller debe seguir interpretando @c false como
         * "fallback al AST evaluator".
         *
         * Despues de   esto sera:
         *   1. Lookup en macro_entry_pc_.  Si no esta -> false.
         *   2. ensure_vm_initialized() -- lazy bootstrap.
         *   3. Verificar que el bytecode esta cargado.
         *   4. Marshalling args -> VM regs.
         *   5. Spawn process en entry_pc, run hasta retorno.
         *   6. Read R0 -> out_result.
         *   7. Cache si @Pure.
         */
        (void)macro_name;
        return false;
    }

    void ComptimeRuntime::ensure_vm_initialized() noexcept {
        if (impl_) return;
        try {
            impl_.reset(new ComptimeVmImpl());
        } catch (...) {
            /* Bootstrap fallo (e.g., out-of-memory creando Loader).
             * Dejamos impl_ en nullptr para que try_invoke devuelva
             * false y el caller use el AST evaluator. */
            impl_.reset();
        }
    }

    bool ComptimeRuntime::invoke_simple_macro(
            const std::string &macro_name,
            const std::vector<uint64_t> &args,
            uint64_t &out_r0) noexcept {
        out_r0 = 0;
        if (args.size() > 12) return false;        /* CALLVM max */
        if (!impl_ || !impl_->proc) return false;  /* No VM cargado. */
        auto it = macro_entry_pc_.find(macro_name);
        if (it == macro_entry_pc_.end()) return false;
        const uint64_t entry_pc = it->second;

        try {
            runtime::ProcessVM *proc = impl_->proc;

            /*  : reset estado del proceso para soportar multi-call.
             * Tras un HALT previo, el RSP puede estar desplazado y el
             * stack puede tener data residual.  Restauramos al estado
             * inicial que setup load_executable, re-pusheando el
             * sentinel HLT como direccion de retorno final del macro. */
            proc->registers.stack_pointer.qword(impl_->initial_rsp);
            proc->registers.base_pointer.qword(impl_->initial_rsp);
            proc->vm_mem.vm_to_host_memcpy(
                impl_->initial_rsp,
                &impl_->hlt_sentinel_va,
                sizeof(impl_->hlt_sentinel_va));
            proc->stack_high      = impl_->initial_rsp;
            proc->stack_low_water = impl_->initial_rsp;

            /* Set RIP + args (CALLVM convention). */
            proc->registers.rip.qword(entry_pc);
            for (size_t i = 0; i < args.size(); ++i) {
                proc->registers.regs[i + 1].qword(args[i]);
            }
            proc->registers.regs[15].qword(args.size());

            /*   multi-call pattern -- aprovecha el single-scheduler
             * sync bypass de vm.start() (runtime.cpp:140-151): con 1
             * scheduler + non-persistent, start() ejecuta run_loop
             * SINCRONAMENTE en el thread llamante, bloqueando hasta
             * que el proceso HALT.  start() es idempotente: cada call
             * vm_running=true al entrar, =false al salir.  No necesita
             * vm.wait().
             *
             * Beneficios:
             *   - Cero coste de thread spawn (mismo thread).
             *   - Multi-call funciona out-of-the-box (cada call es una
             *     sesion completa start->halt->return).
             *   - Determinista: no hay race conditions inter-call.
             */
            /*   : si la macro fue eager-compilada via JIT,
             * setear @c jit_entry_fn antes del start.  El scheduler
             * salta directo al codigo nativo (skip interp dispatch).
             * Cero impacto si la macro no compilo (jit_entry_fn=null). */
            auto jit_it = impl_->jit_code_by_pc.find(entry_pc);
            if (jit_it != impl_->jit_code_by_pc.end() && jit_it->second) {
                proc->jit_entry_fn = jit_it->second;
            }
            impl_->vm.make_ready(impl_->proc_pid);
            impl_->vm.start();
            impl_->vm_started = true;

            /* Lee R0 del proceso completado. */
            out_r0 = proc->registers.regs[0].qword();
            ++call_count_;
            return true;
        } catch (...) {
            return false;
        }
    }

    /**
     * @brief    -- construye la clave del cache memoization.
     *
     * Hash FNV-1a 64-bit sobre @c args concatenado al @c macro_name con
     * separador.  Suficientemente determinista para que dos call sites
     * con misma firma colisionen en la misma entrada.  No criptografica
     * pero rapida (~2 ns por arg).
     */
    static std::string build_memo_key(const std::string &macro_name,
                                       const std::vector<uint64_t> &args) {
        constexpr uint64_t FNV_OFFSET = 14695981039346656037ULL;
        constexpr uint64_t FNV_PRIME  =     1099511628211ULL;
        uint64_t h = FNV_OFFSET;
        for (char c : macro_name) {
            h ^= static_cast<uint8_t>(c);
            h *= FNV_PRIME;
        }
        h ^= 0xFF;   /* separador entre name y args. */
        h *= FNV_PRIME;
        for (uint64_t w : args) {
            for (int b = 0; b < 8; ++b) {
                h ^= (w >> (b * 8)) & 0xFFu;
                h *= FNV_PRIME;
            }
        }
        /* Hex-encode el hash para que sea un string utilizable como key
         * en el unordered_map.  Tambien anyadimos el name para diag. */
        char buf[17];
        std::snprintf(buf, sizeof(buf), "%016llx",
                       static_cast<unsigned long long>(h));
        std::string key;
        key.reserve(macro_name.size() + 17);
        key.assign(macro_name);
        key.push_back(':');
        key.append(buf);
        return key;
    }

    bool ComptimeRuntime::marshal_string(const std::string &s,
                                          uint64_t &out_handle) noexcept {
        out_handle = 0;
        if (!impl_ || !impl_->proc) return false;
        try {
            /* Delegar al helper publico del runtime: mismo path que la
             * instruccion STRMAKE usa internamente (alloc_pinned + populacion
             * del header + hash precomputado).  El handle resultante
             * sobrevive cross-call y se puede pasar como uint64 en R1..R12. */
            const auto *data =
                reinterpret_cast<const uint8_t *>(s.data());
            const auto h = runtime::make_string_flat(
                impl_->proc,
                data,
                static_cast<uint32_t>(s.size()),
                /*length=UINT32_MAX -> auto*/ UINT32_MAX,
                loader::StringEncoding::UTF8);
            if (h == gc::GC_NULL_HANDLE) return false;
            out_handle = static_cast<uint64_t>(h);
            return true;
        } catch (...) {
            return false;
        }
    }

    bool ComptimeRuntime::invoke_string_macro_memoized(
            const std::string &macro_name,
            const std::vector<uint64_t> &args,
            std::string &out_str,
            bool pure) noexcept {
        if (!pure) {
            /* No-cache path: redirige a la version basica. */
            return invoke_string_macro(macro_name, args, out_str);
        }
        try {
            const std::string key = build_memo_key(macro_name, args);
            auto it = memo_cache_.find(key);
            if (it != memo_cache_.end()) {
                out_str = it->second;
                ++memo_hit_count_;
                return true;
            }
            const bool ok = invoke_string_macro(macro_name, args, out_str);
            if (ok) {
                memo_cache_.emplace(key, out_str);
                ++memo_miss_count_;
            }
            return ok;
        } catch (...) {
            return false;
        }
    }

    bool ComptimeRuntime::invoke_string_macro(
            const std::string &macro_name,
            const std::vector<uint64_t> &args,
            std::string &out_str) noexcept {
        out_str.clear();
        /*   : capturamos old_used pre-call solo si trace activo. */
        size_t old_used_before = 0;
        const bool gc_trace_active = []() {
            const char *t = std::getenv("VESTA_ GC_TRACE");
            return t && t[0] == '1';
        }();
        if (gc_trace_active && impl_ && impl_->proc) {
            old_used_before = impl_->proc->gc_heap.old_used();
        }
        uint64_t r0 = 0;
        if (!invoke_simple_macro(macro_name, args, r0)) return false;
        if (r0 == 0) return false;
        if (!impl_ || !impl_->proc) return false;

        try {
            /* Deref handle -> payload host_ptr.  El payload es el
             * StringObject completo (sin GcHeader). */
            const auto handle = static_cast<gc::GcHandle>(r0);
            uint8_t *payload = impl_->proc->gc_heap.deref(handle);
            if (!payload) return false;

            /* Layout StringObject (vease include/vesta_rt/abi.h):
             *   +0   ObjectHeader (24 bytes)
             *   +24  encoding (u8)
             *   +25  kind     (u8)
             *   +28  length   (u32, code-points)
             *   +32  byte_len (u32, bytes en data[])
             *   +36  hash     (u32)
             *   +40  data[]   (byte_len bytes contiguos)
             */
            uint32_t byte_len = 0;
            std::memcpy(&byte_len, payload + 32, sizeof(uint32_t));
            if (byte_len > (16u << 20)) return false;   /* cap defensivo 16 MB */

            out_str.assign(reinterpret_cast<const char *>(payload + 40),
                            byte_len);

            /*    +   (A: Lazy GC):  Los strings intermedios
             * (args marshalados, returns ya extraidos) viven en OldGen
             * pinned pero son colectables.  Sin esto crecen linealmente.
             *
             *   mejora  : en lugar de major_gc en CADA call,
             * acumulamos hasta @c gc_interval invocaciones y barremos
             * en batch.  Cost amortizado ~10us / 64 calls = 0.15us/call
             * (vs 10us/call original) -> 98% reduccion overhead GC.
             *
             * Peak memory queda bounded a ~gc_interval * 128 B (~8KB
             * con default 64), aceptable.  Override via
             * @c VESTA_ GC_INTERVAL.
             *
             * Limpiar R0..R15 SIEMPRE (no en batch) -- esos handles
             * deben morir inmediatamente para que el siguiente call
             * empiece limpio.  Solo el major_gc se difiere. */
            for (int i = 0; i < 16; ++i) {
                impl_->proc->registers.regs[i].qword(0);
            }
            ++impl_->invocations_since_gc;
            if (impl_->gc_interval == 0
             || impl_->invocations_since_gc >= impl_->gc_interval) {
                impl_->proc->gc_heap.major_gc();
                impl_->invocations_since_gc = 0;
            }

            if (gc_trace_active) {
                const size_t after = impl_->proc->gc_heap.old_used();
                std::cerr << "[mc-gc] " << macro_name
                          << " old_used: " << old_used_before
                          << " -> " << after
                          << " (post-gc)\n";
            }
            return true;
        } catch (...) {
            return false;
        }
    }

    void ComptimeRuntime::record_expectation(const std::string &macro_name,
                                              std::vector<uint64_t> args,
                                              std::string expected_str,
                                              std::string src_loc) {
        /*   : el TypeChecker llama aqui justo despues de evaluar
         * un @Macro via el AST evaluator.  Guardamos la tripleta
         * (canonical_name, args, expected) para validacion posterior
         * via VM.  Cero overhead durante type-check: solo push_back.
         *
         * Nota: el nombre del macro CANONICO en el IR es
         * "__macro_<original>"; el TypeChecker conoce solo "<original>"
         * (el nombre escrito por el usuario).  Aqui aceptamos cualquiera
         * de los dos: el shadow_validate prueba ambos al lookup. */
        ShadowExpectation e;
        e.macro_name   = macro_name;
        e.args         = std::move(args);
        e.expected_str = std::move(expected_str);
        e.src_loc      = std::move(src_loc);
        shadow_expectations_.push_back(std::move(e));
    }

    size_t ComptimeRuntime::shadow_validate(
            std::vector<ShadowMismatch> &report) noexcept {
        size_t mismatches = 0;
        for (const auto &exp : shadow_expectations_) {
            ShadowMismatch m;
            m.macro_name = exp.macro_name;
            m.src_loc    = exp.src_loc;
            m.expected   = exp.expected_str;

            /* Resolver nombre canonico: aceptamos tanto "<original>"
             * como "__macro_<original>".  Lookup en macro_entry_pc_. */
            std::string canonical = exp.macro_name;
            if (macro_entry_pc_.find(canonical) == macro_entry_pc_.end()) {
                const std::string alt = "__macro_" + exp.macro_name;
                if (macro_entry_pc_.find(alt) != macro_entry_pc_.end()) {
                    canonical = alt;
                }
            }

            std::string got;
            const bool ok = invoke_string_macro(canonical, exp.args, got);
            if (!ok) {
                m.match  = false;
                m.reason = "invoke_string_macro fallo (macro '" + canonical +
                           "' no resolvio o el VM crasheo)";
                ++mismatches;
            } else if (got != exp.expected_str) {
                m.match  = false;
                m.got    = got;
                m.reason = "discrepancia AST vs VM (longitudes " +
                           std::to_string(exp.expected_str.size()) +
                           " vs " + std::to_string(got.size()) + ")";
                ++mismatches;
            } else {
                m.match  = true;
                m.got    = got;
                m.reason = "ok";
            }
            report.push_back(std::move(m));
        }
        return mismatches;
    }

    bool ComptimeRuntime::load_macros_from_bytes(
            std::vector<uint8_t> bytecode) noexcept {
        if (bytecode.empty()) return false;
        ensure_vm_initialized();
        if (!impl_) return false;
        try {
            /* Cargar el .velb completo desde memoria.  El Loader parsea
             * el header, valida magic/version, mapea las secciones al
             * vm_mem de un nuevo ProcessVM y devuelve el handle.  Cero
             * file I/O: todo se hace sobre el buffer ya en memoria. */
            runtime::ProcessVM *proc =
                impl_->mgr.loader.load_executable(impl_->vm, std::move(bytecode));
            if (!proc) return false;
            impl_->proc     = proc;
            impl_->proc_pid = proc->pid;

            /*  : capturar initial_rsp + hlt_sentinel_va para poder
             * reusar el proceso entre llamadas.  Loader setupó la pila
             * con stack_base_main = 0x10000000 + (local_pid % 0x1000) * 0x100000,
             * con la VA del sentinel HLT a stack_base + 16 y el RSP
             * apuntando a stack_base - 8 (donde se escribio la sentinel VA
             * como direccion de retorno final). */
            impl_->initial_rsp     = proc->registers.stack_pointer.qword();
            const uint64_t stack_base_main =
                0x10000000ULL + (proc->pid.local_pid % 0x1000ULL) * 0x100000ULL;
            impl_->hlt_sentinel_va = stack_base_main + 16;

            /*   : resolver entry PC de cada `__macro_<X>` desde
             * el symbol_table del Executable recien cargado.  Itera la
             * tabla VSYM (populada por linker en el .velb v3), filtra
             * por prefijo `code.__macro_` y registra cada uno en el
             * mapa @c macro_entry_pc_ con la direccion real. */
            const auto &execs = impl_->mgr.loader.executables;
            if (execs.empty()) return true;
            const auto &exe = execs.back();   /* el recien cargado */
            const std::string PREFIX = "code.__macro_";
            for (const auto &kv : exe->symbol_table) {
                if (kv.first.size() <= 5) continue;
                if (kv.first.compare(0, PREFIX.size(), PREFIX) != 0) continue;
                /* kv.first = "code.__macro_my_fn" -> "__macro_my_fn" tras
                 * descartar el prefijo "code." (5 chars). */
                const std::string clean = kv.first.substr(5);
                macro_entry_pc_[clean] = kv.second;
            }

            /*   : eager-compile cada macro via JIT.  OPT-IN via
             * @c VESTA_ JIT=1.  Razon: eager_compile_function tarda
             * ~5-15ms por macro (segun size del IR + Keystone encode).
             * Para macros pequenos invocados pocas veces, el costo del
             * compile excede el ahorro en runtime.  Para builds con
             * macros pesados o invocaciones repetidas (e.g. metaprog
             * heavy), el JIT da 10-50x speedup en la ejecucion de la
             * macro.  El user decide con el env var.
             *
             * TODO   futuro: lazy compile (contador de invocaciones)
             * para que el JIT solo dispare cuando el macro se justifique.
             * Eso elimina la decision manual y da lo mejor de ambos. */
            const char *mc_jit_env = std::getenv("VESTA_ JIT");
            const bool  mc_jit_on  = (mc_jit_env && mc_jit_env[0] == '1');
            if (mc_jit_on) {
                const uint32_t saved_threshold = jit::g_jit_threshold;
                if (saved_threshold == UINT32_MAX) {
                    jit::g_jit_threshold = 1;
                }
                try {
                    for (const auto &kv : macro_entry_pc_) {
                        const std::string &macro_name = kv.first;
                        const uint64_t entry_pc       = kv.second;
                        auto fnit = exe->ir_lookup.find(macro_name);
                        if (fnit == exe->ir_lookup.end()) continue;
                        if (fnit->second >= exe->ir_functions.size()) continue;
                        const ir::IrFunction &ir_fn = exe->ir_functions[fnit->second];
                        try {
                            jit::CompileResult res = jit::eager_compile_function(
                                ir_fn, &exe->ir_lookup, &exe->ir_functions,
                                &exe->symbol_table);
                            if (res.fn) {
                                impl_->jit_code_by_pc[entry_pc] =
                                    reinterpret_cast<void *>(res.fn);
                            }
                        } catch (...) {}
                    }
                } catch (...) {}
                jit::g_jit_threshold = saved_threshold;
            }
            return true;
        } catch (...) {
            return false;
        }
    }

} // namespace vex
