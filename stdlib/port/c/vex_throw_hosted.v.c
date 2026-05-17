// @vex-snippet: vex_throw_hosted
// @vex-requires: vex_macros
// @vex-includes: setjmp.h
// @vex-freestanding-skip: yes

/* Manejo de excepciones de runtime via setjmp/longjmp (modo hosted).
 * El usuario en modo freestanding debe proveer su propia @c vex_throw
 * declarada como @c VEX_NORETURN. */
static jmp_buf vex_exc_buf;
static int     vex_exc_code;
static VEX_UNUSED VEX_NORETURN VEX_COLD void vex_throw(int code) {
    vex_exc_code = code;
    longjmp(vex_exc_buf, 1);
}
