/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribucion obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file proceso_runtime.h
 * @brief Declaracion del contexto de proceso virtual (ProcessVM) de VestaVM.
 *
 * Declara @c ProcessVM: registros generales (R00-R15), PC, SP, BP,
 * flags de estado, pila de llamadas, estado del proceso y campos del
 * planificador.  Unidad minima de ejecucion dentro de la VM.
 */#ifndef PROCESO_RUNTIME_H
#define PROCESO_RUNTIME_H

#include "scheduler.h"
#include "vm_registers.h"
#include "vm_state_event.h"

#include "runtime/pid.h"
#include "gc/gc_heap.h"
#include "gc/raw_allocator.h"
#include "loader/oop_types.h"

#include <atomic>
#include <csetjmp>
#include <vector>

namespace distrib {
    class Mailbox; ///< Declaracion adelantada del buzon de mensajes distribuido
}

namespace runtime {
    class StringInternPool; ///< Pool de interning de strings (definicion en string_intern.h)
}

/**
 * @brief Numero de reducciones por defecto que se le asignan a cada proceso al entrar en ejecucion.
 *
 * Una reduccion equivale a ejecutar una instruccion o un paso costoso de la FSM.
 * Cuando el contador llega a cero el scheduler cambia de proceso (round-robin cooperativo).
 */
#define reductions_remaining_default 8192

namespace runtime {
    struct InstrFormat; ///< Formato de instruccion con metadatos de descodificacion/ejecucion
    class Scheduler;    ///< Gestor de procesos que ejecuta el run_loop

    /**
     * @brief Informacion generada durante la descodificacion de una instruccion.
     *
     * Todas las fases de la FSM (DECODE -> EXECUTE) leen y escriben este struct.
     * Contiene flags de control, los operandos descodificados y un puntero al
     * descriptor de la instruccion (InstrFormat).
     */
    typedef struct DecodedInstr {
        /**
         * @brief Campos de control descodificados del prefijo de la instruccion.
         *
         * Todos los campos se inicializan a cero/false en la declaracion del objeto.
         */
        struct {
            uint8_t is_not_extended: 8; ///< opcode1: 0x00 indica tabla extendida; otro valor indica tabla primaria

            uint8_t opcode_index: 8;    ///< opcode2: indice en la tabla de descodificacion

            uint8_t _signed_instruct: 1; ///< 1 si la instruccion opera con signo; 0 si es sin signo

            /**
             * @brief Modo de la instruccion (tamanyo del operando).
             *
             * Codifica el tamanyo del operando:
             *   0 -> 8 bits, 1 -> 16 bits, 2 -> 32 bits, 3 -> 64 bits.
             */
            uint8_t mode: 2;

            /**
             * @brief Indica si la instruccion ha modificado el PC manualmente.
             *
             * Las instrucciones de salto (JMP, CALL, RET) deben poner este campo a true.
             * Si es false, la fase EXECUTE incrementa el PC en size_instr bytes al terminar.
             * Si es true, se asume que la instruccion ya actualizo el PC y no se incrementa.
             */
            bool did_jump: 1;

            bool blocking: 1; ///< 1 si la instruccion requiere esperar una operacion de E/S desbloqueante

            /**
             * @brief 1 si la instruccion usa registros extendidos/especiales (rsp, rbp, rip, cur0..cur3).
             */
            bool reg_ext: 1;

            /**
             * @brief Direccion de la operacion de memoria.
             *
             * Para instrucciones con acceso a memoria:
             *   0: destino en registro, fuente en memoria  (ej. add r3, [r1 + r2*8])
             *   1: destino en memoria, fuente en registro  (ej. add [r1 + r2*8], r3)
             *
             * En instrucciones sin direccionalidad se usa como metadato de seleccion
             * de variante dentro de la misma familia.
             */
            uint8_t direction: 1;

            uint8_t size_instr: 4; ///< Tamanyo en bytes de la instruccion (maximo 15 bytes)
        } flags_info = {
            0, 0, 0, 0,
            false, false, false, 0, 0
        }; ///< Campos de control inicializados a cero/false

        /**
         * @brief Operandos descodificados de la instruccion (union de todos los formatos).
         *
         * Solo uno de los campos de la union es valido en cada instruccion; el campo
         * activo depende del tipo de instruccion indicado por flags_info.
         */
        union {
            /**
             * @brief Acceso crudo a los 16 bytes de datos de la instruccion.
             */
            struct {
                uint64_t raw1; ///< Primeros 8 bytes en crudo
                uint64_t raw2; ///< Segundos 8 bytes en crudo
            } raw_data;

            /**
             * @brief Operandos para instrucciones de tipo reg, reg.
             *
             * Ejemplo: add r0, r1
             */
            struct {
                uint8_t reg1; ///< Indice del registro destino (o primer operando)
                uint8_t reg2; ///< Indice del registro fuente (o segundo operando)
            } reg_data;

            /**
             * @brief Operandos para instrucciones con mezcla de registros generales y especiales.
             *
             * Usado por instrucciones como XCHG que pueden operar con registros extendidos
             * y generales en la misma instruccion.  Cada registro tiene un bit de flag que
             * indica si es extendido (1) o general (0).
             */
            struct {
                uint8_t reg1      : 6; ///< Indice del registro 1 (general o extendido)
                uint8_t reg1_flags: 1; ///< 1 si reg1 es extendido/especial
                uint8_t unused1   : 1; ///< Sin uso por ahora
                uint8_t reg2      : 6; ///< Indice del registro 2 (general o extendido)
                uint8_t reg2_flags: 1; ///< 1 si reg2 es extendido/especial
                uint8_t unused2   : 1; ///< Sin uso por ahora
            } regs_data_extent;

            /**
             * @brief Operandos para instrucciones de tipo reg, imm o imm, reg.
             *
             * Ejemplo: add r0, 42
             */
            struct {
                uint64_t inmmed; ///< Valor inmediato codificado en la instruccion
                uint8_t  reg;    ///< Indice del registro destino o fuente
            } inmmed_data;

            /**
             * @brief Operandos para instrucciones de acceso a memoria con SIB.
             *
             * Codifica la formula de direccion: [reg_base + reg_index * scale].
             * El campo mode en flags_info determina el tamanyo del acceso.
             *
             * Ejemplo: add [r1 + r2 * 2], r3b
             *   reg_base=r1, reg_index=r2, scale=2, reg_final=r3
             */
            struct {
                uint8_t reg_base;  ///< Registro base de la direccion de memoria
                uint8_t reg_index; ///< Registro indice de la formula SIB
                uint8_t reg_final; ///< Registro destino o fuente de la operacion
                uint8_t scale;     ///< Factor de escala del registro indice (1, 2, 4 u 8)
            } mem_data;

            /**
             * @brief Operandos para instrucciones de acceso a static fields
             *        (getstatic / setstatic, opcodes extended 0x60 / 0x61).
             *
             * Encoding fisico FIXED_8:
             *   [0x00][0x60|0x61][regs_byte][_pad8][offset_u32_LE]
             *     byte2 (regs_byte) = (r0 << 4) | r1
             *     bytes 4-7         = offset uint32 little-endian
             *
             * Para @c getstatic: r0 = r_dst (destino del valor leido),
             *                    r1 = r_class (registro con ClassInfo*).
             * Para @c setstatic: r0 = r_class (registro con ClassInfo*),
             *                    r1 = r_value (valor a almacenar).
             *
             * El opcode siempre opera sobre @c i64 (8 bytes); el frontend
             * Vex realiza @c truncate post-load para tipos mas pequenos
             * (mismo patron que los accesos a campos de instancia).
             */
            struct {
                uint8_t  r0;       ///< Registro 0 (dst para getstatic, class para setstatic)
                uint8_t  r1;       ///< Registro 1 (class para getstatic, value para setstatic)
                uint16_t _pad;     ///< Padding de alineacion
                uint32_t offset;   ///< Offset dentro de @c ClassInfo::static_data (bytes)
            } static_data;

            /**
             * @brief Operandos para fastpush / fastpop con bitmask de registros.
             *
             * Cada bit del mask representa un registro general (bit 0 = r0, ..., bit 15 = r15).
             * fastpush itera los bits ascendentes y empuja en orden r0, r1, ..., r15
             * (los que tengan el bit puesto).  fastpop itera los bits ascendentes
             * tambien, leyendo de offsets descendentes (orden inverso al push),
             * lo que restaura los valores exactos.
             *
             * Encoding fisico FIXED_4:
             *   [0x00][opcode2][mask_lo][mask_hi]
             */
            struct {
                uint16_t mask;     ///< Bitmask de registros (bit r = r0..r15)
            } mask_data;
        } data_instruction = {
            static_cast<uint64_t>(0),
            static_cast<uint64_t>(0)
        }; ///< Operandos inicializados a cero

        InstrFormat *metadata = nullptr; ///< Descriptor de la instruccion: funcion de ejecucion, metadatos, etc.

        /**
         * @brief cache directo del exec function pointer.
         *
         * Antes el hot path del scheduler hacia `decoded_ptr->metadata->exec(...)`
         * que es una doble indireccion (decoded_ptr -> metadata -> exec).
         * Cachear el pointer aqui ahorra una carga por instruccion VM.
         * Lo seteamos junto con metadata en decode_instruction (cache hit y miss).
         */
        void (*exec_cached)(ProcessVM *, const DecodedInstr &) = nullptr;

        uint64_t pc = 0; ///< Direccion virtual del PC donde se encontro esta instruccion

        /**
         * @brief Inline cache para resolucion virtual (CALLVIRT/CALLM monomorfica).
         *
         * Las llamadas virtuales en la practica son MONOMORFICAS: el receptor
         * suele ser de la misma clase en ejecuciones repetidas del mismo PC.
         * Cachear `cached_class -> cached_method` permite saltarse el bounds
         * check + indirect load de la vtable en el caso comun (cache hit).
         *
         * Invalidacion: si `hdr->class_ptr != cached_class` (cache miss), el
         * exec hace lookup completo y actualiza el cache.  El primer hit tras
         * un miss paga el coste de la resolucion; los hits subsiguientes son
         * O(1) sin indireccion.
         *
         * Cero overhead si el call site nunca se ejecuta (campos a nullptr).
         */
        mutable void *cached_class  = nullptr;
        mutable void *cached_method = nullptr;
    } DecodedInstr;


    /**
     * @brief Codigos de error que puede almacenar un hilo de proceso virtual.
     *
     * El campo err_thread de ProcessVM guarda el ultimo error ocurrido.
     * Un valor THREAD_NO_ERROR (0) indica que el proceso no ha fallado.
     */
    typedef enum state_err_thread {
        THREAD_NO_ERROR          = 0, ///< Sin error
        THREAD_UNKNOWN_ERROR,         ///< Error no clasificado
        THREAD_SEGMENTATION_FAULT,    ///< Acceso a memoria sin permisos suficientes
        THREAD_ILLEGAL_INSTRUCTION,   ///< Instruccion no reconocida o prohibida
        THREAD_DIVISION_BY_ZERO,      ///< Division entre cero

        /**
         * @brief El tope de pila (SP) alcanzo el limite inferior del segmento de pila.
         *
         * Ocurre cuando se realiza un PUSH mas alla del limite reservado para la pila.
         */
        THREAD_STACK_OVERFLOW,

        /**
         * @brief Se intento leer de una pila vacia o SP quedo por debajo de BP.
         *
         * El tope de pila siempre debe ser mayor o igual que el puntero de base.
         * Un POP de pila vacia o un decremento ilegal de SP genera este error.
         */
        THREAD_STACK_UNDERFLOW,

        THREAD_INVALID_SYSCALL, ///< Llamada al sistema invalida o no soportada
        THREAD_NULL_POINTER,    ///< UNWRAP encontro un valor nulo (NullPointerException)
    } state_err_thread;

    /**
     * @brief Numero de entradas de la cache de instrucciones descodificadas.
     *
     * Debe ser potencia de dos para que icache_index() pueda usar una mascara AND.
     */
    static constexpr uint32_t ICACHE_SIZE = 1024;

    /**
     * @brief Calcula el indice en la tabla icache para una direccion PC dada.
     *
     * Usa una mascara AND (valida solo si ICACHE_SIZE es potencia de dos) para
     * mapear cualquier PC a un indice dentro del array icache[] del proceso.
     *
     * @param pc Direccion del contador de programa de la instruccion.
     * @return   Indice en el array icache[] (0 .. ICACHE_SIZE-1).
     */
    inline uint32_t icache_index(uint64_t pc) {
        return pc & (ICACHE_SIZE - 1); // mascara AND aprovechando que ICACHE_SIZE es potencia de 2
    }

    /**
     * @class ProcessVM
     * @brief Proceso virtual de la maquina virtual VestaVM.
     *
     * Cada ProcessVM representa un hilo de ejecucion independiente dentro de
     * la VM.  Contiene su propio:
     *   - Conjunto de registros (context_registers_vm).
     *   - Espacio de memoria privado (ArenaManager + VirtualMemory + TLB).
     *   - Heap de GC y asignador raw.
     *   - Cache de instrucciones descodificadas (icache).
     *   - Estado de la FSM (vm_state).
     *
     * El scheduler propietario gestiona el ciclo de vida del proceso.
     * Para que el proceso sea elegible por el scheduler debe llamarse a
     * vm->make_ready(proc->pid) tras crearlo.
     */
    class ProcessVM {
    public:
        /**
         * @brief Safepoint flag (per-proceso).  PRIMER campo de la
         *        clase a proposito para que el JIT-eado pueda emitir
         *        polls con disp8 (mas compactos que disp32).
         *
         * El JIT mantiene @c ProcessVM* en RBX durante toda la funcion.
         * Cada poll (back-edge / post-call) emite:
         *
         *     cmp byte [rbx], 0      ; 4 bytes (REX + cmp + modrm)
         *     jne handler            ; 6 bytes (0F 85 rel32)
         *
         * Total: 10 bytes por poll, ~2 cycles cuando flag=0 (branch
         * predictor lo aprende inmediatamente).  Esencialmente gratis.
         *
         * El GC del propio proceso setea este byte a 1 cuando quiere
         * pausar al thread para stack scan.  El handler invoca
         * @c vrt_safepoint_handler que captura RIP/RBP y cede el control
         * al GC.
         *
         * NOTE: @c uint8_t (no atomic) para que el poll JIT-eado sea
         * un simple @c cmp byte.  Ordenamiento garantizado porque el
         * setter del GC y el reader del handler coordinan via
         * @c std::atomic<bool> @c safepoint_acked posteriormente.
         */
        uint8_t safepoint_flag = 0;
        uint8_t _safepoint_pad[7] = {0};  ///< Alineacion a 8 bytes

        GlobalPID pid; ///< Identificador global del proceso (scheduler_id + local_pid)

        /**
         * @brief Contador de reducciones restantes antes del proximo cambio de contexto.
         *
         * Una reduccion equivale normalmente a ejecutar una instruccion.
         * Cuando llega a cero el scheduler elige otro proceso (planificacion round-robin).
         * Se reinicia al valor reductions_remaining_default en cada quantum.
         */
        uint64_t reductions_remaining = reductions_remaining_default;

        context_registers_vm registers; ///< Contexto completo de registros del proceso

        /**
         * @brief GC stack scanning conservativo.
         *
         * Limite superior INMUTABLE del stack del proceso, seteado al spawn
         * y al cargar el proceso main.  El stack crece hacia abajo desde
         * @c stack_high hacia @c stack_low_water.  El GC stack scan recorre
         * el rango [stack_low_water, stack_high) para encontrar handles y
         * host_ptrs vivos.
         */
        uint64_t stack_high      = 0;

        /**
         * @brief minimo @c rsp visto desde el ultimo GC.
         *
         * Actualizado en @c subsp (1 cmp + cmov, ~1 ns; subsp es raro = 1
         * vez por entry de funcion).  Reseteado al rsp actual tras cada GC.
         * Permite escanear solo el rango realmente usado del stack
         * en lugar del 1 MiB completo.
         */
        uint64_t stack_low_water = 0;

        /**
         * @brief Puntero a codigo JIT a ejecutar al iniciar el proceso.
         *
         * Si != nullptr, el scheduler salta a este codigo nativo ANTES de
         * iniciar el bucle interp.  Util para JIT-ear @c main (que es
         * free function y no se invoca via CALLVIRT, asi que el hook
         * normal no lo dispara).
         *
         * Cuando el JIT-eated @c main retorna, el proceso queda en HALT
         * automaticamente (sin ejecutar el interp).  Se usa una sola vez
         * (el campo se limpia tras la primera invocacion).
         *
         * Set por @c Loader::load_executable cuando JIT esta on y main
         * compila correctamente.
         */
        void *jit_entry_fn = nullptr;

        uint64_t tsc{}; ///< Contador de instrucciones ejecutadas (Time Stamp Counter virtual)

        /**
         * @brief Marca temporal en nanosegundos a la que debe despertar el proceso.
         *
         * Cuando el proceso entra en estado BLOCK (p.ej. por una instruccion sleep),
         * este campo almacena el instante futuro en el que debe pasar a READY.
         * Un valor 0 indica que no hay temporizador activo.
         */
        uint64_t time_sleep{};

        state_err_thread err_thread = THREAD_NO_ERROR; ///< Ultimo error ocurrido en el proceso

        /**
         * @brief Estado actual del proceso dentro de la FSM del scheduler.
         *
         * el campo es @c std::atomic<vm_state> para permitir
         * acceso seguro desde varios hilos (scheduler propietario + remoto
         * que invoca @c make_ready desde otro scheduler en escenarios
         * multi-thread real con @c --schedulers N>1).  Los operadores
         * implicitos @c operator T() y @c operator=(T) realizan load/store
         * con memory_order seq_cst por defecto; las rutas calientes que
         * necesitan ordering relajado usan @c .load(...)/.store(...)
         * explicitamente.
         */
        std::atomic<vm_state> state { NEW };

        /**
         * @brief bandera de wake-up pendiente para resolver
         * la race "lost wakeup" en multi-thread.
         *
         * Cuando otro hilo (msgsend cross-scheduler, fulfill, monitor notify,
         * etc.) invoca @c make_ready y observa que el proceso esta en un
         * estado activo (EXECUTE/DECODE/RUNNING/READY), no puede transicionar
         * el state a READY (perderia la ejecucion en curso).  En su lugar
         * pone @c wake_pending=true.  El scheduler propietario, al transicionar
         * EXECUTE -> WAIT_IO por bloqueo, hace exchange(false) sobre este
         * flag.  Si era true, abandona la transicion y vuelve a READY (un
         * mensaje llego mientras el proceso decidia dormirse).
         */
        std::atomic<bool> wake_pending { false };

        /**
         * @brief B4.3: flag de "notify recibido" para que MONWAIT no
         * re-bloquee tras un wake.
         *
         * Set por @c monnoti / @c monnota en el proceso popeado de la cola
         * CONDVAR.  Consumido por @c monwait al re-ejecutar tras wake:
         * si esta set, lo limpia y AVANZA el PC (sin re-bloquear).
         * Lock-free (atomic exchange).
         */
        std::atomic<bool> condvar_notified { false };

        // --- Cache de instrucciones descodificadas (icache) ---
        DecodedInstr icache[ICACHE_SIZE] = {}; ///< Tabla de instrucciones descodificadas indexada por icache_index(PC)

        DecodedInstr *decoded_ptr = nullptr; ///< Puntero a la entrada icache activa durante DECODE/EXECUTE

        // --- Memoria privada del proceso ---
        vm::ArenaManager manager_mem_priv{}; ///< Gestor de arenas privadas de este proceso

        tlb::LazyHybridTLB tlb{};           ///< TLB privado del proceso
        vm::VirtualMemory  vm_mem;           ///< Interfaz de memoria virtual (combina TLB + ArenaManager)

        // --- GC del proceso ---
        gc::GcHeap       gc_heap{manager_mem_priv, 2 * 1024 * 1024, 8 * 1024 * 1024}; ///< Heap del GC (min 2 MiB, max 8 MiB)
        gc::RawAllocator raw_alloc{};                                                   ///< Asignador raw sin GC

        // --- Sistema de objetos (OOP) ---
        loader::FrameHeader *frame_stack = nullptr; ///< Cabeza de la cadena de FrameHeaders activos (push en CALLVIRT, pop en RET/THROW)

        /**
         * @brief Pool de FrameHeader con free list LIFO.
         *
         * Motivacion: la implementacion ingenua hacia `new
         * loader::FrameHeader{}` por cada CALLVIRT/CALLM/CALLVMR/CALLCLOSURE
         * y `delete frame` por cada RET.  En benchmarks con 30M calls eso
         * son 30M malloc + 30M free a ~150 ns cada uno => ~9 segundos solo
         * en el heap del compilador C++.  Era el cuello de botella principal
         * de cualquier programa intensivo en dispatch virtual.
         *
         * El pool reemplaza esa ruta por O(1) acquire/release sobre un free
         * list intrusivo: cuando un frame esta libre, su campo @c prev
         * apunta al siguiente libre (reusamos el campo porque ya esta
         * presente y los frames libres no participan de la cadena de
         * activacion).  La medicion empirica tras este cambio bajo el
         * coste por call/return a ~5 ns (30x mas rapido).
         *
         * Asigna chunks de 256 frames.  Crece bajo demanda.  Los chunks
         * se liberan en el destructor del pool (al morir el ProcessVM).
         */
        struct FrameHeaderPool {
            std::vector<loader::FrameHeader *> chunks; ///< Chunks alocados con `new[]`.
            loader::FrameHeader              *free_list_head = nullptr;
            static constexpr size_t           CHUNK_SIZE = 256;

            /**
             * @brief Saca un FrameHeader libre del pool.  Si el free list
             * esta vacio, aloca un nuevo chunk de @c CHUNK_SIZE frames.
             * El frame devuelto NO esta inicializado: el caller debe
             * setear todos los campos antes de usarlo.
             */
            inline loader::FrameHeader *acquire() {
                if (!free_list_head) {
                    auto *chunk = new loader::FrameHeader[CHUNK_SIZE];
                    chunks.push_back(chunk);
                    // Encadenar el chunk al free list via campo `prev`.
                    for (size_t i = 0; i < CHUNK_SIZE; ++i) {
                        chunk[i].prev = free_list_head;
                        free_list_head = &chunk[i];
                    }
                }
                loader::FrameHeader *f = free_list_head;
                free_list_head = f->prev;
                return f;
            }

            /**
             * @brief Devuelve un FrameHeader al pool.  El frame puede
             * estar en cualquier estado; lo unico que importa es el
             * campo `prev` que se reusa para el free list.
             */
            inline void release(loader::FrameHeader *f) {
                f->prev = free_list_head;
                free_list_head = f;
            }

            ~FrameHeaderPool() {
                for (auto *c : chunks) delete[] c;
            }
        };

        FrameHeaderPool frame_pool; ///< Pool de FrameHeader para CALLVIRT/CALLM/CALLVMR/RET.

        /**
         * @brief Contador de llamadas activas a `loadmod` no retornadas.
         *
         * Cada `loadmod` incrementa el contador antes de saltar al main del
         * modulo cargado.  El main del modulo tipicamente termina con `hlt`
         * (porque fue compilado para ejecutarse standalone), pero al ser
         * llamado via loadmod debe regresar al caller via RET.  La solucion
         * es runtime: cuando un `hlt` se ejecuta con `loadmod_call_depth>0`,
         * se trata como un RET (decrementa contador, pop ret_addr del stack,
         * salta a el).  Esto evita patchar bytecode (la solucion anterior
         * `patch_first_hlt_to_ret` era fragil ante secuencias de bytes 0x00 0x03
         * que aparecen en imm de mov/callvm aleatoriamente).  Soporta
         * loadmodule anidado (counter > 1) sin esfuerzo extra.
         */
        uint32_t loadmod_call_depth = 0;

        uint64_t current_exception = 0; ///< Handle de la excepcion activa durante el unwinding (0 = sin excepcion)

        /**
         * @brief Frame de excepcion ligero apilado por TRYENTER / desapilado por TRYLEAVE.
         *
         * Permite que compiladores de alto nivel instalen handlers de excepcion en
         * tiempo de ejecucion sin necesidad de anotaciones en el bytecode.
         * do_throw comprueba esta pila antes de recorrer MethodInfo.handlers.
         */
        struct ExceptionFrame {
            uint64_t              handler_pc; ///< Direccion absoluta VM del bloque catch
            loader::ClassInfo    *type;        ///< Tipo capturado (nullptr = catch-all)
            uint64_t              saved_rsp;   ///< RSP al momento del tryenter.
                                               ///< @c do_throw lo restaura antes de saltar
                                               ///< al handler para descartar los push del
                                               ///< regalloc que no llegaron a pop por el throw.
            uint64_t              saved_rbp;   ///< Idem para RBP (frame pointer).
            uint64_t              saved_frame_stack; ///< Puntero al @c frame_stack al
                                                    ///< momento del tryenter; @c do_throw
                                                    ///< unwindea hasta este punto antes de
                                                    ///< saltar al handler (descarta frames
                                                    ///< de calls dentro del try-body que no
                                                    ///< retornaron normalmente).
            uint64_t              saved_regs[16];  ///< Snapshot de R0..R15 al
                                                   ///< momento del tryenter.
                                                   ///< @c do_throw los restaura antes de
                                                   ///< saltar al handler para que el catch
                                                   ///< vea exactamente el mismo estado que
                                                   ///< el try entry (igual que C/C++ exc).
                                                   ///< Resuelve la limitacion clasica de
                                                   ///< que las vars vivas a traves del try
                                                   ///< quedaban con valores stale tras
                                                   ///< el throw (regalloc no preservaba
                                                   ///< los regs en el unwind).
                                                   ///< R0 se preserva PERO el catch lo
                                                   ///< sobreescribe con la excepcion.
            struct ExceptionFrame *prev;       ///< Frame anterior en la pila
        };

        ExceptionFrame *exc_frame_stack = nullptr; ///< Pila de frames TRYENTER activos

        /**
         * @brief Free list de @c ExceptionFrames reciclables.
         *
         * Cuando @c tryleave (bytecode o JIT inline) pop-ea un frame del
         * @c exc_frame_stack, en lugar de hacer @c delete (que llama a
         * @c free de libc, ~30-100 ns + fragmentation del heap), lo empuja
         * a este free list via el campo @c prev.  Subsequent @c tryenter
         * busca aqui primero; si esta vacio, hace @c new normal.
         *
         * Beneficios:
         *   - Cero leak: los frames se reusan, no se descartan.
         *   - O(1) per try/tryleave (sin malloc/free amortizado).
         *   - Inline JIT tryleave puede usar este free list directamente
         *     emitiendo solo 2 instrucciones extra (set prev + update head).
         *
         * Los frames del free list se liberan en el destructor del
         * ProcessVM via @c free_exc_pool (en .cpp).
         */
        ExceptionFrame *exc_free_list = nullptr;

        /// Slot reusable para FatalError instance.  Aloca lazy en
        /// el primer @c throw_fatal y se reutiliza en throws sucesivos
        /// (evita alocacion en heap durante un error fatal, donde el GC
        /// puede estar en mal estado).  Layout: ObjectHeader (24) + 4
        /// fields qword-aligned (kind/pc/message/trace) = 56 bytes.
        /// El message y trace apuntan a buffers tambien lazy-asignados
        /// (@c fatal_msg_buf y @c fatal_trace_buf) que se reusan.
        void   *fatal_slot      = nullptr;
        char   *fatal_msg_buf   = nullptr;   ///< buffer de mensajes (256 bytes)
        char   *fatal_trace_buf = nullptr;   ///< buffer de stack trace (4096 bytes)

        /// Recovery point para errores fatales de OS-level (access
        /// violation / SIGSEGV cuando el bytecode dereferencia un
        /// puntero host invalido).  El scheduler hace setjmp aqui antes
        /// de cada batch de instrucciones; el VEH (Windows) o el
        /// handler de SIGSEGV (POSIX) hace longjmp si av_recovery_active
        /// esta activo y el proceso tiene un try/catch envolvente
        /// (exc_frame_stack != nullptr).  Tras el longjmp el scheduler
        /// llama @c throw_fatal con FATAL_SEGMENTATION_FAULT y la
        /// direccion del AV; el flujo normal de @c do_throw redirige al
        /// handler `catch (FatalError e)`.
        ///
        /// Sin esta infra, accesos a punteros host invalidos (e.g.
        /// `*(i32*)0`, deref de un host_ptr stale tras free) crashean
        /// el proceso entero de la VM en lugar de ser capturados.
        std::jmp_buf av_recovery_jmpbuf;
        bool         av_recovery_active = false;
        uint64_t     pending_av_addr    = 0; ///< direccion bruta del AV (informativa)
        /// Tipo de excepcion host que disparo el recovery:
        ///   0 = AV (segfault, default), 1 = DIVIDE_BY_ZERO, 2 = INT_OVERFLOW.
        /// El scheduler lo consulta tras el @c longjmp para emitir el
        /// @c FatalError adecuado.
        uint32_t     pending_av_kind   = 0;

        StringInternPool *str_intern_pool = nullptr; ///< Pool de strings internados (creado bajo demanda)

        Scheduler &scheduler; ///< Referencia al scheduler propietario de este proceso

        distrib::Mailbox *mailbox = nullptr; ///< Buzon de mensajes distribuido (creado bajo demanda por DistRuntime)

        // --- Campos para procesos creados remotamente via rspawn ---
        uint64_t rspawn_future_id    = 0;            ///< future_id en el nodo origen que debe resolverse con r0 al terminar (0 = no es rspawn)
        uint32_t rspawn_origin_node  = 0xFFFFFFFFu;  ///< node_idx del nodo que envio el rspawn (0xFFFFFFFF = no es rspawn)

        /**
         * @brief Construye un proceso virtual y lo asocia al scheduler indicado.
         * @param scheduler Scheduler que gestionara este proceso.
         * @param pid       Identificador global unico del proceso.
         */
        ProcessVM(Scheduler &scheduler, GlobalPID pid);

        /**
         * @brief Invalida todas las entradas del icache del proceso.
         *
         * Debe llamarse cuando el espacio de codigo del proceso cambia (p.ej.
         * tras JIT o carga dinamica de codigo) para evitar ejecutar instrucciones
         * descodificadas sobre codigo antiguo.
         */
        void reset_cache();

        /**
         * @brief Carga codigo en bruto en la memoria virtual del proceso.
         *
         * Mapea @p code a partir de @p address en el espacio de memoria del
         * proceso y pone su estado a RUNNING para que el scheduler pueda
         * comenzar a ejecutarlo.
         *
         * @param address Direccion virtual donde se escribira el codigo.
         * @param code    Vector de bytes con las instrucciones a cargar.
         */
        void load_raw_code(uint64_t address, const std::vector<uint8_t> &code);

        /**
         * @brief Genera un resumen de estado del proceso en texto.
         * @return Cadena con los valores mas relevantes del proceso.
         */
        std::string vm_summary() const;

        /**
         * @brief Destructor: libera todos los recursos del proceso.
         */
        ~ProcessVM();

        /**
         * @brief Genera una representacion textual detallada del proceso.
         * @return Cadena con el volcado completo del estado del proceso.
         */
        [[nodiscard]] std::string to_string() const;

    private:
    };

    /**
     * @brief Fase: hook opcional para auto-JIT trigger desde
     *        @c exec_instr_callvirt.
     *
     * Llamado tras incrementar @c MethodInfo::invocation_count en cada
     * dispatch CALLVIRT.  Si nullptr (default), no JIT.  Cuando el main
     * binario inicializa el JIT subsystem registra
     * @c &jit::maybe_compile_method aqui, habilitando el trigger.
     *
     * Por que function pointer: el codigo del auto-JIT depende del IR
     * frontend (Selector, IrFunction) que NO esta en @c vesta_rt.
     * El hook permite que vesta_rt despache al JIT sin acoplarse al
     * frontend.  Coste: 1 cmp + 1 branch predicted = ~1 ns por
     * CALLVIRT en programas sin JIT habilitado.
     */
    extern void (*g_callvirt_post_hook)(ProcessVM *vm, loader::MethodInfo *method);

} // namespace runtime

#endif // PROCESO_RUNTIME_H
