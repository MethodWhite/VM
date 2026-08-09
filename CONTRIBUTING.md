# Contribuyendo a VestaVM

Gracias por contribuir.  Antes de empezar, lee:

- [Código de conducta](./CODE_OF_CONDUCT.md)
- [Gobernanza](./GOVERNANCE.md)
- [Manual de mantenimiento](./MAINTAINERS.md)
- [Política de seguridad](./SECURITY.md)
- [Versionado](./VERSIONING.md)

---

## Ramas y modelo de flujo

- `release` — estable, protegida.
- `develop` — integración de features (base para tu PR).
- `feature/*` — tu rama de trabajo.
- `hotfix/*` — parches urgentes contra `release`.

Crea siempre tu rama desde `develop`:

```bash
git checkout develop && git pull
git checkout -b feature/mi-cambio
```

## Antes de escribir código

1. Busca un issue abierto o abre uno para discutir el cambio (sobre todo
   si toca lenguaje, bytecode, ABI o protocolo).
2. Para cambios significativos, presenta un RFC primero
   (`.github/ISSUE_TEMPLATE/rfc.md`).
3. Para bugs de seguridad: `SECURITY.md`, **nunca** en público.

## Estándares de código

- C++17 (GCC 9+, Clang 10+, MSVC 2019+); C11 para código C.
- 4 espacios de indentación, sin tabs.
- `snake_case` para funciones/variables, `PascalCase` para tipos/clases.
- `#pragma once` en headers.
- Funciones pequeñas y enfocadas; una responsabilidad por archivo.
- Sin dependencias innecesarias.
- Código complejo documentado en español o inglés (Doxygen para headers).
- Sin optimizaciones prematuras.

## DCO (Developer Certificate of Origin)

Al enviar un PR aceptas que tienes derecho a contribuir el código y que
el proyecto puede distribuirlo bajo su licencia (VMProject).  Cada commit
debe llevar el `Signed-off-by`:

```bash
git commit -s -m "feat: descripción"
```

`git commit -s` añade automáticamente:

```
Signed-off-by: Tu Nombre <tu@email>
```

Sin esta línea el PR se considerará incompleto y se pedirá enmendarlo.
El CI lo verifica (check de DCO).

## Commits

Usa [Conventional Commits](https://www.conventionalcommits.org/):

- `feat:` nueva funcionalidad
- `fix:` corrección de bug
- `refactor:` cambio sin corrección ni feature
- `perf:` mejora de rendimiento
- `docs:` documentación
- `test:` tests
- `infra:` CI, build, tooling
- `build:` dependencias / build system
- `style:` formato, sin cambios de lógica
- `revert:` revertir un commit

Ejemplos:

```
feat(vex): añadir pattern matching en match/case
fix(runtime): exit code 128+señal tras fallo sin capturar
refactor(jit): partir selector.cpp en módulos
```

## PR process

1. Implementa los cambios en tu rama.
2. Asegura que compila: `cmake -B build && cmake --build build --parallel`.
3. Ejecuta tests: `ctest --test-dir build --output-on-failure --parallel`.
4. Si agregas nuevas instrucciones VM, implementa el handler y sus tests.
5. Si agregas nuevas capabilities del sandbox, actualiza la documentación.
6. Si cambias el formato `.velb`, la ABI o el protocolo VDP, sube la
   versión según `VERSIONING.md` y documenta la incompatibilidad.
7. Formatea el código y revisa que no introduzcas warnings nuevos.
8. Abre el PR contra `develop` con la plantilla.
9. Espera revisión; responde a los comentarios.  No forces el merge.

### Checklist del PR

- [ ] Compila sin errores ni warnings nuevos (`-Wall -Wextra`).
- [ ] Tests pasan localmente y en CI.
- [ ] Commit message en Conventional Commits.
- [ ] Commits con `Signed-off-by` (DCO).
- [ ] Documentación actualizada si procede.
- [ ] Sin secretos ni credenciales en el diff.

## Tests

- Tests unitarios/integradores en `tests/` (un ejecutable por subsistema).
- Tests del lenguaje Vex en `tests/vex/`.
- Nuevos opcodes del VM: añade casos en `tests/` (e.g. `tests/runtime/`).
- Ejecuta: `ctest --test-dir build --output-on-failure --parallel`.

## Dependabot

Los PRs de Dependabot se auto-fusionan si los tests pasan.  No los cierres
sin avisar a los maintainers si bloquean el build.

## Convenciones del ecosistema

Consulta `VEX_ECOSYSTEM_STANDARDS.md` para estándares de módulos,
plugins, paquetes (`vexpm`) y extensión del lenguaje.

## Revisión de código

- Sé respetuoso y específico en los comentarios.
- Prefiere sugerencias accionables sobre juicios generales.
- Los maintainers fusionan solo con CI verde y revisión (según
  `GOVERNANCE.md`).
