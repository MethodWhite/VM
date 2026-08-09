# ADR-0001: Exit code 128+señal tras fallo sin capturar

- **Estado**: Aceptado (2026-08-09)
- **Decidido por**: maintainers (ver `GOVERNANCE.md`)
- **Relacionado**: `VERSIONING.md`, `SECURITY.md`

## Contexto

Cuando un programa Vex moría por un fallo que nadie capturaba (división por
cero, acceso inválido, `panic`, ...), el proceso `vm --run programa.velb`
salía con `EXIT_SUCCESS` (0).  Esto mentía a quien invoca el programa:
scripts, integración continua y wrappers interpretan el código de salida 0
como "todo salió bien".

## Decisión

El proceso sale con los códigos POSIX de siempre: **128 + número de señal**,
igual que haría un proceso en C que muriera por esa señal.

| Fallo | Señal | Exit |
|-------|-------|------|
| División por cero | SIGFPE (8) | 136 |
| NPE / segfault / stack overflow | SIGSEGV (11) | 139 |
| Instrucción ilegal | SIGILL (4) | 132 |
| panic / OOM / excepción nativa | SIGABRT (6) | 134 |
| Otro | — | 1 |

Implementación:

- `runtime::last_fatal_exit_code()` en `include/runtime/exception_runtime.h`
  y `src/runtime/exception_runtime.cpp` mapea `FatalKind` → código.
- `throw_fatal()` registra el kind en las rutas fatales (sin handler).
- `main.cpp` (`--run`) consulta `last_fatal_exit_code()` tras `vm.run()`
  y lo devuelve como exit code.

## Consecuencias

**Positivas**:

- Los scripts y la CI detectan fallos de programa de forma fiable.
- Quien ya mire números de señales no se aprende otros nuevos.
- Cabe en los 8 bits de un exit code (a diferencia de los códigos del
  catálogo de diagnóstico).

**Negativas / costes**:

- Cambia el comportamiento observado de programas que morían silenciosos
  con código 0.  Se considera un arreglo, no una rotura.
- Solo aplica al flujo `--run`; el REPL y los flujos interactivos no se
  ven afectados (no hay programa que terminar).

## Alternativas consideradas

- Devolver un código propio del catálogo de diagnóstico: descartado porque
  no cabe en 8 bits y obliga a aprender códigos nuevos.
- Seguir devolviendo 0: descartado por mentir al invocador.
