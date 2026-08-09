# Security Policy de VestaVM

VestaVM es una máquina virtual que ejecuta código arbitrario y un
compilador que genera código nativo: la seguridad es una propiedad central
del proyecto, no un accesorio.  Esta política define qué versiones se
soportan y cómo se reportan las vulnerabilidades.

---

## Supported Versions

| Version | Supported          |
|---------|--------------------|
| release | :white_check_mark: |
| develop | :white_check_mark: |

Solo la rama `release` recibe parches de seguridad publicados.  `develop`
los recibe de forma anticipada durante el siguiente ciclo.  No hay soporte
de seguridad para versiones anteriores.

## Reportar una vulnerabilidad

**No abras un issue público ni un PR para una vulnerabilidad.**

Usa el proceso de **disclosure coordinada**:

1. Reporta de forma **privada** a:
   - **GitHub Security Advisory**: https://github.com/desmonHak/VM/security/advisories
   - **Email de respaldo**: methodwhite@proton.me
2. Incluye, en la medida de lo posible:
   - Versión afectada y rama.
   - Plataforma (Linux/BSD/macOS/Windows) y arquitectura.
   - Cómo se dispara (código Vex, `.velb`, plugin nativo, red VDP...).
   - Impacto estimado (RCE, DoS, escape del sandbox, lectura de memoria...).
   - Prueba de concepto o ejemplo reproducible.
3. No incluyas el exploit en ningún canal público.

### Qué se espera de ti

- **No exfiltrar** datos de sistemas ajenos.
- **No explotar** la vulnerabilidad más allá de lo necesario para validarla.
- Dar tiempo razonable al equipo para emitir el fix antes de publicar.

### Qué puedes esperar de nosotros

| Fase | Plazo objetivo |
|------|----------------|
| Acuse de recibo y validación | ≤ 72 h |
| Plan de mitigación | ≤ 7 días |
| Fix en `release` (severidad alta/crítica) | ≤ 90 días |
| Divulgación pública coordinada | Tras el fix, por acuerdo |

La divulgación pública se coordina con el reportante; se da crédito público
a quien reporte vulnerabilidades validadas (a menos que pida anonimato).

## Áreas de interés de seguridad

- **Sandbox / capabilities**: escape del sandbox de bytecode o JIT
  (`--vex-caps`, `dlopen`, `spawn`, `loadmod`, `defclass`).
- **JIT**: escritura de código ejecutable, W^X, selftests de memoria.
- **GC / memoria**: use-after-free, double-free, corrupción de heap.
- **Borrow checker**: eludir las garantías de `borrow_mut`/`lend_mut`.
- **FFI**: carga de librerías, símbolos, llamadas con argumentos inválidos.
- **Red (VDP)**: deserialización de mensajes, auth token/mTLS, discovery.
- **Loader**: parsing de `.velb`/`.vel` malformados, rebase, verificación
  de firmas.
- **Distribuido**: `rspawn` remoto, mensajería entre nodos.

## Seguridad por defecto (build)

Ver `SECDEVOPS.md` para la política completa de pipeline y hardening.
Resumen del build:

- Mitigaciones de compilador activadas (PIE, RELRO, stack protector,
  `_FORTIFY_SOURCE`, no-exec stack) en los targets principales.
- W^X en el JIT (modo dual disponible).
- Sandbox con capabilities para módulos no confiables.

## Proceso de un advisory

1. Se crea un **GitHub Security Advisory** (borrador, privado).
2. Se desarrolla el fix en una rama `hotfix/security-*`.
3. Se publica el fix y el advisory al mismo tiempo (o con el embargo pactado).
4. El fix entra al `CHANGELOG.md` bajo `Security`.

## Seguridad en dependencias

- **Dependabot** (`github-actions`, `gitsubmodule`) abre PRs semanales.
- Se revisa la cadena de suministro: los submodulos en `libs/SourceCode/`
  deben proceder de fuentes de confianza y sus versiones deben fijarse.
- El SBOM/SPDX de cada release se publica junto a los binarios.
