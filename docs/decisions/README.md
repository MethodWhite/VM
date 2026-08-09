# Registro de Decisiones de Arquitectura (ADR)

Este directorio guarda las decisiones significativas del proyecto en
formato ADR (Architecture Decision Records).  Cada ADR es un documento
corto que registra **qué** se decidió, **por qué**, y **qué consecuencias**
tuvo.

## Formato

Cada ADR sigue la plantilla clásica:

- **Título**: `ADR-NNNN: título corto`
- **Estado**: Propuesto / Aceptado / Reemplazado / Obsoleto
- **Contexto**: el problema que motivó la decisión.
- **Decisión**: qué se hizo.
- **Consecuencias**: positivas y negativas.

## Reglas

1. Los ADR se numeran secuencialmente (`0001`, `0002`, ...).
2. Solo las decisiones **significativas** requieren ADR (ver
   `GOVERNANCE.md` §3.2): cambios de bytecode/ABI/protocolo, lenguaje,
   sandbox, gobernanza, seguridad.
3. Un ADR aceptado puede ser **reemplazado** por uno nuevo que lo señale
   en su estado.
4. Los ADR no se reescriben en el pasado: se añade uno nuevo.

## Índice

| ADR | Título | Estado |
|-----|--------|--------|
| [0001](./0001-exit-codes-fallback.md) | Exit code 128+señal tras fallo sin capturar | Aceptado |
