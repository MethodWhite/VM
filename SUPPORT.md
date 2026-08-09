# Soporte de VestaVM

Este documento explica dónde obtener ayuda y qué nivel de soporte ofrecerá
cada canal.  Antes de abrir un issue, revisa la documentación:

- [Inicio rápido](./doc/QUICKSTART.md)
- [Lenguaje Vex](./doc/LANGUAGE.md)
- [Arquitectura](./doc/ARCHITECTURE.md)
- [CLI y comandos](./doc/CLI_COMMANDS.md)
- [Preguntas frecuentes en issues etiquetadas `question`](https://github.com/desmonHak/VM/issues)

---

## Canales

### Issues de GitHub

- **Bug**: usa la plantilla `Bug report`.  Incluye versión, SO, comando,
  salida esperada vs real y un ejemplo mínimo reproducible.
- **Feature request**: usa la plantilla `Feature request`.
- **RFC**: para cambios significativos, usa la plantilla `RFC`.

Etiquetas útiles: `bug`, `enhancement`, `discussion`, `question`,
`priority:critical`, `good first issue`, `help wanted`.

### Seguridad

**Nunca** reportes vulnerabilidades en un issue público.  Usa el proceso de
disclosure coordinada de `SECURITY.md`.

### Comunidad

- Discusión de diseño: issues etiquetadas `discussion`.
- El `CODE_OF_CONDUCT.md` aplica en todos los espacios del proyecto.

---

## Niveles de soporte

| Canal | Tiempo de respuesta objetivo | Soporte |
|-------|------------------------------|---------|
| Issues (bug/feature) | 7 días | Best-effort por maintainers |
| RFC | 14 días | Discusión y decisión según gobernanza |
| Vulnerabilidades | 72 h (validación) / 90 días (fix) | Coordinado, privado |
| Dependabot | automático | PRs con tests verdes se auto-fusionan |

El proyecto es mantenido por voluntarios en su tiempo libre.  La respuesta
no está garantizada en un SLA formal, pero se monitoriza con el stale bot.

---

## Plataformas soportadas

- **Linux** (x86_64, principal; AArch64 en roadmap)
- **macOS** (x86_64; Apple Silicon en progreso)
- **FreeBSD / OpenBSD / NetBSD** (soporte en curso)
- **Windows** (x86_64)

Consulta `doc/ROADMAP.md` y el badge de plataformas en el `README.md` para
el estado actual.
