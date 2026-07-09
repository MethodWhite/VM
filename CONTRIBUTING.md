# Contribuyendo a VestaVM

## Rama principal

- `release` — estable, lista para producción
- `develop` — integración de features
- `feature/*` — ramas de trabajo
- `hotfix/*` — parches urgentes sobre release

## Estándares de código

- C++17, C11 para código C
- 4 espacios, sin tabs
- `snake_case` funciones/variables, `PascalCase` tipos/clases
- `#pragma once` en headers
- Funciones pequeñas y enfocadas

## Pull Request process

1. Crea una rama desde `develop`
2. Implementa los cambios
3. Asegura que compila: `cmake -B build && cmake --build build`
4. Ejecuta tests: `ctest --test-dir build --output-on-failure`
5. Si agregas nuevas instrucciones VM, implementa el handler correspondiente
6. Si agregas nuevas caps, actualiza la documentación del sandbox
7. Crea el PR contra `develop` con la plantilla correspondiente

## Commits

Usa [Conventional Commits](https://www.conventionalcommits.org/):
- `feat:` nueva funcionalidad
- `fix:` corrección de bug
- `refactor:` cambio sin corrección ni feature
- `docs:` documentación
- `test:` tests
- `infra:` CI, build, tooling

## Dependabot

Los PRs de Dependabot son auto-merge si los tests pasan.
