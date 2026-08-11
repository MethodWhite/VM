/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribucion obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file native_ffi.h
 * @brief Interfaz de funciones externas (FFI) para llamadas a codigo nativo desde VestaVM.
 *
 * Proporciona la infraestructura necesaria para que el bytecode de VestaVM invoque
 * funciones nativas del sistema operativo (DLL en Windows, .so en Linux) mediante la
 * instruccion @c CALLN.
 *
 * Modelo de funcionamiento:
 *  1. Durante el ensamblado, las directivas @c @Import registran cada simbolo nativo
 *     (modulo + funcion + firma) como una entrada en la tabla de importaciones.
 *  2. En tiempo de carga (@c Loader), @c FFI::resolve_all() resuelve las direcciones
 *     reales usando @c LoadLibrary/GetProcAddress (Windows) o @c dlopen/dlsym (POSIX).
 *  3. Las direcciones resueltas se escriben directamente en el bytecode (patch-sites)
 *     para que la VM pueda invocarlas con un simple call indirecto.
 *
 * Tabla de alias de puntero de funcion (@c CallFn y @c Fn00..Fn12):
 *  - Cada tipo representa una firma con 0..12 argumentos @c uint64_t mas un puntero
 *    de contexto @c void* como primer parametro oculto.
 *  - El selector de firma se determina en tiempo de enlace a partir del campo
 *    @c signature_idx de @c NativeImportKey.
 */

#ifndef NATIVE_FFI_H
#define NATIVE_FFI_H

#include <string>
#include <vector>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include "ffi/vesta_plugin.h"

/**
 * @brief Tipo de puntero de funcion generica para llamadas FFI con hasta 12 argumentos.
 *
 * Primer parametro: puntero de contexto opaco (@c void*).
 * Parametros 2..13: argumentos @c uint64_t (rellenados con cero si no se usan).
 * Valor de retorno: @c uint64_t (el llamante convierte al tipo esperado).
 */
using CallFn = uint64_t(*)(
    void *,
    uint64_t, uint64_t,
    uint64_t, uint64_t,
    uint64_t, uint64_t,
    uint64_t, uint64_t,
    uint64_t, uint64_t,
    uint64_t, uint64_t);

namespace ffi {

    /* --- Alias de punteros de funcion por numero de argumentos --- */

    /** @brief Funcion nativa con 0 argumentos (mas contexto). */
    using Fn00 = uint64_t(*)(void *);
    /** @brief Funcion nativa con 1 argumento (mas contexto). */
    using Fn01 = uint64_t(*)(void *, uint64_t);
    /** @brief Funcion nativa con 2 argumentos (mas contexto). */
    using Fn02 = uint64_t(*)(void *, uint64_t, uint64_t);
    /** @brief Funcion nativa con 3 argumentos (mas contexto). */
    using Fn03 = uint64_t(*)(void *, uint64_t, uint64_t, uint64_t);
    /** @brief Funcion nativa con 4 argumentos (mas contexto). */
    using Fn04 = uint64_t(*)(void *, uint64_t, uint64_t, uint64_t, uint64_t);
    /** @brief Funcion nativa con 5 argumentos (mas contexto). */
    using Fn05 = uint64_t(*)(void *, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
    /** @brief Funcion nativa con 6 argumentos (mas contexto). */
    using Fn06 = uint64_t(*)(void *, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
    /** @brief Funcion nativa con 7 argumentos (mas contexto). */
    using Fn07 = uint64_t(*)(void *, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
    /** @brief Funcion nativa con 8 argumentos (mas contexto). */
    using Fn08 = uint64_t(*)(void *, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
    /** @brief Funcion nativa con 9 argumentos (mas contexto). */
    using Fn09 = uint64_t(*)(void *, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                             uint64_t);
    /** @brief Funcion nativa con 10 argumentos (mas contexto). */
    using Fn10 = uint64_t(*)(void *, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                             uint64_t, uint64_t);
    /** @brief Funcion nativa con 11 argumentos (mas contexto). */
    using Fn11 = uint64_t(*)(void *, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                             uint64_t, uint64_t, uint64_t);
    /** @brief Funcion nativa con 12 argumentos (mas contexto). */
    using Fn12 = uint64_t(*)(void *, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                             uint64_t, uint64_t, uint64_t, uint64_t);

    /**
     * @class FFIError
     * @brief Excepcion lanzada cuando el sistema FFI encuentra un error irrecuperable.
     *
     * Hereda de @c std::runtime_error; el mensaje describe la causa del error
     * (simbolo no encontrado, modulo no cargable, etc.).
     */
    class FFIError : public std::runtime_error {
    public:
        /**
         * @brief Construye una excepcion FFI con el mensaje indicado.
         * @param msg Descripcion del error ocurrido.
         */
        explicit FFIError(const std::string &msg)
            : std::runtime_error(msg) {}
    };

    /**
     * @brief Escribe la direccion de una funcion nativa en un offset del bytecode.
     *
     * Utilizado por el loader para parchear las instrucciones @c CALLN con la
     * direccion real de la funcion nativa una vez resuelta.  La escritura es de
     * 8 bytes (uint64_t) en little-endian, compatible con x86_64.
     *
     * @param bytecode_base Puntero al inicio del buffer de bytecode cargado en memoria.
     * @param off           Offset en bytes desde @p bytecode_base donde escribir la direccion.
     * @param fn            Puntero a la funcion nativa resuelta (obtenido con dlsym / GetProcAddress).
     */
    static inline void patch_call(uint8_t *bytecode_base, uint32_t off, void *fn) {
        uint64_t  addr          = reinterpret_cast<uint64_t>(fn); /* convierte puntero a entero de 64 bits */
        /* memcpy en vez de store directo: el offset del CALLN en el bytecode
         * no es multiplo de 8 (los CALLN pueden estar en cualquier offset) y
         * un store uint64 desalineado es UB (UBSan lo reporta; en x86 el
         * hardware lo tolera pero puede cruzar un limite de pagina).  memcpy
         * se compila al mismo store alineado cuando es posible y es definido. */
        std::memcpy(bytecode_base + off, &addr, sizeof(addr));
    }

    /**
     * @struct NativeImportKey
     * @brief Clave compacta que identifica un simbolo nativo de forma unica.
     *
     * Combina tres indices a tablas de strings internadas:
     *  - @c module_idx    : nombre del modulo (p.ej. "kernel32.dll", "libc.so").
     *  - @c function_idx  : nombre de la funcion (p.ej. "GetTickCount", "read").
     *  - @c signature_idx : firma textual del metodo (p.ej. "(void)->u32").
     *
     * El uso de indices en lugar de cadenas permite:
     *  - Evitar duplicacion de strings en memoria.
     *  - Comparaciones O(1) mediante tres enteros.
     *  - Hashing eficiente en @c NativeImportKeyHash.
     *  - Agrupar multiples patch-sites del mismo simbolo bajo una sola entrada.
     */
    struct NativeImportKey {
        uint32_t module_idx    = 0; ///< Indice al nombre del modulo en FFI::modules.
        uint32_t function_idx  = 0; ///< Indice al nombre de la funcion en FFI::functions.
        uint32_t signature_idx = 0; ///< Indice a la firma del metodo en FFI::signatures.

        /**
         * @brief Compara dos claves por igualdad de los tres indices.
         * @param other Clave con la que comparar.
         * @return @c true si los tres indices coinciden.
         */
        bool operator==(const NativeImportKey &other) const noexcept {
            return module_idx    == other.module_idx    &&
                   function_idx  == other.function_idx  &&
                   signature_idx == other.signature_idx;
        }
    };

    /**
     * @struct NativeImportRuntime
     * @brief Informacion en tiempo de ejecucion para un simbolo nativo importado.
     *
     * El loader crea una entrada por cada simbolo unico encontrado en el bytecode.
     * Contiene:
     *  - La clave del simbolo (@c key) para identificarlo.
     *  - El puntero resuelto (@c resolved_ptr) tras llamar a LoadLibrary+GetProcAddress
     *    o dlopen+dlsym.
     *  - Todos los @c patch_sites: offsets de bytecode donde se debe escribir @c resolved_ptr
     *    para que la instruccion @c CALLN pueda invocar la funcion directamente.
     *
     * El agrupamiento de patch-sites permite resolver cada simbolo nativo una sola vez
     * aunque aparezca en multiples instrucciones @c CALLN del bytecode.
     */
    struct NativeImportRuntime {
        NativeImportKey       key;                    ///< Clave que identifica el simbolo.
        void *                resolved_ptr = nullptr; ///< Direccion nativa resuelta en tiempo de carga.
        std::vector<uint32_t> patch_sites;            ///< Offsets de bytecode donde parchear el CALL.
    };

    /**
     * @struct NativeImportKeyHash
     * @brief Funcion hash para @c NativeImportKey.
     *
     * Combina los tres indices de la clave (module_idx, function_idx, signature_idx)
     * mediante desplazamientos XOR para obtener un valor de 64 bits con baja tasa
     * de colision, apto para @c std::unordered_map.
     */
    struct NativeImportKeyHash {
        /**
         * @brief Calcula el hash de una @c NativeImportKey.
         * @param k Clave a hashear.
         * @return Valor hash de 64 bits.
         */
        size_t operator()(NativeImportKey const &k) const noexcept {
            uint64_t h = k.module_idx;
            h = (h << 20) ^ k.function_idx;
            h = (h << 20) ^ k.signature_idx;
            return std::hash<uint64_t>{}(h);
        }
    };

    /**
     * @class FFI
     * @brief Sistema de FFI (Foreign Function Interface) de VestaVM.
     *
     * Gestiona el ciclo completo de importacion de funciones nativas:
     *
     *  1. **Internado de strings**: los nombres de modulos, funciones y firmas se
     *     almacenan una sola vez y se referencian mediante indices enteros.
     *
     *  2. **Tabla de importaciones**: agrupa bajo una @c NativeImportKey todos los
     *     offsets de bytecode que invocan el mismo simbolo nativo.
     *
     *  3. **Resolucion**: @c resolve_all() carga cada modulo nativo, localiza cada
     *     funcion con @c GetProcAddress / @c dlsym, y llama a @c patch_call() para
     *     escribir la direccion en todos sus patch-sites.
     *
     * @note Un objeto FFI por ejecutable cargado; no es hilo-seguro por si solo.
     */
    class FFI {
    public:
        /* --- Tablas de strings internadas --- */

        /**
         * @brief Vector de nombres de modulos nativos (uno por indice).
         *
         * Ejemplos: "kernel32.dll", "user32.dll", "libc.so.6".
         * Se llena mediante @c intern_module().
         */
        std::vector<std::string> modules;

        /** @brief Mapa string->indice para internar nombres de modulos en O(1). */
        std::unordered_map<std::string, uint32_t> module_map;

        /**
         * @brief Vector de nombres de funciones nativas (uno por indice).
         *
         * Ejemplos: "GetTickCount", "read", "send".
         * Se llena mediante @c intern_function().
         */
        std::vector<std::string> functions;

        /** @brief Mapa string->indice para internar nombres de funciones en O(1). */
        std::unordered_map<std::string, uint32_t> function_map;

        /**
         * @brief Vector de firmas de funciones nativas (una por indice).
         *
         * Ejemplos: "(void)->u32", "(i32,i32)->i32", "(str)->void".
         * Se llena mediante @c intern_signature().
         */
        std::vector<std::string> signatures;

        /** @brief Mapa string->indice para internar firmas en O(1). */
        std::unordered_map<std::string, uint32_t> signature_map;

        /**
         * @brief Tabla principal de importaciones nativas.
         *
         * Cada entrada asocia una @c NativeImportKey (unica por triple modulo/funcion/firma)
         * con su @c NativeImportRuntime, que contiene el puntero resuelto y los
         * patch-sites correspondientes.
         */
        std::unordered_map<NativeImportKey, NativeImportRuntime, NativeImportKeyHash> imports;

        /**
         * @brief Cache de handles de modulos nativos ya cargados.
         *
         * Evita llamar a @c LoadLibrary / @c dlopen mas de una vez por modulo.
         * La clave es el nombre del modulo; el valor es el handle opaco devuelto
         * por el sistema operativo.
         */
        std::unordered_map<std::string, void *> native_modules;

        /**
         * @brief Conjunto de modulos en los que ya se llamo a @c vesta_init.
         *
         * Evita inicializar dos veces el mismo plugin si se carga en multiples
         * ejecutables dentro de la misma sesion del manager.
         */
        std::unordered_set<std::string> initialized_plugins;

        /**
         * @brief Interna un nombre de modulo y devuelve su indice en @c modules.
         *
         * Si el modulo ya existe en @c module_map devuelve el indice existente.
         * Si no, lo agrega al vector @c modules y registra el nuevo indice en @c module_map.
         *
         * @param name Nombre del modulo nativo (p.ej. "kernel32.dll").
         * @return Indice unico del modulo en la tabla @c modules.
         */
        uint32_t intern_module(const std::string &name);

        /**
         * @brief Interna un nombre de funcion nativa y devuelve su indice en @c functions.
         *
         * Semantica identica a @c intern_module pero aplicada a la tabla @c functions.
         *
         * @param name Nombre de la funcion nativa (p.ej. "GetTickCount").
         * @return Indice unico de la funcion en la tabla @c functions.
         */
        uint32_t intern_function(const std::string &name);

        /**
         * @brief Interna una firma de funcion y devuelve su indice en @c signatures.
         *
         * Las firmas describen los tipos de argumentos y de retorno de la funcion
         * nativa; se usan para seleccionar el alias @c FnXX correcto al invocarla.
         *
         * @param sig Firma textual (p.ej. "(i32,i32)->i32").
         * @return Indice unico de la firma en la tabla @c signatures.
         */
        uint32_t intern_signature(const std::string &sig);

        /**
         * @brief Carga un modulo nativo del sistema operativo y devuelve su handle.
         *
         * En Windows usa @c LoadLibraryA(); en POSIX usa @c dlopen() con
         * RTLD_LAZY | RTLD_LOCAL.  El resultado se almacena en @c native_modules para
         * evitar cargas duplicadas en llamadas posteriores.
         *
         * @param name Nombre del modulo a cargar (p.ej. "kernel32.dll", "libm.so.6").
         * @return Handle opaco del modulo cargado, o @c nullptr si falla la carga.
         */
        void *load_native_module(const std::string &name);

        /**
         * @brief Resuelve la direccion de una funcion dentro de un modulo nativo.
         *
         * En Windows usa @c GetProcAddress(); en POSIX usa @c dlsym().
         *
         * @param module Handle opaco del modulo cargado (obtenido con @c load_native_module()).
         * @param func   Nombre de la funcion a localizar.
         * @return Puntero a la funcion nativa, o @c nullptr si no se encontro.
         */
        void *resolve_native_symbol(void *module, const std::string &func);

        /**
         * @brief Resuelve todos los simbolos nativos de la tabla de importaciones y
         *        parchea el bytecode con sus direcciones reales.
         *
         * Recorre @c imports, llama a @c load_native_module() y @c resolve_native_symbol()
         * para cada simbolo, y luego itera sobre @c patch_sites llamando a @c patch_call()
         * por cada offset.  Tras esta llamada el bytecode esta listo para ejecutarse.
         *
         * @param file_base             Puntero al inicio del buffer de bytecode en memoria.
         * @param offset_real_bytecode  Offset desde @p file_base donde comienza el codigo
         *                              ejecutable (para calcular la posicion absoluta de
         *                              cada patch-site).
         * @throws FFIError si un modulo no puede cargarse o una funcion no se encuentra.
         */
        void resolve_all(uint8_t *file_base, uint64_t offset_real_bytecode);

        /**
         * @brief Llama a @c vesta_init en todos los modulos que lo exportan.
         *
         * Recorre @c native_modules; si un modulo exporta el simbolo @c "vesta_init"
         * y aun no fue inicializado (no esta en @c initialized_plugins), lo llama
         * pasando @p api.  Registra el modulo en @c initialized_plugins para evitar
         * llamadas duplicadas en cargas sucesivas.
         *
         * Debe invocarse despues de @c resolve_all, cuando todos los modulos
         * nativos del ejecutable ya estan cargados.
         *
         * @param api Puntero a la estructura @c VestaPluginAPI con los callbacks del manager.
         */
        void call_plugin_inits(const VestaPluginAPI *api);
    };

} // namespace ffi

#endif // NATIVE_FFI_H
