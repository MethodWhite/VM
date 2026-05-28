/**
 * @file sandbox.h
 * @brief Sandbox basado en capabilities para módulos no confiables.
 *
 * Este módulo proporciona un modelo de seguridad opt-in que permite
 * restringir qué operaciones puede ejecutar cada módulo cargado por la
 * VM. Está pensado para escenarios donde el código cargado proviene de
 * fuentes externas (plugins, scripts de usuarios, paquetes de un
 * marketplace) y no se le quiere conceder acceso completo al proceso.
 *
 * ## Modelo conceptual
 *
 * El modelo es **capability-based**: cada módulo lleva un bitmask de
 * permisos (capabilities) y, opcionalmente, whitelists granulares por
 * recurso. Las operaciones potencialmente peligrosas del runtime (FFI,
 * filesystem, red, IPC distribuido, AOP, etc.) consultan estos
 * permisos antes de ejecutarse.
 *
 * El programador del host **decide explícitamente** qué restringir.
 * Por defecto todos los permisos están concedidos (modo "sin sandbox"),
 * lo que mantiene compatibilidad total con código existente. Cuando se
 * activa el sandbox via `--vex-caps`, los chequeos pasan a tener efecto.
 *
 * ## Coste en runtime
 *
 * En modo "sin sandbox" (todas las caps concedidas, sin whitelists) el
 * predicado @c Caps::unrestricted() devuelve @c true y el chequeo
 * colapsa a UN bitwise AND + un compare. El branch predictor del CPU
 * resuelve siempre "no tomado" (~1 ns), por lo que la suite de tests
 * sin sandbox pasa con cero regresión medible.
 *
 * En modo restringido, cada chequeo es:
 *   - 1 lookup del módulo activo (binary search en @c Loader::executables)
 *   - 1 bitwise AND contra la máscara requerida
 *   - 0 ó 1 búsqueda lineal en la whitelist correspondiente
 *
 * Coste total típico: del orden de decenas de nanosegundos, despreciable
 * frente al coste de la operación que se está protegiendo.
 *
 * ## Las 10 capabilities
 *
 * | Cap | Cubre |
 * |:----|:------|
 * | `FS_READ`   | Operaciones de lectura del filesystem. |
 * | `FS_WRITE`  | Operaciones de escritura del filesystem. |
 * | `NET`       | Sockets y conexiones de red. |
 * | `FFI_CALL`  | Invocación de funciones nativas (CALLN / callni). |
 * | `FFI_OPEN`  | Carga de librerías dinámicas (dlopen / LoadLibrary). |
 * | `SPAWN`     | Creación de procesos VM. |
 * | `DISTRIB`   | IPC con nodos remotos (rspawn, msgsend cross-node). |
 * | `CLASSREG`  | Hook de AOP cross-módulo (addadvice). |
 * | `MEM_HOST`  | LOAD/STORE de host_ptr (acceso directo a memoria host). |
 * | `LOADMOD`   | Carga dinámica de otros módulos (loadmodule). |
 *
 * ## Whitelists granulares
 *
 * Sobre la cap booleana, se pueden añadir whitelists para limitar aún
 * más el alcance del permiso:
 *
 *   - **dll_whitelist** (FFI_CALL/FFI_OPEN): nombres de DLL permitidas.
 *     Si la lista no está vacía, solo esas DLLs pueden invocarse aunque
 *     la cap esté concedida.
 *
 *   - **path_whitelist** (FS_READ/FS_WRITE/LOADMOD): prefijos de path
 *     permitidos. Un módulo con `fs:read=/tmp` solo puede leer archivos
 *     bajo `/tmp/...`.
 *
 *   - **host_whitelist** (NET): hosts o pares `host:port` permitidos.
 *     Match exacto, con regla de que `host` (sin port) admite cualquier
 *     puerto en ese host.
 *
 *   - **mem_ranges** (MEM_HOST): rangos `[start, end)` de host pointers
 *     permitidos. Si MEM_HOST está concedida como bool, los rangos se
 *     ignoran. Si MEM_HOST está denegada con rangos, solo los pointers
 *     dentro de esos rangos son aceptados.
 *
 * Una whitelist vacía equivale a "sin restricción adicional" sobre el
 * recurso correspondiente.
 *
 * ## Equivalencias en otros entornos
 *
 *   - Java: @c java.lang.SecurityManager con permisos similares.
 *   - .NET: Code Access Security (modelo deprecado pero conceptualmente equivalente).
 *   - WASI: capabilities entregadas como handles (file descriptors, etc.).
 *   - Deno: flags `--allow-net=host`, `--allow-read=path`, etc.
 *
 * ## Composición con otras capas de aislamiento
 *
 * El sandbox basado en capabilities es la primera línea de defensa.
 * Si el plugin tiene `FFI_CALL` y carga una DLL nativa, esa DLL puede
 * hacer syscalls arbitrarias porque corre con los permisos del proceso
 * host. Para escenarios adversariales, este modelo se combina con:
 *
 *   - Aislamiento por ProcessVM: el plugin corre en un proceso lógico
 *     separado con su propio gc_heap y vm_mem. No comparte memoria con
 *     el caller.
 *
 *   - Aislamiento a nivel de OS: cada plugin se ejecuta en un proceso
 *     OS independiente con seccomp/AppContainer/Job Object. La única
 *     forma de defensa real contra DLLs maliciosas ya cargadas.
 *
 * Esta cabecera implementa solo el primer nivel (in-process). Las otras
 * capas son responsabilidad de otros subsistemas.
 */

#ifndef VESTA_LOADER_SANDBOX_H
#define VESTA_LOADER_SANDBOX_H

#include <cstdint>
#include <string>
#include <vector>

namespace loader {

/**
 * @struct CapWhitelists
 * @brief Conjunto de whitelists granulares asociadas a las capabilities.
 *
 * Cada whitelist limita la cap booleana correspondiente. Si la lista
 * está vacía, no hay restricción adicional (la cap booleana sola decide).
 *
 * El programador del host puebla estas listas al construir un
 * @c Caps desde un string de configuración (ver @c parse_caps).
 */
struct CapWhitelists {
    /**
     * @brief DLLs permitidas para invocaciones FFI.
     *
     * Consultada por @c Caps::dll_allowed cuando @c FFI_CALL o
     * @c FFI_OPEN están concedidas. Match exacto sobre el nombre canónico
     * de la DLL (case-insensitive en Windows, case-sensitive en POSIX).
     *
     * Lista vacía equivale a "sin restricción": cualquier DLL pasa el
     * chequeo siempre que la cap booleana esté concedida.
     */
    std::vector<std::string> dll_whitelist;

    /**
     * @brief Prefijos de path permitidos para filesystem y carga de módulos.
     *
     * Consultada por @c Caps::path_allowed cuando @c FS_READ, @c FS_WRITE
     * o @c LOADMOD están concedidas. El match se hace por @c starts_with
     * con normalización de separadores (`\` se convierte a `/`).
     *
     * Para limitar a un directorio concreto sin matches accidentales en
     * directorios hermanos, conviene terminar la entry con `/`. Por
     * ejemplo, `"/tmp/"` solo matchea `/tmp/foo.txt`, no `/tmpfoo.txt`.
     */
    std::vector<std::string> path_whitelist;

    /**
     * @brief Hosts o pares host:port permitidos para conexiones de red.
     *
     * Consultada por @c Caps::host_allowed cuando @c NET está concedida.
     * Formato: `"host"` (cualquier puerto) o `"host:port"` (match exacto).
     *
     * Match exacto sobre todo el string. Si la entry whitelisted es solo
     * `"host"` (sin port) y el destino solicitado es `"host:1234"`, el
     * match prospera (el host coincide y el puerto queda libre).
     */
    std::vector<std::string> host_whitelist;

    /**
     * @struct MemRange
     * @brief Rango cerrado-abierto @c [start, end) de host pointers.
     */
    struct MemRange { uint64_t start; uint64_t end; };

    /**
     * @brief Rangos de host pointers permitidos para LOAD/STORE.
     *
     * Solo se consulta cuando @c MEM_HOST está DENEGADA. Si MEM_HOST
     * está concedida como bool, esta lista se ignora (fast path).
     *
     * Si MEM_HOST está denegada y la lista NO está vacía, solo los
     * accesos a direcciones que caen en alguno de los rangos pasan.
     * Si está vacía con MEM_HOST denegada, todos los accesos host_ptr
     * son rechazados.
     */
    std::vector<MemRange> mem_ranges;

    /**
     * @brief Devuelve @c true si TODAS las listas están vacías.
     *
     * Es la condición que permite el fast path: cuando no hay
     * whitelists, los chequeos `xxx_allowed` retornan @c true sin
     * iterar nada. Combinado con @c Caps::bits == ALL, habilita el
     * modo "sin sandbox" con cero overhead.
     */
    [[nodiscard]] bool empty() const noexcept {
        return dll_whitelist.empty() && path_whitelist.empty()
            && host_whitelist.empty() && mem_ranges.empty();
    }
};

/**
 * @struct Caps
 * @brief Conjunto de capabilities (bitmask) más whitelists granulares.
 *
 * Representa todos los permisos asociados a un módulo cargado. Cada
 * Executable del Loader lleva una instancia. El runtime la consulta
 * antes de ejecutar operaciones potencialmente peligrosas.
 *
 * Para el modo por defecto (sin sandbox), @c bits = ALL y @c wl vacía
 * -- @c unrestricted() devuelve @c true y los chequeos son no-op.
 */
struct Caps {
    /// Bitmask de capabilities concedidas. Default = ALL (sin sandbox).
    uint32_t bits = 0xFFFFFFFFu;

    /// Whitelists granulares. Vacías por defecto.
    CapWhitelists wl;

    /**
     * @brief Bits individuales asignados a cada capability.
     *
     * Los valores son @c (1u << N) para que la composición vía OR
     * (`FS_READ | FFI_CALL`) sea natural. @c ALL combina todas y es
     * el valor por defecto de @c bits.
     */
    enum : uint32_t {
        FS_READ   = 1u << 0,  ///< Lectura del filesystem.
        FS_WRITE  = 1u << 1,  ///< Escritura del filesystem.
        NET       = 1u << 2,  ///< Sockets y conexiones de red.
        FFI_CALL  = 1u << 3,  ///< Invocación de funciones nativas ya cargadas.
        FFI_OPEN  = 1u << 4,  ///< Carga de librerías dinámicas.
        SPAWN     = 1u << 5,  ///< Creación de procesos VM.
        DISTRIB   = 1u << 6,  ///< IPC con nodos remotos.
        CLASSREG  = 1u << 7,  ///< Hook AOP cross-módulo (addadvice).
        MEM_HOST  = 1u << 8,  ///< Acceso directo a host pointers.
        LOADMOD   = 1u << 9,  ///< Carga dinámica de módulos.

        /// Todas las capabilities concedidas. Default para @c bits.
        ALL  = 0xFFFFFFFFu,

        /// Ninguna capability concedida. Sandbox total.
        NONE = 0u,
    };

    /**
     * @brief Comprueba si el conjunto está completamente sin restringir.
     *
     * Es @c true cuando @c bits == ALL Y @c wl está vacía. Permite que
     * el runtime salte completamente los chequeos cuando el sandbox
     * está desactivado, sin coste apreciable.
     *
     * @return @c true si no hay restricción alguna; @c false en otro caso.
     */
    [[nodiscard]] bool unrestricted() const noexcept {
        return bits == ALL && wl.empty();
    }

    /**
     * @brief Comprueba si una máscara de caps está completamente concedida.
     *
     * El chequeo es un AND bitwise: la máscara requerida debe estar
     * íntegramente presente en @c bits para que retorne @c true.
     *
     * @param required Bitmask con las caps a comprobar. Puede combinar
     *                 varias con OR (`FS_READ | FFI_CALL`).
     * @return @c true si todas las caps de @p required están concedidas.
     */
    [[nodiscard]] bool has(uint32_t required) const noexcept {
        return (bits & required) == required;
    }

    /**
     * @brief Aplica una máscara AND sobre los bits actuales.
     *
     * Solo puede quitar caps, nunca añadir. Esta es la garantía de
     * no-escalation: cuando un módulo restringido carga otro módulo
     * mediante @c loadmodule, las caps del módulo cargado se intersectan
     * con las del caller, y un plugin restringido no puede otorgar más
     * permisos de los que él mismo tiene.
     *
     * @param mask Máscara AND a aplicar.
     */
    void intersect(uint32_t mask) noexcept { bits &= mask; }

    /**
     * @brief Comprueba si una DLL está autorizada por la whitelist.
     *
     * Solo tiene sentido consultarlo cuando @c FFI_CALL o @c FFI_OPEN
     * están concedidas; si lo están y la whitelist no es vacía, este
     * método decide si la DLL específica puede usarse.
     *
     * @param dll_name Nombre canónico de la DLL (con extensión).
     * @return @c true si la whitelist está vacía o contiene la DLL;
     *         @c false si la whitelist no contiene la DLL.
     */
    [[nodiscard]] bool dll_allowed(const std::string &dll_name) const noexcept;

    /**
     * @brief Comprueba si un path está autorizado por la whitelist.
     *
     * Match por prefijo con normalización de separadores. Aplicable
     * tanto a operaciones de filesystem (FS_READ/FS_WRITE) como a
     * carga de módulos (LOADMOD).
     *
     * @param path Path absoluto o relativo a verificar.
     * @return @c true si la whitelist está vacía o algún prefijo coincide;
     *         @c false si la whitelist no contiene un prefijo del path.
     */
    [[nodiscard]] bool path_allowed(const std::string &path) const noexcept;

    /**
     * @brief Comprueba si un host:port está autorizado por la whitelist.
     *
     * @param host_port String del destino, formato `"host"` o `"host:port"`.
     * @return @c true si la whitelist está vacía, coincide exactamente
     *         o coincide solo en el host (cuando la entry no tiene port).
     */
    [[nodiscard]] bool host_allowed(const std::string &host_port) const noexcept;

    /**
     * @brief Comprueba si una dirección host_ptr cae en algún rango whitelisted.
     *
     * Solo se invoca cuando MEM_HOST está denegada. Si MEM_HOST está
     * concedida, este chequeo se omite y todos los pointers pasan.
     *
     * @param ptr Dirección host a verificar.
     * @return @c true si la whitelist está vacía o algún rango contiene
     *         @p ptr; @c false en caso contrario.
     */
    [[nodiscard]] bool mem_addr_allowed(uint64_t ptr) const noexcept;
};

/**
 * @brief Construye un @c Caps a partir de un string de configuración.
 *
 * La sintaxis acepta tres formas:
 *
 *   1. Atajos completos: `"all"` (todo concedido, default) o `"none"`
 *      (sandbox total, nada concedido).
 *
 *   2. Lista de caps separadas por coma, opcionalmente con argumentos
 *      whitelisted tras un `=` y separados internamente con `;`:
 *
 * @code
 *   "fs:read=/tmp,net=localhost:8080,ffi:call=kernel32.dll;user32.dll"
 * @endcode
 *
 *      En el ejemplo anterior, FS_READ se concede con whitelist de
 *      path `[/tmp]`, NET se concede con whitelist de host
 *      `[localhost:8080]`, y FFI_CALL se concede con whitelist de DLLs
 *      `[kernel32.dll, user32.dll]`.
 *
 *   3. Rangos de memoria con notación hex `inicio-fin`:
 *
 * @code
 *   "mem:host=0x10000000-0x20000000"
 * @endcode
 *
 *      Esta forma especial DENIEGA la cap MEM_HOST como bool pero
 *      añade el rango como whitelist, restringiendo el acceso solo a
 *      ese intervalo.
 *
 * @param s String de configuración tal como llega de la CLI o de
 *          `loadmodule`.
 * @return Estructura @c Caps con bits y whitelists pobladas. Tokens
 *         desconocidos se ignoran silenciosamente.
 */
Caps parse_caps(const std::string &s) noexcept;

/**
 * @brief Serializa un @c Caps al formato textual canónico.
 *
 * Función inversa de @c parse_caps. El string resultante, al parsearse,
 * produce un @c Caps equivalente al original (modulo orden de los
 * tokens y caps redundantes).
 *
 * Útil para mensajes de log y diagnóstico ("[sandbox] modulo principal
 * con caps: ...").
 *
 * @param caps Estructura a serializar.
 * @return String legible. `"all"` si está sin restringir, `"none"` si
 *         no hay nada concedido.
 */
std::string caps_to_string(const Caps &caps) noexcept;

/**
 * @brief Nombre legible de una cap concreta a partir de su bit.
 *
 * Solo está pensado para diagnóstico: dado un único bit, retorna el
 * nombre canónico (`"ffi:call"`, `"fs:read"`, etc.).
 *
 * @param single_cap Bit individual (1 << N).
 * @return Cadena estática con el nombre; `"unknown"` si el bit no es
 *         uno de los definidos.
 */
const char *cap_name(uint32_t single_cap) noexcept;

}  // namespace loader

#endif  // VESTA_LOADER_SANDBOX_H
