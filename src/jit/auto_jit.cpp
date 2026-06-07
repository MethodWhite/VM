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
#include "jit/vreg_pipeline.h"
#include "jit/regalloc_rewrite.h"       // OSR glue (set_osr_handler / osr_loop_*)
#include "vesta_rt/public.h"
#include "ffi/virtual_lib_registry.h"  // Sprint JIT-cross-fn: virtual fn lookup
#include "loader/loader.h"
#include "loader/oop_types.h"
#include "ir/ir_optimizer.h"            // C2.4: devirt especulativa + inline
#include "runtime/proceso_runtime.h"
#include "runtime/runtime.h"
#include "runtime/scheduler.h"
#include "runtime/exception_runtime.h"  // callback-ABI: get_current_executing_process + jit_proc_tls_index
#include "ir/ssa_ir.h"
#include <cstring>

#include <capstone/capstone.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <utility>
#include <vector>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace jit {

    /* ===================================================================== */
    /* Threshold global                                                       */
    /* ===================================================================== */

    /**
     * @brief Default: 1500 (JIT por defecto, tier C1-like estilo HotSpot).
     *
     * Un metodo/funcion se JIT-compila tras 1500 invocaciones.  Valor por
     * estudio empirico: el threshold NO es sensible en [100,20000] (mismo
     * resultado), 1500 esta sobre el break-even de metodos pesados (~1k) y bajo
     * los call-counts de codigo hot (millones); los metodos triviales (break-
     * even alto ~20k) se inlinean antes de compilar -> sin compile-waste.
     * Coincide con el tier C1 de HotSpot.
     *
     * Seguridad: el sandbox bajo JIT esta cerrado (maybe_compile_method /
     * maybe_compile_callvm_target / eager-compile de main bailan si
     * sandbox_active -> el interp enforcea las caps).
     *
     * Validado: e2e 263/0 forzando T=1500.  Un bug previo del path
     * native-callback (el thunk no forzaba el compile a threshold moderado ->
     * test 173 colgaba) se arreglo en native_callback.cpp (vex_get_native_thunk
     * fuerza el compile inmediato sea cual sea el threshold).
     *
     * Override: @c set_jit_threshold / env var @c VESTA_JIT_THRESHOLD;
     * `-m jit` = 1 (agresivo), `-m vm`/`-m interp` = UINT32_MAX (interp puro).
     */
    uint32_t g_jit_threshold = 1500;

    /**
     * @brief Warning toggleable + counters de auditoria.
     */
    bool     g_jit_warn_unsupported  = false;
    bool     g_jit_disasm            = false;
    /* Phase D.7: path de registros virtuales (opt-in via VESTA_JIT_VREGS).
     * Default OFF -> el JIT usa el path de slots de siempre (cero cambio).
     * Con el flag, las funciones del subset soportado por el selector vreg
     * (aritmetica / control de flujo / loops, sin GC) se compilan por el
     * register allocator; el resto cae al path de slots via fallback. */
    bool     g_jit_use_vregs         = false;
    /* Sprint string-perf-6 (2026-06-02): emit del MIPS counter per-block
     * en el JIT.  Default OFF porque cuesta ~3ns por block ejecutado;
     * en hot loops con muchos bloques (e.g. bench_branch_unpredict con
     * 13 blocks/iter) el overhead puede llegar al 50%.  Solo se activa
     * cuando el usuario pide --stats o VESTA_JIT_STATS=1. */
    bool     g_jit_emit_instr_counter = false;
    uint64_t g_jit_compiled_count    = 0;
    uint64_t g_jit_unsupported_count = 0;
    uint64_t g_jit_no_ir_count       = 0;

    /* C2 tier-up (2026-06-07).  OPT-IN: g_c2_threshold == 0 (default) deja el
     * C2 totalmente apagado -> el Selector NO emite el contador on-entry y el
     * comportamiento C1 queda intacto (cero regresion).  Cuando se setea
     * VESTA_C2_THRESHOLD=N>0, las funciones con CALLVIRT compiladas por el path
     * Selector llevan un contador de invocaciones; al cruzar N se recompilan
     * (C2) y se hace swap atomico del codigo.  VESTA_C2_LOG=1 loguea las clases
     * observadas en los IC slots durante el recompile (PoC C2.1/C2.2).
     * VESTA_C2_TIER_ALL=1 instrumenta TODAS las funciones (no solo las con
     * CALLVIRT) -- para A/B futuro.
     *
     * NOTA: la observacion de clases solo existe en el path Selector (que usa
     * el PIC via vrt_callvirt_ic); el path vreg (default) hace dispatch inline
     * sin PIC.  Por eso el C2 se valida con VESTA_JIT_VREGS=0.  Wirearlo al
     * path vreg (PIC) y el OSR para hotness de loop-en-main son follow-ups. */
    uint32_t g_c2_threshold = 0;
    bool     g_c2_log       = false;
    bool     g_c2_tier_all  = false;
    bool     g_c2_vreg      = false;  /* experimento: recompilar C2 por path vreg */
    uint64_t g_c2_tierup_count = 0;

    /* C2 reclaim: true cuando el C2 esta activo -> enter_jit instrumenta la
     * quiescencia (ver interp_jit_bridge.h).  Default false = cero overhead. */
    bool g_jit_reclaim_active = false;

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
            /* Phase D.7: el path de REGISTROS VIRTUALES es el DEFAULT del JIT
             * (mide ~2x sobre el path de slots en codigo con presion de
             * registros; fallback transparente a slots para ops no soportadas,
             * con la misma robustez -- vregs==slots en la suite e2e).
             * VESTA_JIT_VREGS=0 lo desactiva (fuerza slots, para A/B testing). */
            const char *vr = std::getenv("VESTA_JIT_VREGS");
            g_jit_use_vregs = !(vr && vr[0] == '0');
            /* C2 tier-up (opt-in).  VESTA_C2_THRESHOLD=N activa el tier-up con
             * umbral N; ausente o 0 = C2 apagado (default). */
            const char *c2t = std::getenv("VESTA_C2_THRESHOLD");
            if (c2t && c2t[0] != '\0') {
                char *end = nullptr;
                const unsigned long v = std::strtoul(c2t, &end, 10);
                if (end != c2t && v <= UINT32_MAX) {
                    g_c2_threshold = static_cast<uint32_t>(v);
                }
            }
            const char *c2l = std::getenv("VESTA_C2_LOG");
            if (c2l && c2l[0] != '\0' && c2l[0] != '0') g_c2_log = true;
            const char *c2a = std::getenv("VESTA_C2_TIER_ALL");
            if (c2a && c2a[0] != '\0' && c2a[0] != '0') g_c2_tier_all = true;
            const char *c2v = std::getenv("VESTA_C2_VREG");
            if (c2v && c2v[0] != '\0' && c2v[0] != '0') g_c2_vreg = true;
            /* Activar la quiescencia de enter_jit SOLO si el C2 esta on.
             * Se setea aqui (init unica, antes de cualquier enter_jit) para
             * que los pares enter/exit esten siempre balanceados. */
            g_jit_reclaim_active = (g_c2_threshold != 0);
        }

        /* Singleton lazy del JIT subsystem (compiler + cache + entries). */
        std::once_flag    g_jit_init_flag;
        CodeCache         *g_code_cache    = nullptr;
        RuntimeEntries    *g_runtime_entries = nullptr;
        JitCompiler       *g_compiler      = nullptr;
        std::mutex         g_compile_mtx;

        /* OSR (Phase D.8, 2c): tabla loop_id -> direccion del OSR-entry del C2
         * precompilado.  Se rellena en eager_compile_function tras compilar el
         * C1 de cada funcion (un C2-con-OSR-entry por loop detectado).  El
         * handler instalado en regalloc_rewrite la consulta cuando un loop
         * cruza el umbral; devolver la direccion dispara el frame-swap C1->C2.
         * Lookup-puro en runtime (sin compilar -> sin GC) => el state-transfer
         * por buffer es GC-safe (host_ptr capturados siguen frescos). */
        std::unordered_map<uint64_t, uint64_t> g_osr_entry_map;

        /** @brief Gate del C2 OPTIMIZADO del OSR (default ON).  VESTA_OSR_OPT=0
         *  -> el C2 es un recompile plano (== C1) para A/B del beneficio del
         *  inline agresivo.  Cacheado (1 getenv). */
        bool osr_opt_enabled() {
            static const bool on = []{
                const char *v = std::getenv("VESTA_OSR_OPT");
                return !(v && v[0] == '0');  // default ON; solo "0" lo apaga
            }();
            return on;
        }

        /** @brief Handler de OSR instalado via set_osr_handler.  Lookup del
         *  OSR-entry precompilado para @p loop_id (0 si no hay variante). */
        uint64_t osr_lookup_handler(uint64_t loop_id) {
            auto it = g_osr_entry_map.find(loop_id);
            return (it != g_osr_entry_map.end()) ? it->second : 0;
        }

        void init_jit_subsystem() {
            g_code_cache      = new CodeCache();
            g_runtime_entries = new RuntimeEntries();
            g_runtime_entries->resolve();
            g_compiler        = new JitCompiler(*g_code_cache, *g_runtime_entries);
            /* OSR: instalar el handler de lookup (antes de cualquier compile,
             * por tanto antes de cualquier trigger en runtime). */
            set_osr_handler(&osr_lookup_handler);
        }

        /* Construye el VregEntries desde los runtime entries resueltos. */
        VregEntries make_vreg_entries() {
            VregEntries e;
            if (g_runtime_entries) {
                e.callvirt  = reinterpret_cast<uint64_t>(g_runtime_entries->callvirt);
                e.gc_deref  = reinterpret_cast<uint64_t>(g_runtime_entries->gc_deref);
                e.gc_handle = reinterpret_cast<uint64_t>(g_runtime_entries->gc_handle_for_ptr);
                e.raw_alloc = reinterpret_cast<uint64_t>(g_runtime_entries->raw_alloc);
                e.raw_free  = reinterpret_cast<uint64_t>(g_runtime_entries->raw_free);
                e.gc_allocp = reinterpret_cast<uint64_t>(g_runtime_entries->gc_alloc_payload);
            }
            return e;
        }

        /* C2-cimiento (2026-06-07): IC slots DIRECCIONABLES por call site.
         *
         * g_ic_slots:    clave (fn_pc<<8 | ordinal) -> addr del slot PIC
         *                (64 bytes: 4 entradas [class_ptr][jit_code]).
         * g_fn_ic_keys:  fn_pc -> lista de claves de sus IC sites (CALLVIRT),
         *                para que el futuro pase C2 itere los slots de una fn
         *                caliente y lea las clases observadas (especular).
         *
         * Ambos se tocan SOLO en compile time (bajo g_compile_mtx).  El runtime
         * (vrt_callvirt_ic) escribe DENTRO de los slots, no en estos mapas.
         *
         * @c get_ic_slot(key): si key!=0 reusa el slot existente del call site
         * (estable entre recompilaciones) o lo crea; si key==0 aloca un slot
         * fresco no-direccionable (NATIVE_ABI / ICs no-PIC). */
        std::unordered_map<uint64_t, uint64_t>              g_ic_slots;
        std::unordered_map<uint64_t, std::vector<uint64_t>> g_fn_ic_keys;

        uint64_t get_ic_slot(uint64_t key) {
            if (g_code_cache == nullptr) return 0;
            if (key != 0) {
                auto it = g_ic_slots.find(key);
                if (it != g_ic_slots.end()) return it->second;
            }
            uint8_t *slot = g_code_cache->alloc(64, 64);
            if (slot == nullptr) return 0;
            std::memset(slot, 0, 64);
            const uint64_t addr = reinterpret_cast<uint64_t>(slot);
            if (key != 0) {
                g_ic_slots[key] = addr;
                g_fn_ic_keys[key >> 8].push_back(key);
            }
            return addr;
        }

        /* C2 tier-up: contador de invocaciones por funcion (keyed por fn_pc),
         * conjunto de fns ya tieradas (idempotencia), y mapa fn_pc -> MethodInfo
         * (para hacer swap de method->jit_code ademas del pc-map).  Tocados
         * bajo g_compile_mtx. */
        std::unordered_map<uint64_t, uint64_t>            g_tier_counters;
        std::unordered_set<uint64_t>                      g_tiered_pcs;
        std::unordered_map<uint64_t, loader::MethodInfo*> g_pc_to_method;
        /* fn_pc -> (code_start, code_size) del C1, para devolver la region al
         * free-list del CodeCache cuando la funcion sube a C2. */
        std::unordered_map<uint64_t, std::pair<uint8_t*, size_t>> g_c1_region;

        /* Quiescencia + free-list pendiente (reclaim C2).  g_jit_active_frames
         * es la profundidad de frames JIT en pila nativa (atomic, tocado en el
         * borde enter_jit).  g_pending_free son regiones C1 a reciclar; se
         * drenan (devuelven al CodeCache) cuando g_jit_active_frames llega a 0.
         * g_pending_free + g_pending_free_count se mutan bajo g_compile_mtx;
         * g_pending_free_count se lee lock-free en jit_frame_exit para decidir
         * si drenar (eventual-consistente: una lectura obsoleta solo difiere el
         * drain al proximo 0). */
        std::atomic<int64_t>                              g_jit_active_frames{0};
        std::vector<std::pair<uint8_t*, size_t>>          g_pending_free;
        std::atomic<size_t>                               g_pending_free_count{0};

        /* Reserva (o reusa) el contador de 8 bytes para fn_pc en el code cache
         * (mismo pool RWX que el codigo; el JIT lee/escribe estos bytes pero no
         * los ejecuta).  Zero-init.  Estable entre recompilaciones del call
         * site (mismo fn_pc -> mismo contador). */
        uint64_t reserve_tier_counter(uint64_t fn_pc) {
            if (g_code_cache == nullptr || fn_pc == 0) return 0;
            auto it = g_tier_counters.find(fn_pc);
            if (it != g_tier_counters.end()) return it->second;
            uint8_t *slot = g_code_cache->alloc(8, 8);
            if (slot == nullptr) return 0;
            std::memset(slot, 0, 8);
            const uint64_t addr = reinterpret_cast<uint64_t>(slot);
            g_tier_counters[fn_pc] = addr;
            return addr;
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

    /* C2 tier-up: handler invocado por el contador on-entry del codigo C1
     * cuando cruza el umbral.  Recompila la funcion (C2) y hace swap del
     * codigo.  Definido mas abajo; se toma su direccion en las opts. */
    void c2_tier_up(runtime::ProcessVM *vm, uint64_t fn_pc) noexcept;

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

        /* SEGURIDAD (sandbox bajo JIT): si hay un sandbox activo (algun modulo
         * con caps restringidas, M.sandbox Sprint A), NO JIT-compilar.  El
         * codigo JIT-eated emite CALLN/CALLNI/dlopen/spawn/etc. SIN el check de
         * capabilities que el interprete SI hace (check_cap_at_pc) -> dejar la
         * funcion en interp garantiza el enforcement del sandbox.  Coste cero
         * cuando no hay sandbox (1 branch predicho not-taken; sandbox_active es
         * false en el caso default).  Prerrequisito para habilitar JIT por
         * defecto sin abrir un hueco de seguridad. */
        if (vm->scheduler.vm_reference.loader_public.sandbox_active) return;

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

        /* Phase D.7 (opt-in): intentar primero el path de registros
         * virtuales.  Si la funcion es del subset soportado por el selector
         * vreg, la compila el register allocator; si no, cae al path de
         * slots de abajo (fallback transparente). */
        if (g_jit_use_vregs) {
            uint8_t *vcode = vreg_compile(*ir_fn, *g_code_cache, {}, make_vreg_entries(), {});
            if (vcode != nullptr) {
                method->jit_code = reinterpret_cast<void *>(vcode);
                if (method->code_vaddr != 0) {
                    register_jit_code_at_pc(method->code_vaddr,
                                            reinterpret_cast<void *>(vcode));
                }
                ++g_jit_compiled_count;
                if (g_jit_warn_unsupported) {
                    std::fprintf(stderr, "[jit-vreg] compilado '%s'\n", key.c_str());
                }
                return;
            }
            if (g_jit_warn_unsupported) {
                std::fprintf(stderr,
                    "[jit-vreg] '%s' no soportada -> fallback a slots\n", key.c_str());
            }
        }

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
        /* IC slots direccionables por call site (C2-cimiento).  PIC de 4
         * entradas x 16 bytes = 64 bytes, cache-line aligned, zero-init.
         * @c get_ic_slot reusa el slot del call site (clave != 0) o aloca uno
         * fresco (clave 0). */
        mc_opts.reserve_ic_slot = [](uint64_t key) -> uint64_t {
            return get_ic_slot(key);
        };
        /* C2 tier-up (opt-in): instrumentar el prologo con el contador
         * on-entry cuando C2 esta activo.  El Selector solo lo emite si la
         * funcion tiene >=1 CALLVIRT (o tier_all). */
        if (g_c2_threshold != 0) {
            mc_opts.tier_up_handler_addr =
                reinterpret_cast<uint64_t>(&c2_tier_up);
            mc_opts.tier_up_threshold = g_c2_threshold;
            mc_opts.tier_up_all_fns   = g_c2_tier_all;
            mc_opts.reserve_tier_counter =
                [](uint64_t pc) -> uint64_t { return reserve_tier_counter(pc); };
        }
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
                /* Phase D.7 (opt-in): callee por el path de registros
                 * virtuales si esta soportada; si no, slots. */
                if (g_jit_use_vregs) {
                    uint8_t *vc = vreg_compile(child_ir, *g_code_cache, {}, make_vreg_entries(), native_resolver);
                    if (vc != nullptr) {
                        const uint64_t a = reinterpret_cast<uint64_t>(vc);
                        g_eager_cache[n] = a;
                        ++g_jit_compiled_count;
                        if (st_ptr2) {
                            auto sit = st_ptr2->find("code." + n);
                            if (sit != st_ptr2->end() && sit->second != 0)
                                register_jit_code_at_pc(sit->second,
                                                        reinterpret_cast<void *>(vc));
                        }
                        if (g_jit_warn_unsupported)
                            std::fprintf(stderr,
                                "[jit-vreg] eager callee compilado '%s'\n", n.c_str());
                        return a;
                    }
                }
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
            /* C2 tier-up: mapear fn_pc (symbol "code.<name>", el mismo que el
             * contador on-entry pasa al handler) -> method + region del codigo
             * C1, para que el swap actualice method->jit_code (ademas del
             * pc-map) y para reciclar la region C1 al subir a C2. */
            if (g_c2_threshold != 0 && owning_symtab != nullptr) {
                auto sit = owning_symtab->find("code." + ir_fn->name);
                if (sit != owning_symtab->end() && sit->second != 0) {
                    g_pc_to_method[sit->second] = method;
                    if (res.code_start != nullptr && res.code_size != 0) {
                        g_c1_region[sit->second] = {
                            const_cast<uint8_t *>(res.code_start), res.code_size };
                    }
                }
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
        uint64_t jit_instr_counter_addr,
        bool callback_entry,
        uint64_t callback_get_proc_addr,
        int32_t callback_tls_gs_disp) {
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

        /* Si ya esta cacheado, retornar el ptr (excepto sentinelas).
         * En modo callback NO usamos g_eager_cache para el top-level: su
         * ABI (entry nativo) difiere del VM_ABI que la cache by-name asume;
         * confundirlos daria un caller VM_ABI saltando a un entry nativo. */
        if (!callback_entry && !ir_fn.name.empty()) {
            auto it = g_eager_cache.find(ir_fn.name);
            if (it != g_eager_cache.end()
             && it->second != 0 && it->second != EAGER_IN_PROGRESS) {
                CompileResult cached{};
                cached.fn = reinterpret_cast<JitFn>(it->second);
                return cached;
            }
        }

        /* Phase D.7 perf-gaps (2026-06-06): el intento vreg top-level se
         * MOVIO mas abajo (tras construir el resolver recursivo de user-fns),
         * para que `main` y cualquier funcion que llame a otras funciones Vex
         * compile por el path de registros virtuales en vez de caer a slots.
         * Antes este bloque corria aqui con resolve_call = {} (vacio): el
         * primer `IrOp::CALL` a una user-fn hacia bailar el vreg a slots
         * (codegen inferior).  Medido: mem_struct/mem_class/pic_real/callvirt
         * tenian su hot loop en main -> corrian por slots.  Ver el bloque
         * reubicado tras `resolver = *resolver_holder;`. */

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
            auto ic_reserver = [](uint64_t key) -> uint64_t {
                return get_ic_slot(key);
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
                /* Phase D.7 (opt-in): callee por el path de registros virtuales.
                 * Pasamos el PROPIO resolver (recursivo) para que los CALLs
                 * del callee se resuelvan a sus direcciones. */
                if (g_jit_use_vregs) {
                    uint8_t *vc = vreg_compile(child_ir, *g_code_cache, *resolver_holder, make_vreg_entries(), resolve_native_fn);
                    if (vc != nullptr) {
                        const uint64_t va = reinterpret_cast<uint64_t>(vc);
                        g_eager_cache[name] = va;
                        ++g_jit_compiled_count;
                        if (sym_resolver) {
                            const uint64_t pc = sym_resolver("code." + name);
                            if (pc != 0)
                                register_jit_code_at_pc(pc, reinterpret_cast<void *>(vc));
                        }
                        if (g_jit_warn_unsupported)
                            std::fprintf(stderr,
                                "[jit-vreg] eager callee compilado '%s'\n", name.c_str());
                        return va;
                    }
                }
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
            top_opts.reserve_ic_slot = [](uint64_t key) -> uint64_t {
                return get_ic_slot(key);
            };
        }
        /* NOTA C2: NO instrumentamos el contador on-entry en los compiles
         * top-level (eager).  Esas funciones (main + targets de CALLVM) son
         * "address-taken" (su direccion la entrego resolve_user_fn / quedan en
         * g_eager_cache) -> el reclaim de su C1 requeriria redirigir CALLs
         * directas horneadas, que no es seguro en v1.  El C2 tier-up se limita
         * a metodos alcanzados por CALLVIRT (path maybe_compile_method), cuyo
         * C1 SI es reciclable (solo refs de PIC + frames en vuelo).  La
         * generalizacion (tabla de CALL indirecta) es un follow-up. */

        /* callback-ABI: el top-level se compila con entry nativo.  El cuerpo
         * sigue siendo VM_ABI (RBX=proc) -> los callees usan el resolver
         * normal (VM_ABI), solo cambia el entry de esta funcion. */
        if (callback_entry) {
            top_opts.callback_entry = true;
            top_opts.callback_get_proc_addr = callback_get_proc_addr;
            top_opts.callback_tls_gs_disp = callback_tls_gs_disp;
        }

        /* Marcar IN_PROGRESS antes de compilar para detectar self-recursion.
         * En callback NO tocamos g_eager_cache (su ABI difiere del VM_ABI). */
        if (!callback_entry && !ir_fn.name.empty()) {
            g_eager_cache[ir_fn.name] = EAGER_IN_PROGRESS;
        }

        /* Phase D.7 perf-gaps (2026-06-06): intento vreg top-level CON el
         * resolver recursivo de user-fns ya construido.  Ahora `main` (y
         * cualquier funcion con `IrOp::CALL` a otra funcion Vex) compila por
         * el path de registros virtuales en vez de bailar a slots al primer
         * call.  El resolver compila recursivamente las callees y devuelve su
         * direccion; los CALLs del vreg emiten `CALL addr` directo.
         *
         * El path vreg NO conoce el modo callback (entry nativo); en callback
         * forzamos el path del Selector (slots + regalloc rewrite) que SI lo
         * implementa. */
        if (g_jit_use_vregs && !callback_entry) {
            uint8_t *vcode = vreg_compile(ir_fn, *g_code_cache, resolver,
                                          make_vreg_entries(), resolve_native_fn);
            if (vcode != nullptr) {
                if (!ir_fn.name.empty())
                    g_eager_cache[ir_fn.name] = reinterpret_cast<uint64_t>(vcode);
                /* NOTA: el registro pc->jit lo hace el caller
                 * (maybe_compile_callvm_target) desde el CompileResult; no lo
                 * duplicamos aqui para no registrar compiles degenerados. */
                /* --- OSR 2c: precompilar una variante C2-con-OSR-entry por cada
                 * loop que el C1 acaba de detectar para esta funcion.  El C1
                 * (vreg_compile arriba) registro sus back-edges en
                 * regalloc_rewrite (loop_ids + header_block).  Aqui, con los
                 * MISMOS resolvers, compilamos el C2 con OSR-entry y guardamos
                 * su direccion en g_osr_entry_map[loop_id].  El handler de
                 * runtime hace lookup -> dispara el frame-swap.  No-op si OSR
                 * esta off (osr_loop_count()==0).  Plano (sin opt) en 2c; la
                 * optimizacion del C2 es el paso siguiente. */
                if (!ir_fn.name.empty()) {
                    const uint32_t nloops = osr_loop_count();
                    for (uint64_t lid = 0; lid < nloops; ++lid) {
                        if (g_osr_entry_map.count(lid)) continue;  // ya precompilado
                        std::string fnm; uint32_t hdr = 0;
                        if (!osr_loop_info(lid, fnm, hdr)) continue;  // abortado/oob
                        if (fnm != ir_fn.name) continue;             // otra funcion
                        /* Red de seguridad: los vids que el C1 capturo al buffer.
                         * El OSR-entry del C2 solo puede leer estos (si su live-in
                         * pide otro, vreg_compile_osr aborta -> no swap). */
                        std::vector<uint32_t> captured;
                        osr_loop_captures(lid, captured);

                        /* --- PASO 4: C2 OPTIMIZADO ---
                         * Inline AGRESIVO de las CALLs del loop caliente (el
                         * code-size no importa: el loop domina el tiempo).  O2
                         * dejo helpers como CALL por su threshold conservador
                         * (12); aqui subimos a 256 -> elimina el CALL + el
                         * marshalling VM_ABI de args por memoria.  Block ids
                         * preservados (el inline esplicea single-block en sitio)
                         * -> el header_block del C1 sigue valido.  Gate
                         * VESTA_OSR_OPT=0 -> recompile plano (para A/B). */
                        const ir::IrFunction *compile_ir = &ir_fn;
                        ir::IrFunction opt_clone;
                        if (osr_opt_enabled() && ir_lookup != nullptr
                         && ir_functions != nullptr) {
                            opt_clone = ir_fn;  /* clon mutable */
                            ir::IrModule tmp;
                            tmp.functions.push_back(opt_clone);
                            /* Anadir las user-fns que el loop llama (para inline). */
                            std::unordered_set<std::string> added;
                            for (const auto &blk : opt_clone.blocks)
                                for (const auto &in2 : blk.instrs)
                                    if (in2.op == ir::IrOp::CALL
                                     && !in2.func_name.empty()
                                     && !added.count(in2.func_name)) {
                                        auto cl = ir_lookup->find(in2.func_name);
                                        if (cl != ir_lookup->end()
                                         && cl->second < ir_functions->size()) {
                                            tmp.functions.push_back(
                                                (*ir_functions)[cl->second]);
                                            added.insert(in2.func_name);
                                        }
                                    }
                            /* Inline agresivo + limpieza a fixpoint. */
                            bool any_opt = false;
                            for (int it = 0; it < 5; ++it) {
                                bool any = ir::ir_pass_inline(tmp, 256);
                                any = ir::ir_pass_const_fold(tmp.functions[0]) || any;
                                any = ir::ir_pass_copy_prop(tmp.functions[0])  || any;
                                any = ir::ir_pass_simplify(tmp.functions[0])   || any;
                                any = ir::ir_pass_dce(tmp.functions[0])        || any;
                                any_opt = any_opt || any;
                                if (!any) break;
                            }
                            opt_clone = std::move(tmp.functions[0]);
                            compile_ir = &opt_clone;
                            if (g_jit_warn_unsupported && any_opt)
                                std::fprintf(stderr,
                                    "[jit-osr] C2 optimizado (inline agresivo) "
                                    "para loop %llu (fn '%s')\n",
                                    static_cast<unsigned long long>(lid),
                                    ir_fn.name.c_str());
                        }

                        uint8_t *osr_entry = nullptr;
                        uint8_t *c2 = vreg_compile_osr(
                            *compile_ir, *g_code_cache, resolver,
                            make_vreg_entries(), resolve_native_fn, hdr,
                            &osr_entry, &captured);
                        if (c2 != nullptr && osr_entry != nullptr) {
                            g_osr_entry_map[lid] =
                                reinterpret_cast<uint64_t>(osr_entry);
                            if (g_jit_warn_unsupported)
                                std::fprintf(stderr,
                                    "[jit-osr] C2-entry para loop %llu (fn '%s', "
                                    "header bb%u) @ %p\n",
                                    static_cast<unsigned long long>(lid),
                                    ir_fn.name.c_str(), hdr,
                                    static_cast<void *>(osr_entry));
                        } else if (g_jit_warn_unsupported) {
                            std::fprintf(stderr,
                                "[jit-osr] loop %llu (fn '%s') no precompilo C2 "
                                "(unsupported o live-in mismatch)\n",
                                static_cast<unsigned long long>(lid),
                                ir_fn.name.c_str());
                        }
                    }
                }
                CompileResult r{};
                r.fn = reinterpret_cast<JitFn>(vcode);
                r.code_start = vcode;
                if (g_jit_warn_unsupported)
                    std::fprintf(stderr, "[jit-vreg] eager compilado '%s'\n",
                                 ir_fn.name.c_str());
                return r;
            }
            if (g_jit_warn_unsupported)
                std::fprintf(stderr,
                    "[jit-vreg] '%s' no soportada (eager) -> slots\n",
                    ir_fn.name.c_str());
        }

        const CompileResult res = g_compiler->compile_with_opts(ir_fn, top_opts);
        if (res.fn != nullptr) {
            ++g_jit_compiled_count;
            /* callback-ABI: NO cachear by-name ni registrar en el pc-map (su
             * entry es nativo, no VM_ABI).  El caller cachea por fn_pc en su
             * propia tabla.  Los callees (VM_ABI) ya se registraron normal. */
            if (!callback_entry && !ir_fn.name.empty()) {
                g_eager_cache[ir_fn.name] = reinterpret_cast<uint64_t>(res.fn);
            }
            /* D.5-callvm-hook: registrar pc -> jit en el mapa global. */
            if (!callback_entry && sym_resolver && !ir_fn.name.empty()) {
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
            if (!callback_entry && !ir_fn.name.empty()) {
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

        /* SEGURIDAD (sandbox bajo JIT): mismo guard que maybe_compile_method.
         * El codigo CALLVM JIT-eated emite CALLN/CALLNI/spawn/etc. sin el check
         * de capabilities (check_cap_at_pc); dejar la funcion en interp garantiza
         * el enforcement.  Coste cero sin sandbox (branch predicho not-taken). */
        if (vm->scheduler.vm_reference.loader_public.sandbox_active) return;

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
            CompileResult __cr = eager_compile_function(
                *ir_fn,
                &owning_exe->ir_lookup,
                &owning_exe->ir_functions,
                &owning_exe->symbol_table,
                resolve_native,
                read_vmem_cb,
                exc_off,
                exc_free_off,
                jit_counter_addr);
            /* BugFix 171 native-callback (2026-06-05): la rama VREG de
             * eager_compile_function registra el codigo SOLO por nombre
             * (g_eager_cache), NO por PC.  El path slot si registra por PC
             * via la registracion top-level, pero el vreg retornaba antes.
             * Sin la entrada pc-map, `lookup_jit_code_at_pc(target_pc)`
             * devuelve null y `as_native_callback` (que busca por PC) genera
             * un thunk=0 -> qsort recibe comparator NULL -> no ordena.
             * Registramos aqui el resultado top-level por PC para cubrir
             * ambos paths (idempotente: register sobrescribe con el mismo fn). */
            if (__cr.fn != nullptr) {
                register_jit_code_at_pc(target_pc,
                                        reinterpret_cast<void *>(__cr.fn));
            }
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

    /* ===================================================================== */
    /* compile_native_callback: entry de ABI C nativo para callbacks Vex      */
    /* ===================================================================== */

    uint64_t compile_native_callback(runtime::ProcessVM *vm,
                                     uint64_t vex_fn_pc) noexcept {
        if (vm == nullptr) return 0;

        /* Cache propia por PC: la misma fn puede tener version VM_ABI (pc-map)
         * y version callback-ABI (esta tabla).  Sentinela 0 = aun no/ fallo. */
        static std::mutex g_cb_mtx;
        static std::unordered_map<uint64_t, uint64_t> g_cb_cache;
        {
            std::lock_guard<std::mutex> lk(g_cb_mtx);
            auto it = g_cb_cache.find(vex_fn_pc);
            if (it != g_cb_cache.end()) return it->second;
        }

        /* Buscar el IrFunction + executable propietario para este PC.
         * Mismo procedimiento que @c maybe_compile_callvm_target. */
        runtime::VM &owning_vm = vm->scheduler.vm_reference;
        const ir::IrFunction *ir_fn = nullptr;
        const loader::Executable *owning_exe = nullptr;
        {
            /* g_exec_pc_to_name esta protegido por g_callvm_mtx (mismo que
             * maybe_compile_callvm_target).  NO usar g_compile_mtx aqui: lo
             * tomara eager_compile_function despues -> deadlock. */
            std::lock_guard<std::mutex> cmlk(g_callvm_mtx);
            for (auto &exe_ptr : owning_vm.loader_public.executables) {
                const loader::Executable *exe = exe_ptr.get();
                auto &pc_to_name = g_exec_pc_to_name[exe];
                if (pc_to_name.empty() && !exe->symbol_table.empty()) {
                    for (const auto &kv : exe->symbol_table) {
                        if (kv.first.rfind("code.", 0) == 0) {
                            pc_to_name[kv.second] = kv.first.substr(5);
                        }
                    }
                }
                auto pn_it = pc_to_name.find(vex_fn_pc);
                if (pn_it == pc_to_name.end()) continue;
                std::string candidate_name = pn_it->second;
                /* Strip sufijo `_entry_<N>` igual que maybe_compile_callvm_target. */
                {
                    const std::string suffix = "_entry_";
                    auto pos = candidate_name.rfind(suffix);
                    if (pos != std::string::npos) {
                        bool only_digits = true;
                        for (size_t k = pos + suffix.size(); k < candidate_name.size(); ++k) {
                            if (candidate_name[k] < '0' || candidate_name[k] > '9') {
                                only_digits = false; break;
                            }
                        }
                        if (only_digits) candidate_name = candidate_name.substr(0, pos);
                    }
                }
                auto lit = exe->ir_lookup.find(candidate_name);
                if (lit == exe->ir_lookup.end()) continue;
                if (lit->second >= exe->ir_functions.size()) continue;
                ir_fn = &exe->ir_functions[lit->second];
                owning_exe = exe;
                break;
            }
        }
        if (ir_fn == nullptr || owning_exe == nullptr) {
            std::lock_guard<std::mutex> lk(g_cb_mtx);
            g_cb_cache[vex_fn_pc] = 0;
            return 0;
        }

        /* Callbacks de resolucion identicos a maybe_compile_callvm_target. */
        ffi::FFI *ffi_ptr = &owning_vm.loader_public.ffi_loader;
        auto resolve_native = [ffi_ptr](const std::string &name) -> uint64_t {
            size_t colon = name.find(':');
            if (colon == std::string::npos) return 0;
            std::string lib  = name.substr(0, colon);
            std::string func = name.substr(colon + 1);
            void *vfn = ffi::lookup_virtual_fn(lib, func);
            if (vfn != nullptr) return reinterpret_cast<uint64_t>(vfn);
            try {
                void *mod = ffi_ptr->load_native_module(lib);
                if (!mod) return 0;
                void *fn = ffi_ptr->resolve_native_symbol(mod, func);
                return reinterpret_cast<uint64_t>(fn);
            } catch (...) { return 0; }
        };
        runtime::ProcessVM *proc_for_read = vm;
        auto read_vmem_cb = [proc_for_read](uint64_t vaddr) -> uint64_t {
            try { return proc_for_read->vm_mem.read_u64(vaddr); }
            catch (...) { return 0; }
        };
        int32_t exc_off = 0, exc_free_off = 0;
        {
            const int64_t off64 =
                reinterpret_cast<int64_t>(&vm->exc_frame_stack) - reinterpret_cast<int64_t>(vm);
            const int64_t free64 =
                reinterpret_cast<int64_t>(&vm->exc_free_list) - reinterpret_cast<int64_t>(vm);
            if (off64  >= INT32_MIN && off64  <= INT32_MAX) exc_off      = static_cast<int32_t>(off64);
            if (free64 >= INT32_MIN && free64 <= INT32_MAX) exc_free_off = static_cast<int32_t>(free64);
        }
        const uint64_t jit_counter_addr = g_jit_emit_instr_counter
            ? reinterpret_cast<uint64_t>(&vm->scheduler.profiler_jit_instr_counter)
            : 0;

        /* Resolver TLS-direct (Win64) o fallback por call para LOAD_PROC. */
        const uint64_t getproc_addr =
            reinterpret_cast<uint64_t>(&runtime::get_current_executing_process);
        int32_t tls_gs_disp = -1;
#if defined(_WIN32)
        {
            const unsigned long idx = runtime::jit_proc_tls_index();
            if (idx != 0xFFFFFFFFu && idx < 64) {
                tls_gs_disp = static_cast<int32_t>(0x1480u + idx * 8u);
            }
        }
#endif

        /* El callback REQUIERE codigo nativo AHORA (su direccion va a Win32/
         * qsort).  Forzar el compile aunque el JIT este desactivado por flag
         * (-m vm): salvar/forzar/restaurar el threshold global, como hacia el
         * wrapper del thunk previo. */
        uint64_t result = 0;
        try {
            const uint32_t saved_thr = g_jit_threshold;
            if (saved_thr == UINT32_MAX) set_jit_threshold(1);
            CompileResult cr = eager_compile_function(
                *ir_fn,
                &owning_exe->ir_lookup,
                &owning_exe->ir_functions,
                &owning_exe->symbol_table,
                resolve_native,
                read_vmem_cb,
                exc_off,
                exc_free_off,
                jit_counter_addr,
                /*callback_entry=*/true,
                getproc_addr,
                tls_gs_disp);
            if (saved_thr == UINT32_MAX) set_jit_threshold(saved_thr);
            result = cr.fn ? reinterpret_cast<uint64_t>(cr.fn) : 0;
            if (g_jit_warn_unsupported) {
                std::fprintf(stderr,
                    "[jit] native-callback pc=0x%llx -> %s\n",
                    static_cast<unsigned long long>(vex_fn_pc),
                    result ? "compiled" : "unsupported");
            }
        } catch (...) {
            result = 0;
        }

        std::lock_guard<std::mutex> lk(g_cb_mtx);
        g_cb_cache[vex_fn_pc] = result;
        return result;
    }

    /* ===================================================================== */
    /* C2 reclaim: quiescencia (enter_jit) + drain del free-list             */
    /* ===================================================================== */

    namespace {
        /* Devuelve las regiones C1 pendientes al free-list del CodeCache.
         * Solo se invoca con g_jit_active_frames == 0 (ningun frame JIT en
         * pila nativa) -> las regiones son inalcanzables (PIC limpiado,
         * dispatch reapuntado a C2, frames en vuelo drenados). */
        void drain_pending_free() {
            std::lock_guard<std::mutex> lk(g_compile_mtx);
            size_t freed = 0;
            if (!g_pending_free.empty() && g_code_cache != nullptr) {
                for (auto &r : g_pending_free) {
                    g_code_cache->free_region(r.first, r.second);
                    ++freed;
                }
            }
            g_pending_free.clear();
            g_pending_free_count.store(0, std::memory_order_release);
            if (g_c2_log && freed != 0) {
                std::fprintf(stderr,
                    "[c2] reclaim: %zu region(es) C1 liberada(s) al free-list "
                    "(quiescencia); free-list ahora = %zu\n",
                    freed, g_code_cache->free_region_count());
            }
        }
    } // namespace anonymous

    void jit_frame_enter() noexcept {
        g_jit_active_frames.fetch_add(1, std::memory_order_acq_rel);
    }

    void jit_frame_exit() noexcept {
        /* fetch_sub devuelve el valor PREVIO; == 1 significa que acabamos de
         * dejarlo en 0 -> instante sin ningun frame JIT en pila.  Si hay
         * regiones pendientes, es seguro reciclarlas ahora. */
        if (g_jit_active_frames.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            if (g_pending_free_count.load(std::memory_order_acquire) != 0) {
                drain_pending_free();
            }
        }
    }

    /* ===================================================================== */
    /* C2 tier-up: recompile-and-swap                                         */
    /* ===================================================================== */
    /*
     * Invocado por el contador on-entry del codigo C1 al cruzar el umbral.
     * (1) resuelve fn_pc -> IrFunction (igual que maybe_compile_callvm_target),
     * (2) [PoC C2.1/C2.2] lee los IC slots del call site y loguea las clases
     *     observadas (frio / monomorfico / polimorfico / megamorfico) si
     *     VESTA_C2_LOG=1,
     * (3) recompila la funcion por el path Selector con opts C2 (mismas que C1
     *     pero SIN contador -> codigo terminal), reusando los IC slots
     *     calientes; las callees ya estan compiladas (resolver no-recursivo via
     *     g_eager_cache),
     * (4) swap atomico: pc-map (dispatch CALLVM) + method->jit_code (CALLVIRT).
     *
     * El codigo C1 viejo NO se libera (leak aceptable v1, sin use-after-free:
     * los frames en curso y las entradas de PIC que lo referencian siguen
     * siendo validos).  Idempotente via g_tiered_pcs.  El recompile corre en el
     * host stack del frame JIT que disparo el contador; no ejecuta codigo Vex
     * -> no hay GC ni re-entrancia de compile (g_compile_mtx no lo tiene este
     * thread).
     */
    void c2_tier_up(runtime::ProcessVM *vm, uint64_t fn_pc) noexcept {
        if (vm == nullptr || fn_pc == 0) return;
        try {
            /* --- Resolucion pc -> IR (bajo g_callvm_mtx, orden consistente
             *     con maybe_compile_callvm_target). --- */
            runtime::VM &owning_vm = vm->scheduler.vm_reference;
            const ir::IrFunction *ir_fn = nullptr;
            const loader::Executable *owning_exe = nullptr;
            std::string fn_name;
            {
                std::lock_guard<std::mutex> cmlk(g_callvm_mtx);
                for (auto &exe_ptr : owning_vm.loader_public.executables) {
                    const loader::Executable *exe = exe_ptr.get();
                    auto &pc_to_name = g_exec_pc_to_name[exe];
                    if (pc_to_name.empty() && !exe->symbol_table.empty()) {
                        for (const auto &kv : exe->symbol_table) {
                            if (kv.first.rfind("code.", 0) == 0)
                                pc_to_name[kv.second] = kv.first.substr(5);
                        }
                    }
                    auto pn_it = pc_to_name.find(fn_pc);
                    if (pn_it == pc_to_name.end()) continue;
                    std::string cand = pn_it->second;
                    /* strip sufijo _entry_<N> (igual que las otras rutas). */
                    {
                        const std::string suffix = "_entry_";
                        auto pos = cand.rfind(suffix);
                        if (pos != std::string::npos) {
                            bool only_digits = true;
                            for (size_t k = pos + suffix.size(); k < cand.size(); ++k)
                                if (cand[k] < '0' || cand[k] > '9') { only_digits = false; break; }
                            if (only_digits) cand = cand.substr(0, pos);
                        }
                    }
                    auto lit = exe->ir_lookup.find(cand);
                    if (lit == exe->ir_lookup.end() || lit->second >= exe->ir_functions.size())
                        continue;
                    ir_fn = &exe->ir_functions[lit->second];
                    owning_exe = exe;
                    fn_name = cand;
                    break;
                }
            }
            if (ir_fn == nullptr || owning_exe == nullptr) return;

            std::lock_guard<std::mutex> lk(g_compile_mtx);
            /* Idempotencia: solo una vez por fn_pc.  La insercion ocurre ya
             * (incluso si saltamos abajo) para que el contador on-entry no
             * vuelva a invocar el handler. */
            if (g_tiered_pcs.count(fn_pc)) return;
            g_tiered_pcs.insert(fn_pc);
            std::call_once(g_jit_init_flag, init_jit_subsystem);

            /* SKIP address-taken: si la direccion de esta funcion fue
             * entregada por resolve_user_fn (esta en g_eager_cache), hay CALLs
             * directas con imm horneado a su C1 que NO podemos redirigir sin
             * recompilar a los callers.  Liberar su C1 seria use-after-free.
             * En v1 NO la subimos a C2 (asi no se crea ningun C1 huerfano ->
             * cero leak).  El target reclaimable son metodos via CALLVIRT
             * (no quedan en g_eager_cache).  La generalizacion (tabla de CALL
             * indirecta) es un follow-up. */
            {
                auto eit = g_eager_cache.find(fn_name);
                if (eit != g_eager_cache.end()
                 && eit->second != 0 && eit->second != EAGER_IN_PROGRESS) {
                    if (g_c2_log)
                        std::fprintf(stderr,
                            "[c2] skip-tier '%s' (address-taken) -- queda C1 (sin leak)\n",
                            fn_name.c_str());
                    return;
                }
            }

            /* --- (2) PoC C2.1/C2.2: leer IC slots + loguear clases. --- */
            if (g_c2_log) {
                auto kit = g_fn_ic_keys.find(fn_pc);
                if (kit == g_fn_ic_keys.end() || kit->second.empty()) {
                    std::fprintf(stderr,
                        "[c2] tier-up '%s' (pc=0x%llx): sin IC sites\n",
                        fn_name.c_str(),
                        static_cast<unsigned long long>(fn_pc));
                } else {
                    for (uint64_t key : kit->second) {
                        auto sit = g_ic_slots.find(key);
                        if (sit == g_ic_slots.end() || sit->second == 0) continue;
                        const uint64_t *slot =
                            reinterpret_cast<const uint64_t *>(sit->second);
                        int distinct = 0;
                        for (int e = 0; e < 4; ++e)
                            if (slot[e * 2] != 0) ++distinct;
                        const char *kind = (distinct == 0) ? "frio"
                                         : (distinct == 1) ? "monomorfico"
                                         : (distinct < 4)  ? "polimorfico"
                                                           : "megamorfico";
                        std::fprintf(stderr,
                            "[c2] call site %s:%u -> %s (clases distintas=%d)\n",
                            fn_name.c_str(),
                            static_cast<unsigned>(key & 0xFFu), kind, distinct);
                    }
                }
            }

            /* --- (3) recompile C2 por el path Selector (terminal). --- */
            SelectorOptions c2;
            c2.mode = SelectorMode::VM_ABI;
            c2.runtime = g_runtime_entries;
            c2.safepoint_handler_addr =
                reinterpret_cast<uint64_t>(g_runtime_entries->safepoint_handler);
            const auto *st_ptr = &owning_exe->symbol_table;
            c2.resolve_symbol = [st_ptr](const std::string &n) -> uint64_t {
                auto it = st_ptr->find(n);
                return it == st_ptr->end() ? 0 : it->second;
            };
            c2.reserve_ic_slot = [](uint64_t key) -> uint64_t {
                return get_ic_slot(key);  /* reusa el PIC caliente */
            };
            ffi::FFI *ffi_ptr = &owning_vm.loader_public.ffi_loader;
            c2.resolve_native_fn = [ffi_ptr](const std::string &name) -> uint64_t {
                size_t colon = name.find(':');
                if (colon == std::string::npos) return 0;
                std::string lib  = name.substr(0, colon);
                std::string func = name.substr(colon + 1);
                void *vfn = ffi::lookup_virtual_fn(lib, func);
                if (vfn != nullptr) return reinterpret_cast<uint64_t>(vfn);
                try {
                    void *mod = ffi_ptr->load_native_module(lib);
                    if (!mod) return 0;
                    return reinterpret_cast<uint64_t>(
                        ffi_ptr->resolve_native_symbol(mod, func));
                } catch (...) { return 0; }
            };
            runtime::ProcessVM *proc = vm;
            c2.read_vmem_u64 = [proc](uint64_t v) -> uint64_t {
                try { return proc->vm_mem.read_u64(v); } catch (...) { return 0; }
            };
            /* Callees ya compiladas en C1 -> resolver NO-recursivo via cache. */
            c2.resolve_user_fn = [](const std::string &n) -> uint64_t {
                auto it = g_eager_cache.find(n);
                if (it != g_eager_cache.end()
                 && it->second != 0 && it->second != EAGER_IN_PROGRESS)
                    return it->second;
                return 0;
            };
            /* Offsets de nursery + register_alloc para inline de NEWOBJ. */
            {
                const int64_t nb = reinterpret_cast<int64_t>(&proc->gc_heap.nursery_bump_)
                                 - reinterpret_cast<int64_t>(proc);
                const int64_t ne = reinterpret_cast<int64_t>(&proc->gc_heap.nursery_end_)
                                 - reinterpret_cast<int64_t>(proc);
                if (nb >= INT32_MIN && nb <= INT32_MAX && ne >= INT32_MIN && ne <= INT32_MAX) {
                    c2.nursery_bump_offset = static_cast<int32_t>(nb);
                    c2.nursery_end_offset  = static_cast<int32_t>(ne);
                    c2.register_alloc_addr = reinterpret_cast<uint64_t>(&vrt_register_alloc);
                }
            }
            /* Offsets de exc para inline de tryleave. */
            {
                const int64_t eo = reinterpret_cast<int64_t>(&proc->exc_frame_stack)
                                 - reinterpret_cast<int64_t>(proc);
                const int64_t fo = reinterpret_cast<int64_t>(&proc->exc_free_list)
                                 - reinterpret_cast<int64_t>(proc);
                if (eo >= INT32_MIN && eo <= INT32_MAX) c2.exc_frame_stack_offset = static_cast<int32_t>(eo);
                if (fo >= INT32_MIN && fo <= INT32_MAX) c2.exc_free_list_offset   = static_cast<int32_t>(fo);
            }
            c2.jit_instr_counter_addr = g_jit_emit_instr_counter
                ? reinterpret_cast<uint64_t>(&proc->scheduler.profiler_jit_instr_counter)
                : 0;
            /* NO tier_up_* -> el codigo C2 es terminal (no se re-instrumenta). */

            /* --- C2.4: devirt especulativa de los IC sites monomorficos. ---
             * Para cada CALLVIRT cuyo PIC observo EXACTAMENTE 1 clase, emite
             * un guard (clase == T) ? CALL directo : CALLVIRT, e inlinea el
             * CALL (si el callee es 1-bloque-RET) + plega.  Correcto por
             * construccion: el callee se resuelve de C->vtable[vtbl_idx] con
             * la MISMA C del guard, asi que si el guard pasa el target es
             * exacto; si no, cae al CALLVIRT original. */
            const ir::IrFunction *compile_target = ir_fn;
            ir::IrFunction spec_clone;
            {
                /* ordinal -> (dst, vtbl_idx) recorriendo los CALLVIRT en el
                 * mismo orden en que el Selector asigno las claves de IC. */
                std::vector<std::pair<ir::IrValueId, uint64_t>> cv_by_ord;
                for (const auto &bb : ir_fn->blocks)
                    for (const auto &ins : bb.instrs)
                        if (ins.op == ir::IrOp::CALLVIRT)
                            cv_by_ord.push_back({ins.dst, ins.imm});

                std::vector<ir::SpecDevirtSite> sites;
                std::unordered_set<std::string> callee_names;
                auto kit2 = g_fn_ic_keys.find(fn_pc);
                if (kit2 != g_fn_ic_keys.end()) {
                    for (uint64_t key : kit2->second) {
                        const uint32_t ord = static_cast<uint32_t>(key & 0xFFu);
                        if (ord >= cv_by_ord.size()) continue;
                        auto sit = g_ic_slots.find(key);
                        if (sit == g_ic_slots.end() || sit->second == 0) continue;
                        const uint64_t *slot =
                            reinterpret_cast<const uint64_t *>(sit->second);
                        int distinct = 0; uint64_t cptr = 0;
                        for (int e = 0; e < 4; ++e)
                            if (slot[e * 2] != 0) { ++distinct; if (distinct == 1) cptr = slot[e * 2]; }
                        if (distinct != 1 || cptr == 0) continue;   /* solo monomorfico */
                        const ir::IrValueId cv_dst = cv_by_ord[ord].first;
                        const uint64_t vtbl_idx    = cv_by_ord[ord].second;
                        if (cv_dst == ir::IR_NO_VALUE) continue;     /* void: sin PHI */
                        loader::ClassInfo *cls = reinterpret_cast<loader::ClassInfo *>(cptr);
                        if (!cls->vtable || vtbl_idx >= cls->vtable_size) continue;
                        loader::MethodInfo *m = cls->vtable[vtbl_idx];
                        if (!m || !m->name.data || m->name.size == 0) continue;
                        std::string cn = (cls->name.data && cls->name.size)
                            ? std::string(reinterpret_cast<const char *>(cls->name.data), cls->name.size)
                            : std::string();
                        std::string mn(reinterpret_cast<const char *>(m->name.data), m->name.size);
                        std::string callee = cn + "__" + mn;
                        /* callee debe existir y ser inlineable (1 bloque, RET):
                         * si no, el CALL del fast-path quedaria sin resolver. */
                        auto cl = owning_exe->ir_lookup.find(callee);
                        if (cl == owning_exe->ir_lookup.end()
                         || cl->second >= owning_exe->ir_functions.size()) continue;
                        const ir::IrFunction &cf = owning_exe->ir_functions[cl->second];
                        if (cf.blocks.size() != 1 || cf.blocks[0].instrs.empty()
                         || cf.blocks[0].instrs.back().op != ir::IrOp::RET) continue;
                        sites.push_back(ir::SpecDevirtSite{cv_dst, cptr, callee});
                        callee_names.insert(callee);
                    }
                }

                if (!sites.empty()) {
                    spec_clone = *ir_fn;  /* clon mutable */
                    if (ir::ir_pass_speculative_devirt(spec_clone, sites)) {
                        ir::IrModule tmp;
                        tmp.functions.push_back(spec_clone);
                        for (const auto &cn : callee_names) {
                            auto cl = owning_exe->ir_lookup.find(cn);
                            if (cl != owning_exe->ir_lookup.end()
                             && cl->second < owning_exe->ir_functions.size())
                                tmp.functions.push_back(owning_exe->ir_functions[cl->second]);
                        }
                        /* inline del CALL fast-path + limpieza a fixpoint. */
                        for (int it = 0; it < 5; ++it) {
                            bool any = ir::ir_pass_inline(tmp);
                            any = ir::ir_pass_const_fold(tmp.functions[0]) || any;
                            any = ir::ir_pass_copy_prop(tmp.functions[0])  || any;
                            any = ir::ir_pass_simplify(tmp.functions[0])   || any;
                            any = ir::ir_pass_dce(tmp.functions[0])        || any;
                            if (!any) break;
                        }
                        spec_clone = std::move(tmp.functions[0]);
                        compile_target = &spec_clone;
                        if (g_c2_log)
                            std::fprintf(stderr,
                                "[c2] devirt especulativa: %zu site(s) monomorfico(s) en '%s'\n",
                                sites.size(), fn_name.c_str());
                    }
                }
            }

            /* Compilar el IR especulado.  Por defecto via el Selector (slots);
             * con VESTA_C2_VREG via el path de registros virtuales (regalloc
             * real) -- experimento para medir si el inline rinde cuando los
             * valores viven en registros y no en slots. */
            JitFn    c2_fn         = nullptr;
            size_t   c2_size       = 0;
            const uint8_t *c2_code = nullptr;
            bool     c2_unsupported = false;
            if (g_c2_vreg) {
                ffi::FFI *ffi2 = &owning_vm.loader_public.ffi_loader;
                auto nat_res = [ffi2](const std::string &name) -> uint64_t {
                    size_t colon = name.find(':');
                    if (colon == std::string::npos) return 0;
                    std::string lib  = name.substr(0, colon);
                    std::string func = name.substr(colon + 1);
                    void *vfn = ffi::lookup_virtual_fn(lib, func);
                    if (vfn) return reinterpret_cast<uint64_t>(vfn);
                    try {
                        void *m = ffi2->load_native_module(lib);
                        if (!m) return 0;
                        return reinterpret_cast<uint64_t>(ffi2->resolve_native_symbol(m, func));
                    } catch (...) { return 0; }
                };
                auto user_res = [](const std::string &n) -> uint64_t {
                    auto it = g_eager_cache.find(n);
                    if (it != g_eager_cache.end()
                     && it->second != 0 && it->second != EAGER_IN_PROGRESS)
                        return it->second;
                    return 0;
                };
                uint8_t *vc = vreg_compile(*compile_target, *g_code_cache, user_res,
                                           make_vreg_entries(), nat_res);
                if (vc != nullptr) { c2_fn = reinterpret_cast<JitFn>(vc); c2_code = vc; }
            }
            CompileResult res{};
            if (c2_fn == nullptr) {
                res = g_compiler->compile_with_opts(*compile_target, c2);
                c2_fn = res.fn; c2_size = res.code_size; c2_code = res.code_start;
                c2_unsupported = res.unsupported;
            }
            if (c2_fn == nullptr) {
                /* No se pudo recompilar (alguna op sin contexto recursivo).
                 * Queda C1; g_tiered_pcs evita reintentos. */
                if (g_c2_log)
                    std::fprintf(stderr,
                        "[c2] recompile '%s' fallo (unsupported=%d) -- queda C1\n",
                        fn_name.c_str(), c2_unsupported ? 1 : 0);
                return;
            }

            /* --- (4) swap + reclaim sin leak. ---
             *
             * (a) Capturar el entry C1 (lo que el PIC cachea) ANTES del swap.
             * (b) Reapuntar dispatch a C2: pc-map (CALLVM) + method->jit_code
             *     (CALLVIRT).  Son stores de puntero; los lectores ven C1 o C2,
             *     ambos validos.
             * (c) Invalidar las entradas de PIC que cachean C1: poner SOLO el
             *     class_ptr a 0 (store de 8 bytes alineado = atomico en x86).
             *     Un lector concurrente del PIC o (i) ve class=0 -> miss ->
             *     re-popula con C2, o (ii) ve la class vieja -> carga el code
             *     viejo (C1, AUN VIVO) -> lo llama sin crash.  NO tocamos el
             *     campo code; el class=0 ya basta para que nadie nuevo entre.
             * (d) Encolar la region C1 a pending-free; se LIBERA recien en la
             *     quiescencia (g_jit_active_frames==0, drain en jit_frame_exit)
             *     cuando NO hay frames en vuelo -> ningun lector la ejecuta y
             *     ningun frame la tiene en pila.  Cero leak, cero
             *     use-after-free. */
            auto mit = g_pc_to_method.find(fn_pc);
            loader::MethodInfo *method =
                (mit != g_pc_to_method.end()) ? mit->second : nullptr;
            const uint64_t old_code = method
                ? reinterpret_cast<uint64_t>(method->jit_code)
                : reinterpret_cast<uint64_t>(lookup_jit_code_at_pc(fn_pc));

            register_jit_code_at_pc(fn_pc, reinterpret_cast<void *>(c2_fn));
            if (method) method->jit_code = reinterpret_cast<void *>(c2_fn);

            /* (c) invalidar entradas de PIC == old_code (class_ptr -> 0). */
            if (old_code != 0) {
                for (auto &kv : g_ic_slots) {
                    uint64_t *slot = reinterpret_cast<uint64_t *>(kv.second);
                    if (slot == nullptr) continue;
                    for (int e = 0; e < 4; ++e) {
                        if (slot[e * 2 + 1] == old_code) {
                            slot[e * 2] = 0;  /* class_ptr: future miss -> C2 */
                        }
                    }
                }
            }

            /* (d) encolar la region C1 para reciclar en la quiescencia. */
            auto rit = g_c1_region.find(fn_pc);
            if (rit != g_c1_region.end()) {
                g_pending_free.push_back(rit->second);
                g_pending_free_count.store(g_pending_free.size(),
                                           std::memory_order_release);
                g_c1_region.erase(rit);
            }

            ++g_c2_tierup_count;
            if (g_c2_log) {
                std::fprintf(stderr,
                    "[c2] tier-up '%s' (pc=0x%llx) -> C2 (%zu bytes, %s); C1 a free-list pendiente\n",
                    fn_name.c_str(), static_cast<unsigned long long>(fn_pc),
                    c2_size, g_c2_vreg ? "vreg" : "slots");
            }
            if (g_jit_disasm && c2_code != nullptr && c2_size != 0) {
                debug_dump_jit_code(fn_name + " [C2]", c2_code, c2_size);
            }
        } catch (...) {
            /* Cualquier fallo deja la funcion en C1 (correcto). */
        }
    }

} // namespace jit
