# Terceras partes (vendored) — VestaVM

Manifest de dependencias de terceros incluidas en el repo (vendored bajo
`libs/SourceCode/`).  Cumple Tier S++ §10 (deps pinneadas / vendoring con
metadatos) y el checklist CHK-TIER-SPLUS.

Las libs que son **submodules git** quedan pinneadas por commit (el SHA en
`.gitmodules`/`.git` fija la versión exacta).  Las carpetas planas (json,
sqlite, cxxopts) se mantienen actualizadas manualmente; este manifest
documenta la revisión en uso.

## Submodules git (version pin por commit)

| Lib | Version / ref | Commit | Licencia |
|-----|---------------|--------|----------|
| capstone | 5.0.7 | `52c66920fc` | BSD-3-Clause + LICENSE_LLVM.TXT |
| ftxui | v6.1.9 | `5cfed50702` | MIT |
| keystone | 0.9.2 | `707bbf4a11` | GPL-2.0 (COPYING) |
| DistanciaLevenshtein | heads/main | `270222c794` | — |
| LibPEparse | heads/main | `443895301f` | — |
| doc/VMdoc | heads/master | `92c9253a31` | — (documentacion) |

## Carpetas planas (sin submodule)

| Lib | Refs en uso | Licencia |
|-----|-------------|----------|
| cxxopts | (vendored, HEAD del repo en el momento) | MIT (LICENSE.md) |
| json (nlohmann) | (vendored) | MIT (LICENSE.MIT) |
| sqlite | amalgamation (sqlite3.c/.h) | Public domain |
| DistanciaLevenshtein | — | — |

## Verificacion en CI

El workflow `sast-sca.yml` ya verifica que cada lib vendored tenga metadatos
identificables (VERSION/README/LICENSE).  Este manifest es la referencia
canonica de versiones para revision manual y para el SBOM de release
(`.github/workflows/release.yml`).

## Como actualizar una lib

- Submodules: `git -C libs/SourceCode/<lib> fetch && git checkout <tag>` y
  commitear el nuevo SHA (el submodule lo fija).
- Planas: reemplazar el contenido y actualizar este manifest con la nueva
  version.
