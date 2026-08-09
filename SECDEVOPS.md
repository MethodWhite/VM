# SECDEVOPS: Seguridad en el ciclo de vida de VestaVM

Este documento define cómo se aplica seguridad en el pipeline de desarrollo
de VestaVM: CI/CD, herramientas, hardening de build y cadena de suministro.
Es la referencia de la práctica **SECDEVOPS** del proyecto.

---

## 1. Principios

1. **Seguridad desde el primer commit**: los checks de seguridad corren en
   cada PR, no solo en releases.
2. **Falla rápido**: un check de seguridad que falla bloquea el merge.
3. **Sin secretos**: ninguna credencial en código, config o logs.
4. **Menor privilegio**: los tokens de CI tienen el mínimo alcance y
   rotación.
5. **Cadena de suministro verificable**: dependencias fijadas, firmas y SBOM.

## 2. Pipeline CI (TierS)

El CI de `ci.yml` define el ciclo por capas:

| Capa | Checks | Bloquea |
|------|--------|---------|
| **Tier 1 — Build** | Compila en Linux/macOS/Windows/FreeBSD | Sí |
| **Tier 2 — Tests** | `ctest` en las plataformas | Sí |
| **Tier 3 — Lint/estático** | `clang-tidy`, `cppcheck`, DCO, Conventional Commits | Sí |
| **Tier 4 — Security** | `gitleaks` (secretos), `bandit`/`semgrep`, dependabot | Sí |
| **Tier 5 — Supply chain** | OpenSSF Scorecard, SBOM/SPDX | Informa |

- `timeout-minutes` acotado por job para evitar builds colgados.
- `fail-fast: false` en la matriz para reportar todas las plataformas.
- Los checks de seguridad usan `continue-on-error` solo donde son
  informativos (nunca en gitleaks).

## 3. Protección de ramas

Ramas `release` y `develop` protegidas:

- Requieren PR con revisión de al menos 1 maintainer (`CODEOWNERS`).
- Requieren CI verde (todos los jobs necesarios).
- Prohíben push directo (excepto `hotfix/*` vía PR).
- Requieren que los commits lleven `Signed-off-by` (DCO).

## 4. Secretos

- Nada de secretos en el repo (verificado por `gitleaks` en CI).
- Los remotos de git **no** llevan tokens en la URL (ver `GOVERNANCE.md` y
  el incidente del token `ghp_` eliminado del remote `fork` en 2026-08-09).
- Los secretos de CI viven en los **GitHub Secrets** del repositorio, no en
  el código ni en las actions.
- Tokens con alcance mínimo y rotación periódica.

## 5. Hardening del build

Aplicado a los targets principales (`vm`, `vmcore`, `vesta_rt`):

- **PIE**: `-fPIE -pie` (ASLR ejecutable).
- **RELRO**: `-Wl,-z,relro,-z,now` (GOT hardening).
- **Stack protector**: `-fstack-protector-strong`.
- **FORTIFY**: `-D_FORTIFY_SOURCE=2` en Release.
- **No-exec stack**: `-Wl,-z,noexecstack`.
- **W^X** en el JIT: modo dual `mmap RW` → `mprotect RX`
  (ver `src/jit/code_cache.cpp`; macOS arm64 requiere `MAP_JIT`).
- **CFI** (opcional, donde el toolchain lo soporte).

## 6. Cadena de suministro

- **Dependabot**: semanal para `github-actions` y `gitsubmodule`.
- Los submodulos de `libs/SourceCode/` se fijan a versiones conocidas y
  se auditan al actualizar.
- **SBOM/SPDX**: cada release genera `sbom.spdx.json` (ver
  `RELEASE_PROCESS.md`) y se publica con los binarios.
- **OpenSSF Scorecard**: workflow semanal que puntúa el repositorio y
  detecta fugas de credenciales, permisos de workflows, etc.

## 7. Política de vulnerabilidades

Ver `SECURITY.md`: disclosure coordinada, tiempos de respuesta y áreas de
interés.  El proceso de advisory se documenta allí.

## 8. Checklist de un PR seguro

- [ ] Sin secretos ni rutas absolutas del desarrollador.
- [ ] Sin `TODO`/`FIXME` de seguridad sin resolver.
- [ ] Entrada de E/S validada (bytecode, red, FFI).
- [ ] Memoria: no introducir nuevas rutas de use-after-free / OOB.
- [ ] Warnings limpios (`-Wall -Wextra`).
- [ ] Tests que cubren el camino nuevo (incluido el camino de error).

## 9. Mejora continua

El estado de estos controles se revisa en cada release.  Cualquier check
que se omita por falsos positivos debe documentarse y reemplazarse, no
eliminarse.
