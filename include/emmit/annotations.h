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
 * @file annotations.h
 * @brief Manejadores de anotaciones del ensamblador Vesta.
 *
 * Las anotaciones son directivas con prefijo '@' en el codigo fuente Vesta que
 * controlan el ensamblado: definen espacios de direcciones, secciones, formato de
 * salida, punto de inicio, importaciones y relocalizaciones.
 *
 * Define:
 *   - AnnotationHandler: tipo alias de la funcion manejadora de cada anotacion.
 *   - apply_*(): funciones que procesan cada tipo de anotacion concreta.
 *   - annotation_handlers: tabla de despacho nombre -> manejador.
 *   - annotation_allow_doc: conjunto de anotaciones permitidas en bloques de documentacion.
 *   - print_context(): imprime el contexto del ensamblador en stdout.
 *   - print_context_with_bytes(): imprime contexto mas volcado de bytes de cada seccion.
 */

#ifndef ANNOTATIONS_H
#define ANNOTATIONS_H

#include <cstring>
#include <functional>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <iomanip>
#include <iostream>

#include "assembly/assembly.h"
#include "linker/velb_linker_bytecode.h"
#include "parser/parser.h"
#include "emmit/struct_context.h"

namespace vm {
    struct AnnotationNode; ///< Declaracion adelantada del nodo de anotacion del AST
}

namespace Assembly::Bytecode {

    class Assembler; ///< Declaracion adelantada del ensamblador

    /**
     * @brief Tipo de funcion manejadora de una anotacion.
     *
     * Cada manejador recibe el nodo de anotacion del AST y el contexto del ensamblador,
     * y aplica el efecto de la anotacion sobre el contexto (por ejemplo, crear una seccion
     * o registrar un punto de inicio).
     */
    using AnnotationHandler = std::function<void(const vm::AnnotationNode *, Assembler &)>;

    /**
     * @brief Procesa la anotacion \@SpaceAddress para definir un espacio de direcciones.
     *
     * Un espacio de direcciones delimita un rango de memoria virtual donde se ubicaran
     * una o mas secciones.  Esta anotacion inicializa la entrada correspondiente en
     * Context::space_address.
     *
     * @param node Nodo AST de la anotacion con los argumentos (nombre, rango).
     * @param ctx  Contexto global del ensamblador que se modifica.
     */
    void apply_space_address(const vm::AnnotationNode *node, Assembler &ctx);

    /**
     * @brief Procesa la anotacion \@Section para crear una seccion dentro de un espacio.
     *
     * Las secciones pueden contener codigo, datos u otro tipo de informacion.
     * Se asocian al espacio de direcciones activo y tienen su propio rango de memoria
     * y tabla de etiquetas.
     *
     * @param node Nodo AST de la anotacion con los argumentos (nombre, espacio, alineacion).
     * @param ctx  Contexto global del ensamblador que se modifica.
     */
    void apply_section(const vm::AnnotationNode *node, Assembler &ctx);

    /**
     * @brief Procesa la anotacion \@Format para indicar el formato de salida del binario.
     *
     * Si no se define el formato, el codigo generado sera plano (sin cabeceras ni secciones).
     * Con formato definido (p.ej. "velb") el emision incluye las cabeceras y tablas del
     * formato .velb del linker.
     *
     * @param node Nodo AST de la anotacion con el nombre del formato.
     * @param ctx  Contexto global del ensamblador que se modifica.
     */
    void apply_format(const vm::AnnotationNode *node, Assembler &ctx);

    /**
     * @brief Procesa la anotacion \@InitPc para establecer el punto de inicio de ejecucion.
     *
     * Acepta una etiqueta o una direccion cruda.  Si se reciben multiples \@InitPc,
     * el ultimo definido prevalece.  Si no se define, la ejecucion comienza en 0x000000.
     *
     * Ejemplo de uso con etiqueta:
     * @code
     *   code:
     *       @InitPc(code)
     *       adds r0, 3
     * @endcode
     *
     * @param node      Nodo AST de la anotacion con la etiqueta o direccion.
     * @param assembler Contexto global del ensamblador que se modifica.
     */
    void apply_init_pc(const vm::AnnotationNode *node, Assembler &assembler);

    /**
     * @brief Procesa la anotacion \@Import para registrar una importacion en el linker.
     *
     * Anade el simbolo importado a la tabla de importaciones del fichero objeto,
     * permitiendo al linker resolver la referencia al enlazar multiples objetos.
     *
     * @param node      Nodo AST de la anotacion con el nombre del simbolo importado.
     * @param assembler Contexto global del ensamblador que se modifica.
     */
    void apply_import(const vm::AnnotationNode *node, Assembler &assembler);

    /**
     * @brief Procesa la anotacion \@Relative para indicar una relocalizacion relativa.
     *
     * Calcula la direccion relativa de una etiqueta respecto a la instruccion que usa
     * la anotacion, lo que permite generar codigo PIC (Position Independent Code).
     * La anotacion no se aplica directamente; el emisor la usa para registrar la
     * relocalizacion en la tabla correspondiente.
     *
     * @param node      Nodo AST de la anotacion con la etiqueta referenciada.
     * @param assembler Contexto global del ensamblador que se modifica.
     */
    void apply_relative(const vm::AnnotationNode *node, Assembler &assembler);

    /**
     * @brief Procesa la anotacion \@Absolute para indicar una relocalizacion absoluta.
     *
     * Calcula la direccion absoluta de una etiqueta.  La anotacion no se aplica
     * directamente; el emisor la usa para registrar la relocalizacion en la tabla.
     *
     * @param node      Nodo AST de la anotacion con la etiqueta referenciada.
     * @param assembler Contexto global del ensamblador que se modifica.
     */
    void apply_absolute(const vm::AnnotationNode *node, Assembler &assembler);

    /**
     * @brief Procesa la anotacion \@Module para declarar el modulo del fichero fuente.
     *
     * Registra el nombre calificado del modulo ("com.vesta.core") en el contexto del
     * ensamblador y lo convierte en el modulo activo para los simbolos que se emitan
     * a continuacion.  Solo debe aparecer una vez por fichero fuente.
     *
     * Ejemplo:
     * @code
     *   @Module(com.vesta.collections)
     * @endcode
     *
     * @param node      Nodo AST de la anotacion; node->value contiene el nombre del modulo.
     * @param assembler Contexto global del ensamblador que se modifica.
     */
    void apply_module(const vm::AnnotationNode *node, Assembler &assembler);

    /**
     * @brief Procesa la anotacion \@Export para marcar un simbolo como publico del modulo.
     *
     * Anade el simbolo al listado de exports del modulo activo en el contexto.
     * Solo los simbolos exportados son accesibles desde otros modulos.
     *
     * Ejemplo:
     * @code
     *   @Module(com.vesta.collections)
     *   @Export(List)
     *   @Export(Map)
     * @endcode
     *
     * @param node      Nodo AST de la anotacion; node->value contiene el nombre del simbolo.
     * @param assembler Contexto global del ensamblador que se modifica.
     */
    void apply_export(const vm::AnnotationNode *node, Assembler &assembler);

    /**
     * @brief Procesa la anotacion \@Generic para declarar parametros de tipo en el emitter.
     *
     * Registra los parametros de tipo de la clase o metodo que sigue, separados por comas
     * en el valor de la anotacion.  El emitter usa esta informacion durante la fase de
     * monomorphization para generar especializaciones.
     *
     * Ejemplo:
     * @code
     *   @Generic(T)
     *   @Generic(K,V)
     * @endcode
     *
     * @param node      Nodo AST de la anotacion; node->value contiene los parametros ("T" o "K,V").
     * @param assembler Contexto global del ensamblador que se modifica.
     */
    void apply_generic(const vm::AnnotationNode *node, Assembler &assembler);

    /**
     * @brief Tabla de despacho de anotaciones: nombre -> funcion manejadora.
     *
     * Mapea el nombre de cada anotacion reconocida por el ensamblador a su funcion
     * de procesamiento correspondiente.  Las entradas con lambdas vacias estan
     * reservadas para futura implementacion.
     */
    static std::unordered_map<std::string, AnnotationHandler> annotation_handlers = {
        { "SpaceAddress", apply_space_address }, ///< Define un espacio de direcciones
        { "Format",       apply_format        }, ///< Establece el formato de salida
        { "Section",      apply_section       }, ///< Crea una seccion dentro de un espacio
        { "InitPc",       apply_init_pc       }, ///< Establece el punto de entrada
        { "IniAddress",   [](const vm::AnnotationNode *a, Assembler &ctx) { /* pendiente */ } },
        { "EndAddress",   [](const vm::AnnotationNode *a, Assembler &ctx) { /* pendiente */ } },
        { "Name",         [](const vm::AnnotationNode *a, Assembler &ctx) { /* pendiente */ } },
        { "Import",       apply_import        }, ///< Registra una importacion para el linker
        { "Relative",     apply_relative      }, ///< Relocalizacion relativa (PIC)
        { "Absolute",     apply_absolute      }, ///< Relocalizacion absoluta
        { "Module",       apply_module        }, ///< Declara el modulo del fichero fuente
        { "Export",       apply_export        }, ///< Marca un simbolo como publico del modulo
        { "Generic",      apply_generic       }, ///< Declara parametros de tipo (monomorphization)
    };

    /**
     * @brief Conjunto de nombres de anotacion permitidos en bloques de documentacion.
     *
     * Las anotaciones de documentacion describen clases, metodos, campos, etc. y
     * no generan bytecode; solo enriquecen los metadatos del fichero objeto.
     */
    static std::unordered_set<std::string> annotation_allow_doc = {
        "Args",
        "Class",
        "Method",
        "Description",
        "Access",
        "Extends",
        "Interface",
        "Field",
        "Name",
        "Offset",
        "Type",
        "Size",
        "Warning"
    };

    /**
     * @brief Imprime el contexto del ensamblador en stdout.
     *
     * Muestra el formato de salida, los espacios de direcciones, las secciones
     * con sus rangos y las etiquetas definidas en cada seccion.
     *
     * @param ctx Contexto del ensamblador a mostrar.
     */
    static void print_context(const Context &ctx) {
        std::cout << "=== CONTEXT ===\n";
        std::cout << "Formato de salida: " << ctx.format_output << "\n\n";

        for (const auto &[spaceName, space]: ctx.space_address) {
            std::cout << "== Space: " << spaceName << " ==\n";
            std::cout << "  Rango: [0x"
                    << std::hex << space.range.address_init
                    << ", 0x" << space.range.address_final << "]\n";

            // imprimir nombre de la seccion (hasta 16 bytes, terminado en nulo)
            std::cout << "  Nombre seccion (16 bytes): ";
            for (int i = 0; i < 16; i++) {
                if (space.name_section[i] == 0) break;
                std::cout << space.name_section[i];
            }
            std::cout << "\n";

            // recorrer secciones del espacio
            for (const auto &[secName, sec]: space.table_section) {
                std::cout << "  -- Section: " << secName << " Alineacion: 0x" << sec.size_align_section << " --\n";
                std::cout << "     Rango: [0x"
                        << std::hex << sec.memory.address_init
                        << ", 0x" << sec.memory.address_final << "]\n";

                // mostrar etiquetas de la seccion
                if (sec.table_label.empty()) {
                    std::cout << "     (sin labels)\n";
                } else {
                    std::cout << "     Labels:\n";
                    for (const auto &[labelName, label]: sec.table_label) {
                        std::cout << "       * " << labelName
                                << " @ offset 0x" << std::hex << label.address << "\n";
                    }
                }
            }

            std::cout << "\n";
        }
    }

    /**
     * @brief Imprime el contexto del ensamblador junto con el volcado de bytes de cada seccion.
     *
     * Ademas de la informacion mostrada por print_context(), muestra los bytes reales
     * de cada seccion extraidos del vector @p bytes, con formato de 16 bytes por linea
     * y offset en hexadecimal.
     *
     * @param ctx   Contexto del ensamblador a mostrar.
     * @param bytes Vector con los bytes del binario generado.
     */
    static void print_context_with_bytes(const Context &             ctx,
                                         const std::vector<uint8_t> &bytes) {
        std::cout << "=== CONTEXT + BYTES ===\n";
        std::cout << "Formato de salida: " << ctx.format_output << "\n\n";

        for (const auto &[spaceName, space]: ctx.space_address) {
            std::cout << "== Space: " << spaceName << " ==\n";
            std::cout << "  Rango: [0x"
                    << std::hex << space.range.address_init
                    << ", 0x" << space.range.address_final << "]\n";

            // imprimir nombre de la seccion (hasta 16 bytes, terminado en nulo)
            std::cout << "  Nombre seccion (16 bytes): ";
            for (int i = 0; i < 16; i++) {
                if (space.name_section[i] == 0) break;
                std::cout << space.name_section[i];
            }
            std::cout << "\n";

            // recorrer secciones del espacio
            for (const auto &[secName, sec]: space.table_section) {
                std::cout << "  -- Section: " << secName
                        << " (align 0x" << std::hex << sec.size_align_section << ") --\n";

                std::cout << "     Rango virtual: [0x"
                        << std::hex << sec.memory.address_init
                        << ", 0x" << sec.memory.address_final << "]\n";

                // mostrar etiquetas de la seccion
                if (sec.table_label.empty()) {
                    std::cout << "     (sin labels)\n";
                } else {
                    std::cout << "     Labels:\n";
                    for (const auto &[labelName, label]: sec.table_label) {
                        std::cout << "       * " << labelName
                                << " @ offset 0x" << std::hex << label.address << "\n";
                    }
                }

                // calcular rango de bytes de la seccion dentro del binario
                std::cout << "     Bytes:\n";
                uint64_t start = sec.memory.address_init  - space.range.address_init;
                uint64_t end   = sec.memory.address_final - space.range.address_init;

                if (start >= bytes.size()) {
                    std::cout << "       (fuera del rango del binario)\n";
                    continue;
                }

                end = std::min<uint64_t>(end, bytes.size());

                const size_t BYTES_PER_LINE = 16; // bytes por fila en el volcado

                for (uint64_t i = start; i < end; i++) {
                    if ((i - start) % BYTES_PER_LINE == 0) {
                        // imprimir offset de fila al inicio de cada linea
                        std::cout << "\n       "
                                << std::setw(8) << std::setfill('0') << std::hex << i
                                << ": ";
                    }
                    std::cout << std::setw(2) << std::setfill('0')
                            << std::hex << (int) bytes[i] << " ";
                }

                std::cout << "\n\n";
            }

            std::cout << "\n";
        }
    }

} // namespace Assembly::Bytecode

#endif // ANNOTATIONS_H
