/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/auto_jit.cpp
 * @brief Implementacion del auto-JIT trigger.
 *
 * Singleton lazy del @c JitCompiler (con su @c CodeCache + @c RuntimeEntries).
 * La construccion se difiere a la primera vez que se compila un metodo, asi
 * programas que jamas alcanzan el threshold no pagan el coste de inicializar
 * el JIT subsystem (10-50 KB de VM reservada para code cache).
 */

#include "jit/auto_jit.h"
#include "jit/jit_compiler.h"
#include "jit/code_cache.h"
#include "jit/runtime_entries.h"
#include "vesta_rt/public.h"
#include "ffi/virtual_lib_registry.h"  // Sprint JIT-cross-fn: virtual fn lookup
#include "loader/loader.h"
#include "loader/oop_types.h"
#include "runtime/proceso_runtime.h"
#include "runtime/runtime.h"
#include "runtime/scheduler.h"
#include "ir/ssa_ir.h"
#include <cstring>

#include <capstone/capstone.h>

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <string>

namespace jit {

    /* ===================================================================== */
    /* Threshold global                                                       */
    /* ===================================================================== */

    /**
     * @brief Default: UINT32_MAX (JIT off).  Override via @c set_jit_threshold
     *        o env var @c VESTA_JIT_THRESHOLD.
     */
    uint32_t g_jit_threshold = UINT32_MAX;

    /**
     * @brief Warning toggleable + counters de auditoria.
     */
    bool     g_jit_warn_unsupported  = false;
    bool     g_jit_disasm            = false;
    /* Sprint string-perf-6 (2026-06-02): emit del MIPS counter per-block
     * en el JIT.  Default OFF porque cuesta ~3ns por block ejecutado;
     * en hot loops con muchos bloques (e.g. bench_branch_unpredict con
     * 13 blocks/iter) el overhead puede llegar al 50%.  Solo se activa
     * cuando el usuario pide --stats o VESTA_JIT_STATS=1. */
    bool     g_jit_emit_instr_counter = false;
    uint64_t g_jit_compiled_count    = 0;
    uint64_t g_jit_unsupported_count = 0;
    uint64_t g_jit_no_ir_count       = 0;

    /* Lazy init del threshold desde env var en la primera consulta. */
    namespace {
        std::once_flag g_env_init_flag;
        void init_threshold_from_env() {
            const char *env = std::getenv("VESTA_JIT_THRESHOLD");
            if (env && env[0] != '\0') {
                char *end = nullptr;
                const unsigned long v = std::strtoul(env, &end, 10);
                if (end != env && v <= UINT32_MAX) {
                    g_jit_threshold = static_cast<uint32_t>(v);
                }
            }
            /* Tambien leer VESTA_JIT_WARN_UNSUPPORTED para activar el
             * sistema de warnings de IR ops no soportadas. */
            const char *warn = std::getenv("VESTA_JIT_WARN_UNSUPPORTED");
            if (warn && warn[0] != '\0' && warn[0] != '0') {
                g_jit_warn_unsupported = true;
            }
            /* VESTA_JIT_DISASM=1 -> dumpear los bytes generados + disasm. */
            const char *dis = std::getenv("VESTA_JIT_DISASM");
            if (dis && dis[0] != '\0' && dis[0] != '0') {
                g_jit_disasm = true;
            }
            /* VESTA_JIT_STATS=1 -> emit del MIPS counter per-block (opt-in
             * porque cuesta ~3ns/block; en hot loops el overhead es alto). */
            const char *stats = std::getenv("VESTA_JIT_STATS");
            if (stats && stats[0] != '\0' && stats[0] != '0') {
                g_jit_emit_instr_counter = true;
            }
        }

        /* Singleton lazy del JIT subsystem (compiler + cache + entries). */
        std::once_flag    g_jit_init_flag;
        CodeCache         *g_code_cache    = nullptr;
        RuntimeEntries    *g_runtime_entries = nullptr;
        JitCompiler       *g_compiler      = nullptr;
        std::mutex         g_compile_mtx;

        void init_jit_subsystem() {
            g_code_cache      = new CodeCache();
            g_runtime_entries = new RuntimeEntries();
            g_runtime_entries->resolve();
            g_compiler        = new JitCompiler(*g_code_cache, *g_runtime_entries);
        }
    }

    /* Sprint B.1: expose el CodeCache global al runtime (native_callback.cpp).
     * Bypass el namespace anonimo de los singletons; el caller solo recibe
     * un puntero const-correcto. */
    CodeCache *get_or_init_code_cache() noexcept {
        std::call_once(g_jit_init_flag, init_jit_subsystem);
        return g_code_cache;
    }

    namespace {

        /* Cache global de compilaciones eager: nombre -> ptr al codigo nativo.
         * Sentinela IN_PROGRESS (1) marca compilation en curso para detectar
         * ciclos (mutua recursion).  Nullptr explicito (0) significa "no
         * compilable" (cached failure, evita reintentos).
         *
         * Vive bajo g_compile_mtx para safety multi-thread aunque por ahora
         * solo el load_executable invoca eager-compile (1 thread).  Compartido
         * tambien con maybe_compile_method (D.3-C+ auto-JIT trigger). */
        constexpr uint64_t EAGER_IN_PROGRESS = 1;
        std::unordered_map<std::string, uint64_t> g_eager_cache;
    } // namespace anonymous

    void set_jit_threshold(uint32_t threshold) noexcept {
        g_jit_threshold = threshold;
    }

    /* Forzar lectura del env var VESTA_JIT_THRESHOLD (+ warn/disasm) AHORA
     * en lugar de esperar al primer maybe_compile_*.  Util para que main.cpp
     * llame al inicio y la eager-compile pass del Loader vea el threshold
     * correcto.
     *
     * Idempotente: usa el mismo std::call_once que el path lazy.  Si ya
     * corrio (otro thread o lazy fire previo), no-op. */
    void init_threshold_from_env_now() noexcept {
        std::call_once(g_env_init_flag, init_threshold_from_env);
    }

    /* ===================================================================== */
    /* maybe_compile_method                                                   */
    /* ===================================================================== */

    void maybe_compile_method(runtime::ProcessVM *vm,
                              loader::MethodInfo *method) noexcept {
        /* Init lazy de threshold desde env (1 vez por proceso). */
        std::call_once(g_env_init_flag, init_threshold_from_env);

        /* Fast path: si JIT esta off (threshold = UINT32_MAX) o el
         * metodo ya fue JIT-compilado o el counter no llego al
         * threshold, retornar inmediatamente.  Coste: 1 cmp + 1 cmp =
         * ~1 ns (branch predicted). */
        if (g_jit_threshold == UINT32_MAX) return;
        if (method->jit_code != nullptr)   return;
        if (method->invocation_count < g_jit_threshold) return;

        /* Slow path: el counter cruzo el threshold y el metodo NO
         * tiene jit_code aun.  Intentar compilar. */

        /* Construir lookup key: "<ClassName>__<methodName>".
         * stringx es un struct {uint8_t* data, uint32_t size}; lo
         * convertimos a string via reinterpret_cast (los bytes son
         * UTF-8/ASCII puro segun el frontend). */
        auto stringx_to_str = [](const loader::stringx &sx) -> std::string {
            if (!sx.data || sx.size == 0) return {};
            return std::string(reinterpret_cast<const char *>(sx.data), sx.size);
        };
        std::string key;
        if (method->owner_class) {
            const std::string cls = stringx_to_str(method->owner_class->name);
            if (!cls.empty()) {
                key += cls;
                key += "__";
            }
        }
        key += stringx_to_str(method->name);

        /* Buscar el IrFunction en los executables del Loader.
         * El Loader vive en VM (compartido entre procesos).  Iteramos
         * sus executables y consultamos su ir_lookup. */
        const ir::IrFunction *ir_fn = nullptr;
        const std::unordered_map<std::string, uint64_t> *owning_symtab = nullptr;
        const std::unordered_map<std::string, size_t> *owning_lookup  = nullptr;
        const std::vector<ir::IrFunction> *owning_funcs                = nullptr;
        runtime::VM &owning_vm = vm->scheduler.vm_reference;

        /* Construir lista de claves alternativas a probar (en orden de
         * preferencia).  Los constructores en Vex se mangleean como
         * "ClassName__ctor" en IR, pero MethodInfo::name == ClassName
         * para constructores -> el key naive "ClassName__ClassName"
         * no funciona.  Detectamos el caso y agregamos fallback. */
        std::vector<std::string> candidate_keys{key};
        if (method->owner_class) {
            const std::string cls = stringx_to_str(method->owner_class->name);
            const std::string mtd = stringx_to_str(method->name);
            if (!cls.empty() && cls == mtd) {
                /* Es ctor: probar "ClassName__ctor". */
                candidate_keys.push_back(cls + "__ctor");
            }
        }

        for (const auto &exe : owning_vm.loader_public.executables) {
            for (const auto &k : candidate_keys) {
                auto it = exe->ir_lookup.find(k);
                if (it != exe->ir_lookup.end()
                 && it->second < exe->ir_functions.size()) {
                    ir_fn = &exe->ir_functions[it->second];
                    owning_symtab = &exe->symbol_table;
                    owning_lookup = &exe->ir_lookup;
                    owning_funcs  = &exe->ir_functions;
                    break;
                }
            }
            if (ir_fn) break;
        }

        if (!ir_fn) {
            /* Sin IR disponible (e.g. .velb v2 sin seccion @ir, o
             * fn no encontrada).  No intentamos otra vez en
             * subsequent calls -- marcamos como "fallido" via
             * increment del counter mas alla del threshold para que
             * el check `invocation_count < threshold` no vuelva a
             * disparar.  Workaround simple: setear counter a
             * UINT32_MAX. */
            ++g_jit_no_ir_count;
            if (g_jit_warn_unsupported) {
                std::fprintf(stderr,
                    "[jit] no IR encontrado para metodo '%s' (build sin --vex o .velb v2)\n",
                    key.c_str());
            }
            method->invocation_count = UINT32_MAX;
            return;
        }

        /* Init lazy del JIT subsystem. */
        std::call_once(g_jit_init_flag, init_jit_subsystem);

        /* Compilar bajo mutex (el JitRegistry necesita acceso exclusivo
         * + el code cache puede tener races en multi-thread). */
        std::lock_guard<std::mutex> lk(g_compile_mtx);

        /* Doble-check tras adquirir el lock (otro thread podria haber
         * compilado mientras nosotros esperabamos). */
        if (method->jit_code != nullptr) return;

        /* Phase: compile_with_opts pasando el symbol_table de la
         * executable que poseyo este metodo, para que el mini-parser
         * resuelva @Absolute("X") y los CALLs a __module_init / __new_<X>. */
        SelectorOptions mc_opts;
        mc_opts.mode = SelectorMode::VM_ABI;
        mc_opts.runtime = g_runtime_entries;
        mc_opts.safepoint_handler_addr = reinterpret_cast<uint64_t>(
            g_runtime_entries->safepoint_handler);
        if (owning_symtab != nullptr) {
            const auto *st_ptr = owning_symtab;
            mc_opts.resolve_symbol = [st_ptr](const std::string &n) -> uint64_t {
                auto it = st_ptr->find(n);
                return it == st_ptr->end() ? 0 : it->second;
            };
        }
        /* Resolver native fn (CALLN): obtener acceso al FFI via el
         * Loader del VM owning. */
        /* IC slot reservation: aloca 16 bytes en el code cache (mismo
         * pool RWX) y los zero-init.  El JIT-eated codigo lee/escribe
         * estos bytes pero no los ejecuta. */
        CodeCache *cc_ptr = g_code_cache;
        mc_opts.reserve_ic_slot = [cc_ptr]() -> uint64_t {
            /* PIC: 4 entries x 16 bytes (class + jit_code) = 64 bytes.
             * Layout:
             *   +0..+15  entry 0: [class_ptr][jit_code]
             *   +16..+31 entry 1: [class_ptr][jit_code]
             *   +32..+47 entry 2: [class_ptr][jit_code]
             *   +48..+63 entry 3: [class_ptr][jit_code]
             * Cache-line aligned (64 bytes). */
            uint8_t *slot = cc_ptr->alloc(64, 64);
            if (slot) {
                for (int i = 0; i < 64; ++i) slot[i] = 0;
            }
            return reinterpret_cast<uint64_t>(slot);
        };
        ffi::FFI *ffi_ptr = &owning_vm.loader_public.ffi_loader;
        auto native_resolver = [ffi_ptr](const std::string &name) -> uint64_t {
            size_t colon = name.find(':');
            if (colon == std::string::npos) return 0;
            std::string lib  = name.substr(0, colon);
            std::string func = name.substr(colon + 1);
            /* Sprint JIT-cross-fn 2026-06-01: virtual lib registry FIRST.
             * Permite resolver libs in-process como "vesta_runtime",
             * "vesta_comptime" via punteros C registrados sin pasar por
             * LoadLibrary.  Bloqueante para callbacks B.1 (el thunk
             * generator vive en `vex_get_native_thunk` virtual fn). */
            void *vfn = ffi::lookup_virtual_fn(lib, func);
            if (vfn != nullptr) {
                return reinterpret_cast<uint64_t>(vfn);
            }
            try {
                void *mod = ffi_ptr->load_native_module(lib);
                if (!mod) return 0;
                void *fn = ffi_ptr->resolve_native_symbol(mod, func);
                return reinterpret_cast<uint64_t>(fn);
            } catch (...) {
                return 0;
            }
        };
        mc_opts.resolve_native_fn = native_resolver;
        /* lectura de vm_mem en compile-time para inlining.
         * Permite que el JIT lea el valor cacheado en @c s_<X>
         * (typically @c ClassInfo*) durante el compile de @c __new_<X>
         * y emita @c mov rN, imm64 directo en lugar de @c vrt_vm_read_u64
         * en cada iteracion.  El proceso (vm) tiene el static_data del
         * .velb ya cargado al momento del JIT compile (poblado por
         * @c __module_init durante el arranque). */
        runtime::ProcessVM *proc_for_read = vm;
        mc_opts.read_vmem_u64 = [proc_for_read](uint64_t vaddr) -> uint64_t {
            try {
                return proc_for_read->vm_mem.read_u64(vaddr);
            } catch (...) {
                return 0;
            }
        };
        /* offsets desde @c ProcessVM* hacia los punteros
         * @c nursery_bump_ y @c nursery_end_ del @c GcHeap embebido.
         * Se computan en runtime via aritmetica de direcciones porque
         * @c GcHeap NO es standard-layout (referencias + vector).  Una
         * vez computados, el JIT los embebe como disp32 en las
         * instrucciones x86-64 del bump-pointer inline. */
        const int64_t nb_off = reinterpret_cast<int64_t>(&proc_for_read->gc_heap.nursery_bump_)
                             - reinterpret_cast<int64_t>(proc_for_read);
        const int64_t ne_off = reinterpret_cast<int64_t>(&proc_for_read->gc_heap.nursery_end_)
                             - reinterpret_cast<int64_t>(proc_for_read);
        if (nb_off >= INT32_MIN && nb_off <= INT32_MAX
         && ne_off >= INT32_MIN && ne_off <= INT32_MAX) {
            mc_opts.nursery_bump_offset = static_cast<int32_t>(nb_off);
            mc_opts.nursery_end_offset  = static_cast<int32_t>(ne_off);
            mc_opts.register_alloc_addr = reinterpret_cast<uint64_t>(
                &vrt_register_alloc);
        }
        /* Offset de exc_frame_stack + exc_free_list para inline tryleave
         * (7 instr x86-64 vs ~20-30 de vrt_tryleave call) sin leak. */
        {
            const int64_t exc_off =
                reinterpret_cast<int64_t>(&proc_for_read->exc_frame_stack)
                - reinterpret_cast<int64_t>(proc_for_read);
            const int64_t free_off =
                reinterpret_cast<int64_t>(&proc_for_read->exc_free_list)
                - reinterpret_cast<int64_t>(proc_for_read);
            if (exc_off >= INT32_MIN && exc_off <= INT32_MAX) {
                mc_opts.exc_frame_stack_offset = static_cast<int32_t>(exc_off);
            }
            if (free_off >= INT32_MIN && free_off <= INT32_MAX) {
                mc_opts.exc_free_list_offset = static_cast<int32_t>(free_off);
            }
        }
        /* Direccion absoluta del contador MIPS para JIT.  El proc tiene
         * referencia al scheduler estable durante su vida; su counter
         * vive en una addr fija.  Embebemos directo como imm64 en el
         * codigo emitido. */
        /* Sprint string-perf-6: counter solo se emite si --stats / VESTA_JIT_STATS=1. */
        mc_opts.jit_instr_counter_addr = g_jit_emit_instr_counter
            ? reinterpret_cast<uint64_t>(&proc_for_read->scheduler.profiler_jit_instr_counter)
            : 0;
        /* Resolver user-fns: para evitar recursion arbitraria con cycles,
         * usamos cache global g_eager_cache + resolver simple no-recursive
         * (intenta lookup en cache; si no, intenta eager compile de la
         * callee).  Compartido con eager_compile_function. */
        if (owning_lookup != nullptr && owning_funcs != nullptr) {
            const auto *lk_ptr = owning_lookup;
            const auto *fn_ptr = owning_funcs;
            const auto *st_ptr2 = owning_symtab;
            auto ic_reserver_mc = mc_opts.reserve_ic_slot;
            auto vmem_reader_mc = mc_opts.read_vmem_u64;
            const int32_t nb_off_mc = mc_opts.nursery_bump_offset;
            const int32_t ne_off_mc = mc_opts.nursery_end_offset;
            const uint64_t reg_alloc_mc = mc_opts.register_alloc_addr;
            const int32_t exc_off_mc = mc_opts.exc_frame_stack_offset;
            const int32_t exc_free_off_mc = mc_opts.exc_free_list_offset;
            const uint64_t jit_counter_mc = mc_opts.jit_instr_counter_addr;
            mc_opts.resolve_user_fn = [lk_ptr, fn_ptr, st_ptr2, native_resolver, ic_reserver_mc, vmem_reader_mc, nb_off_mc, ne_off_mc, reg_alloc_mc, exc_off_mc, exc_free_off_mc, jit_counter_mc](const std::string &n) -> uint64_t {
                auto cit = g_eager_cache.find(n);
                if (cit != g_eager_cache.end()) {
                    if (cit->second == EAGER_IN_PROGRESS) return 0;
                    return cit->second;
                }
                auto lit = lk_ptr->find(n);
                if (lit == lk_ptr->end() || lit->second >= fn_ptr->size()) {
                    g_eager_cache[n] = 0;
                    return 0;
                }
                g_eager_cache[n] = EAGER_IN_PROGRESS;
                const ir::IrFunction &child_ir = (*fn_ptr)[lit->second];
                SelectorOptions child_opts;
                child_opts.mode = SelectorMode::VM_ABI;
                child_opts.runtime = g_runtime_entries;
                child_opts.safepoint_handler_addr = reinterpret_cast<uint64_t>(
                    g_runtime_entries->safepoint_handler);
                if (st_ptr2) {
                    const auto *sp = st_ptr2;
                    child_opts.resolve_symbol = [sp](const std::string &x) -> uint64_t {
                        auto i2 = sp->find(x);
                        return i2 == sp->end() ? 0 : i2->second;
                    };
                }
                child_opts.resolve_native_fn = native_resolver;
                child_opts.reserve_ic_slot = ic_reserver_mc;
                child_opts.read_vmem_u64 = vmem_reader_mc;
                child_opts.nursery_bump_offset = nb_off_mc;
                child_opts.nursery_end_offset  = ne_off_mc;
                child_opts.register_alloc_addr = reg_alloc_mc;
                child_opts.exc_frame_stack_offset = exc_off_mc;
                child_opts.exc_free_list_offset = exc_free_off_mc;
                child_opts.jit_instr_counter_addr = jit_counter_mc;
                CompileResult cres = g_compiler->compile_with_opts(child_ir, child_opts);
                const uint64_t addr = cres.fn ? reinterpret_cast<uint64_t>(cres.fn) : 0;
                g_eager_cache[n] = addr;
                if (cres.fn) ++g_jit_compiled_count;
                else         ++g_jit_unsupported_count;
                /* Sprint JIT-cross-fn 2026-06-01: registrar pc -> jit_code
                 * en el mapa global para que `lookup_jit_code_at_pc` pueda
                 * encontrar las callees compiladas en cascade.  Sin esto,
                 * `as_native_callback(fn)` no podia obtener el thunk para
                 * fns compiladas via cascade (solo via top-level eager o
                 * via maybe_compile_*). */
                if (cres.fn && st_ptr2) {
                    auto sit = st_ptr2->find("code." + n);
                    if (sit != st_ptr2->end() && sit->second != 0) {
                        register_jit_code_at_pc(sit->second,
                                                reinterpret_cast<void *>(cres.fn));
                    }
                }
                if (g_jit_warn_unsupported) {
                    if (cres.fn) {
                        std::fprintf(stderr, "[jit] eager compiled '%s' (%zu bytes, %zu MInstrs)\n",
                                     n.c_str(), cres.code_size, cres.instr_count);
                    } else {
                        std::fprintf(stderr, "[jit] eager '%s' no se compilo (unsupported=%d)\n",
                                     n.c_str(), cres.unsupported ? 1 : 0);
                    }
                }
                if (g_jit_disasm && cres.fn) {
                    debug_dump_jit_code(n, cres.code_start, cres.code_size);
                }
                return addr;
            };
        }

        const CompileResult res = g_compiler->compile_with_opts(*ir_fn, mc_opts);
        if (res.fn != nullptr) {
            /* Exito: asignar jit_code.  El proximo CALLVIRT despacha
             * automaticamente al codigo nativo via el hook D.3-C. */
            method->jit_code = reinterpret_cast<void *>(res.fn);
            /* D.5-callvm-hook: tambien registrar pc -> jit para que
             * @c exec_instr_callvm pueda dispatchar a este metodo
             * cuando se invoque directamente via CALLVM (e.g. desde
             * codigo interp con label resuelto al @c code_vaddr). */
            if (method->code_vaddr != 0) {
                register_jit_code_at_pc(method->code_vaddr,
                                        reinterpret_cast<void *>(res.fn));
            }
            ++g_jit_compiled_count;
            if (g_jit_warn_unsupported) {
                std::fprintf(stderr,
                    "[jit] compiled '%s' (%zu bytes, %zu MInstrs)\n",
                    key.c_str(), res.code_size, res.instr_count);
            }
            if (g_jit_disasm) {
                debug_dump_jit_code(key, res.code_start, res.code_size);
            }
        } else {
            /* Fallo: marcar para no reintentar (igual que IR ausente).
             * res.unsupported = true significa que el Selector encontro
             * una IR op que no sabe lowerizar; printamos warning para
             * que el dev sepa que IR op anyadir. */
            ++g_jit_unsupported_count;
            if (g_jit_warn_unsupported) {
                std::fprintf(stderr,
                    "[jit] '%s' no se compilo (Selector unsupported=%d) -- "
                    "fallback a interp\n",
                    key.c_str(), res.unsupported ? 1 : 0);
            }
            method->invocation_count = UINT32_MAX;
        }
    }

    /* ===================================================================== */
    /* get_jit_stats_summary                                                  */
    /* ===================================================================== */

    /* ===================================================================== */
    /* eager_compile_function                                                 */
    /* ===================================================================== */

    /* (g_eager_cache + EAGER_IN_PROGRESS declarados arriba en namespace
     * anonimo para que maybe_compile_method tambien los pueda usar
     * sin forward-decls feos.) */

    CompileResult eager_compile_function(
        const ir::IrFunction &ir_fn,
        const std::unordered_map<std::string, size_t> *ir_lookup,
        const std::vector<ir::IrFunction> *ir_functions,
        const std::unordered_map<std::string, uint64_t> *symbol_table,
        std::function<uint64_t(const std::string &)> resolve_native_fn,
        std::function<uint64_t(uint64_t)> read_vmem_u64,
        int32_t exc_frame_stack_offset,
        int32_t exc_free_list_offset,
        uint64_t jit_instr_counter_addr) {
        /* Init lazy de threshold desde env (1 vez por proceso). */
        std::call_once(g_env_init_flag, init_threshold_from_env);

        /* Si JIT esta off, no compilar. */
        if (g_jit_threshold == UINT32_MAX) {
            return CompileResult{};
        }

        /* Init lazy del JIT subsystem. */
        std::call_once(g_jit_init_flag, init_jit_subsystem);

        /* Compilar bajo el mismo mutex que maybe_compile_method (el
         * JitRegistry necesita acceso exclusivo). */
        std::lock_guard<std::mutex> lk(g_compile_mtx);

        /* Si ya esta cacheado, retornar el ptr (excepto sentinelas). */
        if (!ir_fn.name.empty()) {
            auto it = g_eager_cache.find(ir_fn.name);
            if (it != g_eager_cache.end()
             && it->second != 0 && it->second != EAGER_IN_PROGRESS) {
                CompileResult cached{};
                cached.fn = reinterpret_cast<JitFn>(it->second);
                return cached;
            }
        }

        /* Recursive resolver: cuando el Selector encuentra CALL a una
         * funcion user, llama a este callback que (1) chequea cache,
         * (2) busca su IR, (3) recursivamente eager-compila, (4) devuelve
         * la direccion.  Ciclos se cortan con el sentinela IN_PROGRESS:
         * si una funcion B en compilacion llama a A que tambien esta en
         * compilacion (mutua recursion), el inner resolve devuelve 0 y
         * B queda unsupported -- los ciclos en lenguajes user requieren
         * indirect-call-tables (Phase D.5+). */
        /* Phase: resolver de simbolos via symbol_table del Loader.
         * Resuelve `@Absolute("code.s_K")` strings dentro de raw_asm a su
         * direccion VM absoluta.  Nullptr-safe: si no hay symbol_table,
         * el callback retorna 0 y el mini-parser cae a unsupported para
         * @Absolute refs (backward compatible). */
        std::function<uint64_t(const std::string &)> sym_resolver{};
        if (symbol_table != nullptr) {
            const auto *sym_ptr = symbol_table;
            sym_resolver = [sym_ptr](const std::string &name) -> uint64_t {
                auto it = sym_ptr->find(name);
                if (it == sym_ptr->end()) return 0;
                return it->second;
            };
        }

        std::shared_ptr<std::function<uint64_t(const std::string &)>> resolver_holder;
        std::function<uint64_t(const std::string &)> resolver{};
        if (ir_lookup != nullptr && ir_functions != nullptr) {
            const auto *lookup_ptr = ir_lookup;
            const auto *funcs_ptr  = ir_functions;
            resolver_holder = std::make_shared<std::function<uint64_t(const std::string &)>>();
            /* Cuerpo del resolver: captura su propio shared_ptr para que
             * la recursion pueda referenciar el mismo callback en cada
             * nivel.  Esto permite resolucion arbitrariamente profunda
             * sin perder el callback. */
            CodeCache *cc_for_ic = g_code_cache;
            auto ic_reserver = [cc_for_ic]() -> uint64_t {
                uint8_t *slot = cc_for_ic->alloc(16, 16);
                if (slot) std::memset(slot, 0, 16);
                return reinterpret_cast<uint64_t>(slot);
            };
            *resolver_holder = [lookup_ptr, funcs_ptr, resolver_holder, sym_resolver, resolve_native_fn, ic_reserver, read_vmem_u64, exc_frame_stack_offset, exc_free_list_offset, jit_instr_counter_addr]
                               (const std::string &name) -> uint64_t {
                /* Chequear cache primero. */
                auto cit = g_eager_cache.find(name);
                if (cit != g_eager_cache.end()) {
                    if (cit->second == EAGER_IN_PROGRESS) return 0;
                    return cit->second;
                }
                /* Buscar IR de la callee. */
                auto lit = lookup_ptr->find(name);
                if (lit == lookup_ptr->end() || lit->second >= funcs_ptr->size()) {
                    g_eager_cache[name] = 0;
                    return 0;
                }
                /* Marcar IN_PROGRESS antes de recursar. */
                g_eager_cache[name] = EAGER_IN_PROGRESS;
                /* Recursive compile con el MISMO resolver para que la
                 * callee pueda resolver SUS propias user CALLs.  Use
                 * shared_ptr capturado para que la lambda inner haga ref
                 * a este callback. */
                const ir::IrFunction &child_ir = (*funcs_ptr)[lit->second];
                SelectorOptions opts;
                opts.mode = SelectorMode::VM_ABI;
                opts.runtime = g_runtime_entries;
                opts.safepoint_handler_addr = reinterpret_cast<uint64_t>(
                    g_runtime_entries->safepoint_handler);
                opts.resolve_user_fn = *resolver_holder;  /* SAME resolver */
                opts.resolve_symbol  = sym_resolver;      /* propagar Phase D.3-H */
                opts.resolve_native_fn = resolve_native_fn;  /* propagar CALLN resolver */
                opts.reserve_ic_slot = ic_reserver;          /* IC slots */
                opts.read_vmem_u64 = read_vmem_u64;          /* Phase D.7.opt inline */
                opts.exc_frame_stack_offset = exc_frame_stack_offset; /* inline tryleave */
                opts.exc_free_list_offset = exc_free_list_offset;     /* no leak */
                opts.jit_instr_counter_addr = jit_instr_counter_addr; /* MIPS counter */
                CompileResult cres = g_compiler->compile_with_opts(child_ir, opts);
                const uint64_t addr = cres.fn ? reinterpret_cast<uint64_t>(cres.fn) : 0;
                g_eager_cache[name] = addr;
                if (cres.fn) ++g_jit_compiled_count;
                else         ++g_jit_unsupported_count;
                /* D.5-callvm-hook: registrar pc -> jit en el mapa global
                 * para que exec_instr_callvm pueda dispatchar interp ->
                 * JIT al llamar a esta funcion via @c callvm. */
                if (cres.fn && sym_resolver) {
                    const uint64_t pc = sym_resolver("code." + name);
                    if (pc != 0) {
                        register_jit_code_at_pc(pc, reinterpret_cast<void *>(cres.fn));
                    }
                }
                if (g_jit_warn_unsupported) {
                    if (cres.fn) {
                        std::fprintf(stderr, "[jit] eager compiled '%s' (%zu bytes, %zu MInstrs)\n",
                                     name.c_str(), cres.code_size, cres.instr_count);
                    } else {
                        std::fprintf(stderr, "[jit] eager '%s' no se compilo (unsupported=%d)\n",
                                     name.c_str(), cres.unsupported ? 1 : 0);
                    }
                }
                if (g_jit_disasm && cres.fn) {
                    debug_dump_jit_code(name, cres.code_start, cres.code_size);
                }
                return addr;
            };
            resolver = *resolver_holder;
        }

        /* Setup opciones del top-level compile (main). */
        SelectorOptions top_opts;
        top_opts.mode = SelectorMode::VM_ABI;
        top_opts.runtime = g_runtime_entries;
        top_opts.safepoint_handler_addr = reinterpret_cast<uint64_t>(
            g_runtime_entries->safepoint_handler);
        top_opts.resolve_user_fn = resolver;
        top_opts.resolve_symbol  = sym_resolver;
        top_opts.resolve_native_fn = resolve_native_fn;
        top_opts.read_vmem_u64 = read_vmem_u64;
        top_opts.exc_frame_stack_offset = exc_frame_stack_offset;
        top_opts.exc_free_list_offset = exc_free_list_offset;
        top_opts.jit_instr_counter_addr = jit_instr_counter_addr;
        {
            CodeCache *cc_top = g_code_cache;
            top_opts.reserve_ic_slot = [cc_top]() -> uint64_t {
                uint8_t *slot = cc_top->alloc(16, 16);
                if (slot) std::memset(slot, 0, 16);
                return reinterpret_cast<uint64_t>(slot);
            };
        }

        /* Marcar IN_PROGRESS antes de compilar para detectar self-recursion. */
        if (!ir_fn.name.empty()) {
            g_eager_cache[ir_fn.name] = EAGER_IN_PROGRESS;
        }

        const CompileResult res = g_compiler->compile_with_opts(ir_fn, top_opts);
        if (res.fn != nullptr) {
            ++g_jit_compiled_count;
            if (!ir_fn.name.empty()) {
                g_eager_cache[ir_fn.name] = reinterpret_cast<uint64_t>(res.fn);
            }
            /* D.5-callvm-hook: registrar pc -> jit en el mapa global. */
            if (sym_resolver && !ir_fn.name.empty()) {
                const uint64_t pc = sym_resolver("code." + ir_fn.name);
                if (pc != 0) {
                    register_jit_code_at_pc(pc, reinterpret_cast<void *>(res.fn));
                }
            }
            if (g_jit_disasm) {
                debug_dump_jit_code(ir_fn.name.empty() ? "<anon>" : ir_fn.name,
                                    res.code_start, res.code_size);
            }
        } else {
            ++g_jit_unsupported_count;
            if (!ir_fn.name.empty()) {
                g_eager_cache[ir_fn.name] = 0;  /* cachear failure */
            }
        }
        return res;
    }

    /* ===================================================================== */
    /* debug_dump_jit_code (debugging aid)                         */
    /* ===================================================================== */
    void debug_dump_jit_code(const std::string &name,
                             const uint8_t *code,
                             size_t code_size) {
        if (!code || code_size == 0) {
            std::fprintf(stderr, "[jit disasm] '%s': codigo vacio\n", name.c_str());
            return;
        }
        std::fprintf(stderr, "[jit disasm] === %s (%zu bytes @ %p) ===\n",
                     name.c_str(), code_size, static_cast<const void *>(code));
        /* Hex dump compacto: 16 bytes por linea con offset. */
        for (size_t i = 0; i < code_size; i += 16) {
            std::fprintf(stderr, "  %04zx:", i);
            for (size_t j = 0; j < 16 && i + j < code_size; ++j) {
                std::fprintf(stderr, " %02x", code[i + j]);
            }
            std::fprintf(stderr, "\n");
        }
        /* Disasm via Capstone (x86-64). */
        csh handle;
        if (cs_open(CS_ARCH_X86, CS_MODE_64, &handle) != CS_ERR_OK) {
            std::fprintf(stderr, "[jit disasm] cs_open fallo\n");
            return;
        }
        cs_option(handle, CS_OPT_DETAIL, CS_OPT_OFF);
        cs_insn *insn = nullptr;
        const size_t count = cs_disasm(handle, code, code_size,
                                       reinterpret_cast<uint64_t>(code),
                                       0, &insn);
        if (count > 0) {
            for (size_t i = 0; i < count; ++i) {
                std::fprintf(stderr, "  %p: %-12s %s\n",
                             reinterpret_cast<void *>(insn[i].address),
                             insn[i].mnemonic, insn[i].op_str);
            }
            cs_free(insn, count);
        } else {
            std::fprintf(stderr, "  (Capstone no pudo desensamblar)\n");
        }
        cs_close(&handle);
        std::fprintf(stderr, "[jit disasm] === fin %s ===\n", name.c_str());
    }

    std::string get_jit_stats_summary() {
        std::ostringstream oss;
        oss << "[jit stats] threshold=";
        if (g_jit_threshold == UINT32_MAX) {
            oss << "(disabled)";
        } else {
            oss << g_jit_threshold;
        }
        oss << "  compiled=" << g_jit_compiled_count
            << "  unsupported=" << g_jit_unsupported_count
            << "  no_ir=" << g_jit_no_ir_count;
        return oss.str();
    }

    /* ===================================================================== */
    /* Sprint D.5-callvm-trigger: counter por PC + auto-compile en CALLVM     */
    /* ===================================================================== */
    /*
     * Diseño: similar a maybe_compile_method pero parametrizado por PC en
     * lugar de MethodInfo*.  El hook en exec_instr_callvm llama a este
     * helper en cada CALLVM cuyo target NO esta en el pc-map; cuando el
     * counter cruza el threshold, busca la IR correspondiente al PC y la
     * compila via @c eager_compile_function (que reusa el pipeline
     * recursivo: callees se compilan transitively).
     *
     * Counter por PC: @c unordered_map<uint64_t, uint32_t>.  Valor sentinela
     * UINT32_MAX = "ya intentado y fallo" (no reintentar -- evita oscilar
     * compile/fail/recompile en loops calientes con funciones unsupported).
     *
     * Reverse symbol_table (pc -> name): construido lazy por executable;
     * cacheado en @c g_exec_pc_to_name por puntero del executable.  El
     * symbol_table del .velb tiene entries "code.<fn_name>" -> pc; el
     * cache invierte y strip el prefijo "code.".
     */
    namespace {
        std::unordered_map<uint64_t, uint32_t> g_callvm_counters;
        std::unordered_map<uint64_t, uint8_t>  g_callvm_attempts; /* JIT-cross-fn 2026-06-01 */
        std::mutex                              g_callvm_mtx;
        /* Sprint JIT-cross-fn counter cache reintentos:
         * Cuando un compile attempt falla, en lugar de marcar
         * `cnt = UINT32_MAX` permanente, registramos un attempt count.
         * El proximo intento ocurre tras RETRY_INTERVAL invocaciones
         * adicionales.  Tras MAX_ATTEMPTS fails, queda como cached
         * failure final (UINT32_MAX).  Asi cuando una fn falla por
         * contexto temporal (e.g. dep callee no estaba compilada
         * todavia), reintentar puede tener exito si el contexto
         * cambia entre invocaciones (ahora callee ya esta JIT-eada). */
        constexpr uint32_t kCompileRetryInterval = 1000;
        constexpr uint8_t  kCompileMaxAttempts   = 3;
        /* pc -> name (lazy por executable).  Key: puntero al Executable.  */
        std::unordered_map<const void *,
            std::unordered_map<uint64_t, std::string>> g_exec_pc_to_name;
    }

    /* Forward decl: definida mas abajo (interfaz publica). */
    void maybe_compile_callvm_target(runtime::ProcessVM *vm,
                                     uint64_t target_pc) noexcept;

    /* ===================================================================== */
    /* Sprint D.5-callvm-hook: mapa pc -> jit_code                              */
    /* ===================================================================== */
    /*
     * Diseño:
     *   - @c g_pc_jit_active es un flag rapido (lectura sin lock) que el hot
     *     path de @c exec_instr_callvm consulta antes de tocar el mapa.
     *     Falso por defecto = cero overhead cuando JIT esta off.
     *   - El mapa @c g_pc_to_jit_code esta protegido por @c g_pc_jit_mtx
     *     porque la compilacion puede correr en paralelo a la ejecucion
     *     en Phase D.8 (multi-thread compile).  Hoy single-thread compile,
     *     pero el mutex no hace daño y prepara la transicion.
     *   - El flag NUNCA se resetea (incluso si limpiamos el mapa para
     *     tests).  El coste es 1 hashmap lookup extra por callvm tras un
     *     reset, despreciable.
     */
    bool g_pc_jit_active = false;
    namespace {
        std::mutex                          g_pc_jit_mtx;
        std::unordered_map<uint64_t, void*> g_pc_to_jit_code;
    }

    void register_jit_code_at_pc(uint64_t vaddr, void *fn) noexcept {
        if (!fn || vaddr == 0) return;
        std::lock_guard<std::mutex> lk(g_pc_jit_mtx);
        g_pc_to_jit_code[vaddr] = fn;
        g_pc_jit_active = true;
        if (g_jit_warn_unsupported) {
            std::fprintf(stderr, "[jit] pc-map register: 0x%llx -> %p\n",
                         static_cast<unsigned long long>(vaddr), fn);
        }
    }

    void *lookup_jit_code_at_pc(uint64_t vaddr) noexcept {
        if (!g_pc_jit_active) return nullptr;
        std::lock_guard<std::mutex> lk(g_pc_jit_mtx);
        auto it = g_pc_to_jit_code.find(vaddr);
        return (it == g_pc_to_jit_code.end()) ? nullptr : it->second;
    }

    void clear_jit_code_at_pc_map() noexcept {
        std::lock_guard<std::mutex> lk(g_pc_jit_mtx);
        g_pc_to_jit_code.clear();
        /* NO reseteamos g_pc_jit_active: el flag es sticky a proposito
         * (ver comentario arriba).  Tests pueden setearlo manualmente. */
    }

    /* ===================================================================== */
    /* Sprint D.5-callvm-trigger: implementacion                              */
    /* ===================================================================== */

    /**
     * @brief Counter trigger para CALLVM/CALLVMR: cada miss del pc-map
     *        incrementa el counter de ese PC; al cruzar threshold,
     *        intenta compilar la funcion via @c eager_compile_function.
     *
     * Coste fast path (JIT off o counter < threshold): 1 lock + 1 hash
     * lookup + 1 increment = ~30 ns.  Coste slow path (threshold cruzado):
     * el del compile completo (~1-10 ms para funciones tipicas).  Solo
     * paga slow path UNA vez por funcion en toda la ejecucion (tras el
     * compile, el pc-map captura todos los CALLVMs subsiguientes).
     *
     * Sentinela UINT32_MAX en el counter = "ya intentado, fallo".  No
     * reintenta para evitar oscilacion.  Si la funcion era compilable
     * pero el primer intento fallo por race conditions, el usuario debe
     * reiniciar el proceso (caso muy raro).
     */
    void maybe_compile_callvm_target(runtime::ProcessVM *vm,
                                     uint64_t target_pc) noexcept {
        std::call_once(g_env_init_flag, init_threshold_from_env);
        if (g_jit_threshold == UINT32_MAX) {
            return;
        }
        /* Si ya esta compilado (otro hilo gano la carrera), salir. */
        if (lookup_jit_code_at_pc(target_pc) != nullptr) return;

        /* Acquire mutex para counter + tabla de executables.  Necesario
         * porque maybe_compile_method tambien usa g_compile_mtx; la
         * separacion en mutex distinto (g_callvm_mtx para counters) evita
         * contencion innecesaria entre CALLVM hot path y CALLVIRT path. */
        std::lock_guard<std::mutex> cmlk(g_callvm_mtx);
        auto &cnt = g_callvm_counters[target_pc];
        if (cnt == UINT32_MAX) return;       /* cached failure FINAL */
        ++cnt;
        if (cnt < g_jit_threshold) {
            return;
        }

        /* Sprint JIT-cross-fn counter cache reintentos:
         * Determinar si toca compilar AHORA segun attempt count.
         * Attempt #i ocurre cuando cnt = threshold + i * RETRY_INTERVAL. */
        auto &att = g_callvm_attempts[target_pc];
        if (att >= kCompileMaxAttempts) {
            /* Ya agotamos los reintentos -- marcar permanente. */
            cnt = UINT32_MAX;
            return;
        }
        const uint32_t needed = g_jit_threshold
                              + static_cast<uint32_t>(att) * kCompileRetryInterval;
        if (cnt < needed) {
            /* Aun no toca el siguiente attempt; warming. */
            return;
        }
        /* Esta es la attempt #(att+1).  Incrementar attempt antes del
         * compile para que reintentos paralelos no entren simultaneo. */
        ++att;

        /* Buscar el IrFunction correspondiente a target_pc iterando los
         * executables del VM.  Construir pc->name lazy por executable. */
        runtime::VM &owning_vm = vm->scheduler.vm_reference;
        const ir::IrFunction *ir_fn = nullptr;
        const loader::Executable *owning_exe = nullptr;
        std::string fn_name;
        for (auto &exe_ptr : owning_vm.loader_public.executables) {
            const loader::Executable *exe = exe_ptr.get();
            /* Construir pc->name lazy (1 vez por executable). */
            auto &pc_to_name = g_exec_pc_to_name[exe];
            if (pc_to_name.empty() && !exe->symbol_table.empty()) {
                for (const auto &kv : exe->symbol_table) {
                    /* Solo nos interesan los simbolos "code.<fn_name>".
                     * Stripped el prefijo "code." para matchear con
                     * @c ir_lookup. */
                    if (kv.first.rfind("code.", 0) == 0) {
                        pc_to_name[kv.second] = kv.first.substr(5);
                    }
                }
            }
            auto pn_it = pc_to_name.find(target_pc);
            if (pn_it == pc_to_name.end()) continue;
            std::string candidate_name = pn_it->second;
            /* Sprint JIT-cross-fn 2026-06-01: el frontend Vex anyade
             * sufijo `_entry_<N>` al label del bytecode (entry block).
             * El ir_lookup usa el nombre limpio.  Strip el sufijo si
             * existe para que el lookup matchee. */
            {
                const std::string suffix = "_entry_";
                auto pos = candidate_name.rfind(suffix);
                if (pos != std::string::npos) {
                    /* Confirmar que tras el sufijo solo hay digitos. */
                    bool only_digits = true;
                    for (size_t k = pos + suffix.size();
                         k < candidate_name.size(); ++k) {
                        if (candidate_name[k] < '0' || candidate_name[k] > '9') {
                            only_digits = false;
                            break;
                        }
                    }
                    if (only_digits) {
                        candidate_name = candidate_name.substr(0, pos);
                    }
                }
            }
            auto lit = exe->ir_lookup.find(candidate_name);
            if (lit == exe->ir_lookup.end()) continue;
            if (lit->second >= exe->ir_functions.size()) continue;
            ir_fn = &exe->ir_functions[lit->second];
            owning_exe = exe;
            fn_name = candidate_name;
            break;
        }

        if (ir_fn == nullptr) {
            /* No hay IR para este PC (e.g. helper sintetico sin nombre,
             * o codigo de runtime que no esta en ir_functions).  Cached
             * failure ya esta en cnt = UINT32_MAX. */
            ++g_jit_no_ir_count;
            return;
        }

        /* Construir callbacks equivalentes a los que loader.cpp pasa al
         * eager_compile inicial.  Reusan ffi_loader del VM + el propio
         * proceso para vm_mem.read_u64 + offsets calculados sobre el
         * propio objeto ProcessVM. */
        ffi::FFI *ffi_ptr = &owning_vm.loader_public.ffi_loader;
        auto resolve_native = [ffi_ptr](const std::string &name) -> uint64_t {
            size_t colon = name.find(':');
            if (colon == std::string::npos) return 0;
            std::string lib  = name.substr(0, colon);
            std::string func = name.substr(colon + 1);
            /* Sprint JIT-cross-fn: virtual lib registry FIRST. */
            void *vfn = ffi::lookup_virtual_fn(lib, func);
            if (vfn != nullptr) {
                return reinterpret_cast<uint64_t>(vfn);
            }
            try {
                void *mod = ffi_ptr->load_native_module(lib);
                if (!mod) return 0;
                void *fn = ffi_ptr->resolve_native_symbol(mod, func);
                return reinterpret_cast<uint64_t>(fn);
            } catch (...) {
                return 0;
            }
        };
        runtime::ProcessVM *proc_for_read = vm;
        auto read_vmem_cb = [proc_for_read](uint64_t vaddr) -> uint64_t {
            try {
                return proc_for_read->vm_mem.read_u64(vaddr);
            } catch (...) {
                return 0;
            }
        };
        int32_t exc_off = 0, exc_free_off = 0;
        {
            const int64_t off64 =
                reinterpret_cast<int64_t>(&vm->exc_frame_stack)
                - reinterpret_cast<int64_t>(vm);
            const int64_t free64 =
                reinterpret_cast<int64_t>(&vm->exc_free_list)
                - reinterpret_cast<int64_t>(vm);
            if (off64  >= INT32_MIN && off64  <= INT32_MAX) exc_off       = static_cast<int32_t>(off64);
            if (free64 >= INT32_MIN && free64 <= INT32_MAX) exc_free_off  = static_cast<int32_t>(free64);
        }
        /* Sprint string-perf-6: counter solo se emite si --stats / VESTA_JIT_STATS=1. */
        const uint64_t jit_counter_addr = g_jit_emit_instr_counter
            ? reinterpret_cast<uint64_t>(&vm->scheduler.profiler_jit_instr_counter)
            : 0;

        /* Llamar a eager_compile_function que ya hace el trabajo
         * completo: construye el resolver recursivo, registra en
         * pc-map al exito (via mi D.5-callvm-hook), maneja cache,
         * todo.  Si exito, el pc-map captura este PC y todos sus
         * callees transitivamente. */
        try {
            (void) eager_compile_function(
                *ir_fn,
                &owning_exe->ir_lookup,
                &owning_exe->ir_functions,
                &owning_exe->symbol_table,
                resolve_native,
                read_vmem_cb,
                exc_off,
                exc_free_off,
                jit_counter_addr);
            /* Independiente del resultado: si exito, register_jit_code_at_pc
             * ya se llamo desde dentro de eager_compile_function via la
             * registracion top-level.  Si fallo, queda como cached failure
             * tanto en g_eager_cache (por nombre) como en cnt (UINT32_MAX). */
            if (g_jit_warn_unsupported) {
                const bool ok = (lookup_jit_code_at_pc(target_pc) != nullptr);
                std::fprintf(stderr,
                    "[jit] callvm-trigger pc=0x%llx name='%s' -> %s\n",
                    static_cast<unsigned long long>(target_pc),
                    fn_name.c_str(),
                    ok ? "compiled" : "unsupported");
            }
        } catch (...) {
            /* Cualquier excepcion del compile se ignora; quedamos en
             * cached failure y futuras invocaciones fallback a interp. */
        }
    }

} // namespace jit
