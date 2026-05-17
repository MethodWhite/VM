/**
 * @file exception_runtime.h
 * @brief Sistema de excepciones FatalError capturables desde Vex.
 *
 * Antes los errores VM "duros" (NPE, div0, illegal opcode,
 * stack overflow, segfault, etc.) escribian @c vm->err_thread =
 * @c THREAD_* y disparaban @c EVT_ERROR -> el proceso moria sin
 * posibilidad de captura desde bytecode.
 *
 * introduce la clase predefinida @c FatalError, registrada en el
 * @c ClassRegistry del Loader publico al crear la VM.  Cualquier opcode
 * que detecte un error duro ahora invoca @c throw_fatal(vm, kind, msg)
 * que:
 *
 *   - Si @c vm->exc_frame_stack es @c nullptr (caso normal: sin try
 *     activo) -> ruta antigua: set @c err_thread + @c EVT_ERROR ->
 *     proceso muere igual que antes.  Cero overhead anadido.
 *
 *   - Si hay un handler activo -> aloca una @c FatalError instance,
 *     llena los campos (kind, pc, message, stack_trace), y delega en
 *     @c do_throw para que la pila de TRYENTER + @c handler_pc lo
 *     atrape igual que cualquier otra excepcion.  El usuario captura
 *     con @c try { ... } catch (FatalError e) { ... } en Vex.
 *
 * @c FatalError tiene 4 fields ABI fijos (offsets se exponen al
 * lowering del frontend):
 *
 *   ObjectHeader  (24 bytes)
 *   +24  i32  kind         -> codigo @c THREAD_* (NULL_PTR, DIV0, etc.)
 *   +32  u64  pc           -> direccion VM donde ocurrio el error
 *   +40  u64  message_ptr  -> puntero host a string nul-terminado
 *   +48  u64  trace_ptr    -> puntero host a stack trace formateado
 *
 * Usamos un slot estatico por proceso (@c vm->fatal_slot) para evitar
 * alocacion durante el throw -- el GC podria estar en mal estado al
 * momento del error.  Un solo slot basta porque las excepciones no se
 * anidan dentro de un mismo throw (el handler limpia la previa antes
 * de relanzar otra).
 */
#pragma once

#include "loader/oop_types.h"

#include <cstdint>
#include <cstddef>
#include <string>

// Forward declarations para evitar dependencias circulares.
namespace loader {
    class Loader;
}
namespace runtime {
    class ProcessVM;
}

namespace runtime {

    /**
     * @brief Codigos numericos para el campo @c FatalError.kind.
     *
     * Coinciden con @c state_err_thread (@c proceso_runtime.h) para que
     * el usuario pueda comparar facilmente: @c if (e.kind == 1) -> NPE.
     * Documentar en la API de Vex como constantes.
     */
    enum FatalKind : uint32_t {
        FATAL_NULL_POINTER       = 1,   ///< deref de puntero null o handle invalido
        FATAL_DIVISION_BY_ZERO   = 2,   ///< div / mod entre cero (signed o unsigned)
        FATAL_STACK_OVERFLOW     = 3,   ///< RSP supero el limite de pila
        FATAL_STACK_UNDERFLOW    = 4,   ///< POP / RET con RSP en frontera
        FATAL_ILLEGAL_INSTRUCTION= 5,   ///< opcode invalido o operando malformado
        FATAL_INVALID_SYSCALL    = 6,   ///< calln a simbolo no resuelto
        FATAL_SEGMENTATION_FAULT = 7,   ///< acceso a VA fuera de cualquier mapeo
        FATAL_NATIVE_CRASH       = 8,   ///< plugin nativo crasheo (SEH/SIGSEGV)
        FATAL_NATIVE_EXCEPTION   = 9,   ///< plugin nativo lanzo C++ exception
        FATAL_OUT_OF_MEMORY      = 10,  ///< alloc fallo (raw o GC)
        FATAL_USER_ABORT         = 11,  ///< builtin Vex @c panic("...")
    };

    /**
     * @brief Class info global de @c FatalError.  Inicializada por
     *        @c init_exception_classes() al construir la primera VM.
     *
     * Acceso lock-free tras el init: la inicializacion ocurre antes de
     * que cualquier proceso este corriendo.  En multi-thread la primera
     * VM hace el define_class y las posteriores lo encuentran via
     * @c by_name_ del registry sin re-crear.
     */
    extern loader::ClassInfo *g_fatal_error_class;

    /**
     * @brief Registra la clase @c FatalError en el ClassRegistry del
     *        loader publico si aun no existe.  Idempotente.
     *
     * Llamado desde el constructor de @c VM tras inicializar el loader
     * privado.  No falla: si el registry ya tiene FatalError simplemente
     * actualiza @c g_fatal_error_class al puntero existente.
     *
     * @param loader Loader publico del manager (donde vive el registry).
     */
    void init_exception_classes(loader::Loader &loader);

    /**
     * @brief Lanza un @c FatalError capturable desde el opcode actual.
     *
     * Patron de uso desde @c exec_instruction_*.cpp:
     *
     * @code
     *   if (deref_target == 0) {
     *       runtime::throw_fatal(vm, runtime::FATAL_NULL_POINTER,
     *           "deref de puntero null en GETFIELD");
     *       return;
     *   }
     * @endcode
     *
     * Comportamiento:
     *   - Si @c vm->exc_frame_stack != nullptr (handler activo): aloca
     *     o reutiliza el slot @c vm->fatal_slot, llena la instancia
     *     con kind/pc/message + stack trace formateado, e invoca
     *     @c do_throw para que se atrape como excepcion normal.
     *   - Si no hay handler: ruta antigua (set @c err_thread + emit
     *     @c EVT_ERROR).  El proceso muere igual que antes pero la VM
     *     y los demas procesos siguen vivos (que ya era el comportamiento
     *     pre.
     *
     * Coste cuando NO hay handler activo: 1 lectura + 1 branch +
     * 2 stores (fast path).  Cuando SI hay: ~50 ns por la alocacion
     * lazy del slot + memcpy del message.
     *
     * @param vm        Proceso donde se origina el error.
     * @param kind      Codigo @c FatalKind del error.
     * @param message   String C nul-terminado con descripcion legible.
     */
    void throw_fatal(ProcessVM *vm, uint32_t kind, const char *message);

    /**
     * @brief Variante de @c throw_fatal con mensaje formateado tipo printf.
     *
     * Util cuando el mensaje incluye valores dinamicos (offsets, registros,
     * nombres de clase).  Limita a 512 bytes; trunca con "..." si excede.
     */
    void throw_fatalf(ProcessVM *vm, uint32_t kind,
                      const char *fmt, ...);

    /**
     * @brief Metadata de debug por metodo.  Se rellena durante
     *        @c __module_init via el opcode @c setmethdbg que el frontend
     *        Vex emite tras cada @c defmethod con la info del .vex source.
     *
     * Layout muy compacto: nombre de archivo (compartido entre todos los
     * metodos del modulo) + linea de inicio del metodo en el source.
     * Suficiente para que el stack trace muestre "(myfile.vex:42)" en
     * cada frame.  Opcionalmente extensible a tabla pc -> line para
     * precision instruction-level (no implementado en MVP).
     */
    struct MethodDebug {
        std::string source_file;   ///< nombre del .vex source (e.g. "foo.vex")
        uint32_t    start_line;    ///< linea 1-based del inicio del metodo
    };

    /**
     * @brief Registra metadata de debug para un MethodInfo.  Idempotente:
     *        si la entrada ya existe, la sobreescribe.  Llamado por el
     *        opcode @c setmethdbg desde @c __module_init.
     */
    void register_method_debug(loader::MethodInfo *method,
                                const char *file, size_t file_len,
                                uint32_t line);

    /**
     * @brief Lookup de metadata de debug para un MethodInfo.
     * @return puntero a la entrada, o @c nullptr si no esta registrada.
     */
    const MethodDebug *lookup_method_debug(loader::MethodInfo *method);

    /**
     * @brief Construye un stack trace estilo Java para el proceso actual.
     *
     * Formato de cada linea: "  at <ClassName>.<methodName>(line:<n>)"
     * cuando la metadata esta disponible; "  at <pc=0xADDR>" como
     * fallback para frames sin metodo (e.g. CALLN nativo).
     *
     * El primer frame siempre es donde ocurrio el error (PC actual);
     * los siguientes recorren @c frame_stack hasta el origen del proceso.
     *
     * El buffer @p out se trunca a @p out_size bytes incluido el nul
     * terminador.  Devuelve la longitud escrita sin contar el nul.
     *
     * @param vm        Proceso a inspeccionar.
     * @param out       Buffer destino.
     * @param out_size  Tamano de @p out (debe ser >= 64 para mensajes utiles).
     * @return          Numero de bytes escritos en @p out (sin nul).
     */
    size_t build_stack_trace(ProcessVM *vm, char *out, size_t out_size);

    // ---------------------------------------------------------------------
    // OS-level access violation -> FatalError capturable.
    //
    // Cuando el bytecode dereferencia un puntero host invalido (el caso
    // mas comun: `*null_host_ptr`, deref de un host_ptr stale tras free,
    // o aritmetica que sale del bloque malloc'eado), el OS dispara un
    // EXCEPTION_ACCESS_VIOLATION (Windows) o SIGSEGV (POSIX).  Sin la
    // infraestructura de abajo, la VM entera crashea sin que el `try`
    // del usuario lo atrape.
    //
    // El plan:
    //   - install_host_av_handler() registra UN VEH global (Windows) /
    //     SIGSEGV handler (POSIX) la primera vez que se llama.  Idempotente.
    //   - El scheduler hace setjmp(proc->av_recovery_jmpbuf) antes de
    //     cada batch de instrucciones y marca av_recovery_active=true.
    //   - Define el thread-local @c g_executing_proc al inicio del batch.
    //   - Si ocurre un AV durante la ejecucion: el handler lee el TLS,
    //     verifica av_recovery_active y exc_frame_stack, guarda la
    //     direccion del AV en proc->pending_av_addr, y dispara longjmp
    //     (POSIX directamente; Windows via redireccion de RIP a un stub
    //     que llama longjmp).
    //   - Tras el longjmp, el scheduler llama throw_fatal con
    //     FATAL_SEGMENTATION_FAULT.  El flujo normal de do_throw saltara
    //     al handler `catch (FatalError e)` del usuario.
    // ---------------------------------------------------------------------

    /**
     * @brief Instala el handler global de access violations.  Idempotente
     *        (registra solo la primera llamada via std::once).
     *
     * Se llama desde el constructor de VM, tras init_exception_classes.
     */
    void install_host_av_handler() noexcept;

    /**
     * @brief Setea (o limpia con nullptr) el ProcessVM cuyo bytecode
     *        esta corriendo en el thread actual.  Lo llama el scheduler
     *        antes y despues de cada batch de instrucciones.
     *
     * El handler de AV consulta este TLS para identificar a quien
     * enviar el longjmp.  Si esta a nullptr cuando ocurre un AV, el
     * handler delega a la cadena de SEH normal (la VM crashea como
     * antes).
     */
    void set_current_executing_process(ProcessVM *proc) noexcept;

    /**
     * @brief Devuelve el ProcessVM actualmente en ejecucion en este
     *        thread, o nullptr si no hay ninguno.
     */
    ProcessVM *get_current_executing_process() noexcept;

} // namespace runtime
