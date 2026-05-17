// @vex-snippet: vex_instrument
// @vex-requires: vex_macros
// @vex-includes: stdio.h, stdlib.h, string.h, time.h
// @vex-freestanding-skip: yes

/* =========================================================================
 * Runtime de instrumentacion enriquecido (--instrument trace|profile).
 *
 * Features:
 *   - Trace tree con drawing chars Unicode (├── │ └──) y depth coloreado
 *   - PID + TID por linea
 *   - Timing per-funcion (rdtsc / clock_gettime) en TLS
 *   - Profile summary (count, total_ns, min/max/avg) ordenado por total
 *     descendente al @c atexit cuando el modo es @c profile
 *   - Hooks de usuario via weak symbols:
 *       vex_user_on_enter(name, depth, pid)
 *       vex_user_on_leave(name, value, elapsed_ns, depth, pid)
 *     Por defecto no-op; el usuario los puede sobrescribir linkando su
 *     propia implementacion (mismo nombre, no @c weak).
 *
 * El mode (trace vs profile) se selecciona via macro @c VEX_INSTRUMENT_MODE
 * que el transpiler emite al inicio del @c .c segun el flag CLI.
 *   1 = trace   (imprime ENTER/LEAVE en tiempo real)
 *   2 = profile (acumula stats, solo imprime tabla al exit)
 *   3 = trace + profile combinados
 *
 * Color auto-detect: solo emite ANSI si stderr es TTY.
 * =========================================================================
 */

#ifndef VEX_INSTRUMENT_MODE
#  define VEX_INSTRUMENT_MODE 1   /* trace */
#endif

#define VEX_INSTR_TRACE   (VEX_INSTRUMENT_MODE & 1)
#define VEX_INSTR_PROFILE (VEX_INSTRUMENT_MODE & 2)

#if defined(_WIN32)
#  ifndef _WIN32_WINNT
#    define _WIN32_WINNT 0x0600
#  endif
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <unistd.h>
#  include <sys/types.h>
#  include <pthread.h>
#endif

/* ----- Timing helpers ----- */
static VEX_UNUSED uint64_t vt_now_ns_(void) {
#if defined(_WIN32)
    static LARGE_INTEGER freq = {{0, 0}};
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (uint64_t)((c.QuadPart * 1000000000ULL) / (uint64_t)freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

static VEX_UNUSED int vt_pid_(void) {
#if defined(_WIN32)
    return (int)GetCurrentProcessId();
#else
    return (int)getpid();
#endif
}

static VEX_UNUSED int vt_tid_(void) {
#if defined(_WIN32)
    return (int)GetCurrentThreadId();
#else
    /* gettid es Linux-specific; usar pthread_self truncado como proxy. */
    return (int)((uintptr_t)pthread_self() & 0xFFFFFF);
#endif
}

/* ----- Color auto-detect (cached) ----- */
static int vt_color_cached_ = -1;
static int vt_use_color_(void) {
    if (vt_color_cached_ >= 0) return vt_color_cached_;
#if defined(_WIN32)
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    DWORD t = h ? GetFileType(h) : 0;
    vt_color_cached_ = (t == FILE_TYPE_CHAR);
    if (vt_color_cached_) {
        /* Habilitar virtual terminal processing en Windows 10+. */
        DWORD mode = 0;
        if (GetConsoleMode(h, &mode)) {
            SetConsoleMode(h, mode | 0x0004 /* ENABLE_VIRTUAL_TERMINAL_PROCESSING */);
        }
    }
#else
    vt_color_cached_ = isatty(2);
#endif
    /* Permitir override via env var VEX_TRACE_NO_COLOR. */
    if (vt_color_cached_) {
        const char *e = getenv("VEX_TRACE_NO_COLOR");
        if (e && e[0] && e[0] != '0') vt_color_cached_ = 0;
    }
    return vt_color_cached_;
}

#define VT_COLOR(s) (vt_use_color_() ? s : "")
#define VT_RESET    VT_COLOR("\x1b[0m")
#define VT_DIM      VT_COLOR("\x1b[90m")
#define VT_BOLD     VT_COLOR("\x1b[1m")
#define VT_GREEN    VT_COLOR("\x1b[32m")
#define VT_YELLOW   VT_COLOR("\x1b[33m")
#define VT_CYAN     VT_COLOR("\x1b[36m")
#define VT_RED      VT_COLOR("\x1b[91m")

static const char *vt_palette_(int d) {
    static const char *const c[6] = {
        "\x1b[32m", "\x1b[33m", "\x1b[36m",
        "\x1b[35m", "\x1b[34m", "\x1b[91m"
    };
    return vt_use_color_() ? c[((d % 6) + 6) % 6] : "";
}

/* ----- TLS frame stack (depth + timing) ----- */
#define VEX_TRACE_MAX_DEPTH 256
typedef struct VexTraceFrame {
    const char *name;
    uint64_t    start_ns;
    int         depth;
} VexTraceFrame;

static VEX_TLS VexTraceFrame vt_stack_[VEX_TRACE_MAX_DEPTH];
static VEX_TLS int           vt_depth_ = 0;

/* ----- Profile stats (lock-protected, global cross-thread) ----- */
typedef struct VexProfileEntry {
    const char *name;     /* puntero al string literal en .rodata */
    uint64_t    count;
    uint64_t    total_ns;
    uint64_t    min_ns;
    uint64_t    max_ns;
    struct VexProfileEntry *next;
} VexProfileEntry;

#if defined(_WIN32)
static CRITICAL_SECTION vt_profile_mtx_;
#else
static pthread_mutex_t  vt_profile_mtx_;
#endif
static VexProfileEntry *vt_profile_head_  = 0;
static int              vt_profile_inited_ = 0;
static int              vt_atexit_reg_    = 0;

static void vt_profile_init_(void) {
    if (vt_profile_inited_) return;
#if defined(_WIN32)
    InitializeCriticalSection(&vt_profile_mtx_);
#else
    pthread_mutex_init(&vt_profile_mtx_, 0);
#endif
    vt_profile_inited_ = 1;
}

static void vt_lock_(void) {
#if defined(_WIN32)
    EnterCriticalSection(&vt_profile_mtx_);
#else
    pthread_mutex_lock(&vt_profile_mtx_);
#endif
}
static void vt_unlock_(void) {
#if defined(_WIN32)
    LeaveCriticalSection(&vt_profile_mtx_);
#else
    pthread_mutex_unlock(&vt_profile_mtx_);
#endif
}

static int vt_cmp_total_desc_(const void *a, const void *b) {
    const VexProfileEntry *pa = *(VexProfileEntry *const *)a;
    const VexProfileEntry *pb = *(VexProfileEntry *const *)b;
    if (pb->total_ns > pa->total_ns) return  1;
    if (pb->total_ns < pa->total_ns) return -1;
    return 0;
}

static void vt_profile_report_(void) {
    /* Recolectar entradas en array, ordenar por total_ns desc, imprimir. */
    if (!vt_profile_inited_) return;
    vt_lock_();
    int n = 0;
    for (VexProfileEntry *e = vt_profile_head_; e; e = e->next) n++;
    if (n == 0) { vt_unlock_(); return; }
    VexProfileEntry **arr = (VexProfileEntry**)malloc(sizeof(*arr) * (size_t)n);
    if (!arr) { vt_unlock_(); return; }
    int i = 0;
    for (VexProfileEntry *e = vt_profile_head_; e; e = e->next) arr[i++] = e;
    qsort(arr, (size_t)n, sizeof(*arr), vt_cmp_total_desc_);

    fprintf(stderr, "\n%s== Vex Profile (sorted by total time, pid=%d) ==%s\n",
            VT_BOLD, vt_pid_(), VT_RESET);
    fprintf(stderr, "%s%-32s %10s %14s %12s %12s %12s%s\n",
            VT_DIM, "function", "count", "total_ns", "avg_ns", "min_ns", "max_ns", VT_RESET);
    fprintf(stderr, "%s%s%s\n", VT_DIM,
        "---------------------------------------------------------------------------"
        "-----------------------", VT_RESET);
    for (int k = 0; k < n; ++k) {
        VexProfileEntry *e = arr[k];
        uint64_t avg = e->count ? (e->total_ns / e->count) : 0;
        fprintf(stderr, "%-32s %10llu %14llu %12llu %12llu %12llu\n",
                e->name ? e->name : "?",
                (unsigned long long)e->count,
                (unsigned long long)e->total_ns,
                (unsigned long long)avg,
                (unsigned long long)e->min_ns,
                (unsigned long long)e->max_ns);
    }
    free(arr);
    vt_unlock_();
}

static VexProfileEntry *vt_profile_find_or_add_(const char *name) {
    /* Buscar por puntero (name viene de @c .rodata; mismo string literal
     * mismo puntero).  Si no se encuentra, insertar al frente. */
    for (VexProfileEntry *e = vt_profile_head_; e; e = e->next) {
        if (e->name == name) return e;
    }
    VexProfileEntry *e = (VexProfileEntry*)calloc(1, sizeof(VexProfileEntry));
    if (!e) return 0;
    e->name    = name;
    e->min_ns  = (uint64_t)-1;
    e->next    = vt_profile_head_;
    vt_profile_head_ = e;
    return e;
}

/* ----- Tree drawing chars ----- */
static void vt_print_indent_(FILE *f, int depth, int is_leaf_marker, int closing) {
    /* Prefijo de tree.  @c is_leaf_marker = 1 -> usar @c "├─" o @c "└─"
     * al final.  @c closing=1 -> close marker (leave).  Para depth 0, no
     * imprimir nada (raiz). */
    if (depth <= 0) return;
    for (int i = 0; i < depth - 1; ++i) {
        fprintf(f, "%s%s%s", vt_palette_(i), "\xe2\x94\x82 ", VT_RESET);  /* "│ " */
    }
    if (is_leaf_marker) {
        const char *marker = closing
            ? "\xe2\x94\x94\xe2\x95\xb4"   /* └╴ */
            : "\xe2\x94\x9c\xe2\x94\x80";  /* ├─ */
        fprintf(f, "%s%s%s", vt_palette_(depth - 1), marker, VT_RESET);
    }
}

/* ----- User-overridable hooks (weak symbols).
 * El usuario puede proveer implementaciones en su propio C linkado contra
 * el generado para anyadir log custom, telemetria, breakpoints, etc.
 * Las versiones default son no-op.
 * ----- */
__attribute__((weak)) void vex_user_on_enter(const char *fn_name,
                                                int depth, int pid) {
    (void)fn_name; (void)depth; (void)pid;
}

__attribute__((weak)) void vex_user_on_leave(const char *fn_name,
                                                int64_t value,
                                                uint64_t elapsed_ns,
                                                int depth, int pid) {
    (void)fn_name; (void)value; (void)elapsed_ns; (void)depth; (void)pid;
}

/* ----- Entry points llamados por el codigo generado ----- */
static VEX_UNUSED void vex_trace_enter(const char *fn_name) {
    vt_profile_init_();
    if (!vt_atexit_reg_) {
        vt_atexit_reg_ = 1;
#if VEX_INSTR_PROFILE
        atexit(vt_profile_report_);
#endif
    }
    int d = vt_depth_;
    if (d < VEX_TRACE_MAX_DEPTH) {
        vt_stack_[d].name     = fn_name;
        vt_stack_[d].start_ns = vt_now_ns_();
        vt_stack_[d].depth    = d;
    }
    vt_depth_++;

#if VEX_INSTR_TRACE
    vt_print_indent_(stderr, d + 1, 1, 0);
    fprintf(stderr, "%s\xe2\x96\xb6 %s%s %s[pid=%d tid=%d d=%d]%s\n",
            VT_BOLD, fn_name ? fn_name : "?", VT_RESET,
            VT_DIM, vt_pid_(), vt_tid_(), d, VT_RESET);
    fflush(stderr);
#endif

    vex_user_on_enter(fn_name, d, vt_pid_());
}

static VEX_UNUSED void vex_trace_leave(const char *fn_name, int64_t value) {
    int new_depth = vt_depth_ - 1;
    if (new_depth < 0) new_depth = 0;
    vt_depth_ = new_depth;

    uint64_t elapsed = 0;
    if (new_depth < VEX_TRACE_MAX_DEPTH) {
        elapsed = vt_now_ns_() - vt_stack_[new_depth].start_ns;
    }

#if VEX_INSTR_PROFILE
    /* Acumular en stats globales (lock por contencion cross-thread). */
    vt_lock_();
    VexProfileEntry *e = vt_profile_find_or_add_(fn_name);
    if (e) {
        e->count++;
        e->total_ns += elapsed;
        if (elapsed < e->min_ns) e->min_ns = elapsed;
        if (elapsed > e->max_ns) e->max_ns = elapsed;
    }
    vt_unlock_();
#endif

#if VEX_INSTR_TRACE
    vt_print_indent_(stderr, new_depth + 1, 1, 1);
    fprintf(stderr, "%s\xe2\x97\x80 %s%s = %s%lld%s  %s[%lluns]%s\n",
            VT_BOLD, fn_name ? fn_name : "?", VT_RESET,
            VT_YELLOW, (long long)value, VT_RESET,
            VT_DIM, (unsigned long long)elapsed, VT_RESET);
    fflush(stderr);
#endif

    vex_user_on_leave(fn_name, value, elapsed, new_depth, vt_pid_());
}
