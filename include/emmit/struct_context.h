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
 * @file struct_context.h
 * @brief Estructuras de datos del contexto del ensamblador Vesta.
 *
 * Define todas las estructuras que modelan el estado del ensamblador durante
 * la emision de bytecode:
 *
 *   - Type: enumeracion de tipos de relocalizacion (Relative*, Absolute*, Native_Method).
 *   - Relocation: entrada individual en la tabla de relocalizaciones.
 *   - Label: etiqueta con nombre, offset relativo y tamano.
 *   - Section: seccion con rango de memoria, etiquetas y tamano real.
 *   - Space: espacio de direcciones con secciones ordenadas.
 *   - ImportEntry: entrada de la tabla de importaciones.
 *   - Context: contexto global del ensamblador (espacios, relocalizaciones, importaciones).
 *   - LabelLookupResult: resultado de busqueda de etiqueta en el contexto.
 *   - Funciones auxiliares: align_down, align_up, size_relocation_emmit,
 *     mode_to_type_relocation_abs/rel, compute_space_size, find_label_in_context*.
 */

#ifndef STRUCT_CONTEXT_H
#define STRUCT_CONTEXT_H

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <optional>
#include <system_error>
#include <vector>

#include "emmit_decl.h"
#include "linker/velb_linker_bytecode.h"

namespace Assembly::Bytecode {

    /**
     * @brief Alinea @p value hacia abajo al multiplo de @p alignment mas proximo.
     *
     * @param value     Valor a alinear.
     * @param alignment Alineacion objetivo (debe ser potencia de 2).
     * @return Valor alineado hacia abajo.
     */
    static inline uint64_t align_down(uint64_t value, uint64_t alignment) {
        return value & ~(alignment - 1); // mascara para alinear hacia abajo
    }

    /**
     * @brief Alinea @p value hacia arriba al multiplo de @p alignment mas proximo.
     *
     * Necesario para calcular el inicio de la siguiente pagina o seccion alineada.
     *
     * @param value     Valor a alinear.
     * @param alignment Alineacion objetivo (debe ser potencia de 2, p.ej. 4096 para paginas).
     * @return Valor alineado hacia arriba.
     */
    static inline uint64_t align_up(uint64_t value, uint64_t alignment) {
        return (value + alignment - 1) & ~(alignment - 1); // redondear y alinear
    }

    /**
     * @brief Tipo de relocalizacion en el modulo objeto .velb.
     *
     * Las relocalizaciones permiten al linker o al loader parchearlo el binario
     * con las direcciones correctas tras resolver los simbolos.
     */
    typedef enum class Type {
        /**
         * @brief Desplazamiento relativo de 64 bits.
         *
         * El campo contiene: destino - (PC + tamano_del_campo).
         * Permite codigo position-independent (PIC); el loader puede mover el codigo
         * sin romper las referencias.
         *
         * Ejemplo:
         * @code
         *   PC = 0x1000, destino = 0x2000
         *   offset = 0x2000 - (0x1000 + 4) = 0xFFC  -- el campo contiene 0xFFC
         * @endcode
         */
        Relative64,

        Relative40, ///< Desplazamiento relativo de 40 bits

        /**
         * @brief Desplazamiento relativo de 32 bits.
         *
         * Es el tipo mas comun para saltos y llamadas en la practica (equivale a
         * x86 rel32, ARM64 rel26 reescalado, etc.).
         */
        Relative32,

        Relative16, ///< Desplazamiento relativo de 16 bits
        Relative8,  ///< Desplazamiento relativo de 8 bits

        /**
         * @brief Direccion absoluta de 64 bits.
         *
         * El campo contiene la direccion exacta del simbolo en el espacio de
         * direcciones final, p.ej. una direccion de funcion importada de una DLL.
         */
        Absolute64,

        Absolute40, ///< Direccion absoluta de 40 bits (para instrucciones ALU con SIB)
        Absolute32, ///< Direccion absoluta de 32 bits
        Absolute16, ///< Direccion absoluta de 16 bits
        Absolute8,  ///< Direccion absoluta de 8 bits

        /**
         * @brief Relocalizacion para metodos nativos del host.
         *
         * El campo es un indice de 8 bytes en la tabla de importaciones que el
         * loader resuelve en tiempo de carga con la direccion real del metodo.
         */
        Native_Method,

        NO_VALID ///< Valor de error o no inicializado
    } Type;

    /**
     * @brief Devuelve el numero de bytes que se deben reservar en el bytecode para una relocalizacion.
     *
     * @param type Tipo de relocalizacion.
     * @return Tamano en bytes del campo de relocalizacion (1, 2, 4, 5 u 8).
     * @throws std::runtime_error si el tipo es NO_VALID o desconocido.
     */
    static uint8_t size_relocation_emmit(Type type) {
        switch (type) {
            case Type::Relative64:
            case Type::Absolute64:
            case Type::Native_Method:  return 8;
            case Type::Relative40:
            case Type::Absolute40:     return 5;
            case Type::Relative32:
            case Type::Absolute32:     return 4;
            case Type::Relative16:
            case Type::Absolute16:     return 2;
            case Type::Relative8:
            case Type::Absolute8:      return 1;
            default:
                throw std::runtime_error("size_relocation_emmit(Type type): Unknown type");
        }
    }

    /**
     * @brief Obtiene el tipo de relocalizacion absoluta correspondiente a un campo mode de 2 bits.
     *
     * @param mode Campo mode de 2 bits (0=1B, 1=2B, 2=4B, 3=8B).
     * @return Tipo de relocalizacion absoluta; NO_VALID si el modo no es valido.
     */
    static Type mode_to_type_relocation_abs(uint8_t mode) {
        switch (mode_to_bytes(mode)) {
            case 1: return Type::Absolute8;
            case 2: return Type::Absolute16;
            case 4: return Type::Absolute32;
            case 8: return Type::Absolute64;
            default: return Type::NO_VALID;
        }
    }

    /**
     * @brief Obtiene el tipo de relocalizacion relativa correspondiente a un campo mode de 2 bits.
     *
     * @param mode Campo mode de 2 bits (0=1B, 1=2B, 2=4B, 3=8B).
     * @return Tipo de relocalizacion relativa; NO_VALID si el modo no es valido.
     */
    static Type mode_to_type_relocation_rel(uint8_t mode) {
        switch (mode_to_bytes(mode)) {
            case 1: return Type::Relative8;
            case 2: return Type::Relative16;
            case 4: return Type::Relative32;
            case 8: return Type::Relative64;
            default: return Type::NO_VALID;
        }
    }

    /**
     * @brief Entrada en la tabla de relocalizaciones del modulo objeto.
     */
    typedef struct Relocation {
        std::string symbol;     ///< Nombre del simbolo a resolver por el linker
        std::string section;    ///< Nombre de la seccion donde ocurre la relocalizacion
        uint64_t    offset = 0; ///< Offset dentro de la seccion donde parchelar
        Type        type;       ///< Tipo de relocalizacion (tamano y modo relativo/absoluto)
    } Relocation;

    /**
     * @brief Tipo de relocalizacion del tamano de puntero en la maquina actual.
     *
     * Se inicializa segun sizeof(void*): Absolute64, Absolute32, Absolute16 o Absolute8.
     */
    static Type size_ptr_in_this_machine =
            (sizeof(void *) == 8) ? Type::Absolute64 :
            (sizeof(void *) == 4) ? Type::Absolute32 :
            (sizeof(void *) == 2) ? Type::Absolute16 :
            (sizeof(void *) == 1) ? Type::Absolute8  :
                                    Type::NO_VALID;

    /**
     * @brief Etiqueta (label) con nombre, offset relativo a la seccion y tamano.
     *
     * El offset es relativo al inicio de la seccion que contiene la etiqueta,
     * no al espacio de direcciones global.
     */
    typedef struct Label {
        std::string name;       ///< Nombre de la etiqueta (identificador unico en la seccion)
        uint64_t    address{};  ///< Offset relativo al inicio de la seccion
        uint64_t    size{};     ///< Tamano del bloque de codigo/datos asociado a la etiqueta
    } Label;

    /**
     * @brief Seccion dentro de un espacio de direcciones.
     *
     * Una seccion puede contener codigo, datos u otro tipo de informacion.
     * Tiene su propio rango de memoria virtual, una tabla de etiquetas y un
     * tamano real de bytes generados.
     */
    typedef struct Section {
        uint64_t    file_offset = 0;        ///< Offset en el fichero binario (usado por el loader)
        std::string name;                   ///< Nombre de la seccion
        range_memory memory{};              ///< Rango de memoria virtual de la seccion
        uint64_t    size_real = 0;          ///< Tamano real en bytes de bytecode generado
        uint32_t    size_align_section = 0; ///< Alineacion de la seccion en bytes (p.ej. 4096)

        /**
         * @brief Tabla de etiquetas de la seccion (nombre -> Label).
         */
        std::unordered_map<std::string, Label> table_label;

        /**
         * @brief Establece el rango de memoria virtual de la seccion.
         *
         * @param init  Direccion virtual de inicio de la seccion.
         * @param final Direccion virtual de fin de la seccion.
         */
        void set_range(uint64_t init, uint64_t final) {
            memory.address_init  = init;
            memory.address_final = final;
        }

        /**
         * @brief Anade una etiqueta a la seccion con su offset y tamano.
         *
         * Las direcciones de las etiquetas son relativas al inicio de la seccion.
         *
         * @param name    Nombre de la etiqueta.
         * @param address Offset relativo al inicio de la seccion.
         * @param size    Tamano del bloque asociado a la etiqueta.
         */
        void add_label(const std::string &name, uint64_t address, uint64_t size) {
            table_label[name] = Label{name, address, size};
        }

        /**
         * @brief Busca una etiqueta por nombre en la seccion.
         *
         * @param name Nombre de la etiqueta a buscar.
         * @return Puntero a la Label si existe; nullptr si no se encuentra.
         */
        Label *get_label(const std::string &name) {
            auto it = table_label.find(name);
            if (it == table_label.end()) return nullptr;
            return &it->second;
        }

        /**
         * @brief Calcula el rango de memoria virtual de la seccion.
         *
         * Toma el @p base_address como inicio, anade size_real y aplica la
         * alineacion @p bytes_aligned para calcular el fin de la seccion.
         *
         * @param base_address Direccion base a partir de la que inicia esta seccion.
         * @param bytes_aligned Numero de bytes para la alineacion (p.ej. 4096).
         */
        void compute_range(uint64_t base_address, uint64_t bytes_aligned) {
            uint64_t start    = base_address;
            uint64_t end_real = base_address + size_real;

            uint64_t aligned_init = align_down(start, bytes_aligned);
            if (aligned_init < base_address) aligned_init = base_address;

            uint64_t aligned_end = align_up(end_real, size_align_section);

            memory.address_init  = aligned_init;
            memory.address_final = aligned_end;
        }

        /**
         * @brief Actualiza el tamano de una etiqueta existente en la seccion.
         *
         * @param name Nombre de la etiqueta a actualizar.
         * @param size Nuevo tamano en bytes.
         * @throws std::runtime_error si la etiqueta no se encuentra.
         */
        void update_label_size(const std::string &name, uint64_t size) {
            auto it = table_label.find(name);
            if (it == table_label.end()) {
                throw std::runtime_error("update_label_size: label no encontrada: " + name);
            }
            it->second.size = size;
        }
    } Section;

    /**
     * @brief Espacio de direcciones que contiene una o mas secciones.
     *
     * Un espacio de direcciones delimita un rango continuo de memoria virtual.
     * Las secciones dentro de el se ordenan por direccion de inicio y sus rangos
     * se calculan de forma contigua.
     */
    typedef struct Space {
        uint64_t     file_offset = 0;   ///< Offset en el fichero (usado por el linker)
        range_memory range;             ///< Rango de memoria virtual del espacio
        std::string  name_section;      ///< Nombre del espacio (almacenado en metadatos .velb)

        /**
         * @brief Tabla de secciones del espacio (nombre -> Section).
         */
        std::unordered_map<std::string, Section> table_section;

        /**
         * @brief Vector de punteros a secciones en orden de insercion.
         *
         * Se usa para calcular los rangos en el orden correcto de las secciones.
         */
        std::vector<Section *> ordered_sections;

        /**
         * @brief Anade una seccion al espacio y la registra en el vector ordenado.
         *
         * @param sec Seccion a anadir (se copia a la tabla interna).
         */
        void add_section(const Section &sec) {
            table_section[sec.name] = sec;
            ordered_sections.push_back(&table_section[sec.name]); // mantener orden
        }

        /**
         * @brief Crea y anade una seccion con nombre y rango de direcciones.
         *
         * @param name  Nombre de la nueva seccion.
         * @param init  Direccion virtual de inicio (relativa al espacio).
         * @param final Direccion virtual de fin (relativa al espacio).
         */
        void add_section(const std::string &name, uint64_t init, uint64_t final) {
            Section sec;
            sec.name                 = name;
            sec.memory.address_init  = init;
            sec.memory.address_final = final;
            sec.size_real            = 0;
            sec.size_align_section   = 1;
            add_section(sec);
        }

        /**
         * @brief Establece el nombre identificativo del espacio de direcciones.
         *
         * El nombre se almacena en la tabla de metadatos del ejecutable .velb.
         *
         * @param name Nombre del espacio de direcciones.
         */
        void set_name(const std::string &name) {
            name_section = name;
        }

        /**
         * @brief Libera todas las secciones del espacio.
         */
        void clear() {
            table_section.clear();
        }

        /**
         * @brief Calcula el rango de memoria de cada seccion del espacio de forma contigua.
         *
         * Ordena las secciones por direccion de inicio y llama a compute_range()
         * sobre cada una usando la direccion final de la anterior como base.
         *
         * @param bytes_aligned Alineacion en bytes para cada seccion (p.ej. 4096).
         */
        void compute_sections_ranges(uint64_t bytes_aligned) {
            // ordenar secciones por direccion de inicio para computar en orden
            std::sort(ordered_sections.begin(), ordered_sections.end(),
                      [](const Section *a, const Section *b) {
                          return a->memory.address_init < b->memory.address_init;
                      });

            uint64_t start = range.address_init; // base del espacio de direcciones
            for (Section *sec: ordered_sections) {
                sec->compute_range(start, bytes_aligned);
                start += sec->size_real; // la siguiente seccion empieza tras el final real
            }
        }
    } Space;

    /**
     * @brief Calcula el tamano real total de un espacio de direcciones.
     *
     * Suma los tamanos reales de todas sus secciones.
     *
     * @param s Espacio de direcciones a medir.
     * @return Tamano real total en bytes.
     */
    static uint64_t compute_space_size(const Space &s) {
        uint64_t total = 0;
        for (auto &[name, sec]: s.table_section) {
            total += sec.size_real; // acumular tamano de cada seccion
        }
        return total;
    }

    /**
     * @brief Entrada del sistema de modulos del ensamblador.
     *
     * Representa un modulo declarado con la anotacion \@Module.  Agrupa los
     * simbolos que ese modulo exporta (\@Export) e importa (\@Import) para que
     * el emitter aplique el prefijo de modulo y el linker verifique visibilidad.
     */
    struct ModuleEntry {
        std::string qualified_name;            ///< nombre calificado: "com.vesta.collections"
        std::vector<std::string> exports;      ///< simbolos publicos del modulo
        std::vector<std::string> imports;      ///< modulos de los que depende este modulo
    };

    /**
     * @brief Entrada en la tabla de importaciones del contexto del ensamblador.
     *
     * Cada entrada representa un simbolo externo que el loader debe resolver
     * en tiempo de carga consultando la biblioteca indicada.
     */
    struct ImportEntry {
        std::string library;   ///< Nombre de la biblioteca (p.ej. "kernel32.dll")
        std::string function;  ///< Nombre del simbolo importado (p.ej. "GetTickCount")
        uint32_t    index;     ///< Indice en la tabla de importaciones del fichero objeto
    };

    /**
     * @brief Contexto global del ensamblador durante la emision de bytecode.
     *
     * Agrupa todos los datos necesarios para un proceso de ensamblado completo:
     *   - Espacios de direcciones con sus secciones y etiquetas.
     *   - Tabla de relocalizaciones para el linker.
     *   - Tabla de importaciones para el loader dinamico.
     *   - Formato de salida y alineacion de secciones.
     */
    typedef struct Context {
        /**
         * @brief Punto de inicio de ejecucion (valor del PC al cargar el modulo).
         *
         * Se define con la anotacion \@InitPc.  Si no se define, la VM comienza
         * en la direccion 0 del espacio de memoria virtual.
         */
        uint64_t start_pc{};

        /**
         * @brief Tabla de importaciones (vector de ImportEntry).
         *
         * Usar import_lookup para busquedas O(1) por clave "lib:funcion".
         */
        std::vector<ImportEntry> import_table;

        /**
         * @brief Indice rapido de importaciones: "lib:funcion" -> indice en import_table.
         *
         * Permite verificar en O(1) si un simbolo ya esta importado antes de anadirlo.
         */
        std::unordered_map<std::string, uint64_t> import_lookup;

        /**
         * @brief Tabla de espacios de direcciones (nombre -> Space).
         */
        std::unordered_map<std::string, Space> space_address;

        /**
         * @brief Tabla de relocalizaciones del modulo (todas las entradas en orden).
         */
        std::vector<Relocation> relocations;

        /**
         * @brief Indice de relocalizaciones por simbolo: nombre -> lista de indices en relocations.
         *
         * Permite encontrar rapidamente todas las referencias a un simbolo dado.
         */
        std::unordered_map<std::string, std::vector<size_t> > reloc_by_symbol;

        /**
         * @brief Anade una relocalizacion a la tabla y la indexa por simbolo.
         *
         * La relocalizacion puede resolverse en el linker (si el simbolo es local)
         * o en el loader en tiempo de ejecucion (si es externo).
         *
         * @param rel Relocalizacion a registrar.
         */
        void add_relocation(const Relocation &rel) {
            size_t index = relocations.size(); // indice de la nueva entrada
            relocations.push_back(rel);        // anadir al vector principal
            reloc_by_symbol[rel.symbol].push_back(index); // indexar por simbolo
        }

        /**
         * @brief Devuelve todos los punteros a las relocalizaciones de un simbolo.
         *
         * @param symbol Nombre del simbolo cuyas relocalizaciones se buscan.
         * @return Vector de punteros constantes a Relocation; vacio si no hay entradas.
         */
        std::vector<const Relocation *> get_relocations_for(const std::string &symbol) const {
            std::vector<const Relocation *> result;
            auto it = reloc_by_symbol.find(symbol);
            if (it == reloc_by_symbol.end()) return result;
            for (size_t idx: it->second) {
                result.push_back(&relocations[idx]);
            }
            return result;
        }

        /**
         * @brief Limpia las tablas de relocalizaciones (vector e indice).
         */
        void clear_relocations() {
            relocations.clear();
            reloc_by_symbol.clear();
        }

        /**
         * @brief Indica si el simbolo indicado tiene alguna relocalizacion registrada.
         *
         * @param symbol Nombre del simbolo a consultar.
         * @return true si existe al menos una relocalizacion para el simbolo.
         */
        bool has_relocations(const std::string &symbol) const {
            return reloc_by_symbol.count(symbol) > 0;
        }

        /**
         * @brief Formato de salida del bytecode generado.
         *
         * Por defecto "raw" (codigo plano sin cabeceras).  Se puede cambiar
         * con la anotacion \@Format; solo la ultima definicion surte efecto.
         */
        std::string format_output = "raw";

        /**
         * @brief Alineacion en bytes de cada seccion del binario.
         *
         * Para el formato .velb debe ser 4096 (tamano de pagina).
         * El valor por defecto 1 significa sin alineacion.
         */
        uint32_t bytes_aligned = 0x1;

        /**
         * @brief Crea un nuevo espacio de direcciones y lo anade al contexto.
         *
         * @param name  Nombre identificativo del espacio.
         * @param init  Direccion virtual de inicio del espacio.
         * @param final Direccion virtual de fin del espacio.
         */
        void add_space(const std::string &name, uint64_t init, uint64_t final) {
            Space sp;
            sp.range.address_init  = init;
            sp.range.address_final = final;
            sp.set_name(name);
            space_address[name] = sp;
        }

        /**
         * @brief Busca un espacio de direcciones por nombre.
         *
         * @param name Nombre del espacio a buscar.
         * @return Puntero al Space si existe; nullptr si no se encuentra.
         */
        Space *get_space(const std::string &name) {
            auto it = space_address.find(name);
            if (it != space_address.end()) return &it->second;
            return nullptr;
        }

        // --- sistema de modulos ---

        /**
         * @brief Tabla de modulos declarados con \@Module en el fichero fuente.
         *
         * Clave: nombre calificado del modulo ("com.vesta.collections").
         * Valor: ModuleEntry con sus listas de exports e imports.
         */
        std::unordered_map<std::string, ModuleEntry> modules;

        /**
         * @brief Nombre del modulo activo durante la emision de bytecode.
         *
         * Se establece al procesar la anotacion \@Module.  El emitter lo usa
         * para prefijar los nombres de clases y simbolos globales.
         * Cadena vacia significa que no se ha declarado modulo.
         */
        std::string current_module;

        // --- genericos (monomorphization) ---

        /**
         * @brief Mapa de instanciaciones genericas ya generadas.
         *
         * Clave: firma de la instanciacion ("List<int>", "Map<String,int>").
         * Valor: puntero opaco a la ClassInfo de la especializacion ya emitida.
         * Evita generar dos veces el mismo codigo para la misma instanciacion.
         */
        std::unordered_map<std::string, void*> generic_instances;

        /**
         * @brief Libera todos los espacios de direcciones del contexto.
         */
        void clear() {
            for (auto &kv: space_address) kv.second.clear();
            space_address.clear();
        }

        /**
         * @brief Busca una seccion por nombre en todos los espacios de direcciones.
         *
         * @param name Nombre de la seccion a buscar.
         * @return Puntero a la Section si existe; nullptr si no se encuentra.
         */
        Section *get_section(const std::string &name) {
            for (auto &[spaceName, space]: space_address) {
                auto it = space.table_section.find(name);
                if (it != space.table_section.end()) return &it->second;
            }
            return nullptr;
        }

        /**
         * @brief Calcula los rangos de memoria de todas las secciones en todos los espacios.
         *
         * Llama a Space::compute_sections_ranges() para cada espacio usando bytes_aligned.
         */
        void compute_all_ranges() {
            for (auto &[name, sp]: space_address) {
                sp.compute_sections_ranges(bytes_aligned);
            }
        }

        // === Tabla de debug (modulo-local) ===
        // Pares (byte_offset_dentro_del_modulo, source_line) acumulados
        // por el Assembler al emitir cada Instruccion cuyo source_line > 0
        // (i.e., compiladas con --vex-debug).  El linker los consume al
        // ensamblar el .velb final, sumando module_base_offset al
        // byte_offset para producir entradas DebugLineEntry con offsets
        // absolutos dentro del .velb.
        struct DebugLineRec {
            uint32_t byte_offset; ///< offset dentro del bytecode del modulo
            uint32_t source_line; ///< linea fuente Vex (1-based)
        };
        std::vector<DebugLineRec> debug_lines;

        // Nombre del archivo fuente Vex que origino este modulo.  Se usa
        // como `file_offset` (resuelto al strings blob) en la seccion
        // debug del .velb.  Si vacio, el linker no escribe info de file.
        std::string debug_source_file;
    } Context;

    /**
     * @brief Resultado de una busqueda de etiqueta en el contexto del ensamblador.
     *
     * Contiene punteros directos a la Label, la Section y el Space encontrados,
     * ademas de la direccion absoluta calculada si los rangos estan disponibles.
     */
    struct LabelLookupResult {
        Label *                 label   = nullptr; ///< Puntero a la etiqueta encontrada
        Section *               section = nullptr; ///< Seccion que contiene la etiqueta
        Space *                 space   = nullptr; ///< Espacio que contiene la seccion
        std::string             space_name;        ///< Nombre del espacio (clave del mapa)
        std::string             section_name;      ///< Nombre de la seccion (clave del mapa)
        std::optional<uint64_t> absolute_address;  ///< Direccion absoluta calculada si disponible

        /**
         * @brief Indica si la busqueda encontro la etiqueta.
         *
         * @return true si label != nullptr.
         */
        [[nodiscard]] bool found() const { return label != nullptr; }
    };

    /**
     * @brief Busca una etiqueta por nombre en todos los espacios del contexto.
     *
     * Recorre todos los espacios y sus secciones en orden de iteracion del mapa
     * (no determinista) y devuelve el primer resultado encontrado.
     *
     * Complejidad: O(num_secciones + num_etiquetas) en el peor caso.
     *
     * @code{.cpp}
     * auto res = find_label_in_context(ctx, "start");
     * if (res.found()) {
     *     if (res.absolute_address) std::cout << "Addr: " << *res.absolute_address << "\n";
     * }
     * @endcode
     *
     * @param ctx        Contexto donde buscar.
     * @param label_name Nombre de la etiqueta.
     * @return LabelLookupResult con los punteros y direccion; found() == false si no existe.
     */
    static LabelLookupResult find_label_in_context(Context &ctx, const std::string &label_name) {
        LabelLookupResult out;
        for (auto &space_kv: ctx.space_address) {
            Space &            space      = space_kv.second;
            const std::string &space_name = space_kv.first;
            for (auto &sec_kv: space.table_section) {
                Section &          sec          = sec_kv.second;
                const std::string &section_name = sec_kv.first;
                auto               it           = sec.table_label.find(label_name);
                if (it != sec.table_label.end()) {
                    out.label        = &it->second;
                    out.section      = &sec;
                    out.space        = &space;
                    out.space_name   = space_name;
                    out.section_name = section_name;
                    // calcular direccion absoluta si los rangos estan definidos
                    if (space.range.address_init != 0 || sec.memory.address_init != 0) {
                        uint64_t abs = space.range.address_init
                                     + sec.memory.address_init
                                     + it->second.address;
                        out.absolute_address = abs;
                    }
                    return out;
                }
            }
        }
        return out; // not found
    }

    /**
     * @brief Version const de find_label_in_context (no modifica el contexto).
     *
     * @param ctx        Contexto donde buscar (const).
     * @param label_name Nombre de la etiqueta.
     * @return LabelLookupResult con los punteros; found() == false si no existe.
     */
    static LabelLookupResult find_label_in_context_const(const Context &ctx,
                                                          const std::string &label_name) {
        LabelLookupResult out;
        for (const auto &space_kv: ctx.space_address) {
            const Space &      space      = space_kv.second;
            const std::string &space_name = space_kv.first;
            for (const auto &sec_kv: space.table_section) {
                const Section &    sec          = sec_kv.second;
                const std::string &section_name = sec_kv.first;
                auto               it           = sec.table_label.find(label_name);
                if (it != sec.table_label.end()) {
                    out.label        = const_cast<Label *>(&it->second);
                    out.section      = const_cast<Section *>(&sec);
                    out.space        = const_cast<Space *>(&space);
                    out.space_name   = space_name;
                    out.section_name = section_name;
                    uint64_t abs     = space.range.address_init
                                     + sec.memory.address_init
                                     + it->second.address;
                    out.absolute_address = abs;
                    return out;
                }
            }
        }
        return out;
    }

    /**
     * @brief Busca una etiqueta por nombre dentro de un espacio de direcciones concreto.
     *
     * @param ctx        Contexto donde buscar.
     * @param space_name Nombre del espacio donde se limita la busqueda.
     * @param label_name Nombre de la etiqueta.
     * @return LabelLookupResult con los punteros; found() == false si no existe.
     */
    static LabelLookupResult find_label_in_space(Context &ctx,
                                                  const std::string &space_name,
                                                  const std::string &label_name) {
        LabelLookupResult out;
        auto it_space = ctx.space_address.find(space_name);
        if (it_space == ctx.space_address.end()) return out; // espacio no encontrado

        Space &space = it_space->second;
        for (auto &sec_kv: space.table_section) {
            Section &          sec          = sec_kv.second;
            const std::string &section_name = sec_kv.first;
            auto               it           = sec.table_label.find(label_name);
            if (it != sec.table_label.end()) {
                out.label        = &it->second;
                out.section      = &sec;
                out.space        = &space;
                out.space_name   = space_name;
                out.section_name = section_name;
                uint64_t abs     = space.range.address_init
                                 + sec.memory.address_init
                                 + it->second.address;
                out.absolute_address = abs;
                return out;
            }
        }
        return out;
    }

} // namespace Assembly::Bytecode

#endif // STRUCT_CONTEXT_H
