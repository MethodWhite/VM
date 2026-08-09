# Proceso de Release de VestaVM

Este documento define el ciclo de release, desde `develop` hasta el
artefacto publicado y etiquetado.

---

## Modelo

GitFlow simplificado:

```
release (estable, protegida)
   ^ merge de develop (PR de release)
develop (integración)
   ^ merge de feature/* (PRs normales)
feature/*   -> trabajo de contributors
hotfix/*    -> desde release, se fusiona a release y develop
```

## Ciclo

### 1. Congelación de features

- Se abre un **PR de release** `develop -> release`.
- A partir de ese momento solo entran fixes (no features nuevas) a ese PR.
- La rama `release` es la única protegida: nadie pushea directo.

### 2. Estabilización

- Ejecutar la suite completa de tests: `ctest --test-dir build --output-on-failure --parallel`.
- Verificar el binario principal: `build/vm --version`.
- Comprobar los checks de CI (build en Linux/macOS/Windows + los nuevos
  targets de FreeBSD y análisis estático).

### 3. Versionado

- Incrementar la versión en `vex.toml` según `VERSIONING.md` (SemVer).
- Escribir la entrada correspondiente en `CHANGELOG.md`:
  - `[vMAJOR.MINOR.PATCH] - <fecha>` con secciones `Added / Fixed /
    Changed / Security / Removed`.
- Actualizar el badge de estado/tests del `README.md` si procede.

### 4. Tag y firma

- Crear un tag anotado firmado:
  ```bash
  git tag -s vMAJOR.MINOR.PATCH -m "VestaVM vMAJOR.MINOR.PATCH"
  git push origin vMAJOR.MINOR.PATCH
  ```
- Si el entorno no tiene clave de firma configurada, se usa un tag anotado
  normal y se documenta en el release notes que el binario va sin firma.

### 5. Publicación

- Crear el **GitHub Release** asociado al tag con las notas de
  `CHANGELOG.md` de esa versión.
- Adjuntar artefactos (si el CI los genera): binarios por plataforma,
  checksums (`sha256sum`) y el SBOM/SPDX de la versión.
- Publicar también el `.velb` de los módulos estándar si el ecosistema lo
  requiere (ver `vesta_core/`).

### 6. Post-release

- Fusionar el PR de release en `develop` (si no se hizo con merge)
  para mantener `develop` al día.
- Cerrar issues/PRs referenciados por el release.
- Si hubo un hotfix, asegurar que `develop` lo contiene.

## Hotfix

```bash
git checkout release
git checkout -b hotfix/nombre
# fix...
git commit -m "fix: ..."
# PR hotfix -> release (y luego cherry-pick/merge a develop)
```

Los hotfixes solo tocan `release` cuando hay una versión publicada y el
fix no puede esperar al siguiente ciclo normal.

## Definición de "hecho" (Definition of Done)

Un release está completo cuando:

- [ ] Todos los tests pasan en las plataformas soportadas.
- [ ] `build/vm --version` reporta la versión correcta.
- [ ] `CHANGELOG.md` tiene la entrada de la versión.
- [ ] El tag `vMAJOR.MINOR.PATCH` existe y está firmado (si es posible).
- [ ] El GitHub Release tiene notas y artefactos.
- [ ] El SBOM/SPDX de la versión se ha generado y adjuntado.
- [ ] `develop` contiene el release (o el hotfix, si aplica).

## Seguridad en el proceso

- Los releases llevan **firma** cuando hay clave disponible.
- Los binarios publicados se acompañan de **checksums**.
- Cualquier fix de seguridad va precedido del advisory de `SECURITY.md` y
  se anota en la sección `Security` del changelog.
