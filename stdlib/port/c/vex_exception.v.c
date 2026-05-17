// @vex-snippet: vex_exception
// @vex-requires: vex_macros
// @vex-includes: stdint.h, setjmp.h, stdio.h
// @vex-freestanding-skip: yes

/* Excepciones de usuario: stack de @c jmp_buf via linked list TLS.
 * @c panic almacena el mensaje en @c vex_exc_value y hace @c longjmp
 * al frame top.  El handler de @c try/catch hace @c goto * a la
 * direccion del label del bloque @c try_handler_N (labels-as-values
 * GCC: @c &&bb_<id>).
 *
 * Para freestanding: el snippet se omite y el programador debe proveer
 * @c vex_panic_with_str + @c vex_exc_top + @c vex_exc_value en su
 * propio codigo si quiere try/catch. */
typedef struct vex_exc_frame {
    jmp_buf buf;
    int64_t type_tag;        /* tipo del catch (sentinel para FatalError) */
    struct vex_exc_frame *prev;
} vex_exc_frame;

static VEX_TLS vex_exc_frame *vex_exc_top   = 0;
static VEX_TLS int64_t        vex_exc_value = 0;

static VEX_UNUSED VEX_NORETURN VEX_COLD void vex_panic_with_str(const void *msg) {
    vex_exc_value = (int64_t)(intptr_t)msg;
    if (vex_exc_top != 0) {
        longjmp(vex_exc_top->buf, 1);
    }
    /* Uncaught panic: print msg + abort cleanly. */
    fputs("[vex] uncaught panic: ", stderr);
    if (msg) fputs((const char*)msg, stderr);
    fputc('\n', stderr);
    vex_throw(1);
}
