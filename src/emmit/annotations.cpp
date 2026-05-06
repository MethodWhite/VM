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
 * @file annotations.cpp
 * @brief Implementacion de los manejadores de anotaciones del ensamblador de VestaVM.
 *
 * Implementa las funciones @c apply_space_address(), @c apply_section(),
 * @c apply_format(), @c apply_init_pc(), @c apply_import() y otras funciones
 * del mapa @c annotation_handlers.  Tambien implementa @c print_context() y
 * @c print_context_with_bytes() para depuracion del contexto del ensamblador.
 */
#include "emmit/annotations.h"

#include "cli/sync_io.h"
#include "emmit/parser_to_bytecode.h"


namespace Assembly::Bytecode {
    const vm::AnnotationNode *find_child_raw(
        const vm::AnnotationNode *node,
        const std::string &       key
    ) {
        for (const auto &child: node->children) {
            if (child->key == key)
                return child.get();
        }
        return nullptr;
    }


    /**
     * Funcion auxiliar para buscar un hijo por nombre. No es obligatorio que exista
     * @param node Nodo padre
     * @param key nombre del nodo hijo
     * @return nodo hijo que coindice
     */
    const vm::AnnotationNode *optional_find_child(
        const vm::AnnotationNode *node,
        const std::string &       key
    ) {
        return find_child_raw(node, key);
    }

    /**
     * Funcion auxiliar para buscar un hijo por nombre, este debe existir o genera
     * una excepcion.
     * @param node Nodo padre
     * @param key nombre del nodo hijo
     * @return nodo hijo que coindice
     */
    const vm::AnnotationNode *expect_find_child(
        const vm::AnnotationNode *node,
        const std::string &       key
    ) {
        if (auto *found = find_child_raw(node, key))
            return found;

        throw std::runtime_error(
            "La anotacion '" + node->key +
            "' requiere el operando '" + key + "' pero no fue encontrado"
        );
    }

    void apply_import(const vm::AnnotationNode *node, Assembler &assembler) {
        // una notacion Import puede tener N notaciones Method
        for (uint32_t i = 0; i < node->children.size(); ++i) {
            // obtenemos el nodo hijo, un Method
            auto &method_node = node->children[i];
            if (method_node.get()->key != "Method") {
                vesta::scout() << "En la notacion Import, se encontro una notacion que no es un Method: " <<
                        node->value << std::endl;
                exit(EXIT_FAILURE);
            }

            if (method_node->children.size() != 2) {
                vesta::scout() << "En la notacion Method no se encontro una notacion Lib y una notacion Name: " <<
                        node->value << std::endl;
                exit(EXIT_FAILURE);
            }
            auto lib_name_node = expect_find_child(method_node.get(), "Lib");  // nombre de la lib/dll
            auto func_name     = expect_find_child(method_node.get(), "Name"); // nombre del metodo


            // clave unica para evitar duplicados
            std::string key = lib_name_node->value + ":" + func_name->value;

            // ?ya existe?
            auto it = assembler.ctx.import_lookup.find(key);
            if (it != assembler.ctx.import_lookup.end()) {
                continue; // ya esta registrado
            }

            // crear nueva entrada
            ImportEntry entry;
            entry.library  = lib_name_node->value;
            entry.function = func_name->value;
            entry.index    = assembler.ctx.import_table.size();

            assembler.ctx.import_lookup[key] = entry.index;
            assembler.ctx.import_table.push_back(entry);
        }
    }

    void apply_space_address(const vm::AnnotationNode *node, Assembler &assembler) {
        // Buscar los operandos por nombre
        const vm::AnnotationNode *nameNode =
                expect_find_child(node, "Name");

        const vm::AnnotationNode *iniNode =
                expect_find_child(node, "IniAddress");

        const vm::AnnotationNode *endNode =
                expect_find_child(node, "EndAddress");

        // Extraer valores
        std::string string_space = nameNode->value;
        uint64_t    IniAddress   = vm::parse_number(iniNode->value);
        uint64_t    EndAddress   = vm::parse_number(endNode->value);

        // indicar el final del nombre en el caracter 16
        //string_space[15] = 0;

        // Crear el espacio
        assembler.ctx.add_space(string_space, IniAddress, EndAddress);
    }


    void apply_init_pc(const vm::AnnotationNode *node, Assembler &assembler) {
        std::optional<uint64_t> n = vm::parse_number_safe(node->value);
        if (n != std::nullopt) {
            // si es un numero valido
            assembler.ctx.start_pc = n.value();
            return;
        }

        if (assembler.current_section == nullptr) {
            vesta::scout() << "No se a definido ninguna seccion y por tanto no se puede encontrar la label: " << node->
                    value << std::endl;
            exit(EXIT_FAILURE);
        }

        /**
         * Buscar la label, en teoria, no deberia haber labels con el mismo nombre en todo_
         * el codigo. Buscamos en todos los espacios de direcciones, en todas las seccines.
         */
        auto res2 = find_label_in_context(assembler.ctx, node->value);
        if (res2.found()) {
            // Para poder indicar el PC desde una label, es necesario conocer en que seccion se encuentra la label, y
            // en que espacio de direcciones, se encuentra la seccion.
            // una vez se conoce estos 3 datos, el calculo es la suma de la direccion inicial del espacio de direcciones,
            // mas el offset de la seccion dentro de ese espacio de direcciones mas el offset del label dentro de la
            // seccion
            assembler.ctx.start_pc =
                    res2.space->range.address_init +
                    res2.section->memory.address_init +
                    res2.label->address;
            return;
        }

        vesta::scout() << "No se pudo encontrar la label: " << node->value <<
                ", no fue definida o se declaro despues de la notacion." << std::endl;
        exit(EXIT_FAILURE);
    }

    void apply_section(const vm::AnnotationNode *node, Assembler &assembler) {
        // Obtener el nombre de la seccion
        const vm::AnnotationNode *nameNode     = expect_find_child(node, "Name");
        std::string               section_name = nameNode->value;

        // Obtener el SpaceAddress asociado
        const vm::AnnotationNode *spaceNode  = expect_find_child(node, "SpaceAddress");
        std::string               space_name = spaceNode->value;

        // miramos si el usuario introdujo un nodo de tipo alineacion
        const vm::AnnotationNode *alignNode = optional_find_child(node, "Align");

        // Buscar el espacio en el contexto
        Space *sp = assembler.ctx.get_space(space_name);
        if (!sp) {
            throw std::runtime_error(
                "El espacio '" + space_name +
                "' no existe, pero fue referenciado por la seccion '" +
                section_name + "'"
            );
        }

        // Crear la seccion
        Section sec;
        sec.name = section_name;

        // la alineacion para esta seccion sera la alineacion configurada globalmente
        // si no se definio una notacion Align dentro de la notacion de seccion.
        if (alignNode == nullptr) {
            sec.size_align_section = assembler.ctx.bytes_aligned;
        } else sec.size_align_section = vm::parse_number(alignNode->value);

        sec.memory.address_final = 0;
        sec.memory.address_init  = 0;
        //  hereda el rango del espacio
        //sec.memory = sp->range;

        // Insertarla en el espacio
        sp->add_section(sec);

        // indicar al ensamblador la seccion nueva, usarla
        assembler.current_section = assembler.ctx.get_section(sec.name);
    }

    void apply_format(const vm::AnnotationNode *node, Assembler &ctx) {
        if (node->value.empty()) {
            throw std::runtime_error(
                "La anotacion '" + node->key +
                "' requiere que se defina un formato a usar, pero no se definio ninguno"
            );
        }

        // cambiamos el formato raw al definido por el usuario.
        ctx.ctx.format_output = node->value;
    }

    // no debe hacer nada al aplicarse desde el analizador, se debe aplicar
    // en el emisor
    void apply_relative(const vm::AnnotationNode *node, Assembler &assembler) {}

    // no debe hacer nada al aplicarse desde el analizador, se debe aplicar
    // en el emisor
    void apply_absolute(const vm::AnnotationNode *node, Assembler &assembler) {}

    /**
     * @brief Implementa \@Module(nombre): establece el modulo activo del fichero fuente.
     *
     * Crea una entrada en el mapa de modulos del contexto si aun no existe y
     * actualiza current_module con el nombre calificado recibido.
     */
    void apply_module(const vm::AnnotationNode *node, Assembler &assembler) {
        if (node->value.empty()) {
            throw std::runtime_error(
                "La anotacion '@Module' requiere un nombre calificado, p.ej. @Module(com.vesta.core)"
            );
        }

        const std::string &mod_name = node->value;

        // crear entrada si no existe
        if (assembler.ctx.modules.find(mod_name) == assembler.ctx.modules.end()) {
            ModuleEntry entry;
            entry.qualified_name = mod_name;
            assembler.ctx.modules[mod_name] = entry;
        }

        assembler.ctx.current_module = mod_name; // activar el modulo
    }

    /**
     * @brief Implementa \@Export(simbolo): registra un simbolo publico del modulo activo.
     *
     * Si no hay modulo activo, lanza una excepcion pidiendo que se declare \@Module primero.
     */
    void apply_export(const vm::AnnotationNode *node, Assembler &assembler) {
        if (node->value.empty()) {
            throw std::runtime_error(
                "La anotacion '@Export' requiere un nombre de simbolo, p.ej. @Export(MiClase)"
            );
        }

        if (assembler.ctx.current_module.empty()) {
            throw std::runtime_error(
                "La anotacion '@Export' requiere que se declare '@Module' antes en el mismo fichero"
            );
        }

        const std::string &sym     = node->value;
        ModuleEntry &      entry   = assembler.ctx.modules[assembler.ctx.current_module];
        // evitar duplicados en la lista de exports
        for (const auto &e: entry.exports) {
            if (e == sym) return;
        }
        entry.exports.push_back(sym);
    }

    /**
     * @brief Implementa \@Generic(T) o \@Generic(K,V): registra parametros de tipo.
     *
     * Los parametros separados por coma se almacenan en generic_instances con el
     * valor vacio como marcador; el emitter los interpreta durante la monomorphization.
     * La clave es el nombre del parametro (p.ej. "T", "K", "V").
     */
    void apply_generic(const vm::AnnotationNode *node, Assembler &assembler) {
        if (node->value.empty()) {
            throw std::runtime_error(
                "La anotacion '@Generic' requiere al menos un parametro de tipo, p.ej. @Generic(T)"
            );
        }

        // descomponer la lista de parametros separados por comas
        const std::string &raw = node->value;
        std::string param;
        for (size_t i = 0; i <= raw.size(); ++i) {
            if (i == raw.size() || raw[i] == ',') {
                // eliminar espacios
                size_t s = 0, e = param.size();
                while (s < e && param[s] == ' ') ++s;
                while (e > s && param[e - 1] == ' ') --e;
                std::string trimmed = param.substr(s, e - s);
                if (!trimmed.empty()) {
                    // registrar el parametro con valor nulo (marcador de genericidad)
                    assembler.ctx.generic_instances[trimmed] = nullptr;
                }
                param.clear();
            } else {
                param += raw[i];
            }
        }
    }
}
