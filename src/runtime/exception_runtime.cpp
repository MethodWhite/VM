/**
 * @file exception_runtime.cpp
 * @brief Implementacion del sistema FatalError
 */

#include "runtime/exception_runtime.h"
#include "runtime/proceso_runtime.h"
#include "runtime/scheduler.h"
#include "runtime/runtime.h"
#include "loader/loader.h"
#include "loader/class_registry.h"
#include "debug/debugger.h"

#include <csetjmp>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <unordered_map>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <signal.h>
#endif

namespace runtime {

    // ---------------------------------------------------------------------
    // Estado global de la clase FatalError
    // ---------------------------------------------------------------------

    loader::ClassInfo *g_fatal_error_class = nullptr;
    static std::once_flag g_fatal_init_flag;

    // ---------------------------------------------------------------------
    // Tabla de debug por metodo.  Mapea MethodInfo* -> MethodDebug.
    // Mutex porque puede llenarse concurrentemente desde varios procesos
    // (cada VM ejecuta su __module_init).  Lecturas concurrentes con
    // shared_lock no necesarias en MVP -- el lookup es raro (solo en
    // build_stack_trace que solo corre tras un crash).
    // ---------------------------------------------------------------------

    static std::unordered_map<loader::MethodInfo *, MethodDebug> g_method_debug;
    static std::mutex g_method_debug_mtx;

    void register_method_debug(loader::MethodInfo *method,
                                const char *file, size_t file_len,
                                uint32_t line) {
        if (!method) return;
        std::lock_guard<std::mutex> lk(g_method_debug_mtx);
        MethodDebug md;
        md.source_file.assign(file ? file : "", file ? file_len : 0);
        md.start_line = line;
        g_method_debug[method] = std::move(md);
    }

    const MethodDebug *lookup_method_debug(loader::MethodInfo *method) {
        if (!method) return nullptr;
        std::lock_guard<std::mutex> lk(g_method_debug_mtx);
        auto it = g_method_debug.find(method);
        if (it == g_method_debug.end()) return nullptr;
        return &it->second;
    }

    // Offsets de los fields de FatalError dentro del slot de 56 bytes.
    // Documentados en exception_runtime.h.  Constantes para que los
    // accesos sean directos sin tener que mirar el FieldInfo en cada
    // throw (perf-critical path).
    constexpr uint32_t FATAL_OFF_KIND    = sizeof(loader::ObjectHeader);       // 24
    constexpr uint32_t FATAL_OFF_PC      = FATAL_OFF_KIND   + 8;               // 32
    constexpr uint32_t FATAL_OFF_MSG_PTR = FATAL_OFF_PC     + 8;               // 40
    constexpr uint32_t FATAL_OFF_TRACE_PTR = FATAL_OFF_MSG_PTR + 8;            // 48
    constexpr uint32_t FATAL_SLOT_SIZE   = FATAL_OFF_TRACE_PTR + 8;            // 56

    // ---------------------------------------------------------------------
    // Init: registra FatalError en el ClassRegistry del Loader publico.
    // ---------------------------------------------------------------------

    void init_exception_classes(loader::Loader &loader_ref) {
        // Lock once: la primera VM hace el define_class; las siguientes
        // ven g_fatal_error_class ya inicializado y salen sin tocar el
        // registry.  init_once garantiza thread-safety.
        std::call_once(g_fatal_init_flag, [&loader_ref]() {
            auto &reg = loader_ref.class_registry();

            // Si ya existe (e.g. test que reinicia la VM), reutilizamos.
            if (auto *existing = reg.find_class("FatalError")) {
                g_fatal_error_class = existing;
                return;
            }

            // Definicion de los 4 fields fijos.  Todos kind=PRIMITIVE
            // size 8 para que los offsets sean predecibles y sin padding
            // adicional (el ABI documentado en exception_runtime.h asume
            // qword-aligned).
            std::vector<loader::FieldDecl> fields;
            fields.reserve(4);
            {
                loader::FieldDecl f{};
                f.name = "kind";
                f.kind = loader::FIELD_PRIMITIVE;
                f.size_bytes = 8;            // se pad a 8 aunque sea i32 logico
                f.access = loader::FIELD_PUBLIC;
                fields.push_back(f);
            }
            {
                loader::FieldDecl f{};
                f.name = "pc";
                f.kind = loader::FIELD_PRIMITIVE;
                f.size_bytes = 8;
                f.access = loader::FIELD_PUBLIC;
                fields.push_back(f);
            }
            {
                loader::FieldDecl f{};
                f.name = "message";
                f.kind = loader::FIELD_PRIMITIVE;    // tratado como puntero opaco
                f.size_bytes = 8;
                f.access = loader::FIELD_PUBLIC;
                fields.push_back(f);
            }
            {
                loader::FieldDecl f{};
                f.name = "stack_trace";
                f.kind = loader::FIELD_PRIMITIVE;
                f.size_bytes = 8;
                f.access = loader::FIELD_PUBLIC;
                fields.push_back(f);
            }

            // Sin metodos por ahora (en Vex se accederan via getfield
            // directo).  Sin super (hereda de Object implicito).
            g_fatal_error_class = reg.define_class(
                "FatalError",
                /*super=*/nullptr,
                /*interfaces=*/{},
                fields,
                /*methods=*/{},
                /*flags=*/0);
        });
    }

    // ---------------------------------------------------------------------
    // Stack trace builder estilo Java
    // ---------------------------------------------------------------------

    size_t build_stack_trace(ProcessVM *vm, char *out, size_t out_size) {
        if (!out || out_size < 2) return 0;
        size_t pos = 0;
        auto append = [&](const char *s, size_t n) {
            const size_t avail = (out_size - 1) - pos;
            const size_t cp = (n < avail) ? n : avail;
            std::memcpy(out + pos, s, cp);
            pos += cp;
        };
        auto append_str = [&](const char *s) {
            if (!s) return;
            append(s, std::strlen(s));
        };
        auto append_hex = [&](uint64_t v) {
            char buf[20];
            int n = std::snprintf(buf, sizeof(buf), "0x%llX",
                                   (unsigned long long)v);
            if (n > 0) append(buf, (size_t)n);
        };
        auto append_dec = [&](uint64_t v) {
            char buf[24];
            int n = std::snprintf(buf, sizeof(buf), "%llu",
                                   (unsigned long long)v);
            if (n > 0) append(buf, (size_t)n);
        };

        // Cabecera con info del proceso y PC actual.
        append_str("Stack trace (Vesta):\n");

        // Frame 0: el PC actual (donde ocurrio el error).  Si hay
        // method en frame_stack lo usamos; si no, solo PC.
        loader::FrameHeader *fr = vm->frame_stack;
        const uint64_t cur_pc = vm->registers.rip.raw();

        // Helper: append "(file.vex:line)" o "(pc=0xADDR)".
        auto append_dbg = [&](loader::MethodInfo *m, uint64_t pc,
                              const char *pc_label) {
            const MethodDebug *md = m ? lookup_method_debug(m) : nullptr;
            if (md && !md->source_file.empty()) {
                append_str(" (");
                append(md->source_file.data(), md->source_file.size());
                append_str(":");
                append_dec((uint64_t)md->start_line);
                append_str(")");
            } else {
                append_str(" (");
                append_str(pc_label);
                append_str("=");
                append_hex(pc);
                append_str(")");
            }
        };

        // Primer frame: usar metodo del top de la pila si existe.
        append_str("  at ");
        if (fr && fr->method) {
            const auto &mname = fr->method->name;
            if (mname.data && mname.size > 0) {
                append((const char *)mname.data, mname.size);
            } else {
                append_str("<unknown>");
            }
            // Clase del metodo (owner_class o owner_class si esta).
            if (fr->method->owner_class) {
                const auto &cname = fr->method->owner_class->name;
                if (cname.data && cname.size > 0) {
                    append_str(" [");
                    append((const char *)cname.data, cname.size);
                    append_str("]");
                }
            }
            append_dbg(fr->method, cur_pc, "pc");
            append_str("\n");
        } else {
            append_str("<top> (pc=");
            append_hex(cur_pc);
            append_str(")\n");
        }

        // Frames intermedios: recorrer frame_stack hasta el origen.
        // Limitamos a 64 frames para evitar runaway en casos degenerados.
        int depth = 0;
        for (loader::FrameHeader *p = fr; p != nullptr && depth < 64;
             p = p->prev, ++depth)
        {
            append_str("  at ");
            if (p->method) {
                const auto &mname = p->method->name;
                if (mname.data && mname.size > 0) {
                    append((const char *)mname.data, mname.size);
                } else {
                    append_str("<unknown>");
                }
                if (p->method->owner_class) {
                    const auto &cname = p->method->owner_class->name;
                    if (cname.data && cname.size > 0) {
                        append_str(" [");
                        append((const char *)cname.data, cname.size);
                        append_str("]");
                    }
                }
                append_dbg(p->method, p->return_pc, "return_pc");
            } else {
                append_str("<frameless> (return_pc=");
                append_hex(p->return_pc);
                append_str(")");
            }
            append_str("\n");
        }
        if (depth >= 64) {
            append_str("  ... (truncated, depth >= 64)\n");
        }

        // Pid del proceso para diagnostico cross-process.
        append_str("  in process pid=");
        append_dec(vm->pid.local_pid);
        append_str(" sched=");
        append_dec(vm->pid.scheduler_id);
        append_str("\n");

        out[pos] = '\0';
        return pos;
    }

    // ---------------------------------------------------------------------
    // Helper interno: asegura que vm tiene los buffers reservados.
    // Lazy alloc en el primer throw_fatal del proceso.
    // ---------------------------------------------------------------------

    static void ensure_fatal_buffers(ProcessVM *vm) {
        if (!vm->fatal_slot) {
            vm->fatal_slot = std::calloc(1, FATAL_SLOT_SIZE);
        }
        if (!vm->fatal_msg_buf) {
            vm->fatal_msg_buf = (char *)std::malloc(256);
            if (vm->fatal_msg_buf) vm->fatal_msg_buf[0] = '\0';
        }
        if (!vm->fatal_trace_buf) {
            vm->fatal_trace_buf = (char *)std::malloc(4096);
            if (vm->fatal_trace_buf) vm->fatal_trace_buf[0] = '\0';
        }
    }

    // ---------------------------------------------------------------------
    // Forward decl: do_throw vive en exec_instruction_oop.cpp.
    // ---------------------------------------------------------------------
    void do_throw(ProcessVM *vm, uint64_t exception_ptr);

    /**
     * @brief Mapea FATAL_* a state_err_thread para la ruta antigua.
     */
    static state_err_thread fatal_to_thread_err(uint32_t kind) noexcept {
        switch (kind) {
            case FATAL_NULL_POINTER:        return THREAD_NULL_POINTER;
            case FATAL_DIVISION_BY_ZERO:    return THREAD_DIVISION_BY_ZERO;
            case FATAL_STACK_OVERFLOW:      return THREAD_STACK_OVERFLOW;
            case FATAL_STACK_UNDERFLOW:     return THREAD_STACK_UNDERFLOW;
            case FATAL_ILLEGAL_INSTRUCTION: return THREAD_ILLEGAL_INSTRUCTION;
            case FATAL_INVALID_SYSCALL:     return THREAD_INVALID_SYSCALL;
            case FATAL_SEGMENTATION_FAULT:  return THREAD_SEGMENTATION_FAULT;
            default:                        return THREAD_SEGMENTATION_FAULT;
        }
    }

    // ---------------------------------------------------------------------
    // throw_fatal: ruta dual (capturable / fatal-original).
    // ---------------------------------------------------------------------

    void throw_fatal(ProcessVM *vm, uint32_t kind, const char *message) {
        if (!vm) return;

        // Notificar al debugger de la excepcion ANTES de elegir ruta
        // (capturable o fatal).  El cliente recibe evento "exception"
        // con clase + pid; util para parar y ver el contexto incluso
        // si el codigo del usuario captura la excepcion.  Si no hay
        // debugger activo (caso comun), la rama es 1 carga + 1 branch
        // bien predicho (~0 ns en hot path).
        if (vm->scheduler.vm_reference.debugger != nullptr) {
            vm->scheduler.vm_reference.debugger->on_exception(
                vm->pid.local_pid,
                message ? std::string(message) : std::string("FatalError"));
        }

        // FAST PATH: si no hay handler activo, ruta antigua.  Esto es
        // el caso comun (programas sin try/catch envolvente).  Cero
        // overhead anadido: 1 lectura + 1 branch.
        if (vm->exc_frame_stack == nullptr) {
            vm->err_thread = fatal_to_thread_err(kind);
            vm->scheduler.on_event(EVT_ERROR);
            return;
        }

        // Si por algun motivo la clase FatalError no esta registrada
        // (init_exception_classes no se llamo), no podemos lanzar como
        // excepcion -- caemos al camino antiguo.
        if (g_fatal_error_class == nullptr) {
            vm->err_thread = fatal_to_thread_err(kind);
            vm->scheduler.on_event(EVT_ERROR);
            return;
        }

        // SLOW PATH (handler activo): construir la instancia FatalError.
        ensure_fatal_buffers(vm);
        if (!vm->fatal_slot) {
            // OOM en el slot mismo: ruta antigua.
            vm->err_thread = fatal_to_thread_err(kind);
            vm->scheduler.on_event(EVT_ERROR);
            return;
        }

        // Inicializar ObjectHeader.
        auto *hdr = reinterpret_cast<loader::ObjectHeader *>(vm->fatal_slot);
        std::memset(hdr, 0, sizeof(loader::ObjectHeader));
        hdr->class_ptr  = g_fatal_error_class;
        hdr->flags      = 0;
        hdr->hash_code  = 0;

        // Llenar fields por offset directo.
        char *base = (char *)vm->fatal_slot;
        const uint64_t pc_now = vm->registers.rip.raw();
        *(uint64_t *)(base + FATAL_OFF_KIND)    = (uint64_t)kind;
        *(uint64_t *)(base + FATAL_OFF_PC)      = pc_now;

        // Copiar mensaje al buffer reusable (truncar a 255).
        if (vm->fatal_msg_buf && message) {
            std::strncpy(vm->fatal_msg_buf, message, 255);
            vm->fatal_msg_buf[255] = '\0';
        }
        *(uint64_t *)(base + FATAL_OFF_MSG_PTR) =
            (uint64_t)(uintptr_t)vm->fatal_msg_buf;

        // Construir stack trace (ya incluye el PC y los frames).
        if (vm->fatal_trace_buf) {
            build_stack_trace(vm, vm->fatal_trace_buf, 4096);
        }
        *(uint64_t *)(base + FATAL_OFF_TRACE_PTR) =
            (uint64_t)(uintptr_t)vm->fatal_trace_buf;

        // Lanzar via do_throw (mismo patron que `throw new X` en bytecode).
        do_throw(vm, (uint64_t)(uintptr_t)vm->fatal_slot);
    }

    // ---------------------------------------------------------------------
    // exec_instr_panic: lee mensaje de la VM y lanza FATAL_USER_ABORT.
    // ---------------------------------------------------------------------

    // =====================================================================
    // OS-level access violation -> FatalError capturable.
    // =====================================================================

    // Thread-local: ProcessVM cuyo bytecode esta corriendo.  El handler
    // de AV/SIGSEGV lo consulta para identificar el receptor del longjmp.
    // Limitacion: el VM corre cada proceso en su scheduler, asi que un
    // mismo OS thread tiene UN ProcessVM activo a la vez.  Si en el
    // futuro se anade preemption con context-switch dentro del run_loop,
    // habria que actualizar el TLS antes de cambiar de proceso.
#if defined(__GNUC__) || defined(__clang__)
    static __thread ProcessVM *t_executing_proc = nullptr;
#elif defined(_MSC_VER)
    static __declspec(thread) ProcessVM *t_executing_proc = nullptr;
#else
    static thread_local ProcessVM *t_executing_proc = nullptr;
#endif

    void set_current_executing_process(ProcessVM *proc) noexcept {
        t_executing_proc = proc;
    }

    ProcessVM *get_current_executing_process() noexcept {
        return t_executing_proc;
    }

    static std::once_flag g_av_handler_init_flag;

#if defined(_WIN32)
    /**
     * @brief Stub al que el VEH redirige RIP cuando ocurre un AV
     *        capturable.  Corre en contexto de usuario normal (no async)
     *        asi que el longjmp es seguro aqui.
     *
     * Marcado @c noinline para que el compilador no inline el contenido
     * en otro punto del binary -- queremos una direccion estable que el
     * VEH pueda apuntar.
     */
    static void __attribute__((noinline)) av_recovery_stub() {
        ProcessVM *proc = t_executing_proc;
        if (proc != nullptr && proc->av_recovery_active) {
            std::longjmp(proc->av_recovery_jmpbuf, 1);
        }
        // Sin recovery activo: no podemos hacer nada utilidad; salir.
        // En la practica esto no deberia ocurrir porque el VEH ya
        // verifica av_recovery_active antes de redirigir.
        std::abort();
    }

    /**
     * @brief Vectored Exception Handler global.  Solo trata
     *        EXCEPTION_ACCESS_VIOLATION cuando el thread actual esta
     *        ejecutando bytecode con un try/catch envolvente.  En todos
     *        los demas casos retorna EXCEPTION_CONTINUE_SEARCH para no
     *        interferir con el manejo normal de excepciones (otros
     *        plugins, debugger, etc.).
     */
    static LONG WINAPI vex_av_veh(EXCEPTION_POINTERS *info) {
        if (info == nullptr || info->ExceptionRecord == nullptr) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        const DWORD code = info->ExceptionRecord->ExceptionCode;
        // Solo manejamos AV.  Otras excepciones (div0 hardware, illegal
        // instr, debug breakpoints, etc.) siguen su curso normal.
        if (code != EXCEPTION_ACCESS_VIOLATION) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        ProcessVM *proc = t_executing_proc;
        if (proc == nullptr || !proc->av_recovery_active) {
            // No hay bytecode corriendo o el scheduler no ha armado el
            // setjmp: la VM crashea como antes (comportamiento legado).
            return EXCEPTION_CONTINUE_SEARCH;
        }
        // Capturar la direccion del fault (segundo elemento de
        // ExceptionInformation: 0=read/write flag, 1=virtual addr).
        if (info->ExceptionRecord->NumberParameters >= 2) {
            proc->pending_av_addr =
                (uint64_t)info->ExceptionRecord->ExceptionInformation[1];
        } else {
            proc->pending_av_addr = 0;
        }
        // Redirigir RIP al stub que hara longjmp en contexto normal.
        // No tocamos RSP: el stub es una funcion C++ valida; reusara
        // el frame del callee, que ya tiene espacio suficiente.
#if defined(_M_X64) || defined(__x86_64__)
        info->ContextRecord->Rip = (DWORD64)(uintptr_t)&av_recovery_stub;
#elif defined(_M_IX86) || defined(__i386__)
        info->ContextRecord->Eip = (DWORD)(uintptr_t)&av_recovery_stub;
#elif defined(_M_ARM64) || defined(__aarch64__)
        info->ContextRecord->Pc  = (DWORD64)(uintptr_t)&av_recovery_stub;
#else
        // Plataforma desconocida: dejar que la VM crashee como antes.
        return EXCEPTION_CONTINUE_SEARCH;
#endif
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    void install_host_av_handler() noexcept {
        std::call_once(g_av_handler_init_flag, []() {
            // Primer parametro = 1 -> registrar al inicio de la cadena
            // (antes del default Windows handler).  Asi capturamos AVs
            // antes de que llegue el "Application has stopped" del OS.
            (void)AddVectoredExceptionHandler(1u, &vex_av_veh);
        });
    }
#else
    // POSIX: handler de SIGSEGV.  El handler usa siglongjmp directamente
    // ya que en POSIX longjmp desde un signal handler es bien definido
    // si se usa sigsetjmp con savesigs=1.
    static void posix_sigsegv_handler(int /*sig*/, siginfo_t *info, void *ctx) {
        (void)ctx;
        ProcessVM *proc = t_executing_proc;
        if (proc == nullptr || !proc->av_recovery_active) {
            // Restaurar handler default y dejar que el OS mate el proceso.
            std::signal(SIGSEGV, SIG_DFL);
            std::raise(SIGSEGV);
            return;
        }
        proc->pending_av_addr = info ? (uint64_t)(uintptr_t)info->si_addr : 0;
        std::longjmp(proc->av_recovery_jmpbuf, 1);
    }

    void install_host_av_handler() noexcept {
        std::call_once(g_av_handler_init_flag, []() {
            struct sigaction sa{};
            sa.sa_flags     = SA_SIGINFO | SA_NODEFER | SA_ONSTACK;
            sa.sa_sigaction = &posix_sigsegv_handler;
            sigemptyset(&sa.sa_mask);
            (void)sigaction(SIGSEGV, &sa, nullptr);
            (void)sigaction(SIGBUS,  &sa, nullptr);
        });
    }
#endif

} // namespace runtime

// Definicion del exec_instr fuera del namespace runtime para matchear
// la declaracion en runtime/exec_instruction.h (que usa namespace runtime
// implicitamente al ser includado en archivos que ya estan en namespace
// runtime).  Pero para mantener consistencia con los otros exec_instr_*,
// va dentro del namespace runtime tambien.

namespace runtime {

    /**
     * @brief Layout de SetMethDebugParams (24 bytes en VM mem).
     */
    struct SetMethDebugParams {
        uint64_t method_ptr;
        uint64_t file_addr;
        uint32_t file_len;
        uint32_t start_line;
    };

    void exec_instr_setmethdbg(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t r_method = instr.data_instruction.reg_data.reg1;
        const uint8_t r_params = instr.data_instruction.reg_data.reg2;
        (void)r_method;  // por ahora usamos solo el params (que tambien
                         // lleva method_ptr); r_method redundante para
                         // futuros usos del decoder.

        const uint64_t params_addr = vm->registers.regs[r_params].qword();
        if (params_addr == 0) return;

        SetMethDebugParams sp{};
        vm->vm_mem.read_bytes(params_addr, &sp, sizeof(sp));
        if (sp.method_ptr == 0) return;

        // Leer nombre de archivo del VM mem (truncar a 256 bytes).
        char fbuf[256];
        size_t flen = (size_t)sp.file_len;
        if (flen > 255) flen = 255;
        if (sp.file_addr != 0 && flen > 0) {
            vm->vm_mem.read_bytes(sp.file_addr, fbuf, flen);
        }
        fbuf[flen] = '\0';

        register_method_debug(
            reinterpret_cast<loader::MethodInfo *>(sp.method_ptr),
            fbuf, flen, sp.start_line);
    }

    void exec_instr_panic(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t r_addr = instr.data_instruction.reg_data.reg1;
        const uint8_t r_len  = instr.data_instruction.reg_data.reg2;

        const uint64_t vm_addr = vm->registers.regs[r_addr].qword();
        const uint64_t len     = vm->registers.regs[r_len].qword();

        // Leer el mensaje desde la VM mem a un buffer de pila.  Truncar a
        // 255 bytes (la copia interna en throw_fatal lo trunca a 256
        // incluyendo nul).  Si len = 0 usamos un mensaje por defecto.
        char buf[256];
        if (len == 0 || vm_addr == 0) {
            std::strncpy(buf, "panic", sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
        } else {
            const size_t n_copy = (len < 255) ? (size_t)len : 255;
            vm->vm_mem.read_bytes(vm_addr, buf, n_copy);
            buf[n_copy] = '\0';
        }

        throw_fatal(vm, FATAL_USER_ABORT, buf);
    }

    void throw_fatalf(ProcessVM *vm, uint32_t kind, const char *fmt, ...) {
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        const int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        if (n < 0) {
            throw_fatal(vm, kind, "<fmt error>");
            return;
        }
        // vsnprintf trunca implicitamente y ya pone nul terminador.
        throw_fatal(vm, kind, buf);
    }

} // namespace runtime
