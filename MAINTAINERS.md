# Maintainers de VestaVM

Los maintainers son las personas con autoridad de revisión y fusión sobre
el proyecto.  Sus áreas exactas están definidas en `.github/CODEOWNERS`.

## Dictador benevolente

| Persona | GitHub | Área |
|---------|--------|------|
| David López T. | [@desmonHak](https://github.com/desmonHak) | Dirección, lenguaje Vex, runtime, VM |

## Maintainers

| Persona | GitHub | Área principal |
|---------|--------|----------------|
| David López T. | [@desmonHak](https://github.com/desmonHak) | runtime, jit, gc, vex, parser, ir, optimizer, install |
| MethodWhite | [@MethodWhite](https://github.com/MethodWhite) | CI/CD, tooling, build, docs, portabilidad (Linux/BSD/macOS), seguridad |

## Responsabilidades comunes

- Revisar y fusionar PRs en sus áreas (o escalarlos al resto de maintainers).
- Mantener la compilación y los tests verdes en su área.
- Responder a issues etiquetados con sus subsistemas.
- Participar en el proceso de decisión técnica (`GOVERNANCE.md`).
- Mantener al día la documentación de su área.

## Incorporar un nuevo maintainer

1. Propuesta por un maintainer existente (issue o PR de gobernanza).
2. El candidato debe haber demostrado contribuciones sostenidas de calidad
   (≥ 3 meses de PRs revisados y fusionados).
3. Votación de maintainers según `GOVERNANCE.md` §3.
4. Añadirlo a `MAINTAINERS.md` y a `.github/CODEOWNERS`.

## Remoción

Un maintainer puede dejar de serlo:

- Por petición propia.
- Por inactividad prolongada (≈ 6 meses sin actividad en el proyecto).
- Por violación reiterada del código de conducta o de las políticas.

La remoción la decide el BD con consulta a los maintainers restantes.
