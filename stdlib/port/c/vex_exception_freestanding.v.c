// @vex-snippet: vex_exception_freestanding
// @vex-requires: vex_macros
// @vex-includes: stdint.h, setjmp.h
// @vex-freestanding-skip: no

/* Variante freestanding del runtime de excepciones.  Misma estructura
 * de @c vex_exc_frame pero @c vex_panic_with_str NO escribe a stderr
 * (no hay stdio).  El usuario en su codigo decide que hacer en
 * uncaught panic via su propia @c vex_throw (e.g. puerto serie + hlt). */
typedef struct vex_exc_frame {
    jmp_buf buf;
    int64_t type_tag;
    struct vex_exc_frame *prev;
} vex_exc_frame;

static VEX_TLS vex_exc_frame *vex_exc_top   = 0;
static VEX_TLS int64_t        vex_exc_value = 0;

static VEX_UNUSED VEX_NORETURN VEX_COLD void vex_panic_with_str(const void *msg) {
    vex_exc_value = (int64_t)(intptr_t)msg;
    if (vex_exc_top != 0) {
        longjmp(vex_exc_top->buf, 1);
    }
    /* Uncaught en freestanding: delegar al usuario via vex_throw. */
    vex_throw(1);
}
