// @vex-snippet: vex_throw_freestanding
// @vex-requires: vex_macros
// @vex-includes:
// @vex-freestanding-skip: no

/* Modo freestanding: @c vex_throw NO se implementa aqui.  El usuario
 * debe proveer una funcion @c VEX_NORETURN @c "void vex_throw(int)"
 * en su propio codigo (e.g. que escriba a un puerto serie + halt).
 * El transpiler la referencia como @c extern. */
extern VEX_NORETURN void vex_throw(int code);
extern int vex_exc_code; /* opcional; el usuario decide si tracker */
