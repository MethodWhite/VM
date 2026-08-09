# Gobernanza de VestaVM

Este documento describe cómo se gobierna el proyecto VestaVM: quién toma
decisiones, cómo se toman y qué papel juega cada participante.

> **Estado**: adoptado el 2026-08-09.  Es un documento vivo; los cambios
> se proponen vía PR contra `release` y se ratifican según el proceso de
> decisión de abajo.

---

## 1. Modelo de gobernanza

VestaVM usa un modelo **benevolent-dictator + maintainers** (BDL), el mismo
que proyectos como Python, Linux (de facto) o Rust usan en la práctica:

- El **dictador benevolente (BD)** tiene la última palabra en decisiones
  disputadas y fija la dirección a largo plazo.
- Los **maintainers** tienen commit access, revisan PRs y mantienen
  subsistemas concretos (definidos en `MAINTAINERS.md` y `.github/CODEOWNERS`).
- Los **contributors** proponen cambios vía PR; cualquiera puede abrir issues.

Este modelo equilibra velocidad de decisión (una cabeza con autoridad) con
revisión colegiada (los maintainers revisan y votan en el día a día).

## 2. Roles

### 2.1 Dictador benevolente (BD)

Responsabilidades:

- Fijar la visión y la hoja de ruta a largo plazo (`doc/ROADMAP.md`).
- Resolver disputas de diseño que los maintainers no consiguen cerrar.
- Aprobar cambios de gobernanza y de alcance del lenguaje (`VEX_ECOSYSTEM_STANDARDS.md`).
- Nombrar (y, en última instancia, destituir) maintainers.

### 2.2 Maintainers

Responsabilidades:

- Revisar y fusionar PRs en sus áreas (según `CODEOWNERS`).
- Mantener la calidad: compilar, tests verdes, código según `CONTRIBUTING.md`.
- Responder a issues en sus subsistemas.
- Participar en el proceso de decisión técnica.
- Mantener la política de seguridad (`SECURITY.md`) en su área.

### 2.3 Contributors

Responsabilidades:

- Abrir issues claros y accionables.
- Enviar PRs pequeños, revisables y con tests.
- Seguir el `CONTRIBUTING.md` y el código de conducta.

## 3. Proceso de decisión

### 3.1 Decisiones rutinarias

Las decisiones de implementación dentro de un subsistema las toman sus
maintainers al revisar y fusionar PRs.  No requieren votación.

### 3.2 Decisiones significativas

Cambios que alteran:

- El **formato de bytecode** (`.velb` / secciones) o la **ABI nativa**.
- El **lenguaje Vex** (sintaxis, semántica, keywords) o sus estándares.
- El **protocolo de red VDP** (compatibilidad entre nodos).
- El **sandbox / sistema de capabilities**.
- La **gobernanza** o las **políticas** de seguridad/versión.

requieren un **RFC** (`.github/ISSUE_TEMPLATE/rfc.md`) y aprobación por
**consenso de maintainers**, con confirmación del BD si hay empate o disputa.

### 3.3 Votación

- Quórum: 50% de maintainers activos.
- Aprobación: 2/3 de los votos emitidos.
- El BD puede vetar o imponer una decisión, pero debe motivarlo por escrito
  en el RFC/issue.

### 3.4 Registro de decisiones (ADRs)

Las decisiones significativas se registran en `docs/decisions/` (ver
`docs/decisions/README.md`).  Cada ADR es un documento corto: contexto,
decisión y consecuencias.  El formato sigue el patrón clásico de ADR
(`https://adr.github.io/`).

## 4. Ramas y modelo de flujo

Ver `RELEASE_PROCESS.md` y `CONTRIBUTING.md`.  Resumen:

- `release` — estable, protegida, lista para producción.
- `develop` — integración de features.
- `feature/*` — trabajo de contributors.
- `hotfix/*` — parches urgentes contra `release`.
- Los maintainers fusionan; nadie pushea directamente a `release`/`develop`.

## 5. Derechos de contribución y propiedad

El proyecto usa la **licencia VMProject** (ver `LICENSE.md`).  Al enviar un
PR, los contribuidores aceptan la cláusula **Developer Certificate of Origin**
(DCO) descrita en `CONTRIBUTING.md`.  El copyright de las contribuciones se
retiene por sus autores; el proyecto recibe licencia para redistribuirlas.

## 6. Conflictos de interés

- Los maintainers no deben fusionar su propio PR sin al menos una revisión
  de otro maintainer.
- Las decisiones de dependencias/seguridad deben documentar cualquier
  interés comercial.

## 7. Enmiendas a la gobernanza

Cualquier cambio a este documento sigue el proceso de decisión significativa
(sección 3.2).  Se propone como PR contra `release`, se discute en el PR y
se ratifica con la votación de maintainers.

## 8. Preguntas y contacto

- Para preguntas sobre gobernanza: abrir un issue con la etiqueta `discussion`.
- Para reportes de seguridad: `SECURITY.md` (privado, siempre).
