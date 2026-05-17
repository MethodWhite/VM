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
            uint8_t *slot = cc_ptr->alloc(16, 16);
            if (slot) {
                slot[0] = slot[1] = slot[2] = slot[3] = 0;
                slot[4] = slot[5] = slot[6] = slot[7] = 0;
                slot[8] = slot[9] = slot[10] = slot[11] = 0;
                slot[12] = slot[13] = slot[14] = slot[15] = 0;
            }
            return reinterpret_cast<uint64_t>(slot);
        };
        ffi::FFI *ffi_ptr = &owning_vm.loader_public.ffi_loader;
        auto native_resolver = [ffi_ptr](const std::string &name) -> uint64_t {
            size_t colon = name.find(':');
            if (colon == std::string::npos) return 0;
            std::string lib  = name.substr(0, colon);
            std::string func = name.substr(colon + 1);
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
            mc_opts.resolve_user_fn = [lk_ptr, fn_ptr, st_ptr2, native_resolver, ic_reserver_mc, vmem_reader_mc, nb_off_mc, ne_off_mc, reg_alloc_mc](const std::string &n) -> uint64_t {
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
                CompileResult cres = g_compiler->compile_with_opts(child_ir, child_opts);
                const uint64_t addr = cres.fn ? reinterpret_cast<uint64_t>(cres.fn) : 0;
                g_eager_cache[n] = addr;
                if (cres.fn) ++g_jit_compiled_count;
                else         ++g_jit_unsupported_count;
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
        std::function<uint64_t(uint64_t)> read_vmem_u64) {
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
            *resolver_holder = [lookup_ptr, funcs_ptr, resolver_holder, sym_resolver, resolve_native_fn, ic_reserver, read_vmem_u64]
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
                CompileResult cres = g_compiler->compile_with_opts(child_ir, opts);
                const uint64_t addr = cres.fn ? reinterpret_cast<uint64_t>(cres.fn) : 0;
                g_eager_cache[name] = addr;
                if (cres.fn) ++g_jit_compiled_count;
                else         ++g_jit_unsupported_count;
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

} // namespace jit
