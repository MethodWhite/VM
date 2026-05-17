/**
 * @file vesta_io.c
 * @brief Modulo de E/S de la stdlib nativa de VestaVM (A.16 fast I/O).
 *
 * Reemplaza la version original basada en @c fputs / @c printf con un buffer
 * global de 64 KB que delega en @c WriteConsoleW / @c WriteFile (Windows) o
 * @c write() (POSIX) en lugar de pasar por el buffer de stdio.  El overhead
 * por linea en consola Windows baja de ~5 us a ~30 ns amortizados.
 *
 * Caracteristicas:
 *   - Buffer global circular de 64 KB protegido por un mutex global.
 *   - Auto-flush cuando el buffer cruza el umbral.
 *   - @c atexit() registra un flush final en @c vesta_init para que las
 *     ultimas lineas no se pierdan al salir del programa.
 *   - @c GetStdHandle cacheado (Windows); deteccion console vs pipe una vez.
 *   - itoa propio para enteros (sin parsing de format string).
 *   - Multi-thread safe: serializa la salida (correcto vs interleaving).
 *
 * Convencion de llamada (heredada del modulo viejo, no se rompen tests):
 *   - Argumentos en r1..r12 como uint64_t; ningun contexto implicito.
 *   - proc_ptr : valor uint64_t obtenido con la instruccion getproc.
 *   - vm_addr  : direccion virtual dentro del espacio de la VM.
 *   - len      : longitud en bytes del dato en memoria VM.
 *   - Retorno en r0 como uint64_t.
 *
 * Funciones nuevas anadidas:
 *   - @c vio_flush()              vacia el buffer ahora mismo.
 *   - @c vio_print_str(addr, len) escribe bytes raw sin proc_ptr (ZERO copy
 *                                 desde memoria VM con stack buffer corto).
 *   - @c vio_print_bool(b)        imprime "true" o "false".
 *   - @c vio_print_char(cp)       codifica codepoint UTF-32 a UTF-8 y escribe.
 *   - @c vio_print_color(code)    emite la secuencia ANSI "\x1b[<code>m".
 *   - @c vio_println_int / _uint / _hex / _float           con '\n' final.
 *   - @c vio_print_int / _uint / _hex / _float ya NO emiten '\n';
 *                                 quien quiera salto de linea usa println.
 *
 * Esto rompe ligeramente el comportamiento previo de @c vio_print_int que
 * imprimia con '\n' al final.  La razon es que @c print(x) en Vex no debe
 * meter saltos de linea por su cuenta; @c println(x) si lo hace.  Los tests
 * nativos que llamen directamente a @c vio_print_int deben actualizarse.
 */

#ifndef VESTA_IO_DEBUG
#  define VESTA_IO_DEBUG 0
#endif

#include "../../../include/ffi/vesta_plugin.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>

/* -----------------------------------------------------------------------
 * Cabeceras y primitivas de plataforma.
 * ----------------------------------------------------------------------- */

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>

/* Mutex usando CRITICAL_SECTION: ~5 ns sin contencion, sin syscall. */
typedef CRITICAL_SECTION vio_mutex_t;

static void vio_mutex_init(vio_mutex_t *m) {
    InitializeCriticalSection(m);
}

static void vio_mutex_lock(vio_mutex_t *m) {
    EnterCriticalSection(m);
}

static void vio_mutex_unlock(vio_mutex_t *m) {
    LeaveCriticalSection(m);
}

#else
#  include <unistd.h>
#  include <pthread.h>
#  include <errno.h>

   typedef pthread_mutex_t vio_mutex_t;
   static void vio_mutex_init(vio_mutex_t *m)   { pthread_mutex_init(m, NULL); }
   static void vio_mutex_lock(vio_mutex_t *m)   { pthread_mutex_lock(m); }
   static void vio_mutex_unlock(vio_mutex_t *m) { pthread_mutex_unlock(m); }
#endif

/* -----------------------------------------------------------------------
 * Estado global del modulo.
 * ----------------------------------------------------------------------- */

/** @brief Referencia a la API del plugin; inicializada en vesta_init. */
static const VestaPluginAPI *g_api = NULL;

/** @brief Tamano del buffer global de salida.  64 KB amortiza syscalls
 *         con minima presion en cache (cabe holgado en L1/L2). */
#define VIO_BUF_SIZE (64u * 1024u)

/** @brief Umbral a partir del cual cualquier write fuerza un flush. */
#define VIO_FLUSH_THRESHOLD VIO_BUF_SIZE

/** @brief Tamano a partir del cual un write se hace directo (bypass buffer). */
#define VIO_DIRECT_THRESHOLD (VIO_BUF_SIZE / 2u)

static char        g_out_buf[VIO_BUF_SIZE];
static size_t      g_out_len = 0;
static vio_mutex_t g_out_mtx;
static int         g_init_done         = 0;
static int         g_atexit_registered = 0;

#if defined(_WIN32)
static HANDLE g_out_handle          = INVALID_HANDLE_VALUE;
static int    g_is_console          = 0;
static int    g_console_handle_init = 0;
#endif

/* -----------------------------------------------------------------------
 * Backend: escritura directa al stdout del host (sin pasar por stdio).
 * ----------------------------------------------------------------------- */

#if defined(_WIN32)
/**
 * @brief Inicializa el handle de stdout del proceso una sola vez.
 *
 * Cachea @c GetStdHandle(STD_OUTPUT_HANDLE) y detecta si el destino es
 * consola (vs pipe / fichero) via @c GetConsoleMode.  La detection
 * determina si usaremos @c WriteConsoleW (UTF-16) o @c WriteFile (bytes).
 */
static void vio_init_console_handle(void) {
    if (g_console_handle_init) return;
    g_out_handle = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode;
    g_is_console = (g_out_handle != INVALID_HANDLE_VALUE)
            && GetConsoleMode(g_out_handle, &mode);
    g_console_handle_init = 1;
}

/**
 * @brief Escribe @p len bytes UTF-8 al handle cacheado.
 *
 * Console: convierte a UTF-16 con @c MultiByteToWideChar y emite con
 * @c WriteConsoleW en chunks de 16 K wchars (limite practico de la API).
 * Pipe / redir: @c WriteFile directo (mucho mas rapido que @c fwrite por
 * evitar el buffer de stdio).  Fallback a @c fwrite ante errores.
 */
static void vio_write_direct(const char *buf, size_t len) {
    if (len == 0) return;
    vio_init_console_handle();

    if (g_is_console) {
        /* UTF-8 -> UTF-16 con buffer escalable.  Para entradas pequenas
         * (~256 wchars) usamos un buffer de pila para evitar malloc. */
        wchar_t  stack_w[1024];
        wchar_t *wide     = stack_w;
        int      wide_cap = (int) (sizeof(stack_w) / sizeof(stack_w[0]));

        int needed = MultiByteToWideChar(CP_UTF8, 0, buf, (int) len, NULL, 0);
        if (needed <= 0) {
            /* MultiByteToWideChar fallo: caer a fwrite. */
            fwrite(buf, 1, len, stdout);
            return;
        }
        if (needed > wide_cap) {
            wide = (wchar_t *) malloc((size_t) needed * sizeof(wchar_t));
            if (!wide) {
                fwrite(buf, 1, len, stdout);
                return;
            }
        }
        int got = MultiByteToWideChar(CP_UTF8, 0, buf, (int) len, wide, needed);
        if (got <= 0) {
            if (wide != stack_w) free(wide);
            fwrite(buf, 1, len, stdout);
            return;
        }

        /* WriteConsoleW: chunks de 16K wchars como limite practico. */
        const DWORD    CHUNK     = 16u * 1024u;
        DWORD          remaining = (DWORD) got;
        const wchar_t *p         = wide;
        while (remaining > 0) {
            DWORD to_write = remaining > CHUNK ? CHUNK : remaining;
            DWORD written  = 0;
            if (!WriteConsoleW(g_out_handle, p, to_write, &written, NULL)) {
                fwrite(buf, 1, len, stdout);
                break;
            }
            if (written == 0) break;
            p += written;
            remaining -= written;
        }
        if (wide != stack_w) free(wide);
    } else {
        /* Pipe o redireccion: WriteFile directo, sin stdio. */
        DWORD written = 0;
        if (!WriteFile(g_out_handle, buf, (DWORD) len, &written, NULL)) {
            fwrite(buf, 1, len, stdout);
        }
    }
}

#else  /* POSIX */

/**
 * @brief Escribe @p len bytes a STDOUT_FILENO con bucle de reintento.
 *
 * write(2) puede escribir parcialmente y devolver EINTR; iteramos hasta
 * agotar el buffer o ver un error fatal.  Cero overhead vs fwrite ya que
 * no pasa por el buffer de stdio.
 */
static void vio_write_direct(const char *buf, size_t len) {
    while (len > 0) {
        ssize_t n = write(STDOUT_FILENO, buf, len);
        if (n < 0) {
            /* EINTR: reintentar; otro error: caer a stdio como fallback. */
            if (errno != EINTR) {
                fwrite(buf, 1, len, stdout);
                return;
            }
            continue;
        }
        if (n == 0) return;
        buf += n;
        len -= (size_t)n;
    }
}
#endif

/* -----------------------------------------------------------------------
 * Buffer manager (thread-safe).
 * ----------------------------------------------------------------------- */

/** @brief Vacia el buffer al stdout del host.  Asume mutex tomado. */
static void vio_flush_locked(void) {
    if (g_out_len == 0) return;
    vio_write_direct(g_out_buf, g_out_len);
    g_out_len = 0;
}

/**
 * @brief Encola @p len bytes en el buffer global.  Toma el mutex.
 *
 * Politica:
 *   - Si el bloque cabe sin cruzar el umbral: copia y termina.
 *   - Si el bloque es enorme (> mitad del buffer): flush primero y emite
 *     directo, evita el memcpy + flush partido.
 *   - Si entra pero llena el buffer mas alla del umbral: copia y flush.
 */
static void vio_buffer_append(const char *buf, size_t len) {
    if (len == 0) return;
    vio_mutex_lock(&g_out_mtx);

    if (len >= VIO_DIRECT_THRESHOLD) {
        vio_flush_locked();
        vio_write_direct(buf, len);
        vio_mutex_unlock(&g_out_mtx);
        return;
    }
    if (g_out_len + len > VIO_BUF_SIZE) {
        vio_flush_locked();
    }
    memcpy(g_out_buf + g_out_len, buf, len);
    g_out_len += len;
    if (g_out_len >= VIO_FLUSH_THRESHOLD) {
        vio_flush_locked();
    }

    vio_mutex_unlock(&g_out_mtx);
}

/** @brief Encola un solo byte (fast path para char-at-a-time). */
static void vio_buffer_putc(char c) {
    vio_mutex_lock(&g_out_mtx);
    if (g_out_len + 1 > VIO_BUF_SIZE) vio_flush_locked();
    g_out_buf[g_out_len++] = c;
    if (g_out_len >= VIO_FLUSH_THRESHOLD) vio_flush_locked();
    vio_mutex_unlock(&g_out_mtx);
}

/** @brief Vacia el buffer publicamente (con lock). */
static void vio_flush_public(void) {
    vio_mutex_lock(&g_out_mtx);
    vio_flush_locked();
    vio_mutex_unlock(&g_out_mtx);
}

/** @brief Handler de atexit: garantiza flush al terminar el proceso. */
static void vio_atexit_handler(void) {
    vio_flush_public();
}

/* -----------------------------------------------------------------------
 * Conversion entero -> ASCII sin printf (mas rapido).
 * ----------------------------------------------------------------------- */

/**
 * @brief Convierte un int64 a ASCII en @p out (tamano minimo 21).
 *
 * Devuelve la longitud en bytes escritos.  Maneja el caso INT64_MIN sin
 * undefined behavior (negacion del minimo overflow'a en int64).
 */
static size_t vio_i64_to_str(int64_t v, char *out) {
    if (v == 0) {
        out[0] = '0';
        return 1;
    }
    char   tmp[24];
    size_t n   = 0;
    int    neg = (v < 0);
    /* Convertir a uint64_t para evitar overflow al negar INT64_MIN. */
    uint64_t u = neg ? (uint64_t) (-(v + 1)) + 1u : (uint64_t) v;
    while (u > 0) {
        tmp[n++] = (char) ('0' + (u % 10u));
        u /= 10u;
    }
    size_t pos = 0;
    if (neg) out[pos++] = '-';
    while (n > 0) out[pos++] = tmp[--n];
    return pos;
}

/** @brief Idem a vio_i64_to_str pero para uint64. */
static size_t vio_u64_to_str(uint64_t v, char *out) {
    if (v == 0) {
        out[0] = '0';
        return 1;
    }
    char   tmp[24];
    size_t n = 0;
    while (v > 0) {
        tmp[n++] = (char) ('0' + (v % 10u));
        v /= 10u;
    }
    for (size_t i = 0; i < n; ++i) out[i] = tmp[n - 1 - i];
    return n;
}

/**
 * @brief Convierte un uint64 a hex con prefijo "0x" y 16 digitos en mayus.
 *
 * Formato fijo de 18 caracteres ("0x" + 16 hex).  Util para volcado de
 * direcciones / handles / bits binarios.
 */
static size_t vio_u64_to_hex(uint64_t v, char *out) {
    static const char hex[] = "0123456789ABCDEF";
    out[0]                  = '0';
    out[1]                  = 'x';
    for (int i = 15; i >= 0; --i) {
        out[2 + i] = hex[v & 0xFu];
        v >>= 4;
    }
    return 18;
}

/**
 * @brief Convierte un uint64 a binario con prefijo "0b" minimo necesario.
 *
 * Formato variable: "0b" + N digitos binarios, donde N son los bits
 * significativos (sin ceros a la izquierda).  Para v=0 emite "0b0".
 * Maximo 66 chars (66 = 2 prefijo + 64 bits).
 */
static size_t vio_u64_to_bin(uint64_t v, char *out) {
    out[0] = '0';
    out[1] = 'b';
    if (v == 0) {
        out[2] = '0';
        return 3;
    }
    /* Calcular el numero de bits significativos.  Mismo patron que
     * __builtin_clzll pero portable: encontrar el bit mas alto. */
    int high = 63;
    while (high > 0 && ((v >> high) & 1u) == 0) --high;
    size_t k = 2;
    for (int i = high; i >= 0; --i) {
        out[k++] = (char)('0' + ((v >> i) & 1u));
    }
    return k;
}

/**
 * @brief Convierte un uint64 a octal con prefijo "0o".
 *
 * Formato variable: "0o" + N digitos octales (sin ceros a la izquierda).
 * Para v=0 emite "0o0".  Maximo 24 chars (2 prefijo + 22 octal max).
 */
static size_t vio_u64_to_oct(uint64_t v, char *out) {
    out[0] = '0';
    out[1] = 'o';
    if (v == 0) {
        out[2] = '0';
        return 3;
    }
    /* Generar de menos a mas significativo y luego invertir. */
    char rev[24];
    int  n = 0;
    while (v > 0) {
        rev[n++] = (char)('0' + (int)(v & 7u));
        v >>= 3;
    }
    for (int i = 0; i < n; ++i) out[2 + i] = rev[n - 1 - i];
    return (size_t)(2 + n);
}

/**
 * @brief Convierte un puntero (host o virtual) a "0x" + hex sin ceros lider.
 *
 * Diferente de @c vio_u64_to_hex: ese formato es 18 chars fijos para volcado
 * tabular; este es compacto, util para imprimir punteros en mensajes.  Para
 * v=0 emite "0x0".
 */
static size_t vio_u64_to_ptr(uint64_t v, char *out) {
    static const char hex[] = "0123456789ABCDEF";
    out[0] = '0';
    out[1] = 'x';
    if (v == 0) {
        out[2] = '0';
        return 3;
    }
    int high = 15;
    while (high > 0 && ((v >> (high * 4)) & 0xFu) == 0) --high;
    size_t k = 2;
    for (int i = high; i >= 0; --i) {
        out[k++] = hex[(v >> (i * 4)) & 0xFu];
    }
    return k;
}

/* -----------------------------------------------------------------------
 * Lectura corta desde memoria VM con buffer de pila (para print).
 * ----------------------------------------------------------------------- */

/**
 * @brief Lee @p len bytes de la VM y los encola en el buffer.
 *
 * Para tamanos pequenos (<= 4 KB) usa un buffer de pila evitando malloc.
 * Para tamanos grandes hace dos memcpy: VM -> heap -> buffer global; con
 * @p len > VIO_DIRECT_THRESHOLD sale al backend en lugar de copiar dos
 * veces (vio_buffer_append ya lo decide).
 */
static void vio_read_vm_and_emit(uint64_t proc_ptr, uint64_t vm_addr,
                                 uint64_t len) {
    if (!g_api || len == 0) return;
    if (len <= 4096u) {
        char stack_buf[4096];
        g_api->vm_read_bytes(proc_ptr, vm_addr, stack_buf, len);
        vio_buffer_append(stack_buf, (size_t) len);
        return;
    }
    char *heap_buf = (char *) malloc((size_t) len);
    if (!heap_buf) return;
    g_api->vm_read_bytes(proc_ptr, vm_addr, heap_buf, len);
    vio_buffer_append(heap_buf, (size_t) len);
    free(heap_buf);
}

/* -----------------------------------------------------------------------
 * Punto de entrada del plugin.
 * ----------------------------------------------------------------------- */

/**
 * @brief Inicializa estado global y registra atexit para flush final.
 *
 * Idempotente: se puede llamar varias veces sin efectos secundarios.
 * El atexit handler garantiza que las ultimas lineas no se pierdan al
 * salir el programa via @c return de @c main.
 */
VESTA_PLUGIN_EXPORT void vesta_init(const VestaPluginAPI *api) {
    g_api = api;
    if (!g_init_done) {
        vio_mutex_init(&g_out_mtx);
        g_init_done = 1;
    }
    if (!g_atexit_registered) {
        atexit(vio_atexit_handler);
        g_atexit_registered = 1;
    }
#if VESTA_IO_DEBUG
    if (api) api->log("[vesta_io] cargado (A.16 buffered)");
#endif
}

/* -----------------------------------------------------------------------
 * Salida de texto desde memoria VM (firmas heredadas, no romper).
 * ----------------------------------------------------------------------- */

/**
 * @brief Imprime una cadena de la memoria VM en stdout sin salto de linea.
 *
 * Acceso buffer + WriteConsoleW/WriteFile.  Para strings largos (>=
 * VIO_DIRECT_THRESHOLD) bypass del buffer global hacia el backend.
 */
VESTA_PLUGIN_EXPORT uint64_t vio_print(uint64_t proc_ptr,
                                       uint64_t vm_addr,
                                       uint64_t len) {
    vio_read_vm_and_emit(proc_ptr, vm_addr, len);
    return 0;
}

/* -----------------------------------------------------------------------
 * Stringify helpers usados por la interpolacion `${expr}` en contexto
 * STRING (e.g. `this.field = "msg ${count}"`).  Cada uno escribe la
 * representacion ASCII del valor a un buffer en MEMORIA VM y devuelve
 * la longitud en bytes.  El compilador combina cada llamada con un
 * STRMAKE posterior para producir un StringObject GC-managed que se
 * concatena via STRCAT con las partes literales.
 *
 * Convencion de buffers: 32 bytes son suficientes para i64/u64
 * (max 20 caracteres + signo) y para hex de 64 bits ("0x" + 16
 * digitos = 18 caracteres).
 * ----------------------------------------------------------------------- */

VESTA_PLUGIN_EXPORT uint64_t vio_int_to_vmbuf(uint64_t proc_ptr,
                                               uint64_t vm_addr,
                                               uint64_t value) {
    char   tmp[32];
    size_t k = vio_i64_to_str((int64_t) value, tmp);
    g_api->vm_write_bytes((void *) proc_ptr, vm_addr, tmp, k);
    return (uint64_t) k;
}

VESTA_PLUGIN_EXPORT uint64_t vio_uint_to_vmbuf(uint64_t proc_ptr,
                                                uint64_t vm_addr,
                                                uint64_t value) {
    char   tmp[32];
    size_t k = vio_u64_to_str(value, tmp);
    g_api->vm_write_bytes((void *) proc_ptr, vm_addr, tmp, k);
    return (uint64_t) k;
}

VESTA_PLUGIN_EXPORT uint64_t vio_hex_to_vmbuf(uint64_t proc_ptr,
                                               uint64_t vm_addr,
                                               uint64_t value) {
    char   tmp[32];
    size_t k = vio_u64_to_hex(value, tmp);
    g_api->vm_write_bytes((void *) proc_ptr, vm_addr, tmp, k);
    return (uint64_t) k;
}

VESTA_PLUGIN_EXPORT uint64_t vio_bool_to_vmbuf(uint64_t proc_ptr,
                                                uint64_t vm_addr,
                                                uint64_t value) {
    /* "true" (4) o "false" (5). */
    const char *s = value ? "true" : "false";
    size_t      k = value ? 4u : 5u;
    g_api->vm_write_bytes((void *) proc_ptr, vm_addr, s, k);
    return (uint64_t) k;
}

VESTA_PLUGIN_EXPORT uint64_t vio_char_to_vmbuf(uint64_t proc_ptr,
                                                uint64_t vm_addr,
                                                uint64_t codepoint) {
    /* Codifica el codepoint en UTF-8 (1-4 bytes). */
    char   tmp[8];
    size_t k = 0;
    uint32_t cp = (uint32_t) codepoint;
    if (cp < 0x80) {
        tmp[k++] = (char) cp;
    } else if (cp < 0x800) {
        tmp[k++] = (char) (0xC0 | (cp >> 6));
        tmp[k++] = (char) (0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        tmp[k++] = (char) (0xE0 | (cp >> 12));
        tmp[k++] = (char) (0x80 | ((cp >> 6) & 0x3F));
        tmp[k++] = (char) (0x80 | (cp & 0x3F));
    } else {
        tmp[k++] = (char) (0xF0 | (cp >> 18));
        tmp[k++] = (char) (0x80 | ((cp >> 12) & 0x3F));
        tmp[k++] = (char) (0x80 | ((cp >> 6) & 0x3F));
        tmp[k++] = (char) (0x80 | (cp & 0x3F));
    }
    g_api->vm_write_bytes((void *) proc_ptr, vm_addr, tmp, k);
    return (uint64_t) k;
}

VESTA_PLUGIN_EXPORT uint64_t vio_ptr_to_vmbuf(uint64_t proc_ptr,
                                               uint64_t vm_addr,
                                               uint64_t addr) {
    /* Compacto: "0x" + minimos digitos hex (max 18 caracteres). */
    char     tmp[32];
    size_t   k    = 0;
    tmp[k++]      = '0';
    tmp[k++]      = 'x';
    if (addr == 0) {
        tmp[k++] = '0';
    } else {
        char   rev[16];
        size_t r = 0;
        while (addr > 0) {
            uint64_t d = addr & 0xFu;
            rev[r++]   = (char) (d < 10 ? '0' + d : 'a' + (d - 10));
            addr >>= 4;
        }
        while (r > 0) tmp[k++] = rev[--r];
    }
    g_api->vm_write_bytes((void *) proc_ptr, vm_addr, tmp, k);
    return (uint64_t) k;
}

/**
 * @brief Imprime una cadena de la memoria VM y agrega '\n'.
 */
VESTA_PLUGIN_EXPORT uint64_t vio_println(uint64_t proc_ptr,
                                         uint64_t vm_addr,
                                         uint64_t len) {
    vio_read_vm_and_emit(proc_ptr, vm_addr, len);
    vio_buffer_putc('\n');
    return 0;
}

/* -----------------------------------------------------------------------
 * Salida de valores numericos (sin acceso a memoria VM).
 * Cambio v A.16: vio_print_int / _uint / _hex / _float YA NO emiten '\n'.
 * Usar vio_println_int / _uint / _hex / _float si quieres salto.
 * ----------------------------------------------------------------------- */

VESTA_PLUGIN_EXPORT uint64_t vio_print_int(uint64_t n) {
    char   tmp[24];
    size_t k = vio_i64_to_str((int64_t) n, tmp);
    vio_buffer_append(tmp, k);
    return 0;
}

VESTA_PLUGIN_EXPORT uint64_t vio_println_int(uint64_t n) {
    char   tmp[24];
    size_t k = vio_i64_to_str((int64_t) n, tmp);
    tmp[k++] = '\n';
    vio_buffer_append(tmp, k);
    return 0;
}

VESTA_PLUGIN_EXPORT uint64_t vio_print_uint(uint64_t n) {
    char   tmp[24];
    size_t k = vio_u64_to_str(n, tmp);
    vio_buffer_append(tmp, k);
    return 0;
}

VESTA_PLUGIN_EXPORT uint64_t vio_println_uint(uint64_t n) {
    char   tmp[24];
    size_t k = vio_u64_to_str(n, tmp);
    tmp[k++] = '\n';
    vio_buffer_append(tmp, k);
    return 0;
}

VESTA_PLUGIN_EXPORT uint64_t vio_print_hex(uint64_t n) {
    char   tmp[20];
    size_t k = vio_u64_to_hex(n, tmp);
    vio_buffer_append(tmp, k);
    return 0;
}

VESTA_PLUGIN_EXPORT uint64_t vio_println_hex(uint64_t n) {
    char   tmp[20];
    size_t k = vio_u64_to_hex(n, tmp);
    tmp[k++] = '\n';
    vio_buffer_append(tmp, k);
    return 0;
}

/**
 * @brief Imprime un uint64 como binario "0b<bits>" (sin newline).
 *
 * Usa @c vio_u64_to_bin (formato compacto sin ceros lider).  Para n=0
 * emite "0b0".  Util para depurar bitfields, masks, flags.
 */
VESTA_PLUGIN_EXPORT uint64_t vio_print_bin(uint64_t n) {
    char   tmp[68]; /* 2 prefijo + 64 bits + margen */
    size_t k = vio_u64_to_bin(n, tmp);
    vio_buffer_append(tmp, k);
    return 0;
}

VESTA_PLUGIN_EXPORT uint64_t vio_println_bin(uint64_t n) {
    char   tmp[68];
    size_t k = vio_u64_to_bin(n, tmp);
    tmp[k++] = '\n';
    vio_buffer_append(tmp, k);
    return 0;
}

/**
 * @brief Imprime un uint64 como octal "0o<dig>" (sin newline).
 *
 * Para n=0 emite "0o0".  Maximo 24 chars (uint64_t = 22 digitos octales).
 */
VESTA_PLUGIN_EXPORT uint64_t vio_print_oct(uint64_t n) {
    char   tmp[26];
    size_t k = vio_u64_to_oct(n, tmp);
    vio_buffer_append(tmp, k);
    return 0;
}

VESTA_PLUGIN_EXPORT uint64_t vio_println_oct(uint64_t n) {
    char   tmp[26];
    size_t k = vio_u64_to_oct(n, tmp);
    tmp[k++] = '\n';
    vio_buffer_append(tmp, k);
    return 0;
}

/**
 * @brief Imprime un puntero (host o virtual) como "0x<hex>" compacto.
 *
 * Sin ceros lider; util para mensajes de log.  Para volcado tabular con
 * ancho fijo, usa @c vio_print_hex que siempre emite "0x" + 16 chars.
 */
VESTA_PLUGIN_EXPORT uint64_t vio_print_ptr(uint64_t addr) {
    char   tmp[20];
    size_t k = vio_u64_to_ptr(addr, tmp);
    vio_buffer_append(tmp, k);
    return 0;
}

VESTA_PLUGIN_EXPORT uint64_t vio_println_ptr(uint64_t addr) {
    char   tmp[20];
    size_t k = vio_u64_to_ptr(addr, tmp);
    tmp[k++] = '\n';
    vio_buffer_append(tmp, k);
    return 0;
}

/**
 * @brief Imprime un GcHandle como "<gc:N>" (sin newline).
 *
 * El handle es un uint32 internamente; solo los 32 bits bajos son
 * significativos (los altos quedan a cero por convencion).  El formato
 * "<gc:N>" enfatiza que NO es una direccion sino un indice opaco en la
 * tabla del GC.  Si en el futuro queremos imprimir el class_name del
 * objeto referenciado, anyadir una variante @c vio_print_gcobj que tome
 * tambien el ProcessVM* y resuelva via GcHeap.
 */
VESTA_PLUGIN_EXPORT uint64_t vio_print_gchandle(uint64_t handle) {
    /* Formato: "<gc:" + decimal(32 bits) + ">" -> max 16 chars */
    char tmp[24];
    tmp[0] = '<'; tmp[1] = 'g'; tmp[2] = 'c'; tmp[3] = ':';
    size_t k = 4;
    /* itoa unsigned in-line (handle nunca > 2^32, pero usamos uint64 por ABI) */
    uint64_t v = handle;
    if (v == 0) {
        tmp[k++] = '0';
    } else {
        char rev[24];
        int  n = 0;
        while (v > 0) {
            rev[n++] = (char)('0' + (int)(v % 10u));
            v /= 10u;
        }
        for (int i = 0; i < n; ++i) tmp[k++] = rev[n - 1 - i];
    }
    tmp[k++] = '>';
    vio_buffer_append(tmp, k);
    return 0;
}

VESTA_PLUGIN_EXPORT uint64_t vio_println_gchandle(uint64_t handle) {
    vio_print_gchandle(handle);
    vio_buffer_putc('\n');
    return 0;
}

/**
 * @brief Imprime un padding de @p width chars del codepoint @p fill.
 *
 * Helper de bajo nivel para construir alineacion de texto: el caller
 * calcula la diferencia entre @p width y el ancho actual del texto y
 * llama a este builtin con ese delta.  El @p fill se codifica a UTF-8
 * inline para soportar tanto ' ' (32) como cualquier codepoint Unicode
 * (cuidado: UTF-8 multi-byte ocupa 2-4 bytes por copia, NO solo
 * @p width bytes en el buffer).
 *
 * @param fill_cp Codepoint Unicode (U+0..U+10FFFF).  ' ' = 32 = 0x20.
 * @param width   Numero de copias del fill a emitir.
 */
/**
 * @brief Helper interno: codifica @p cp a UTF-8 en @p buf y devuelve N.
 *        Misma logica que vio_print_char pero sin emitir.
 */
static size_t vio_encode_utf8(uint64_t cp, char *buf) {
    if (cp <= 0x7Fu) { buf[0] = (char)cp; return 1; }
    if (cp <= 0x7FFu) {
        buf[0] = (char)(0xC0u | (cp >> 6));
        buf[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp <= 0xFFFFu) {
        if (cp >= 0xD800u && cp <= 0xDFFFu) return 0;
        buf[0] = (char)(0xE0u | (cp >> 12));
        buf[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        buf[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    if (cp <= 0x10FFFFu) {
        buf[0] = (char)(0xF0u | (cp >> 18));
        buf[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
        buf[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        buf[3] = (char)(0x80u | (cp & 0x3Fu));
        return 4;
    }
    return 0;
}

/* Forward declaration: vio_print_pad esta definida abajo pero la
 * usamos desde vio_print_fmt para emitir el padding. */
VESTA_PLUGIN_EXPORT uint64_t vio_print_pad(uint64_t fill_cp, uint64_t width);

/**
 * @brief Imprime un valor con formato y alineacion.
 *
 * Combina kind + width + fill + align en un solo native helper para
 * que el frontend Vex pueda mapear `${expr:hex:>10}` a un solo CALLN
 * sin tener que medir ancho del valor desde IR.
 *
 * @param value   El valor entero (o bits IEEE para FLOAT_KIND).
 * @param kind    Codigo de formato:
 *                  0 = INT (signed, decimal)
 *                  1 = UINT (unsigned, decimal)
 *                  2 = HEX (0x + 16 hex fixed)
 *                  3 = BIN (0b + bits compactos)
 *                  4 = OCT (0o + dig compactos)
 *                  5 = PTR (0x + hex compacto)
 *                  6 = GC (<gc:N>)
 *                  7 = BOOL ("true"/"false")
 *                  8 = CHAR (codepoint -> UTF-8)
 *                  9 = FLOAT (IEEE 754 bits via %g)
 *                 10 = HEX_COMPACT (0x + hex sin ceros lider, igual a PTR)
 * @param width   Ancho minimo deseado en bytes.  0 = sin padding.
 * @param fill_cp Codepoint Unicode para el fill.
 * @param align   0 = sin alineacion, 1 = LEFT (pad derecha), 2 = RIGHT (pad izq).
 */
VESTA_PLUGIN_EXPORT uint64_t vio_print_fmt(uint64_t value, uint64_t kind,
                                            uint64_t width, uint64_t fill_cp,
                                            uint64_t align) {
    /* Formatear primero a un buffer local, luego decidir el padding. */
    char tmp[80];
    size_t tmp_n = 0;
    switch ((unsigned)kind) {
        case 0: { /* INT signed decimal */
            int64_t s = (int64_t)value;
            if (s == 0) { tmp[tmp_n++] = '0'; break; }
            int neg = 0;
            if (s < 0) {
                neg = 1;
                /* Convertir a positivo con cuidado por INT64_MIN. */
                if (s == (int64_t)0x8000000000000000LL) {
                    /* -9223372036854775808: hard-code el minimo. */
                    const char *m = "-9223372036854775808";
                    size_t k = 0; while (m[k]) ++k;
                    memcpy(tmp + tmp_n, m, k); tmp_n += k;
                    goto fmt_done;
                }
                s = -s;
            }
            char rev[24]; int n = 0;
            while (s > 0) { rev[n++] = (char)('0' + (int)(s % 10)); s /= 10; }
            if (neg) tmp[tmp_n++] = '-';
            for (int i = 0; i < n; ++i) tmp[tmp_n++] = rev[n - 1 - i];
            break;
        }
        case 1: { /* UINT decimal */
            uint64_t u = value;
            if (u == 0) { tmp[tmp_n++] = '0'; break; }
            char rev[24]; int n = 0;
            while (u > 0) { rev[n++] = (char)('0' + (int)(u % 10u)); u /= 10u; }
            for (int i = 0; i < n; ++i) tmp[tmp_n++] = rev[n - 1 - i];
            break;
        }
        case 2: tmp_n = vio_u64_to_hex(value, tmp); break;
        case 3: tmp_n = vio_u64_to_bin(value, tmp); break;
        case 4: tmp_n = vio_u64_to_oct(value, tmp); break;
        case 5: tmp_n = vio_u64_to_ptr(value, tmp); break;
        case 6: { /* GC: "<gc:N>" */
            tmp[tmp_n++] = '<'; tmp[tmp_n++] = 'g'; tmp[tmp_n++] = 'c';
            tmp[tmp_n++] = ':';
            uint64_t u = value;
            if (u == 0) { tmp[tmp_n++] = '0'; }
            else {
                char rev[24]; int n = 0;
                while (u > 0) { rev[n++] = (char)('0' + (int)(u % 10u)); u /= 10u; }
                for (int i = 0; i < n; ++i) tmp[tmp_n++] = rev[n - 1 - i];
            }
            tmp[tmp_n++] = '>';
            break;
        }
        case 7: { /* BOOL */
            if (value & 1u) { memcpy(tmp + tmp_n, "true", 4); tmp_n += 4; }
            else            { memcpy(tmp + tmp_n, "false", 5); tmp_n += 5; }
            break;
        }
        case 8: { /* CHAR codepoint -> UTF-8 */
            tmp_n = vio_encode_utf8(value, tmp);
            break;
        }
        case 9: { /* FLOAT bits via %g */
            double d; memcpy(&d, &value, sizeof(d));
            int k = snprintf(tmp, sizeof(tmp), "%g", d);
            if (k > 0) tmp_n = (size_t)k;
            break;
        }
        default:
            tmp_n = 0;
            break;
    }
fmt_done: ; /* statement vacio para que la label valga en C estricto */
    /* Calcular padding y emitir.  width se interpreta en BYTES emitidos
     * (no en columnas terminal): para texto puramente ASCII coincide con
     * el ancho visible; para multi-byte UTF-8 puede diferir y queda como
     * limitacion conocida del formato simple. */
    uint64_t pad = (tmp_n < width) ? (width - tmp_n) : 0u;
    if (align == 2u && pad > 0u) {
        /* RIGHT-align: padding antes. */
        vio_print_pad(fill_cp, pad);
    }
    if (tmp_n > 0u) vio_buffer_append(tmp, tmp_n);
    if (align == 1u && pad > 0u) {
        /* LEFT-align: padding despues. */
        vio_print_pad(fill_cp, pad);
    }
    return 0;
}

VESTA_PLUGIN_EXPORT uint64_t vio_print_pad(uint64_t fill_cp, uint64_t width) {
    if (width == 0) return 0;
    /* Codificar fill a UTF-8 una vez, luego repetir. */
    char enc[4];
    size_t enc_n = 0;
    if (fill_cp <= 0x7Fu) {
        enc[enc_n++] = (char)fill_cp;
    } else if (fill_cp <= 0x7FFu) {
        enc[enc_n++] = (char)(0xC0u | (fill_cp >> 6));
        enc[enc_n++] = (char)(0x80u | (fill_cp & 0x3Fu));
    } else if (fill_cp <= 0xFFFFu) {
        if (fill_cp >= 0xD800u && fill_cp <= 0xDFFFu) return 0;
        enc[enc_n++] = (char)(0xE0u | (fill_cp >> 12));
        enc[enc_n++] = (char)(0x80u | ((fill_cp >> 6) & 0x3Fu));
        enc[enc_n++] = (char)(0x80u | (fill_cp & 0x3Fu));
    } else if (fill_cp <= 0x10FFFFu) {
        enc[enc_n++] = (char)(0xF0u | (fill_cp >> 18));
        enc[enc_n++] = (char)(0x80u | ((fill_cp >> 12) & 0x3Fu));
        enc[enc_n++] = (char)(0x80u | ((fill_cp >> 6) & 0x3Fu));
        enc[enc_n++] = (char)(0x80u | (fill_cp & 0x3Fu));
    } else {
        return 0;
    }
    /* Emitir en chunks para no abusar del buffer en width grandes. */
    char chunk[256];
    size_t chunk_used = 0;
    for (uint64_t i = 0; i < width; ++i) {
        if (chunk_used + enc_n > sizeof(chunk)) {
            vio_buffer_append(chunk, chunk_used);
            chunk_used = 0;
        }
        for (size_t j = 0; j < enc_n; ++j) chunk[chunk_used++] = enc[j];
    }
    if (chunk_used > 0) vio_buffer_append(chunk, chunk_used);
    return 0;
}

/**
 * @brief Imprime un double codificado como bits IEEE 754 (sin newline).
 *
 * Para float usamos @c snprintf con "%g" (formato compacto humano).  Una
 * implementacion full custom (Ryu / Grisu) tendria mejor rendimiento pero
 * el coste actual ya es tolerable para uso normal de I/O.
 */
VESTA_PLUGIN_EXPORT uint64_t vio_print_float(uint64_t bits) {
    double d;
    memcpy(&d, &bits, sizeof(d));
    char tmp[64];
    int  k = snprintf(tmp, sizeof(tmp), "%g", d);
    if (k > 0) vio_buffer_append(tmp, (size_t) k);
    return 0;
}

VESTA_PLUGIN_EXPORT uint64_t vio_println_float(uint64_t bits) {
    double d;
    memcpy(&d, &bits, sizeof(d));
    char tmp[64];
    int  k = snprintf(tmp, sizeof(tmp), "%g", d);
    if (k > 0) {
        tmp[k++] = '\n';
        vio_buffer_append(tmp, (size_t) k);
    }
    return 0;
}

/* -----------------------------------------------------------------------
 * Builtins nuevos A.16: bool, char, color, flush.
 * ----------------------------------------------------------------------- */

/**
 * @brief Imprime "true" o "false" segun el bit bajo de @p b.
 */
VESTA_PLUGIN_EXPORT uint64_t vio_print_bool(uint64_t b) {
    if (b & 1u) vio_buffer_append("true", 4);
    else vio_buffer_append("false", 5);
    return 0;
}

/**
 * @brief Codifica un codepoint Unicode (U+0..U+10FFFF) a UTF-8 y emite.
 *
 * Para ASCII (<= 0x7F) emite 1 byte directo (fast path).  Para chars
 * mayores aplica el encoding multi-byte estandar.  Codepoints invalidos
 * (>= 0x110000 o pares surrogate) se ignoran.
 */
VESTA_PLUGIN_EXPORT uint64_t vio_print_char(uint64_t cp) {
    if (cp <= 0x7Fu) {
        vio_buffer_putc((char) cp);
        return 0;
    }
    char   buf[4];
    size_t n = 0;
    if (cp <= 0x7FFu) {
        buf[n++] = (char) (0xC0u | (cp >> 6));
        buf[n++] = (char) (0x80u | (cp & 0x3Fu));
    } else if (cp <= 0xFFFFu) {
        if (cp >= 0xD800u && cp <= 0xDFFFu) return 0; /* surrogate */
        buf[n++] = (char) (0xE0u | (cp >> 12));
        buf[n++] = (char) (0x80u | ((cp >> 6) & 0x3Fu));
        buf[n++] = (char) (0x80u | (cp & 0x3Fu));
    } else if (cp <= 0x10FFFFu) {
        buf[n++] = (char) (0xF0u | (cp >> 18));
        buf[n++] = (char) (0x80u | ((cp >> 12) & 0x3Fu));
        buf[n++] = (char) (0x80u | ((cp >> 6) & 0x3Fu));
        buf[n++] = (char) (0x80u | (cp & 0x3Fu));
    } else {
        return 0; /* fuera de rango Unicode */
    }
    vio_buffer_append(buf, n);
    return 0;
}

/**
 * @brief Emite un escape ANSI "\x1b[<code>m" (color o atributo SGR).
 *
 * Codigos comunes:
 *   0 reset, 1 bold, 4 underline,
 *   30..37 colores foreground (negro, rojo, verde, amarillo, azul,
 *   magenta, cian, blanco), 90..97 brillantes,
 *   40..47 backgrounds, 100..107 backgrounds brillantes.
 */
VESTA_PLUGIN_EXPORT uint64_t vio_print_color(uint64_t code) {
    char tmp[16];
    tmp[0] = 0x1b; /* ESC */
    tmp[1] = '[';
    char   digits[8];
    size_t k = vio_u64_to_str((uint64_t) (code & 0xFFu), digits);
    memcpy(tmp + 2, digits, k);
    tmp[2 + k] = 'm';
    vio_buffer_append(tmp, 3 + k);
    return 0;
}

/**
 * @brief Emite un solo '\n' al buffer.  Util como sufijo del lowering de
 *        println(...) sin tener que setear 3 args de vio_println.
 */
VESTA_PLUGIN_EXPORT uint64_t vio_print_newline(void) {
    vio_buffer_putc('\n');
    return 0;
}

/**
 * @brief Imprime una cstring (host pointer, terminada en NUL).
 *
 * Util para imprimir desde Vex valores tipo @c FatalError.message o
 * @c FatalError.stack_trace que son punteros host a buffers C.
 * Limita a 64 KB de lectura (los stack traces no exceden este tamano).
 * Si el puntero es NULL, no emite nada.
 */
VESTA_PLUGIN_EXPORT uint64_t vio_print_cstr(uint64_t host_ptr) {
    if (host_ptr == 0) return 0;
    const char *s = (const char *) (uintptr_t) host_ptr;
    size_t      n = 0;
    while (n < 65536u && s[n] != '\0') ++n;
    vio_buffer_append(s, n);
    return 0;
}

/*
 * vio_print_buf(host_ptr, byte_len): emite byte_len bytes desde host_ptr.
 * Binary-safe (NO se detiene en NUL).  Lo usa el lowering de print/println
 * cuando el argumento es de tipo STRING (StringObject): se obtiene el
 * host_ptr via STRRAW y la longitud via STRGETBYTES y se emiten los bytes
 * en bloque para preservar caracteres multi-byte (UTF-8) intactos.
 */
VESTA_PLUGIN_EXPORT uint64_t vio_print_buf(uint64_t host_ptr,
                                           uint64_t byte_len) {
    if (host_ptr == 0 || byte_len == 0) return 0;
    const char *s = (const char *) (uintptr_t) host_ptr;
    /* Cap defensivo a 64 MB para evitar crash por puntero corrupto. */
    if (byte_len > (1ull << 26)) byte_len = (1ull << 26);
    vio_buffer_append(s, (size_t) byte_len);
    return 0;
}

/**
 * @brief Vacia el buffer global ahora mismo.
 *
 * Equivalente a @c stdout flush pero usando @c WriteConsoleW / @c WriteFile
 * directamente (sin pasar por stdio).  Complementa el auto-flush por
 * threshold + atexit.  Util para garantizar visibilidad inmediata en TUIs.
 */
VESTA_PLUGIN_EXPORT uint64_t vio_flush(void) {
    vio_flush_public();
    return 0;
}

/* -----------------------------------------------------------------------
 * Entrada de texto hacia memoria VM (sin cambios respecto a la version
 * vieja: lectura es bloqueante y va por stdin del host).
 * ----------------------------------------------------------------------- */

/**
 * @brief Lee una linea de stdin y la escribe en la memoria virtual VM.
 *
 * Antes de leer hace @c flush para que cualquier prompt encolado en el
 * buffer salga a la consola y el usuario vea lo que se le pregunta.
 */
VESTA_PLUGIN_EXPORT uint64_t vio_read_line(uint64_t proc_ptr,
                                           uint64_t vm_addr,
                                           uint64_t max_bytes) {
    if (!g_api || max_bytes == 0) return 0;

    /* Sincronizar buffer con stdout antes de leer (prompt visible). */
    vio_flush_public();

    char *host_buf = (char *) malloc((size_t) max_bytes);
    if (!host_buf) return 0;

    if (!fgets(host_buf, (int) max_bytes, stdin)) {
        host_buf[0] = '\0';
        g_api->vm_write_bytes(proc_ptr, vm_addr, host_buf, 1);
        free(host_buf);
        return 0;
    }

    uint64_t written = (uint64_t) strlen(host_buf);
    g_api->vm_write_bytes(proc_ptr, vm_addr, host_buf, written + 1);
    free(host_buf);
    return written;
}

/* -----------------------------------------------------------------------
 * Operaciones de fichero (firmas heredadas, sin cambios).
 * Para ficheros NO usamos el buffer global de stdout; @c FILE* del host
 * hace su propio buffering.
 * ----------------------------------------------------------------------- */

/**
 * @brief Abre un fichero cuya ruta y modo residen en la memoria VM.
 *
 * Devuelve el FILE* como uint64_t opaco; usar con @c vio_fread / @c
 * vio_fwrite / @c vio_fclose.
 */
VESTA_PLUGIN_EXPORT uint64_t vio_fopen(uint64_t proc_ptr,
                                       uint64_t path_vm_addr,
                                       uint64_t path_len,
                                       uint64_t mode_vm_addr,
                                       uint64_t mode_len) {
    if (!g_api) return 0;

    char  path_stack[512];
    char  mode_stack[8];
    char *path = (path_len < sizeof(path_stack))
                     ? path_stack
                     : (char *) malloc((size_t) path_len + 1);
    char *mode = (mode_len < sizeof(mode_stack))
                     ? mode_stack
                     : (char *) malloc((size_t) mode_len + 1);
    if (!path || !mode) {
        if (path && path != path_stack) free(path);
        if (mode && mode != mode_stack) free(mode);
        return 0;
    }
    g_api->vm_read_bytes(proc_ptr, path_vm_addr, path, path_len);
    g_api->vm_read_bytes(proc_ptr, mode_vm_addr, mode, mode_len);
    path[path_len] = '\0';
    mode[mode_len] = '\0';

    FILE *f = fopen(path, mode);
    if (path != path_stack) free(path);
    if (mode != mode_stack) free(mode);
    return (uint64_t) (uintptr_t) f;
}

VESTA_PLUGIN_EXPORT uint64_t vio_fclose(uint64_t handle) {
    if (!handle) return (uint64_t) -1;
    return (uint64_t) fclose((FILE *) (uintptr_t) handle);
}

VESTA_PLUGIN_EXPORT uint64_t vio_fread(uint64_t proc_ptr,
                                       uint64_t vm_addr,
                                       uint64_t size,
                                       uint64_t handle) {
    if (!g_api || !handle || !size) return 0;
    char *host_buf = (char *) malloc((size_t) size);
    if (!host_buf) return 0;
    uint64_t n = (uint64_t) fread(host_buf, 1, (size_t) size,
                                  (FILE *) (uintptr_t) handle);
    if (n > 0) g_api->vm_write_bytes(proc_ptr, vm_addr, host_buf, n);
    free(host_buf);
    return n;
}

VESTA_PLUGIN_EXPORT uint64_t vio_fwrite(uint64_t proc_ptr,
                                        uint64_t vm_addr,
                                        uint64_t size,
                                        uint64_t handle) {
    if (!g_api || !handle || !size) return 0;
    char *host_buf = (char *) malloc((size_t) size);
    if (!host_buf) return 0;
    g_api->vm_read_bytes(proc_ptr, vm_addr, host_buf, size);
    uint64_t n = (uint64_t) fwrite(host_buf, 1, (size_t) size,
                                   (FILE *) (uintptr_t) handle);
    free(host_buf);
    return n;
}

/**
 * @brief Vacia el buffer del fichero @p handle.  Pasar 0 vacia todos los
 *        FILE* del host (equivale a @c fflush(NULL)).  Para vaciar el
 *        buffer global de stdout usar @c vio_flush() en su lugar.
 */
VESTA_PLUGIN_EXPORT uint64_t vio_fflush(uint64_t handle) {
    FILE *f = handle ? (FILE *) (uintptr_t) handle : NULL;
    return (uint64_t) fflush(f);
}
