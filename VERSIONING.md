# Versionado de VestaVM

VestaVM usa [Semantic Versioning 2.0.0](https://semver.org/) con una
salvedad documentada para el formato de bytecode.

---

## Formato

```
MAJOR.MINOR.PATCH[-prerelease]
```

- **MAJOR**: cambios incompatibles en el formato de bytecode `.velb`, la
  ABI nativa, el protocolo VDP, o cambios de API pública del lenguaje que
  rompan programas existentes.
- **MINOR**: nuevas funcionalidades retrocompatibles (nuevas features del
  lenguaje, nuevos opcodes, nuevas instrucciones, nuevas APIs).
- **PATCH**: correcciones retrocompatibles (bugs, seguridad, rendimiento,
  portabilidad).
- **prerelease**: `-alpha.N`, `-beta.N`, `-rc.N` durante el ciclo de
  release.

## Reglas

1. La versión se declara en un único sitio canónico: el `vex.toml`
   (campo `version`) y se refleja en `CHANGELOG.md`.
2. `vm --version` imprime la versión de `vex.toml`.
3. El **bytecode `.velb`** lleva un `VERSION_VELB` independiente (hoy `0x2`).
   Un cambio incompatible de formato incrementa `MAJOR` *y* el
   `VERSION_VELB`; el loader debe rechazar con un mensaje claro los
   formatos que no soporta.
4. La **ABI de los plugins nativos** (`VestaPluginAPI`) se versiona aparte
   (ver `include/ffi/vesta_plugin.h`).
5. Las versiones **0.x** pueden romper compatibilidad en `MINOR` sin subir
   `MAJOR`, como permite SemVer para desarrollo temprano.  Al llegar a
   `1.0.0` se congela la estabilidad.

## Backends de compilador

El `vex.toml` puede declarar `[compiler]` con versiones mínimas
(`@Target("compiler>=1.0")`).  Un programa que usa una feature de `MINOR`
nueva debe declarar `compiler>=X.Y`.

## Compatibilidad de ramas

- `release` y `develop` son las únicas ramas con versión canónica.
- Las ramas `feature/*` y `hotfix/*` no incrementan versión.
- Los cambios de `CHANGELOG.md` se escriben en el PR que cierra un release.

## Etiquetas

Cada release se etiqueta como `vMAJOR.MINOR.PATCH` (ej. `v0.1.0`).  Las
etiquetas de prerelease: `v0.2.0-rc.1`.
