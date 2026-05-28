/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (c) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribucion obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file gc_heap.h
 * @brief Heap generacional de recoleccion de basura por proceso.
 *
 * @section architecture Arquitectura del GcHeap
 *
 * El GcHeap implementa un recolector de basura generacional de dos generaciones
 * (Young/Old) con los siguientes componentes:
 *
 *   Nursery (Young generation)
 *   --------------------------
 *   Bloque contiguo de memoria host asignado via ArenaManager al construir el
 *   heap. La asignacion usa un bump-pointer (puntero de avance lineal), lo que
 *   la hace O(1) y cache-friendly. Cuando el bump-pointer alcanza el limite del
 *   bloque se dispara el minor GC.
 *
 *   OldGen (Old generation)
 *   -----------------------
 *   Conjunto de bloques asignados dinamicamente via ArenaManager conforme se
 *   necesitan. Cada bloque es un array lineal de objetos precedidos por GcHeader.
 *   La asignacion usa first-fit: primero busca slots DEAD reutilizables; si no
 *   hay, avanza al final del ultimo bloque (bump); si tampoco hay espacio pide
 *   un bloque nuevo al ArenaManager.
 *
 *   HandleTable
 *   -----------
 *   Vector de HandleEntry que mapea un GcHandle (uint32_t opaco) a la direccion
 *   host actual del objeto. Permite que el GC mueva objetos durante la evacuacion
 *   sin invalidar ninguna referencia en registros o stack del bytecode, ya que
 *   todo el bytecode usa handles, nunca punteros directos.
 *   Slots libres se reciclan via free_handles_ (freelist LIFO).
 *
 *   RememberedSet
 *   -------------
 *   Vector de GcHandle de objetos OLD que contienen referencias a objetos YOUNG.
 *   Necesario para que el minor GC sea completo sin escanear OldGen entero.
 *   El programador (o el compilador del lenguaje Vesta) debe llamar a
 *   write_barrier() cada vez que escribe una referencia old->young.
 *
 * @section lifecycle Ciclo de vida de un objeto
 *
 * @code
 *   1. NEWOBJ  --> alloc() --> objeto en Nursery, color=WHITE, gen=YOUNG, handle H.
 *   2. [uso]   --> deref(H) devuelve puntero al payload.
 *   3. [muerte]--> drop(H)  --> handle liberado; objeto permanece fisicamente
 *                               en Nursery/OldGen hasta el proximo GC.
 *   4. minor_gc()
 *        - Evacua objetos YOUNG con handle vivo --> OldGen, color=BLACK.
 *        - Objetos YOUNG sin handle (soltados) simplemente se abandonan;
 *          el nursery_bump_ se resetea, reclamando todo el espacio de golpe.
 *   5. major_gc()
 *        - PRE-MARK: todos los objetos OldGen no-DEAD --> WHITE.
 *        - MARK:     handles vivos en OldGen --> BLACK.
 *        - SWEEP:    WHITE --> DEAD (slot reutilizable, old_used_ decrementado).
 * @endcode
 *
 * @section tri_color Algoritmo tri-color
 *
 *   WHITE  -- no visitado; candidato a recolectar si sigue WHITE tras MARK.
 *   GREY   -- alcanzable pero referencias pendientes de procesar (reservado).
 *   BLACK  -- alcanzable y completamente procesado; sobrevive este ciclo.
 *   DEAD   -- liberado por sweep; slot fisico disponible para reutilizacion.
 *             El campo GcHeader::size se preserva para que el scanner pueda
 *             avanzar correctamente y alloc_in_old pueda calcular el tamano.
 *
 * @section stats Estadisticas sin overhead
 *
 * Todos los contadores de GcStats son plain uint64_t (sin atomics ni ramas
 * adicionales). Cada evento incrementa exactamente un campo, y el CPU ya tiene
 * el objeto GcHeap en L1 cache durante la asignacion. El costo adicional es
 * de una instruccion ADD por evento en el hot-path.
 *
 * @section thread_safety Seguridad de hilos
 *
 * GcHeap NO es thread-safe. Esta disenado para uso exclusivo del proceso
 * propietario (un ProcessVM). Si el scheduler migra el proceso a otro hilo
 * debe garantizar exclusion mutua antes de llamar a cualquier metodo del heap.
 */

#ifndef GC_HEAP_H
#define GC_HEAP_H

#include <cstdint>
#include <cstddef>
#include <vector>
#include <unordered_set>

#include "arena/VirtualMemory.h"
#include "arena/arena_manager.h"
#include "gc/wait_table.h"  // WaitTable (lock-free per-bucket)

// forward decl para no incluir el header completo (incluiria
// gc_heap.h indirectamente).  El puntero al owner se usa en major_gc para
// invocar scan_stack_roots con el rsp/stack_high/regs del proceso.
namespace runtime { class ProcessVM; }

namespace gc {
    /**
     * @brief Handle opaco para referenciar objetos gestionados por el GcHeap.
     *
     * El bytecode de Vesta usa GcHandle en lugar de punteros directos. Esto
     * desacopla las referencias del proceso de la ubicacion fisica del objeto,
     * permitiendo que el GC mueva objetos (compactacion/evacuacion) sin
     * invalidar ningun registro de la VM.
     *
     * Un GcHandle es simplemente un indice en la HandleTable interna del heap.
     * El valor GC_NULL_HANDLE (UINT32_MAX) representa una referencia nula.
     *
     * @note Un GcHandle no es un puntero. Nunca hacer reinterpret_cast sobre el.
     */
    using GcHandle = uint32_t;

    /** @brief Valor centinela que representa un handle nulo o invalido. */
    static constexpr GcHandle GC_NULL_HANDLE = UINT32_MAX;

    /**
     * @class PtrHandleMap
     * @brief Hash table flat open-addressing optimizada para
     *        @c uint8_t* -> @c GcHandle.
     *
     * Reemplaza @c std::unordered_map<const uint8_t*, GcHandle> para el
     * hot path del GC: inserts en @c new_handle, erases en
     * @c release_handle / minor_gc sweep, finds en @c scan_stack_roots.
     * std::unordered_map usa node-based allocation (1 malloc por insert
     * ~10-15 ns).  Esta variante usa un solo @c std::vector<Entry>
     * contiguo (cache-friendly + cero allocs por insert despues del
     * grow inicial).
     *
     * = Algoritmo =
     * - Buckets potencia de 2 (mask = capacity - 1).
     * - Hash: @c (ptr >> 3) (los punteros host son 8-aligned).
     * - Linear probing.
     * - Tombstones para erase (key == reinterpret_cast<uint8_t*>(1)).
     * - Resize a 2x cuando load > 0.5.
     *
     * Coste medio por operacion: ~5 ns vs ~20-30 ns de @c unordered_map.
     */
    class PtrHandleMap {
    public:
        struct Entry {
            const uint8_t *key;
            GcHandle       value;
        };

        PtrHandleMap();
        ~PtrHandleMap() = default;

        /** @brief Inserta o sobreescribe.  Retorna true si era nuevo. */
        bool insert_or_assign(const uint8_t *key, GcHandle h);

        /** @brief Busca el handle.  Retorna @c GC_NULL_HANDLE si no existe. */
        GcHandle find(const uint8_t *key) const noexcept;

        /** @brief Elimina la entrada.  Retorna true si existia. */
        bool erase(const uint8_t *key);

        /** @brief Tamano actual (numero de entradas vivas). */
        size_t size() const noexcept { return live_count_; }

        /** @brief True si vacio. */
        bool empty() const noexcept { return live_count_ == 0; }

        /** @brief Limpia todas las entradas (mantiene la capacidad). */
        void clear();

        /** @brief Reserva capacidad para al menos @p n entries. */
        void reserve(size_t n);

    private:
        /// Sentinelas en el campo @c key.
        static constexpr const uint8_t *EMPTY     = nullptr;
        static constexpr uintptr_t      TOMB_RAW  = 1;

        static inline const uint8_t *tombstone() {
            return reinterpret_cast<const uint8_t *>(TOMB_RAW);
        }
        static inline bool is_empty(const uint8_t *k) {
            return k == EMPTY;
        }
        static inline bool is_tomb(const uint8_t *k) {
            return reinterpret_cast<uintptr_t>(k) == TOMB_RAW;
        }
        static inline bool is_live(const uint8_t *k) {
            return !is_empty(k) && !is_tomb(k);
        }
        static inline size_t hash_ptr(const uint8_t *k) noexcept {
            const uintptr_t p = reinterpret_cast<uintptr_t>(k);
            /* Pointers son 8-aligned -> shift 3 bits removes redundancia. */
            return static_cast<size_t>(p >> 3);
        }

        void grow();

        std::vector<Entry> table_;     ///< buckets (capacity = potencia de 2)
        size_t             mask_       = 0;  ///< capacity - 1
        size_t             live_count_ = 0;  ///< entradas vivas (no tombstones)
        size_t             used_       = 0;  ///< slots ocupados (live + tombstones)
        size_t             grow_at_    = 0;  ///< trigger grow cuando used > grow_at_
    };

    /**
     * @brief Activa/desactiva el debug del GC en runtime.
     *
     * Cuando esta activado, las operaciones del GC (major_gc, minor_gc,
     * release_handle, scan_stack_roots, evacuate, etc.) emiten trazas a
     * stderr con prefijo @c [GC] indicando: que operacion, conteos, IDs de
     * handles afectados, y valores relevantes para diagnostico.
     *
     * Activacion via:
     *   - CLI: @c --gc-debug (afecta a todos los procesos de la VM)
     *   - Env: @c VESTA_GC_DEBUG=1 (mismo efecto, alternativa para scripts)
     *   - API: @c gc::set_gc_debug(true) desde codigo C++ o tests
     *
     * En modo debug: cada GC fire imprime `[GC] minor_gc count=N`,
     * `[GC] major_gc count=N`, `[GC] sweep killed h=N`, etc.  Los traces
     * usan @c fflush(stderr) por linea para sobrevivir a un crash.
     *
     * Default: false (silencioso).  Coste cuando esta apagado: cero
     * (un branch predicho por instruccion clave).
     */
    void set_gc_debug(bool enabled) noexcept;
    bool gc_debug_enabled() noexcept;

    /**
     * @brief Modo BUFFERED del debug GC.  Acumula trazas en buffer
     *        thread-local de 64 KB y las flushea en bulk.
     *
     * Performance:
     *   - Modo default (unbuffered):  ~2 us / linea (write syscall directo).
     *   - Modo buffered:              ~100 ns / linea amortizado (memcpy +
     *                                 1 write syscall por 64 KB de trazas).
     *
     * Tradeoff: en modo buffered los ULTIMOS 64 KB de trazas pueden perderse
     * si el VM crashea (no estan flushed a disco).  El atexit handler hace
     * flush al salir limpio; gc_debug_flush() puede invocarse desde signal
     * handlers o pre-crash hooks para forzar flush.
     *
     * Activacion:
     *   - CLI: @c --gc-debug-buffered
     *   - Env: @c VESTA_GC_DEBUG_BUFFERED=1
     *   - API: @c set_gc_debug_buffered(true)
     *
     * Uso recomendado: BUFFERED para diagnosticos largos (millones de trazas)
     * en programas estables.  UNBUFFERED para diagnosticos de crash donde
     * cada linea importa.
     */
    void set_gc_debug_buffered(bool enabled) noexcept;
    bool gc_debug_buffered_enabled() noexcept;

    /**
     * @brief Flush manual del buffer del GC debug.  Util para signal
     *        handlers o pre-crash hooks: garantiza que las trazas
     *        acumuladas en buffer vayan a disco antes de un posible crash.
     *
     * En modo unbuffered es no-op (las trazas ya estan en disco).
     */
    void gc_debug_flush() noexcept;

    /**
     * @brief Estado del objeto dentro del algoritmo de marcado tri-color.
     *
     * Los valores estan codificados en 2 bits dentro de GcHeader para minimizar
     * el tamano de la cabecera y mantener los objetos alineados a 8 bytes.
     *
     * Transiciones durante major_gc():
     * @code
     *   (todos los no-DEAD) --(PRE-MARK)--> WHITE
     *   WHITE (handle vivo)  --(MARK)-----> BLACK
     *   WHITE (sin handle)   --(SWEEP)----> DEAD
     *   BLACK                --(SWEEP)----> sin cambio (sobrevive)
     *   DEAD                 --(SWEEP)----> sin cambio (ya liberado)
     * @endcode
     */
    enum class GcColor : uint8_t {
        WHITE = 0, /**< No visitado; candidato a recolectar. */
        GREY  = 1, /**< Alcanzable; referencias pendientes (reservado). */
        BLACK = 2, /**< Alcanzable y completamente procesado. */
        DEAD  = 3  /**< Liberado por sweep; slot fisico reutilizable. */
    };

    /**
     * @brief Generacion del objeto dentro del heap generacional.
     */
    enum class GcGen : uint8_t {
        YOUNG = 0, /**< Recien asignado; reside en Nursery. */
        OLD   = 1  /**< Sobrevivio al menos un minor GC; reside en OldGen. */
    };

    /**
     * @brief Cabecera de metadatos que precede a cada objeto en el heap GC.
     *
     * Precede inmediatamente al payload del objeto en memoria. El layout es:
     * @code
     *   [GcHeader (8 bytes)] [payload (size bytes)] [padding hasta alinear a 8]
     * @endcode
     *
     * El tamano total de un slot es: (sizeof(GcHeader) + size + 7) & ~7
     *
     * @note alignas(8) garantiza que el payload que sigue a la cabecera este
     *       alineado a 8 bytes, lo que permite almacenar cualquier tipo primitivo
     *       sin violaciones de alineacion.
     *
     * @warning El campo size se preserva incluso cuando color == DEAD, porque
     *          alloc_in_old() necesita calcular el tamano del slot para decidir
     *          si puede reutilizarlo. Nunca poner size = 0 en un slot DEAD.
     */
    struct alignas(8) GcHeader {
        uint32_t size;         /**< Bytes del payload (sin incluir esta cabecera). */
        GcColor  color: 2;     /**< Estado tri-color mas DEAD. */
        GcGen    gen  : 1;     /**< Generacion: YOUNG o OLD. */
        uint8_t  _pad : 5;     /**< Bits reservados para uso futuro. */
        uint8_t  _reserved[3]; /**< Padding hasta 8 bytes; reservado. */
    };

    static_assert(sizeof(GcHeader) == 8,
                  "GcHeader debe medir exactamente 8 bytes");

    /**
     * @brief Entrada en la tabla de handles del GcHeap.
     *
     * La HandleTable es un vector<HandleEntry> indexado por GcHandle.
     * Cuando un handle se libera (drop()), addr se pone a nullptr y live a false,
     * y el slot se recicla via free_handles_ para la siguiente asignacion.
     */
    struct HandleEntry {
        uint8_t *addr; /**< Puntero host al inicio del GcHeader del objeto.
                        *   nullptr si el slot esta libre (live == false). */
        bool     live; /**< true si el handle referencia un objeto valido. */
    };

    /**
     * @brief Entrada en la tabla de referencias debiles del GcHeap.
     *
     * Una referencia debil apunta a un objeto sin impedir su recoleccion.
     * Durante la fase de barrido del major GC, si el objeto apuntado ha muerto,
     * target se pone a GC_NULL_HANDLE automaticamente.
     *
     * Se accede mediante un indice uint32_t (WeakHandle).
     */
    struct WeakEntry {
        GcHandle target; ///< handle del objeto referenciado (GC_NULL_HANDLE si recolectado)
        bool     live;   ///< true si esta entrada esta en uso
    };

    /**
     * @brief Estadisticas acumuladas del GcHeap.
     *
     * Todos los campos son plain uint64_t sin sincronizacion. El heap es
     * per-proceso (un solo hilo lo usa a la vez), por lo que no se necesitan
     * atomics. El overhead de actualizarlas es de una instruccion ADD por evento,
     * ya que el objeto GcHeap suele estar en L1 cache durante las operaciones
     * de asignacion y recoleccion.
     *
     * Se accede via GcHeap::stats() que devuelve una referencia const (sin copia).
     *
     * @note Los contadores son monotonicamente crecientes; nunca se resetean.
     *       Para medir un intervalo, tomar snapshot antes y despues.
     */
    struct GcStats {
        uint64_t alloc_count    = 0; /**< Llamadas a alloc() que tuvieron exito. */
        uint64_t alloc_bytes    = 0; /**< Bytes utiles asignados (lo pedido por el usuario). */
        uint64_t freed_count    = 0; /**< Objetos liberados por major GC (sweep). */
        uint64_t freed_bytes    = 0; /**< Bytes liberados por sweep (slot total). */
        uint64_t promoted_count = 0; /**< Objetos evacuados Nursery -> OldGen. */
        uint64_t promoted_bytes = 0; /**< Bytes de payload evacuados a OldGen. */
        uint64_t minor_gc_count = 0; /**< Ciclos de minor GC ejecutados. */
        uint64_t major_gc_count = 0; /**< Ciclos de major GC ejecutados. */
        uint64_t peak_nursery   = 0; /**< Uso maximo de Nursery en bytes. */
        uint64_t peak_old       = 0; /**< Uso maximo de OldGen en bytes. */
        // ---- GC no-moving en OldGen: metricas de fragmentacion ----
        uint64_t old_reserved_bytes = 0; /**< Bytes reservados por bump pointer en bloques OldGen. */
        uint64_t old_freelist_bytes = 0; /**< Bytes acumulados en free lists tras el ultimo sweep. */
        uint64_t old_alloc_freelist = 0; /**< alloc_in_old satisfechos via free list (rapido). */
        uint64_t old_alloc_bump     = 0; /**< alloc_in_old satisfechos via bump pointer. */
        uint64_t old_alloc_newblock = 0; /**< alloc_in_old que requirieron crear bloque nuevo. */

        /* ---- metricas de precise vs conservative scan ---- */
        /**
         * @brief Numero de roots GC marcados por el precise scan de JIT
         *        frames durante el ultimo major_gc.  Acumula a traves de
         *        multiples GC cycles.
         *
         * En coexistencia con el conservativo:
         *   - precise_roots_marked: handles encontrados en stackmaps de
         *     JIT frames.  Cero si no hay JIT frames activos.
         *   - conservative_roots_marked: handles encontrados por el scan
         *     conservativo (incluye los precise + otros del interp).
         *
         * Comparacion empirica: si `precise_roots_marked > 0` y
         * `conservative_roots_marked - precise_roots_marked` es razonable
         * (los del interp), la integracion es correcta.
         */
        uint64_t precise_roots_marked     = 0;
        uint64_t conservative_roots_marked = 0;
        uint64_t precise_frames_scanned   = 0; /**< JIT frames walked en total. */
    };

    /**
     * @brief Heap generacional de recoleccion de basura por proceso.
     *
     * Implementa un GC de dos generaciones (Nursery + OldGen) con los siguientes
     * algoritmos:
     *
     *   - minor_gc(): evacuacion estilo Cheney. Copia objetos YOUNG vivos a OldGen
     *     en un solo pase sobre la HandleTable. La Nursery se resetea atomicamente
     *     reseteando el bump-pointer, sin coste por objeto muerto.
     *
     *   - major_gc(): mark-and-sweep tri-color sobre OldGen. Tres fases:
     *     PRE-MARK (reset a WHITE), MARK (handles vivos -> BLACK),
     *     SWEEP (WHITE -> DEAD, actualizar old_used_).
     *
     * Uso tipico en bytecode Vesta:
     * @code
     *   GcHandle h = heap.alloc(sizeof(MyObject));
     *   MyObject *obj = reinterpret_cast<MyObject *>(heap.deref(h));
     *   obj->field = 42;
     *   // ... usar obj solo hasta el siguiente GC ...
     *   heap.drop(h);  // objeto elegible para recoleccion
     * @endcode
     *
     * @warning Los punteros devueltos por deref() son invalidos tras cualquier
     *          llamada a minor_gc() o major_gc(), ya que el GC puede mover los
     *          objetos. Siempre llamar a deref() de nuevo despues de un GC.
     *
     * @warning No es thread-safe. Un solo hilo debe usar el heap a la vez.
     */
    class GcHeap {
    public:
        /**
         * @brief Construye el heap y reserva la Nursery.
         *
         * @param arena_mgr    ArenaManager del proceso propietario. OldGen pide
         *                     bloques a traves de el cuando necesita mas espacio.
         * @param nursery_bytes Tamano de la Nursery en bytes. Un tamano pequeno
         *                     provoca minor GC mas frecuentes pero pausas mas cortas.
         *                     Por defecto 1 MB.
         * @param old_threshold Bytes de OldGen usados que disparan un major GC
         *                      automatico al final de cada minor GC. Por defecto 8 MB.
         *
         * @note La Nursery se asigna como una Arena READ|WRITE en arena_mgr para
         *       que quede registrada en el proceso y sea liberada correctamente
         *       incluso si el proceso muere sin llamar al destructor.
         */
        explicit GcHeap(vm::ArenaManager &arena_mgr,
                        size_t            nursery_bytes = 1 * 1024 * 1024,
                        size_t            old_threshold = 8 * 1024 * 1024);

        /**
         * @brief Destructor. Libera la Nursery y todos los bloques de OldGen.
         *
         * Llama a arena_mgr_.free_arena() por cada bloque. No llama a ninguna
         * funcion de finalizacion de objetos (el lenguaje Vesta no tiene
         * destructores a nivel de VM en esta version).
         */
        ~GcHeap();

        GcHeap(const GcHeap &) = delete;

        GcHeap &operator=(const GcHeap &) = delete;

        /**
         * @brief Asigna un objeto de @p size bytes payload en el heap.
         *
         * Flujo de asignacion:
         * @code
         *   1. Fast-path: bump Nursery si hay espacio.        O(1)
         *   2. Nursery llena: disparar minor_gc() y reintentar.
         *   3. Sigue sin espacio: asignar directamente en OldGen.
         *   4. OOM en OldGen: devolver GC_NULL_HANDLE.
         * @endcode
         *
         * El payload se zero-inicializa siempre. El GcHeader se escribe con
         * color=WHITE, gen=YOUNG (o OLD si va directo a OldGen).
         *
         * @param size Bytes del payload (sin incluir GcHeader).
         * @return Handle valido, o GC_NULL_HANDLE si no hay memoria disponible.
         *
         * @note El handle devuelto permanece valido hasta que se llame a drop().
         *       La direccion fisica puede cambiar tras un minor_gc().
         */
        GcHandle alloc(size_t size);

        /**
         * @brief Aloca un objeto NO MOVIBLE (directo en OldGen).
         *
         * Variante de @c alloc que asigna el slot directamente en la
         * generacion vieja, evitando la promocion via @c do_evacuate.
         * El payload pointer obtenido via @c deref(h) permanece estable
         * durante toda la vida del handle: el major_gc no recolecta
         * objetos vivos (solo barre los DEAD), y la OldGen es non-moving.
         *
         * Util para objetos cuyo payload se exporta como @c host_ptr a
         * codigo que no se actualiza tras un GC, como @c StringObject
         * accedido via @c STRRAW para FFI / print / interpolacion.
         *
         * Coste: una llamada extra de overhead vs @c alloc en young (que
         * es bump O(1)); pero @c alloc_in_old usa free lists segregadas
         * por size class, asi que tambien es O(1) amortizado.
         *
         * @param size Tamano del payload en bytes (sin GcHeader).
         * @return Handle valido o GC_NULL_HANDLE si OOM en OldGen.
         */
        GcHandle alloc_pinned(size_t size);

        /**
         * @brief GC stack scanning conservativo con interior scan.
         *
         * Escanea el stack del proceso (rango [rsp, stack_high) leido de
         * @c vm_mem) y los GP regs R0..R15 buscando handles vivos y host_ptrs
         * (incluso interior pointers a objetos OldGen).  Cada uint64_t
         * encontrado pasa por filtros rapidos:
         *
         *   - skip si v == 0 o v < 256 (escalar pequeno, no puede ser handle)
         *   - check si v < handles_.size() y handles_[v].live  -> GcHandle
         *   - check si v esta en ptr_to_handle_ -> host_ptr al payload start
         *   - check si v cae dentro de algun bloque OldGen + buscar el header
         *     contenedor -> interior pointer (ej. STRRAW result al data[])
         *
         * Cada handle encontrado se marca BLACK + se anade al worklist BFS
         * para que mark_reachable lo procese transitivamente.
         *
         * Se llama desde @c major_gc() y @c minor_gc() ANTES del sweep,
         * para reemplazar el modelo "todo handle live = root" por uno
         * preciso basado en alcance real desde stack/regs.
         *
         * Coste: O(stack_size_active + 16 + N_old_blocks).  Para stack
         * tipico de 8 KB y 4 bloques OldGen: ~50-200 microsegundos por GC.
         * Mucho mas rapido que el modelo previo cuando hay muchos handles
         * vivos (que era O(N_handles) iterando handles_ entera).
         *
         * @param rsp        Stack pointer actual del proceso.
         * @param stack_high Limite superior del stack (inmutable post-spawn).
         * @param regs       Array de 16 GP regs (R0..R15).
         * @param vm_mem     VirtualMemory del proceso para leer slots stack.
         * @param worklist   Worklist BFS de mark phase; se anaden handles.
         */
        void scan_stack_roots(uint64_t rsp,
                               uint64_t stack_high,
                               const uint64_t regs[16],
                               vm::VirtualMemory &vm_mem,
                               std::vector<GcHandle> &worklist);

        /**
         * @brief precise scan de JIT frames.
         *
         * Camina la cadena RBP nativa desde @c __builtin_frame_address(0)
         * de @c major_gc.  Por cada JIT frame (identificado via
         * @c jit::JitRegistry::lookup), busca su stackmap y marca los
         * slots GC como roots vivos.
         *
         * Coexistencia con conservativo:
         *   - Ambos scans corren ADDITIVE: precise anade roots que el
         *     conservativo accidentalmente pudo no marcar (estabilidad
         *     adicional) y viceversa.
         *   - Cuando los stackmaps esten battle-tested, Fase 2-lean
         *     excluira rangos JIT del conservativo para eliminar false
         *     positives.
         *
         * Si @c jit::JitRegistry esta vacio (no hay JIT funcs cargadas)
         * la funcion retorna inmediatamente sin walk -- coste cero pre-D.3.
         *
         * @param worklist worklist BFS donde se anaden los handles
         *                 precise marcados como BLACK.
         */
        void scan_jit_roots_precise(std::vector<GcHandle> &worklist);

        /**
         * @brief Intenta marcar un handle como root precise (BLACK + push
         *        worklist) si es valido + vivo + actualmente WHITE.
         * @return true si lo marco, false si no era root valido.
         */
        bool try_mark_precise_handle(GcHandle h,
                                     std::vector<GcHandle> &worklist);

        /**
         * @brief Recorre stack y GP regs reescribiendo host_ptrs de objetos YoungGen
         *        evacuados a sus nuevas addresses en OldGen.
         *
         * Llamado al final de @c minor_gc(), DESPUES de @c do_evacuate.  Para
         * cada slot 8-bytes-aligned del stack y para cada GP reg (R0..R15),
         * comprueba si el valor es un host_ptr en @c forward_table_; si si,
         * escribe la nueva address.
         *
         * Esto elimina la limitacion historica "host_ptrs locales pueden
         * invalidarse tras GC": las locales CLASS del bytecode (cuyo valor
         * es el host_ptr al payload tras gcderef) quedan automaticamente
         * actualizados al evacuarse el objeto.  Sin esto, el bytecode
         * tenia que volver a llamar gcderef tras cada call que pudiera
         * disparar GC -> overhead inaceptable.
         *
         * Coste: O(stack_size_bytes/8) lookups en hashmap (typ. ~100 us)
         * + O(num_regs) (16 lookups).  Solo se llama si `forward_table_`
         * tiene entries (caso comun: pocos objetos sobrevivieron al minor).
         *
         * @param rsp_lo     RSP minimo visto desde el ultimo GC.  Limita el rango.
         * @param stack_high Top del stack (inmutable, configurado al spawn).
         * @param regs       Array de los 16 GP regs (R0..R15) modificable.
         * @param vm_mem     VirtualMemory del proceso para read/write 64-bit.
         */
        void update_stack_forwards(uint64_t rsp_lo,
                                    uint64_t stack_high,
                                    uint64_t regs[16],
                                    vm::VirtualMemory &vm_mem);

        /**
         * @brief registra un handle como root temporal "en construccion".
         *
         * Usado por @c alloc() entre el momento de crear el handle y el
         * retorno al bytecode.  Si durante la construccion del objeto
         * compuesto se dispara otro alloc que hace GC, el handle todavia
         * NO esta en stack/regs del bytecode -> el scan no lo veria -> seria
         * barrido prematuramente.
         *
         * El sweep ignora cualquier handle que coincida con @c pending_alloc_root_.
         * El proximo alloc o cualquier instruccion VM que use el handle
         * efectivamente lo coloca en stack/regs y permite limpiar este pin.
         *
         * Usado tambien para proteger string objects en construccion durante
         * STRMAKE/STRCAT/STRSLICE/etc cuando alguno de esos puede disparar GC.
         */
        GcHandle pending_alloc_root_ = GC_NULL_HANDLE;

        /**
         * @brief asocia el GcHeap con su ProcessVM propietario.
         *
         * Llamado una vez al construir el ProcessVM para que el GC sepa
         * de que stack y regs leer durante el major_gc.  El puntero se
         * mantiene durante toda la vida del proceso (no cambia).  Si es
         * nullptr (proceso especial sin stack scanning, ej. tests), el
         * major_gc cae al modelo previo "todo handle live = root".
         */
        void set_owner_process(runtime::ProcessVM *p) noexcept { owner_proc_ = p; }

        /**
         * @brief Devuelve un puntero al payload del objeto referenciado por @p handle.
         *
         * El puntero apunta inmediatamente despues del GcHeader del objeto.
         * Es valido para leer y escribir hasta la proxima llamada a minor_gc()
         * o major_gc(), ya que el GC puede mover el objeto.
         *
         * @param handle Handle valido devuelto por alloc().
         * @return Puntero al payload, o nullptr si el handle es invalido o libre.
         *
         * @warning Nunca almacenar este puntero mas alla del siguiente GC.
         *          Usar el handle para reconvertir si es necesario.
         */
        uint8_t *deref(GcHandle handle);

        /**
         * @brief Numero total de slots de la HandleTable (incluye libres).
         *
         * Para iterar handles vivos:
         * @code
         *   for (size_t i = 0; i < heap.handle_table_size(); ++i) {
         *       if (heap.is_handle_live(static_cast<GcHandle>(i))) {
         *           uint8_t *p = heap.deref(static_cast<GcHandle>(i));
         *           ...
         *       }
         *   }
         * @endcode
         *
         * Coste O(1).  Util principalmente para el debugger TCP que
         * expone la lista de handles vivos al cliente VSH.
         */
        size_t handle_table_size() const noexcept { return handles_.size(); }

        /**
         * @brief @c true si el handle apunta a un objeto vivo.
         *
         * Usado por el debugger para iterar la HandleTable sin acceso
         * directo a los campos privados.  Coste O(1).
         */
        bool is_handle_live(GcHandle handle) const noexcept {
            return handle < handles_.size() && handles_[handle].live;
        }

        /**
         * @brief Tamano del payload del objeto referenciado por handle.
         *
         * Lee el campo @c size del @c GcHeader inmediatamente antes del
         * payload.  Devuelve 0 si el handle es invalido o muerto.
         */
        uint32_t handle_payload_size(GcHandle handle) const noexcept {
            if (handle >= handles_.size() || !handles_[handle].live
             || !handles_[handle].addr) {
                return 0;
            }
            const auto *h = reinterpret_cast<const GcHeader *>(
                handles_[handle].addr);
            return h->size;
        }

        /**
         * @brief Generacion del objeto (YOUNG/OLD).
         */
        GcGen handle_generation(GcHandle handle) const noexcept {
            if (handle >= handles_.size() || !handles_[handle].live
             || !handles_[handle].addr) {
                return GcGen::OLD;
            }
            const auto *h = reinterpret_cast<const GcHeader *>(
                handles_[handle].addr);
            return h->gen;
        }

        /**
         * @brief Lookup inverso: dado un puntero host al payload, devuelve el GcHandle.
         *
         * Es el inverso de @c deref(): si @c deref(h) devolvio @p host_payload_ptr,
         * entonces @c handle_for_ptr(host_payload_ptr) devuelve @p h.
         *
         * Usado por la instruccion bytecode @c gchandle (0x55) para que el
         * frontend Vex pueda obtener el GcHandle de un objeto cuando solo
         * tiene el host pointer (caso comun: tras @c gcderef en un constructor).
         * El uso primario es @c synchronized(obj) en Vex, que necesita el handle
         * para @c monenter / @c monexit.
         *
         * Coste O(1) amortizado (unordered_map lookup + bucket walk corto).
         * El mapa se mantiene incrementalmente en @c new_handle, @c release_handle
         * y @c do_evacuate, por lo que no hay coste adicional en @c handle_for_ptr.
         *
         * @param host_payload_ptr Puntero host al inicio del payload (justo tras GcHeader),
         *                         tipicamente el resultado de un @c deref previo.
         * @return GcHandle del objeto si lo encuentra, @c GC_NULL_HANDLE si no.
         */
        GcHandle handle_for_ptr(const uint8_t *host_payload_ptr) const noexcept;

        /**
         * @brief Registra una referencia old->young en el remembered set.
         *
         * Debe llamarse cada vez que el codigo de usuario (o el compilador del
         * lenguaje Vesta) escribe un GcHandle YOUNG dentro del payload de un
         * objeto OLD. Sin esta llamada, el minor GC podria no encontrar el objeto
         * YOUNG como alcanzable y recolectarlo prematuramente.
         *
         * No anade duplicados al remembered set.
         *
         * @param old_handle Handle del objeto OLD que contiene el campo modificado.
         *
         * @note Esta funcion solo es necesaria para referencias cross-generacion
         *       (OLD -> YOUNG). Referencias YOUNG -> YOUNG o OLD -> OLD no
         *       necesitan write barrier.
         */
        void write_barrier(GcHandle old_handle);

        /**
         * @brief Incrementa el refcount externo de @p h.
         *
         * Llamado por estructuras de datos NATIVAS que retienen un GcHandle
         * (ej. el plugin @c vesta_collections cuando hace push de un string
         * en un @c ArrayList<string>).  Mientras el refcount sea >0, el GC
         * trata el handle como root vivo durante el mark phase del
         * major_gc, evitando que sea colectado aunque ningun root normal
         * (HandleTable bytecode) lo referencie.
         *
         * Coste: O(1) amortizado (1 lookup + increment).  Sin syscalls,
         * sin scan adicional.
         *
         * @param h Handle a pinnar.  Si @c h == GC_NULL_HANDLE no-op.
         */
        void gc_addref(GcHandle h);

        /**
         * @brief Decrementa el refcount externo de @p h.
         *
         * Llamado por la estructura nativa cuando deja de retener el handle
         * (ej. al hacer pop, remove, clear o destruir el contenedor).  Si
         * el refcount llega a 0, la entrada se elimina del map (el GC ya
         * NO lo trata como root externo).
         *
         * Coste: O(1) amortizado.  No llama al GC: solo desregistra; la
         * coleccion real ocurre en el proximo major_gc si nadie mas
         * referencia el handle.
         *
         * @param h Handle a despinnar.  Si no estaba registrado, no-op.
         */
        void gc_release(GcHandle h);

        /**
         * @brief Numero de handles actualmente pinnados externos.
         *        Util para debug / introspeccion.  Devuelve el size del map
         *        external_refs_, NO la suma de refcounts.
         */
        size_t external_pinned_count() const noexcept {
            return external_refs_.size();
        }

        /**
         * @brief Libera el handle indicando que el objeto ya no es alcanzable.
         *
         * Marca el slot en la HandleTable como libre (live=false, addr=nullptr)
         * y lo recicla para futuras asignaciones. El objeto permanece fisicamente
         * en Nursery u OldGen hasta que el GC lo recolecte.
         *
         * En la Nursery: el objeto es ignorado en la proxima evacuacion y su
         * memoria se reclama cuando el bump-pointer se resetea.
         *
         * En OldGen: el objeto permanece con su GcHeader intacto hasta el proximo
         * major_gc(), donde el PRE-MARK lo pone WHITE y el SWEEP lo marca DEAD.
         *
         * @param h Handle a liberar. Si es invalido o ya esta libre, no hace nada.
         */
        void drop(GcHandle h) {
            release_handle(h);
        }

        /**
         * @brief Crea una referencia debil al objeto indicado por @p target.
         *
         * La referencia debil no impide la recoleccion del objeto.
         * Durante el barrido del major GC, si el objeto es recolectado,
         * la entrada se pone automaticamente a GC_NULL_HANDLE.
         *
         * @param target Handle del objeto a referenciar debilmente.
         * @return Indice uint32_t de la entrada en weak_table_.
         */
        uint32_t alloc_weak(GcHandle target);

        /**
         * @brief Lee el handle apuntado por la referencia debil @p idx.
         *
         * @param idx Indice en weak_table_ obtenido de alloc_weak().
         * @return Handle del objeto si aun esta vivo; GC_NULL_HANDLE si fue recolectado.
         */
        GcHandle deref_weak(uint32_t idx) const;

        /**
         * @brief Libera la entrada de referencia debil @p idx.
         *
         * Marca la entrada como libre para su reutilizacion.
         *
         * @param idx Indice en weak_table_ a liberar.
         */
        void free_weak(uint32_t idx);

        // =====================================================================
        //  Primitivas de monitor (sincronizacion por objeto)
        //
        //  Un monitor es un lock reentrante asociado a un objeto GC.  El estado
        //  del lock (propietario y profundidad) se almacena en ObjectHeader de
        //  cada objeto.  La cola de procesos en espera se almacena en esta clase.
        //
        //  Invariante:
        //    - ObjectHeader::owner_pid == 0  <=>  monitor libre
        //    - ObjectHeader::owner_pid != 0  <=>  monitor adquirido por ese pid
        //    - ObjectHeader::lock_depth >= 1 cuando owner_pid != 0
        //
        //  Los PID codificados en las colas usan el formato:
        //    encoded_pid = (scheduler_id << 32) | local_pid
        // =====================================================================

        /**
         * @brief Intenta adquirir el monitor del objeto referenciado por @p h.
         *
         * Si el monitor esta libre, lo asigna a @p local_pid y devuelve true.
         * Si ya lo posee @p local_pid (lock reentrante), incrementa lock_depth y
         * devuelve true.
         * Si lo posee otro proceso, devuelve false (el llamante debe bloquear).
         *
         * Toma @c owner_encoded de 48 bits (scheduler_id<<32 | local_pid).
         * Globalmente unico: evita la colision local_pid==local_pid entre
         * schedulers distintos (bug Z.11 ext: lock confundido como reentrante).
         *
         * @param h             Handle del objeto cuyo monitor se quiere adquirir.
         * @param owner_encoded PID encoded global del proceso solicitante.
         * @return true si el monitor fue adquirido o incrementado; false si bloqueado.
         */
        bool monitor_try_acquire(GcHandle h, uint64_t owner_encoded);

        /**
         * @brief Libera el monitor del objeto referenciado por @p h.
         *
         * Decrementa lock_depth.  Si llega a 0, marca el monitor como libre y
         * extrae un proceso de la cola de espera para despertarlo.
         *
         * @param h             Handle del objeto cuyo monitor se libera.
         * @param owner_encoded PID encoded global del propietario actual.
         * @return PID codificado del siguiente proceso de la cola de espera
         *         (0 si la cola estaba vacia o el monitor sigue bloqueado).
         */
        uint64_t monitor_release(GcHandle h, uint64_t owner_encoded);

        /**
         * @brief Anade un PID codificado a la cola de espera del monitor de @p h.
         *
         * Se llama desde exec_instr_monenter cuando el monitor esta ocupado, o
         * desde exec_instr_monwait cuando el proceso libera el monitor para esperar.
         *
         * @param h           Handle del objeto cuyo monitor espera el proceso.
         * @param encoded_pid PID codificado: (scheduler_id << 32) | local_pid.
         */
        void monitor_add_waiter(GcHandle h, uint64_t encoded_pid);

        /**
         * @brief Extrae y devuelve un PID codificado de la cola de espera de @p h.
         *
         * Devuelve 0 si la cola esta vacia.  Se usa en MONNOTI para despertar
         * exactamente un proceso.
         *
         * @param h Handle del objeto cuya cola de espera se consulta.
         * @return PID codificado del proceso despertado, o 0 si la cola estaba vacia.
         */
        uint64_t monitor_pop_waiter(GcHandle h);

        /**
         * @brief Extrae y devuelve todos los PID codificados de la cola de @p h.
         *
         * La cola queda vacia tras la llamada.  Se usa en MONNOTA para despertar
         * a todos los procesos que esperan sobre el mismo objeto.
         *
         * @param h Handle del objeto cuya cola de espera se vacia.
         * @return Vector con todos los PID codificados (puede estar vacio).
         */
        std::vector<uint64_t> monitor_pop_all_waiters(GcHandle h);

        // -----------------------------------------------------------------
        // Wait queues con dispatch local-vs-shared (Phase Z).
        // -----------------------------------------------------------------
        // Cuando un handle tiene el @c SHARED_HANDLE_BIT, las wait queues
        // (monitor + condvar) se enrutan a @c vm.shared_wait_table en lugar
        // de la tabla local @c wait_table_, permitiendo que parent y child
        // que comparten el mismo objeto se coordinen cross-process.

        /// Encola @p encoded_pid en la wait queue CONDVAR de @p h.  Usada
        /// por @c monwait para distinguir esperantes que liberan el
        /// monitor (CONDVAR) de los que esperan reentrar (MONITOR).
        void condvar_add_waiter(GcHandle h, uint64_t encoded_pid);

        /// Despierta UN proceso de la wait queue CONDVAR de @p h.  Usada
        /// por @c monnoti.  Devuelve el PID codificado o 0 si vacia.
        uint64_t condvar_pop_waiter(GcHandle h);

        /// Despierta TODOS los procesos de la wait queue CONDVAR de @p h.
        /// Usada por @c monnota.  La cola queda vacia tras la llamada.
        std::vector<uint64_t> condvar_pop_all_waiters(GcHandle h);

        // set_owner_process ya declarada e inlineada arriba.

        /**
         * @brief Minor GC: evacua la Nursery copiando supervivientes a OldGen.
         *
         * Algoritmo (Cheney-style simplificado):
         * @code
         *   Para cada handle H en la HandleTable:
         *     Si H.live && H.addr en rango Nursery && gen == YOUNG:
         *       alloc_in_old(tamano del objeto)
         *       memcpy al nuevo slot
         *       H.addr = nuevo slot (handle actualizado)
         *       objeto original marcado BLACK (forward pointer en payload)
         *   remembered_set.clear()
         *   nursery_bump = nursery_base  (reset O(1))
         *   Si old_used >= old_threshold: major_gc()
         * @endcode
         *
         * Todos los objetos YOUNG sin handle vivo son abandonados en la Nursery
         * y su memoria se reclama gratis con el reset del bump-pointer. Este es el
         * beneficio central de la generacional: limpiar objetos muertos es O(1).
         *
         * @note Provoca una pausa del proceso propietario. Otros procesos de la
         *       VM no se ven afectados.
         */
        void minor_gc();

        /**
         * @brief Major GC: mark-and-sweep tri-color sobre OldGen.
         *
         * Tres fases:
         *
         *   PRE-MARK: recorre todos los objetos OldGen y los pone WHITE (excepto
         *             los ya DEAD). Necesario porque los objetos llegan a OldGen
         *             con color BLACK tras la evacuacion y no hay otro mecanismo
         *             que los resetee cuando su handle se suelta.
         *
         *   MARK:     recorre la HandleTable. Por cada handle vivo cuyo objeto
         *             esta en OldGen, pone el objeto BLACK (alcanzable).
         *
         *   SWEEP:    recorre los bloques de OldGen. Objetos WHITE (sin raiz) se
         *             marcan DEAD, se decrementa old_used_ y se actualizan stats.
         *             Objetos BLACK sobreviven. Objetos DEAD se ignoran.
         *
         * @note Actualmente el MARK no sigue punteros dentro de los objetos
         *       (no hay grafo de objetos en esta version). Toda la liveness se
         *       determina por la HandleTable. Para soportar grafos de objetos
         *       se debe implementar un MARK transitive que siga campos GcHandle.
         *
         * @note Provoca una pausa del proceso propietario. Puede ser largo si
         *       OldGen es grande. Para pausas acotadas considerar un major GC
         *       incremental en versiones futuras.
         */
        void major_gc();

        /**
         * @brief Configura el umbral de OldGen para major GC automatico.
         *
         * Cuando old_used_ >= old_threshold_ al finalizar un minor_gc(), se
         * dispara un major_gc() automaticamente. Un umbral bajo provoca mas
         * ciclos major GC pero mantiene OldGen mas compacto.
         *
         * @param bytes Umbral en bytes. Un valor de 0 deshabilita el major GC
         *              automatico (solo se puede disparar manualmente).
         */
        void set_old_threshold(size_t bytes) {
            old_threshold_ = bytes;
        }

        /** @brief Bytes actualmente usados en la Nursery (distancia bump-base). */
        size_t nursery_used() const {
            return static_cast<size_t>(nursery_bump_ - nursery_base_);
        }

        /** @brief Tamano total de la Nursery en bytes. */
        size_t nursery_total() const {
            return nursery_size_;
        }

        /** @brief Bytes actualmente contabilizados en OldGen. */
        size_t old_used() const {
            return old_used_;
        }

        /**
         * @brief Devuelve una referencia de solo lectura a las estadisticas acumuladas.
         *
         * La referencia es valida mientras el GcHeap exista. No copia los datos.
         * Los contadores son monotonicamente crecientes; para medir un intervalo
         * tomar un snapshot antes y otro despues de la operacion de interes.
         *
         * @return Referencia const a GcStats.
         */
        const GcStats &stats() const {
            return stats_;
        }

        /**
         * @brief Phase D.7.opt: wrapper publico de @c new_handle para que
         *        @c vrt_register_alloc lo invoque tras inlinear bump-pointer.
         *
         * Solo debe usarse cuando el caller YA hizo bump-pointer + init de
         * @c GcHeader + @c ObjectHeader.  Esta funcion SOLO crea el handle.
         */
        GcHandle register_alloc(uint8_t *raw) {
            return new_handle(raw);
        }

    public:
        // --- Nursery (publicos para inline bump-pointer en JIT) ---
        //
        // Phase D.7.opt: el JIT inlinea el fast path de alloc emitiendo
        // accesos directos a @c nursery_bump_ y @c nursery_end_ via
        // offset compile-time desde @c ProcessVM* (rbx en VM_ABI).
        // Esto elimina la llamada a @c vrt_newobj para el caso comun
        // (~5 ns inline vs ~20 ns runtime call).  Los demas usuarios
        // siguen usando @c alloc() para preservar invariantes.
        uint8_t *nursery_base_     = nullptr; ///< Inicio del bloque Nursery.
        uint8_t *nursery_bump_     = nullptr; ///< Proximo byte libre (bump pointer).
        uint8_t *nursery_end_      = nullptr; ///< Fin del bloque Nursery.

    private:
        vm::ArenaManager &arena_mgr_;
        size_t   nursery_size_     = 0;       ///< Tamano total de la Nursery.
        uint64_t nursery_arena_id_ = 0;       ///< ID en ArenaManager para liberar al destruir.

        // --- OldGen (modelo no-moving con free lists segregadas, ver gc_heap.cpp) ---
        struct OldBlock {
            uint8_t *ptr;         ///< Inicio del bloque.
            size_t   size;        ///< Tamano total del bloque en bytes.
            uint64_t arena_id;    ///< ID en ArenaManager para liberar al destruir.
            size_t   bump_offset; ///< Bytes ya consumidos por bump-alloc (offset desde ptr).
                                  ///< Si bump_offset == size: bloque lleno; reciclamos
                                  ///< slots solo via free lists, sin compactar.
        };

        std::vector<OldBlock> old_blocks_;
        size_t                old_used_      = 0; ///< Bytes vivos contabilizados en OldGen.
        size_t                old_threshold_ = 0; ///< Umbral para major GC automatico.

        /// puntero al ProcessVM owner para acceder al stack/regs
        /// durante el major_gc.  Set via set_owner_process() en el ctor del
        /// ProcessVM.  Si es nullptr, major_gc cae al modelo previo.
        runtime::ProcessVM *owner_proc_ = nullptr;

        // ---- (iv) Free lists segregadas para slots OldGen liberados ----
        // Cada slot DEAD reusa los primeros 8 bytes de su payload como
        // puntero al siguiente DEAD del mismo size class (LIFO O(1)).
        // Cero memoria extra por slot.  El tamano del slot esta en
        // hdr->size del propio header (ya existe).
        //
        // Hay 16 size classes para slots pequenyos (<=4096 bytes total) y
        // una free list general para slots grandes.  Los size classes
        // se eligieron para minimizar fragmentacion interna en patrones
        // tipicos de objetos Vesta (header 24B + N campos i32/i64).
        struct FreeNode {
            FreeNode *next;
        };
        static constexpr size_t SMALL_CLASS_COUNT = 16;
        static constexpr size_t SMALL_CLASS_SIZES[SMALL_CLASS_COUNT] = {
            16, 24, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048, 4096
        };
        FreeNode *small_free_lists_[SMALL_CLASS_COUNT] = {nullptr};

        // Slots con total > 4096: free list general (ptr, total).  Buscamos
        // first-fit lineal; en la practica es corta porque slots grandes
        // son raros.  Si crece mucho, considerar ordered insertion + binary
        // search (mejora futura, no critico en v1).
        struct LargeFree {
            uint8_t *ptr;
            size_t   total;
        };
        std::vector<LargeFree> large_free_list_;

        // --- HandleTable ---
        std::vector<HandleEntry> handles_;
        std::vector<GcHandle>    free_handles_; ///< Freelist LIFO de slots reciclables.

        /// Mapa inverso payload_ptr_host -> GcHandle.  Permite lookup O(1)
        /// cuando un objeto se accede por su host pointer pero se necesita
        /// el handle (caso: synchronized en Vex, donde @c this es host_ptr
        /// pero @c monenter requiere GcHandle).  Se mantiene incrementalmente:
        ///   - new_handle()    => insert(addr+sizeof(GcHeader), h)
        ///   - release_handle() => erase
        ///   - do_evacuate()   => erase old + insert new tras mover el objeto
        /// El payload pointer es estable mientras el objeto no se evacue.
        /* Phase D.7.opt: reemplazado @c std::unordered_map por un flat
         * hash map open-addressing para el hot path del GC (new_handle/
         * release_handle ~20-30 ns -> ~5 ns por op). */
        PtrHandleMap ptr_to_handle_;

        // --- Tabla de referencias debiles ---
        std::vector<WeakEntry> weak_table_; ///< Referencias debiles indexadas por uint32_t.

        // --- Colas de espera de monitores (LOCAL per-process) ---
        // Clave: GcHandle del objeto con el monitor ocupado.
        // Valor: lista FIFO de PID codificados ((scheduler_id<<32)|local_pid) esperando.
        std::unordered_map<GcHandle, std::vector<uint64_t>> monitor_waiters_;

        // Wait queues lock-free per-bucket para objetos GC LOCALES.
        // Para objetos shared (handle con SHARED_HANDLE_BIT) se enruta a
        // @c vm.shared_wait_table via @c wait_table_for().
        WaitTable wait_table_;

        /// Devuelve la WaitTable apropiada para @p h: la local del heap si
        /// el handle es local, o la global del VM si es shared.
        inline WaitTable &wait_table_for(GcHandle h) noexcept;

        // --- RememberedSet ---
        std::unordered_set<GcHandle> remembered_set_; ///< Handles OLD con referencias a YOUNG.

        // --- Tabla de forwarding pointers (Cheney-style) ---
        /// Mapa <young_payload_addr, old_payload_addr> poblado por do_evacuate
        /// cuando un objeto YoungGen se mueve a OldGen.  Sirve para que
        /// `update_stack_forwards` (llamado al final de minor_gc) recorra
        /// el stack del proceso y los GP regs, y reemplace cada slot que
        /// contenga un host_ptr a YoungGen evacuada por su nuevo address
        /// en OldGen (estable, non-moving).
        ///
        /// Sin esto, los locales del bytecode que mantienen host_ptrs a
        /// objetos CLASS (resultado de `gcderef` tras NEWOBJ) quedan
        /// dangling tras un minor_gc que evacua sus objetos -> SEGFAULT
        /// al siguiente field access.  La alternativa (mantener handles
        /// en lugar de host_ptrs) costaria 1 gcderef por field access.
        ///
        /// Se vacia al final de cada minor_gc.  Coste por GC:
        /// O(num_evacuated_objects) inserts + O(stack_size_bytes/8) lookups.
        std::unordered_map<const uint8_t *, const uint8_t *> forward_table_;

        // --- External roots (write-barrier para colecciones nativas) ---
        /// Handles GC referenciados por estructuras nativas (ej. ArrayList<string>
        /// en el plugin vesta_collections).  Cada @c gc_addref incrementa el
        /// contador, @c gc_release lo decrementa.  Durante el mark phase,
        /// cualquier handle con refcount > 0 se trata como root vivo aunque
        /// no este referenciado por roots normales (HandleTable bytecode).
        ///
        /// Necesario porque el GC no escanea los slots internos del plugin
        /// (struct opaco desde el punto de vista del runtime).  Sin este
        /// mecanismo, un string almacenado solo en `xs.push(s)` quedaria
        /// invalidado al primer GC tras que el local que tenia s salga de
        /// scope.
        ///
        /// La entrada se elimina cuando el counter llega a 0 (lazy cleanup
        /// para evitar fragmentar el bucket de la hashmap).
        std::unordered_map<GcHandle, uint32_t> external_refs_;

        // --- Estadisticas ---
        GcStats stats_;

        /**
         * @brief Crea un nuevo handle apuntando a @p addr.
         * Reutiliza un slot libre si existe; si no, anade uno nuevo al vector.
         */
        GcHandle new_handle(uint8_t *addr);

        /**
         * @brief Marca el handle @p h como libre y lo recicla en free_handles_.
         * No libera la memoria del objeto.
         */
        void release_handle(GcHandle h);

        /**
         * @brief Asigna @p total_bytes en OldGen.  Wrapper compatible que
         *        descarta el slot_total real (lo usa @c do_evacuate que ya
         *        copia exactamente el tamano original via @c hdr->size).
         *
         * @param total_bytes Tamano total incluyendo GcHeader y padding.
         * @return Puntero al inicio del slot, o nullptr si OOM.
         */
        uint8_t *alloc_in_old(size_t total_bytes);

        /**
         * @brief Variante de @c alloc_in_old que reporta el tamano real del slot.
         *
         * (iv) GC no-moving: el slot devuelto puede ser MAYOR que @p total_bytes
         * cuando se redondea al size class.  El llamante (alloc()) necesita
         * el tamano real para escribirlo en @c hdr->size, asi al re-free
         * el slot encaja exactamente en su size class.
         *
         * Orden de busqueda (todas O(1) excepto large freelist):
         *   1. Free list segregada del size class >= total_bytes.
         *   2. Free list large (slots >4096) con first-fit.
         *   3. Bump pointer en algun OldBlock con espacio.
         *   4. Crear bloque nuevo via ArenaManager.
         *
         * @param total_bytes      Tamano minimo requerido (header + payload + pad).
         * @param out_actual_total [out] Tamano real del slot devuelto (>= total_bytes).
         * @return Puntero al inicio del slot, o nullptr si OOM.
         */
        uint8_t *alloc_in_old_with_total(size_t total_bytes, size_t &out_actual_total);

        /**
         * @brief Evacua el objeto referenciado por @p h de Nursery a OldGen.
         *
         * Si el objeto ya es BLACK o ya esta en OldGen, no hace nada.
         * Tras la evacuacion: el handle apunta al nuevo slot en OldGen,
         * el objeto original se marca BLACK (forward pointer en payload),
         * y se actualizan las estadisticas de promocion.
         *
         * @param h Handle del objeto a evacuar.
         */
        void do_evacuate(GcHandle h);

        /**
         * @brief Evacua el objeto referenciado por @p h de Nursery a OldGen.
         *
         * Wrapper que comprueba addr != nullptr antes de delegar en do_evacuate().
         * Los objetos ya en OldGen o ya marcados BLACK son ignorados.
         */
        void evacuate_object(GcHandle h);

        /**
         * @brief Escanea el payload del objeto @p h buscando handles YOUNG y los evacua.
         *
         * Cada palabra de 4 bytes del payload se interpreta como potencial GcHandle.
         * Si apunta a un objeto YOUNG no evacuado, llama a do_evacuate() y lo
         * anade al worklist para escaneo transitivo posterior (Cheney-style).
         */
        void scan_young_refs(GcHandle h, std::vector<GcHandle> &worklist);

        /**
         * @brief Marca transitivamente todos los objetos OLD alcanzables desde @p h.
         *
         * Escanea el payload de @p h. Si alguna palabra de 4 bytes es un handle
         * valido con addr != nullptr que apunta a un objeto OLD WHITE, lo marca
         * BLACK y lo anade al worklist para propagacion.
         */
        void mark_reachable(GcHandle h, std::vector<GcHandle> &worklist);

        // ---- (iv) Helpers de free list segregada ----

        /**
         * @brief Devuelve el indice del size class cuyo SIZE >= @p total.
         *
         * Para alloc: redondeo HACIA ARRIBA al primer class capaz de
         * contener @p total bytes.  Devuelve @c SMALL_CLASS_COUNT si
         * @p total > 4096 (slot grande, va a la free list general).
         */
        static size_t size_class_ceil(size_t total) noexcept;

        /**
         * @brief Inserta un slot DEAD en la free list correspondiente.
         *
         * Si total esta en rango small, usa el size class exacto (que
         * encaja con el slot porque alloc_in_old siempre redondea al class).
         * Si es grande, va a @c large_free_list_.  El slot se identifica
         * por el puntero al GcHeader y su total (header + payload + pad).
         *
         * Asume que @p hdr->size ya refleja el tamano completo del payload
         * del slot (no el pedido por el usuario).
         */
        void freelist_push(uint8_t *raw_header, size_t total);

        /**
         * @brief Vacia las free lists.  Se llama al inicio de cada sweep
         *        para reconstruirlas desde scratch (los slots DEAD se
         *        reinsertan al recorrer los bloques).
         *
         * No libera memoria; solo limpia los heads.  Los nodos viven
         * embebidos en los slots y se sobrescribiran al re-push.
         */
        void freelist_clear() noexcept;
    };
} // namespace gc

#endif // GC_HEAP_H
