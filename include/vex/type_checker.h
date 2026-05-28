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
 * @file type_checker.h
 * @brief Pase de comprobacion de tipos del frontend Vex.
 *
 * Recorre el AST producido por el parser, resuelve nombres, infiere
 * el tipo de cada expresion (rellenando @c Expr::result_type) y verifica
 * la validez semantica de las operaciones para el subset:
 *
 *   - Funciones top-level y variables globales.
 *   - Variables locales con scope lexico.
 *   - Operaciones aritmeticas, logicas, bitwise y comparacion sobre
 *     tipos compatibles segun reglas estilo C.
 *   - Llamadas a funcion con verificacion de aridad y tipo de argumentos.
 *   - Asignaciones a lvalues
 *
 * Decisiones de hardware / rendimiento:
 *   - Tabla de simbolos como std::vector<unordered_map>: capa por scope.
 *     Lookup en orden inverso (scope mas interno primero) con early-exit.
 *   - Symbols pequenyos (24 bytes): kind + Type + indice de funcion en
 *     el modulo si aplica.  Almacenamiento contiguo en cada scope.
 *   - El check NO modifica la estructura del AST salvo el campo
 *     @c result_type en cada @c Expr; esto evita reasignaciones y mantiene
 *     ownership intacto.
 */

#ifndef VEX_TYPE_CHECKER_H
#define VEX_TYPE_CHECKER_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <memory>
#include "vex/ast.h"
#include "vex/borrow_checker.h"
#include "vex/comptime_vm.h"
#include "vex/diagnostic.h"

namespace vex {
    /**
     * @brief ComptimeValue recursivo via shared_ptr.
     *
     * Necesario shared_ptr para break el cycle de "containers de self".
     * `std::unordered_map<K, ComptimeValue>` con T incompleto es UB en
     * C++17 (no soportado por libstdc++).  Con shared_ptr el container
     * almacena punteros + las copias son O(1) hasta que se necesita
     * deep copy (que se hace explicitamente cuando se necesita modificar
     * sin afectar al original).
     */
    struct ComptimeValue {
        bool        is_str    = false;
        bool        is_array  = false;
        bool        is_struct = false;
        bool        is_type   = false;
        int64_t     value     = 0;
        std::string str;
        std::vector<std::shared_ptr<ComptimeValue>> array_vals;
        std::unordered_map<std::string, std::shared_ptr<ComptimeValue>> struct_fields;
        Type        type_val;
    };
}

namespace vex {

    /**
     * @enum SymbolKind
     * @brief Categoria del simbolo en la tabla.
     */
    enum class SymbolKind : uint8_t {
        Variable,   ///< Variable local o global.
        Param,      ///< Parametro de funcion.
        Function,   ///< Funcion top-level.
        Constant,   ///<       identificador magico de string constante
                    ///<       (ANSI codes RED/GREEN/BOLD/RESET/etc.).
                    ///<       El lowering los convierte en STR_LIT_ADDR
                    ///<       de un literal predefinido.
        Namespace,  ///< Phase M.7: namespace de un modulo importado.
                    ///< Sintaxis: @c "import \"lib_a\";" registra @c lib_a
                    ///< como Symbol::Namespace.  El campo @c ns_index del
                    ///< Symbol apunta al @c imported_namespaces_ del
                    ///< TypeChecker para resolver @c lib_a.simbolo.
    };

    /**
     * @struct FunctionSig
     * @brief Firma de una funcion: tipo de retorno + tipos de parametros.
     */
    struct FunctionSig {
        Type              return_type;
        std::vector<Type> param_types;
        ///  FFI extern: si no esta vacio, esta funcion es un import
        /// de una libreria nativa (ej. "user32.dll", "kernel32.dll" o
        /// "stdlib/native/io/vesta_io").  El lowering al ver una llamada a
        /// una funcion con @c extern_lib != "" emite directamente
        /// @c CALLN @Method("<extern_lib>:<name>") con args en R1..RN,
        /// registrando el import via @c register_native_import.  Sin entry
        /// para extern_lib se trata como funcion Vex normal (CALLVM).
        std::string       extern_lib;
        /// Phase M.5: nombre real del label generado para esta funcion.
        /// Vacio = el nombre visible coincide con el label (caso normal).
        /// No vacio = la funcion fue importada de otro modulo con
        /// mangling automatico (`lib__foo`); el lowering emite @c CALLVM
        /// al label mangled mientras que el resolver de nombres sigue
        /// usando el nombre publico (`foo`).  Cierra la limitacion L.4.
        std::string       mangled_label;
    };

    /**
     * @struct StructFieldInfo
     * @brief Informacion del layout de un campo dentro de un @c struct.
     *
     * @c offset es el desplazamiento en bytes desde el inicio del struct.
     * @c size es el tamano del campo en bytes (sizeof(field.type)).
     * @c type es el tipo semantico ya resuelto (incluye campos struct
     * anidados via Type{STRUCT, name}).
     */
    struct StructFieldInfo {
        std::string name;
        Type        type;
        uint32_t    offset;
        uint32_t    size;
        /// fase C - Bit field metadata.  bit_width=0 indica campo
        /// normal (byte-aligned, ocupa @c size bytes desde @c offset).
        /// bit_width>0 indica bit field: el storage word esta en
        /// @c [offset, offset+size); el bit field empieza en @c bit_offset
        /// (0..size*8-1) dentro de ese word y ocupa @c bit_width bits.
        /// Multiple bit fields del mismo storage word comparten @c offset
        /// y @c size pero con distintos @c bit_offset.
        uint8_t     bit_offset = 0;
        uint8_t     bit_width  = 0;
    };

    /**
     * @struct StructLayout
     * @brief Layout completo de un @c struct: nombre + campos + tamano total.
     *
     * @c size_bytes es el tamano total (con padding al alineamiento del
     * campo mas grande, estilo C).  @c align_bytes es el alineamiento
     * requerido del struct cuando se almacena dentro de otro struct o
     * como campo aislado.
     */
    struct StructLayout {
        std::string                  name;
        std::vector<StructFieldInfo> fields;
        uint32_t                     size_bytes  = 0;
        uint32_t                     align_bytes = 1;
        /// marca `@Introspect` -- el lowering emite
        /// IntrospectInfo POD en static_data para que `find_type("Name")`
        /// runtime lo encuentre.
        bool                         is_introspect = false;
        /// Phase M6.a L.3: visibilidad cross-module.  Solo se exporta a
        /// `.vexi` si es @c true.  Sin keyword `public`/`private` en el
        /// source: @c true (default permisivo).  Sin esto en false, otros
        /// modulos podrian importar el struct via @c only.
        bool                         is_public  = true;
    };

    /**
     * @struct EnumVariantInfo
     * @brief Resumen de una variante de enum (ADT, tagged union).
     *
     * @c tag es el indice asignado por el orden de declaracion (0..N-1).
     * @c field_types lista los tipos de los payload fields en orden.
     * Variantes sin payload tienen @c field_types vacio.
     */
    struct EnumVariantInfo {
        std::string       name;
        uint32_t          tag = 0;
        std::vector<Type> field_types;
    };

    /**
     * @struct EnumLayout
     * @brief Layout completo de un @c enum.
     *
     * Layout de un valor del enum (igual para todas las variantes,
     * para que cualquier variante quepa):
     *   `[+0 i64 tag][+8 payload[0]][+16 payload[1]] ...`
     * @c size_bytes = 8 + 8 * max_payload_fields donde max_payload_fields
     * es el maximo numero de payload fields entre las variantes.  Cada
     * payload se almacena padded a 8 bytes para alineacion uniforme y
     * acceso simple por offset (sin necesidad de calcular offsets por
     * variante).
     */
    struct EnumLayout {
        std::string                  name;
        std::vector<EnumVariantInfo> variants;
        uint32_t                     size_bytes        = 8;  ///< Minimum: solo el tag.
        uint32_t                     max_payload_fields = 0; ///< 0 si todas son sin payload.
        /// marca `@Introspect`.
        bool                         is_introspect = false;
        /// Phase M6.a L.3: visibilidad cross-module.
        bool                         is_public     = true;
    };

    /**
     * @struct ClassMethodInfo
     * @brief Resumen de un metodo de clase para el type checker.
     *
     * @c vtable_index es el slot donde el lowering insertara el metodo
     * en la vtable del ClassRegistry (constructor en posicion 0 por
     * convencion, resto en orden de declaracion).  @c is_constructor
     * y @c is_static replican la info del AST.
     */
    struct ClassMethodInfo {
        std::string             name;
        Type                    return_type;
        std::vector<Type>       param_types;
        uint32_t                vtable_index    = 0;
        bool                    is_constructor  = false;
        /// destructor `~ClassName()`.  Sin params, void retorno.
        /// El lowering lo invoca via CALLVIRT al exit del scope para
        /// instancias locales que NO escapan
        bool                    is_destructor   = false;
        bool                    is_static       = false;
        bool                    is_final        = false;
        /// el lowering, si encuentra un metodo expression-bodied
        /// con esta marca, sustituye la llamada por el cuerpo en el call
        /// site (sin CALLVIRT).  No es heredable: cada override decide su
        /// propio @c is_inline.
        bool                    is_inline       = false;
        /// ctor "trivial zero-init": cuerpo del constructor
        /// solo asigna campos a valores que coinciden con el zero-init
        /// que ya hace el GC (`gc_heap.alloc` memset el payload a 0).  En
        /// ese caso, generate_new_helpers omite el callvirt al ctor en
        /// `__new_<X>` -- ahorra ~9 instrucciones VM por `new` para clases
        /// triviales.  Detectado en el type checker analizando el body.
        bool                    is_zero_init_ctor = false;
        /// Nombre de la clase donde el metodo esta DEFINIDO realmente
        /// (importante para herencia: un metodo heredado por @c Y de
        /// @c X tiene @c defining_class == "X" aunque lay.methods de Y
        /// lo liste).  El lowering usa este nombre para construir el
        /// label del code_vaddr (@c <defining_class>__<name>).
        std::string             defining_class;
        /// Debug info para stack traces.  Llenado por el type
        /// checker al ver el ClassMethodDecl original.  El lowering lo
        /// emite en __module_init via @c setmethdbg.
        std::string             source_file;
        uint32_t                source_line = 0;
    };

    /**
     * @struct ClassLayout
     * @brief Layout completo de una clase Vex.
     *
     * Reusa @c StructFieldInfo para los campos (offset, size, type).
     * @c size_bytes es el tamano total de la instancia (sin contar el
     * @c ObjectHeader: el ClassRegistry suma sizeof(ObjectHeader) al
     * registrar).  @c methods enumera todos los metodos en orden de
     * declaracion; el indice en el vector es el vtable_index.
     */
    struct ClassLayout {
        std::string                  name;
        std::string                  super_name;
        std::vector<std::string>     interface_names;
        std::vector<StructFieldInfo> fields;     // solo campos de instancia
        std::vector<StructFieldInfo> static_fields;
        std::vector<ClassMethodInfo> methods;
        uint32_t                     size_bytes  = 0;
        /// Para clases importadas cross-module via Phase M:
        ///   - @c name lleva el nombre mangled (e.g. "buffer__Buffer")
        ///     usado para identidad de tipo dentro del consumer.
        ///   - @c imported_helper_suffix lleva el nombre LOCAL en el modulo
        ///     dep (e.g. "Buffer") que es el sufijo del helper
        ///     `__new_<helper_suffix>` ya emitido en el .vel del dep.
        /// Para clases locales del consumer, este campo queda vacio y el
        /// lowering usa @c name como sufijo del helper (comportamiento
        /// historico).
        std::string                  imported_helper_suffix;
        /// Numero de fields heredados (los primeros @c inherited_field_count
        /// elementos de @c fields fueron copiados de la superclase y NO
        /// deben re-emitirse como deffield en __module_init).  El resto
        /// son los fields propios de esta clase.
        uint32_t                     inherited_field_count        = 0;
        /// Idem para static_fields.
        uint32_t                     inherited_static_field_count = 0;
        /// Si esta declaracion proviene de @c interface (sin instancias,
        /// metodos abstractos).  El lowering omite la generacion de
        /// __new_<X> y de bodies de metodo, pero SI emite defclass para
        /// que sea localizable via reflexion.  El validador rechaza @c new
        /// X() para layouts con esta marca.
        bool                         is_interface = false;
        /// true si la clase es @c @Aspect (contiene @Before/@After/@Around).
        /// Habilita el devirt monomorfico saber que CALLVIRTs no se pueden
        /// resolver estaticamente en este modulo (los advice chains corren
        /// al despachar dinamicamente; un CALL directo los saltaria).
        bool                         is_aspect = false;
        /// marca `@Introspect` -- el lowering emite
        /// IntrospectInfo POD en static_data para que `find_type("Name")`
        /// runtime lo encuentre.
        bool                         is_introspect = false;
        /// true si la clase declara `~ClassName()`.  Computado tras
        /// agregar todos los metodos.  El type checker usa este flag para
        /// rechazar escapes ilegales: una instancia con destructor NO puede
        /// asignarse a un FieldAccessExpr (objeto/struct field) ni a un
        /// IndexExpr (slot de array nativo) porque rompe el modelo RAII --
        /// el destructor solo se invoca al exit del scope local; sin
        /// scope owner el handle quedaria con destructor pendiente
        /// indefinido.  Returns siguen permitidos (el caller toma owner).
        bool                         has_destructor = false;
        /// true si la clase tiene al menos un campo (de instancia)
        /// cuyo tipo es CLASS y esa clase tiene su propio @c has_destructor
        /// (transitivamente).  En ese caso la regla de escape se RELAJA:
        /// asignar una instancia destructible a `obj.field` es legal porque
        /// el destructor del contenedor (auto-sintetizado si no existe)
        /// invocara recursivamente el destructor del field al destruirse.
        ///
        /// Para clases con @c has_destructible_field == true pero sin
        /// destructor declarado por el usuario, el lowering sintetiza un
        /// destructor implicito que solo recorre los fields destructibles.
        ///
        /// Importante: el computo de este flag requiere un punto-fijo
        /// porque las clases pueden ser mutuamente recursivas (e.g.
        /// LinkedList<T> { Node head; } y Node { Node next; T payload; }).
        /// El TypeChecker itera hasta estabilizarse antes de usar este flag.
        bool                         has_destructible_field = false;
        /// la clase ya esta registrada en runtime (e.g. FatalError
        /// pre-creada por @c init_exception_classes en cada VM).  El
        /// lowering NO debe emitir @c defclass / @c deffield / @c defmethod
        /// para esta clase; tampoco generar @c __new_<X> ni bodies de
        /// metodo.  El @c findclass(name) en runtime ya la encuentra.
        /// Acceso a campos via @c getfield con offsets fijos del ABI.
        bool                         is_runtime_predefined = false;
        /// Phase M6.a L.3: visibilidad cross-module.
        bool                         is_public             = true;
    };

    /**
     * @struct Symbol
     * @brief Entrada de la tabla de simbolos.
     *
     * @c type sirve para Variable/Param.  Para Function se ignora y se
     * consulta @c sig_index en una tabla aparte (vector contiguo).
     */
    struct Symbol {
        SymbolKind kind = SymbolKind::Variable;
        Type       type{};
        uint32_t   sig_index = 0; ///< Indice en TypeChecker::function_sigs_, si kind==Function.
        bool       is_const  = false;
        /// Nombre del alias de tipo cuando el simbolo se declaro con un alias
        /// "magico" como `Class`, `Method`, `Field`, `Object`.  El tipo
        /// subyacente es i64 (para reflexion: handles del ClassRegistry).
        /// Permite que `Class cls = ...` registre cls -> "Class" y que
        /// `cls.getMethod("foo")` baje a `getMethod(cls, "foo")` sin que
        /// el usuario escriba la builtin standalone.  Vacio para variables
        /// normales.
        std::string reflection_alias;
        /// Phase M.7: indice en @c TypeChecker::imported_namespaces_
        /// cuando @c kind == SymbolKind::Namespace.  Sin uso para los
        /// demas kinds.
        uint32_t   ns_index = 0;
    };

    /**
     * @class TypeChecker
     * @brief Pase de comprobacion sobre un ModuleNode.
     */
    class TypeChecker {
    public:
        /**
         * @brief Construye el checker sobre un modulo y su sumidero de errores.
         *
         * @param mod   Modulo AST a verificar.  Sera modificado solo en
         *              los campos result_type de las expresiones internas.
         * @param diags Sumidero de diagnosticos.
         */
        TypeChecker(ast::ModuleNode &mod, Diagnostics &diags);
        ~TypeChecker();  //limpia g_active_typechecker thread_local

        /**
         * @brief Ejecuta el checker.
         *
         * @return @c true si no hubo errores.  Si devuelve @c false, el
         *         lowering NO debe ejecutarse (hay nodos sin tipo).
         */
        bool run();

        /**
         * @brief Acceso de solo lectura a la tabla de layouts de struct.
         *
         * El lowering la consulta para calcular offsets de campos y
         * tamano total en bytes al reservar variables de tipo struct.
         */
        const std::unordered_map<std::string, StructLayout> &
        struct_layouts() const noexcept { return struct_layouts_; }

        /**
         * @brief Acceso de solo lectura a la tabla de layouts de clases.
         *
         * El lowering la consulta para emitir el bloque __module_init
         * (defclass + deffield + defmethod) y para resolver offsets de
         * campos al traducir GETFIELD / SETFIELD.
         */
        const std::unordered_map<std::string, ClassLayout> &
        class_layouts() const noexcept { return class_layouts_; }

        /**
         * @brief Acceso de solo lectura a la tabla de layouts de enums.
         *
         * El lowering la consulta para emitir el codigo de constructor de
         * variante (STORE de tag + payloads) y de match (jumptable +
         * extraccion de bindings via LOAD por offset).
         */
        const std::unordered_map<std::string, EnumLayout> &
        enum_layouts() const noexcept { return enum_layouts_; }

        /**
         * @brief Acceso de solo lectura al ModuleNode AST.  Phase M.L7
         * lo usa @c export_typechecker_to_vexi para iterar
         * @c GlobalVarDecl y extraer sus tipos sin tener que mantener
         * un mapa paralelo en el TypeChecker.
         */
        const ast::ModuleNode &ast_module() const noexcept { return mod_; }

        /**
         * @brief Resuelve un TypeNode AST a su Type semantico.
         *
         * Aplica resolucion de aliases (typedef/using) y reconocimiento
         * de structs.  Expuesto publico para que el lowering pueda
         * resolver el tipo declarado de variables locales sin duplicar
         * la logica de bilinkeo nombre->tipo.
         */
        Type resolve_type_node(const ast::TypeNode *tn) const {
            return type_from_node(tn);
        }

        /**
         * @brief Genera (o devuelve cacheado) el nombre concreto de un
         *        instanciado generico.  Public para que el pre-pase de
         *        monomorphizacion (helpers static en type_checker.cpp)
         *        pueda invocarlo.  Detalles en la docstring del impl.
         */
        std::string monomorphize_class(const std::string &template_name,
                                        const std::vector<Type> &args,
                                        const SourceLoc &loc);

        /**
         * @brief L2.3: monomorphizacion de enum generico.  Crea una copia
         * concreta del template (con type_params=[T,U,...]) sustituyendo
         * los parametros por args concretos.  Devuelve el nombre mangled
         * (e.g. "Maybe_i32") que se usa como struct_name del Type STRUCT.
         */
        std::string monomorphize_enum(const std::string &template_name,
                                       const std::vector<Type> &args,
                                       const SourceLoc &loc);

        /// L2.3: ¿el nombre es un enum template generico?
        bool is_generic_enum_template(const std::string &name) const noexcept {
            return generic_enum_templates_.count(name) > 0;
        }

        /// L2.3: stack de contexto.  Cuando check_var_decl o check_assign
        /// procesa `Maybe<i32> a = Maybe.Some(42)`, push ("Maybe","Maybe_i32")
        /// para que `Maybe.Some(42)` o `Maybe.None` resuelvan al mangled
        /// correcto.  Pop tras el check.
        void push_expected_enum(const std::string &templ, const std::string &mang) {
            expected_enum_stack_.push_back({templ, mang});
        }
        void pop_expected_enum() noexcept {
            if (!expected_enum_stack_.empty()) expected_enum_stack_.pop_back();
        }
        const std::string *expected_enum_mangled(const std::string &templ) const noexcept {
            for (auto it = expected_enum_stack_.rbegin();
                 it != expected_enum_stack_.rend(); ++it) {
                if (it->first == templ) return &it->second;
            }
            return nullptr;
        }

        /**
         * @brief Accesor publico a la firma de una funcion top-level.
         *
         * El lowering lo usa en @c lower_call para detectar parametros de
         * tipo STRING y promover automaticamente literales pasados como
         * argumento (`helper("hola")`) a StringObject inline via STRMAKE.
         * Sin esta promocion, el callee recibiria la direccion VM del
         * literal en vez del GcHandle al StringObject -> crash en strraw.
         *
         * @param name Nombre de la funcion.
         * @return Puntero a su FunctionSig o nullptr si no es funcion conocida.
         */
        const FunctionSig *function_sig_by_name(const std::string &name) const;

        /**
         * @brief Si @p name es una funcion extern, devuelve "@extern:<lib>:<name>".
         *        En cualquier otro caso devuelve "".
         *
         * Usado por @c unique_with / @c shared_with en el lowering para
         * generar el literal_deleter de la CleanupAction con el formato
         * que @c emit_cleanups_all sabe interpretar como CALLN a libreria
         * nativa (en lugar de CALLVM a funcion Vesta).
         */
        std::string lookup_extern_qualified(const std::string &name) const;

    private:
        // -----------------------------------------------------------------
        // Pases globales.
        // -----------------------------------------------------------------

        /**
         * @brief Pase 1: registra funciones y variables globales en el scope global.
         */
        void collect_globals();

        /**
         * @brief Pase 2: chequea el cuerpo de cada funcion declarada.
         */
        void check_functions();

        // -----------------------------------------------------------------
        // Visit de statements.
        // -----------------------------------------------------------------

        void check_stmt(ast::Stmt *s, const Type &fn_return_type);
        void check_block(ast::BlockStmt *b, const Type &fn_return_type);
        void check_var_decl(ast::VarDeclStmt *vd);
        void check_if(ast::IfStmt *s, const Type &fn_return_type);
        void check_while(ast::WhileStmt *s, const Type &fn_return_type);
        void check_for(ast::ForStmt *s, const Type &fn_return_type);
        void check_return(ast::ReturnStmt *s, const Type &fn_return_type);

        // -----------------------------------------------------------------
        // Visit de expresiones (rellena result_type).
        //
        // Devuelven el tipo deducido y al mismo tiempo lo escriben en
        // @c e->result_type para que el lowering lo lea sin recomputar.
        // -----------------------------------------------------------------

        Type check_expr(ast::Expr *e);
        Type check_binary(ast::BinaryExpr *e);
        Type check_unary(ast::UnaryExpr *e);
        Type check_assign(ast::AssignExpr *e);
        Type check_call(ast::CallExpr *e);
        Type check_ident(ast::IdentExpr *e);
        Type check_field_access(ast::FieldAccessExpr *e);
        Type check_index(ast::IndexExpr *e);
        Type check_this(ast::ThisExpr *e);
        Type check_new(ast::NewExpr *e);

        /**
         * @brief type-checking de una expresion lambda.
         *
         * Pasos:
         *   1. Construye el Type{FUNCTION, ...} a partir de la firma
         *      declarada (params con tipo) o, si los params no llevan
         *      tipo, los deja como VOID a la espera de inferencia desde
         *      el contexto.  El context-driven type narrowing se realiza
         *      en check_var_decl / check_assign cuando el destino es
         *      @c fn(T1, T2) -> R.
         *   2. Empuja un nuevo @c LambdaCtx con outer_depth = scopes_.size().
         *   3. Push de scope con los params como locales.
         *   4. Recursivamente type-checks @c body; cualquier IdentExpr a un
         *      scope @c < outer_depth se anade automaticamente a
         *      @c expr->captures via la rama de @c check_ident.
         *   5. Pop del scope y del LambdaCtx.
         *   6. Devuelve @c Type::make_function(params, return_type).
         *
         * El tipo del return se infiere del primer @c ReturnStmt visto
         * (los siguientes deben ser compatibles); si el body no tiene
         * @c return, el tipo es VOID.
         */
        Type check_lambda(ast::LambdaExpr *e);

        /**
         * @brief type-checking de un @c MatchExpr.
         *
         * Pasos:
         *   1. Type-check del scrutinee.  Debe ser un valor cuyo tipo
         *      sea un enum registrado (kind==STRUCT con struct_name en
         *      enum_layouts_).
         *   2. Para cada arm:
         *      - Validar que @c variant_name existe en el enum (o es @c _).
         *      - Validar que @c bindings.size() coincide con el numero
         *        de payload fields de la variante.
         *      - Push de scope local con los bindings tipados.
         *      - Lower del body (return_type del enclosing function lo
         *        propaga el caller).
         *   3. Validar exhaustividad: o (a) hay un arm @c _, o (b)
         *      todas las variantes del enum tienen al menos un arm.
         *      Si no se cumple, reportar error claro.
         *
         * Devuelve @c Type{VOID}: en MVP el match es statement-like, no
         * produce valor utilizable como expresion.  Para usar el match
         * como expresion, el usuario asigna dentro de cada arm a una
         * variable comun.
         */
        Type check_match(ast::MatchExpr *e);

        /**
         * @brief Recorre el cuerpo de un metodo de clase con el contexto
         *        adecuado: anade @c this como variable implicita y los
         *        parametros declarados.  El primer parametro del metodo
         *        (en bytecode) es @c this; los demas vienen detras.
         */
        void check_class_method(const ClassLayout &cls, ast::ClassMethodDecl *m);

        /**
         * @brief Pase 0 extendido: registra todas las clases del modulo
         *        con sus layouts (fields + methods) antes del checking
         *        de cuerpos.  Permite que un metodo refiera a otra clase
         *        sin importar el orden de declaracion.
         */
        void collect_classes();

        // -----------------------------------------------------------------
        // Tabla de simbolos: scopes apilados.
        // -----------------------------------------------------------------

        void push_scope();
        void pop_scope();

        /**
         * @brief Inserta @p sym con el nombre dado en el scope mas interno.
         * @return @c false si ya existia (redefinicion); el caller debe
         *         emitir el diagnostico correspondiente.
         */
        bool declare(const std::string &name, Symbol sym);

        /**
         * @brief Busca @p name desde el scope interno hacia el global.
         * @return Puntero al Symbol o nullptr si no existe.
         */
        const Symbol *lookup(const std::string &name) const;

        /**
         * @brief Variante de @c lookup que ademas devuelve el indice del
         *        scope donde se encontro (0 = global, scopes_.size() - 1 = top).
         *
         * Necesaria para el analisis de capturas de @c LambdaExpr: si el
         * indice del scope es @c < lambda_ctx.outer_depth, la variable
         * pertenece al entorno exterior y debe capturarse en el env block.
         *
         * @param name        Nombre a buscar.
         * @param depth_out   Si no es null y se encuentra, escribe el indice
         *                    del scope.  Sin tocar si no se encuentra.
         * @return Puntero al Symbol o nullptr.
         */
        const Symbol *lookup_with_depth(const std::string &name,
                                        size_t *depth_out) const;
    public:
        /**
         * @brief Phase M.L26: acceso de solo lectura al set de nombres que
         * @c lookup_with_depth ha resuelto exitosamente durante el check.
         * El @c compile_vex_project lo usa para detectar imports
         * declarados pero no usados y emitir warnings.
         */
        const std::unordered_set<std::string> &referenced_names() const noexcept {
            return referenced_names_;
        }

        /// Phase M.L23: marca un nombre como importado de otro modulo.
        /// El export del @c .vexi del modulo actual lo filtra por
        /// defecto (no se re-exporta).  Si @c is_reexport es @c true ,
        /// se anyade tambien al set de re-exportados y SE EXPORTA al
        /// @c .vexi (semantica @c public @c import @c "x"; ).
        void mark_imported(const std::string &name, bool is_reexport) {
            imported_names_.insert(name);
            if (is_reexport) reexported_imported_names_.insert(name);
        }
        bool is_imported(const std::string &name) const noexcept {
            return imported_names_.count(name) > 0;
        }
        bool is_reexported(const std::string &name) const noexcept {
            return reexported_imported_names_.count(name) > 0;
        }
    private:

        /**
         * @brief Convierte un TypeNode AST a Type semantico.
         */
        Type type_from_node(const ast::TypeNode *tn) const;

        /**
         * @brief Verifica si una asignacion entre tipos CLASS es valida
         *        considerando la jerarquia de interfaces / superclases.
         *
         * Devuelve true cuando @p target.struct_name es una superclase o
         * interfaz (transitiva) implementada por @p value.struct_name.
         * Solo se aplica a CLASS<->CLASS; otras combinaciones se delegan
         * a @c types_assignable.
         */
        bool class_is_assignable(const Type &target, const Type &value) const noexcept;

    public:
        /**
         * @brief tabla de constantes comptime declaradas con
         * `comptime const T NAME = expr;` a nivel modulo.
         *
         * Cada entrada `{name -> {type, value_bits}}` es poblada por el
         * type checker tras evaluar el init con @c comptime_eval_expr.
         * El lowering consulta esta tabla en @c lower_ident para
         * inlinear el valor como @c IrOp::CONST en cada uso (cero
         * overhead, sin storage runtime).  El evaluador comptime
         * (`comptime_eval_expr`) tambien la consulta para resolver
         * identifiers a su valor cacheado, permitiendo `comptime const`
         * en cualquier expresion comptime (incluida `comptime if`).
         */
        struct ComptimeConst {
            Type        type;
            bool        is_str    = false;
            bool        is_array  = false;
            bool        is_struct = false;
            bool        is_type   = false;    ///< contiene un Type
            bool        is_mutable = false;
            int64_t     value     = 0;
            std::string str_value;
            std::vector<std::shared_ptr<ComptimeValue>> array_vals;
            std::unordered_map<std::string, std::shared_ptr<ComptimeValue>> struct_fields;
            Type        type_val;
            // v4: atributos del usuario (@align/@hot/@cold/@section).
            // Solo significativos para comptime constants TOP-LEVEL
            // (GlobalVarDecl con is_comptime=true).  El lowering los
            // propaga al @c static_data_meta tras intern.
            uint16_t    attr_align   = 0;     ///< 0 = default; sino potencia de 2
            bool        attr_hot     = false;
            bool        attr_cold    = false;
            std::string attr_section;
        };
        const std::unordered_map<std::string, ComptimeConst> &
        comptime_const_values() const noexcept { return comptime_const_values_; }
        std::unordered_map<std::string, ComptimeConst> &
        comptime_const_values() noexcept { return comptime_const_values_; }

        /**
         * @brief stack de scopes locales de comptime const.
         *
         * Se push al entrar a una funcion, un bloque @c comptime { ... },
         * un body de @c comptime for o un cuerpo de @c comptime fn; se
         * pop al salir.  Los identifiers se resuelven buscando desde el
         * top (mas reciente) hacia abajo; finalmente caen en la tabla
         * global @c comptime_const_values_ .
         */
        const std::vector<std::unordered_map<std::string, ComptimeConst>> &
        comptime_const_locals() const noexcept { return comptime_const_locals_; }
        std::vector<std::unordered_map<std::string, ComptimeConst>> &
        comptime_const_locals() noexcept { return comptime_const_locals_; }
        void push_comptime_scope() {
            comptime_const_locals_.emplace_back();
        }
        void pop_comptime_scope() {
            if (!comptime_const_locals_.empty()) {
                comptime_const_locals_.pop_back();
            }
        }
        void register_comptime_local(const std::string &name,
                                      ComptimeConst v) {
            if (comptime_const_locals_.empty()) push_comptime_scope();
            comptime_const_locals_.back()[name] = std::move(v);
        }

        /**
         * @brief registro de funciones declaradas como `comptime fn`.
         *
         * Cuando una llamada se resuelve a un nombre presente aqui, el
         * evaluador @c comptime_eval_expr la interpreta en compile-time
         * en lugar de generar codigo runtime.  Soporta recursion con un
         * limite de profundidad (defensivo contra loops infinitos).
         */
        const std::unordered_map<std::string, const ast::FunctionDecl *> &
        comptime_fns() const noexcept { return comptime_fns_; }
        void register_comptime_fn(const std::string &name,
                                   const ast::FunctionDecl *fn) {
            comptime_fns_[name] = fn;
        }
        /// Contador de recursion para detectar loops infinitos en
        /// comptime fn (limite 256 niveles).
        int &comptime_recursion_depth() noexcept {
            return comptime_recursion_depth_;
        }

        /// stack de scopes de type-params bindeados para
        /// comptime fn genericos.  Cuando se llama `my_fn<T1, T2>(...)`,
        /// push scope con {"T1" -> resolved_t1, "T2" -> resolved_t2},
        /// eval body, pop.  @c resolve_type_node consulta este stack
        /// para sustituir IdentExpr de tipo (e.g. `T` en `sizeof<T>()`)
        /// por el tipo bindeado.
        std::vector<std::unordered_map<std::string, Type>> &
        comptime_type_locals() noexcept { return comptime_type_locals_; }
        const std::vector<std::unordered_map<std::string, Type>> &
        comptime_type_locals() const noexcept { return comptime_type_locals_; }
        void push_comptime_type_scope() {
            comptime_type_locals_.emplace_back();
        }
        void pop_comptime_type_scope() {
            if (!comptime_type_locals_.empty()) {
                comptime_type_locals_.pop_back();
            }
        }
        void register_comptime_type(const std::string &name, Type t) {
            if (comptime_type_locals_.empty()) push_comptime_type_scope();
            comptime_type_locals_.back()[name] = std::move(t);
        }
        /// Lookup en el stack de type-params.  Devuelve true + setea
        /// @c out_t si encontro el nombre.
        /// accesor publico a diagnostics, requerido por
        /// @c comptime_compile para reportar errores de parsing de los
        /// fragmentos de fuente que recibe como input.  Mutable porque
        /// el evaluador comptime (`comptime_eval_expr`) recibe @c const
        /// @c TypeChecker& pero necesita anyadir errores cuando un
        /// fragmento es invalido.
        Diagnostics &diagnostics() const noexcept { return diags_; }
        bool lookup_comptime_type(const std::string &name, Type &out_t) const {
            for (auto it = comptime_type_locals_.rbegin();
                 it != comptime_type_locals_.rend(); ++it) {
                auto hit = it->find(name);
                if (hit != it->end()) {
                    out_t = hit->second;
                    return true;
                }
            }
            return false;
        }

    private:
        std::unordered_map<std::string, ComptimeConst> comptime_const_values_;
        std::vector<std::unordered_map<std::string, ComptimeConst>>
            comptime_const_locals_;
        std::unordered_map<std::string, const ast::FunctionDecl *>
            comptime_fns_;
        int comptime_recursion_depth_ = 0;
        std::vector<std::unordered_map<std::string, Type>>
            comptime_type_locals_;
        /// profundidad de scopes `synchronized` activos en el
        /// path actual.  Incrementa al entrar a un SynchronizedStmt y
        /// decrementa al salir.  Los builtins @c wait / @c notify /
        /// @c notifyAll lo consultan: si es 0 se emite error claro
        /// "debe llamarse dentro de un synchronized".
        int synchronized_depth_ = 0;
        /// contador para @c gensym -- emite identificadores
        /// frescos unicos por llamada, util para macros hygenic que
        /// quieren introducir nombres sin colision con el scope del
        /// caller.  El contador es per-TypeChecker (no global) asi
        /// cada compilacion empieza desde 0.
        uint64_t gensym_counter_ = 0;
        /// cache de memoizacion para fns @Pure.  Key =
        /// nombre+args serializados; value = ComptimeEvalResult cacheado
        /// via shared_ptr para evitar tener que conocer el size del tipo
        /// en este header (ComptimeEvalResult vive en comptime_introspect.h
        /// y depende de ComptimeValue de este header, asi que evitamos el
        /// ciclo via puntero).  Llamadas repetidas con mismos args
        /// retornan el resultado sin reejecutar el body.
        std::unordered_map<std::string, std::shared_ptr<struct ComptimeEvalResult>>
            pure_macro_cache_;
        /// runtime dedicado a ejecutar @Macros lowered al IR.
        /// Lazy: construido vacio aqui, la VM interna se inicializa al
        /// primer @c try_invoke.  Vida util = vida util del TypeChecker.
        /// Reusado cross-macro durante toda la compilacion.
        ComptimeRuntime comptime_runtime_;

        /// contadores de hits/misses del path VM-only en
        /// el call site del @Macro.  hits = invocacion VM exitosa
        /// (resultado usado, AST eval saltado); misses = VM no aplico
        /// (flag off, sin bytecode, args no codificables, o invoke
        /// fallo) y caimos al AST evaluator.  Reset al start de run().
        uint32_t macro_vmonly_hits_   = 0;
        uint32_t macro_vmonly_misses_ = 0;

    public:
        uint64_t next_gensym_id() noexcept { return gensym_counter_++; }
        std::unordered_map<std::string, std::shared_ptr<struct ComptimeEvalResult>> &
        pure_macro_cache() noexcept { return pure_macro_cache_; }
        ComptimeRuntime       &comptime_runtime()       noexcept { return comptime_runtime_; }
        const ComptimeRuntime &comptime_runtime() const noexcept { return comptime_runtime_; }

        /// Phase MC.9: snapshot publico de los contadores VM-only para
        /// diagnostico desde main.cpp (cuando VESTA_MC_VERBOSE esta on).
        uint32_t macro_vmonly_hits()   const noexcept { return macro_vmonly_hits_; }
        uint32_t macro_vmonly_misses() const noexcept { return macro_vmonly_misses_; }
    private:

        // -----------------------------------------------------------------
        // Datos.
        // -----------------------------------------------------------------

        ast::ModuleNode &mod_;
        Diagnostics     &diags_;

        // Pila de scopes: scopes_[0] = global, scopes_.back() = mas interno.
        std::vector<std::unordered_map<std::string, Symbol>> scopes_;

        // Almacen de firmas de funciones (referenciadas por sig_index).
        std::vector<FunctionSig> function_sigs_;

        /// mapa nombre -> indice en function_sigs_ que sobrevive
        /// al @c pop_scope() final de @c run().  Necesario porque el lowering
        /// consulta firmas POST-check para auto-promover literales a STRING
        /// cuando el parametro espera STRING (sin esto, el lookup en scopes_
        /// devolveria nullptr porque el scope global ya fue cerrado).  Se
        /// rellena en cada @c function_sigs_.push_back y nunca se limpia.
        std::unordered_map<std::string, uint32_t> sig_by_name_;

        /// Phase M.2.e: simbolos de funcion importados via .vexi que
        /// deben declararse en el scope global al inicio de run().  El
        /// constructor del TypeChecker NO ha pusheado scope todavia,
        /// asi que las llamadas a register_imported_function durante
        /// la fase de inyeccion no pueden hacer declare() directamente.
        /// Esta cola se drena al inicio de run() tras push_scope().
        std::vector<std::pair<std::string, uint32_t>> pending_imported_fn_names_;

        /// Phase M.L7: cola paralela para variables globales importadas.
        struct PendingGlobal {
            std::string name;
            Type        type;
            bool        is_const = false;
            bool        has_init_value = false;
            uint64_t    init_value = 0;
            /// v4: si @c is_str es @c true, el valor del global es un
            /// string almacenado en @c str_value.  El lowering materializa
            /// via STRMAKE en @c lower_ident.
            bool        is_str = false;
            std::string str_value;
        };
        std::vector<PendingGlobal> pending_imported_globals_;
        /// Phase M.L7: declaracion adelantada del struct + map.  El
        /// accesor publico @c imported_global_consts() en la seccion
        /// public devuelve este miembro.
    public:
        struct ImportedGlobalConst {
            Type        type;
            uint64_t    value = 0;
            /// v4: para comptime const string cross-module.
            bool        is_str = false;
            std::string str_value;
        };
    private:
        std::unordered_map<std::string, ImportedGlobalConst>
            imported_global_consts_;

        /// Phase M6.a L.3: visibilidad por simbolo top-level.
        /// El TypeChecker rellena estos sets al procesar cada decl segun
        /// @c FunctionDecl::is_public, @c GlobalVarDecl::is_public, etc.
        /// El emitter de .vexi consulta para filtrar simbolos privados.
        std::unordered_map<std::string, bool> function_is_public_;
        std::unordered_map<std::string, bool> global_is_public_;
        std::unordered_map<std::string, bool> typedef_is_public_;

        /// Phase M.L26: set de nombres que @c lookup_with_depth resolvio
        /// exitosamente.  Mutable porque el lookup es @c const pero el
        /// tracking es metadata observacional, no afecta la semantica.
        /// Util para detectar imports declarados pero nunca usados +
        /// emitir warnings.
        mutable std::unordered_set<std::string> referenced_names_;

        /// Phase M.L23: set de nombres importados desde @c .vexi de otros
        /// modulos.  El export del @c .vexi del modulo actual los filtra
        /// por DEFAULT (no se re-exportan) salvo que esten tambien en
        /// @c reexported_imported_names_ (marcados con @c public import).
        std::unordered_set<std::string> imported_names_;
        std::unordered_set<std::string> reexported_imported_names_;

        /// LANG.fix-3: mapa persistente local_name -> ns_idx para que
        /// @c resolve_type_node pueda resolver namespaces tras el
        /// @c pop_scope final.  Sin esto, la lowering pierde los
        /// namespaces porque viven en @c scopes_ (popped).
        std::unordered_map<std::string, uint32_t> ns_idx_by_local_name_;
    public:
        const std::unordered_map<std::string, uint32_t> &
        ns_idx_by_local_name() const { return ns_idx_by_local_name_; }
    private:

    public:
        /// Phase M.7: namespace de un modulo importado.  Cada entry
        /// contiene los simbolos publicos del @c .vexi indexados por
        /// nombre.  Cuando el TypeChecker ve @c "buf.Buffer" o
        /// @c "lib_a.valor_a", busca @c "buf"/@c "lib_a" en la pila de
        /// scopes (debe ser Symbol::Namespace con @c ns_index apuntando
        /// aqui) y resuelve el simbolo dentro del namespace.
        struct ImportedNamespace {
            /// Nombre del modulo original (e.g. "lib_a"); util para
            /// emitir el label mangled al hacer CALL.
            std::string module_name;
            /// Simbolos del namespace: nombre publico -> indice en
            /// @c symbols.  El lookup es O(1) en uso normal.
            std::unordered_map<std::string, uint32_t> by_name;
            struct Sym {
                std::string mangled_label;  ///< label real en el .vel (e.g. "lib_a__valor_a")
                FunctionSig sig;              ///< firma para validacion en check_call
                Type        var_type;         ///< para Variable/Constant
                uint8_t     kind = 0;         ///< 0=Function, 1=Variable/Const, 2=TypeAlias
                /// Para kind=1 (Constant): valor literal si esta disponible.
                /// El lowering lo emite como CONST inline (cero overhead vs
                /// `const` runtime).  Solo valido cuando @c has_const_value.
                bool        has_const_value = false;
                int64_t     const_value     = 0;
            };
            std::vector<Sym> symbols;
        };

    private:
        std::vector<ImportedNamespace> imported_namespaces_;

        /// Cola de namespaces pendientes a declarar en el scope global
        /// (mismo patron que @c pending_imported_fn_names_).
        std::vector<std::pair<std::string, uint32_t>> pending_imported_ns_names_;

    public:
        /// @brief Acceso al registro de namespaces (para el lowering).
        const std::vector<ImportedNamespace> &imported_namespaces() const noexcept {
            return imported_namespaces_;
        }

        /// @brief Registra un namespace importado.  Devuelve el indice
        /// asignado en @c imported_namespaces_.  El caller (compiler_project)
        /// llama esto tras parsear el .vexi del dep para registrar
        /// `import "lib_a";` o `import "lib_a" as foo;`.
        /// @param local_name Nombre con el que se accede en el modulo
        ///                   actual (e.g. "lib_a" o "foo" si hay alias).
        /// @param module_name Nombre del modulo origen (para mensajes).
        uint32_t register_imported_namespace(
                const std::string &local_name,
                const std::string &module_name);

        /// @brief Anyade un simbolo al namespace registrado en @p ns_index.
        /// Llamado por el compiler_project durante la inyeccion de cada
        /// VexiSymbol cuyo modulo se importo plain (sin `only`).
        void register_namespace_symbol(uint32_t ns_index,
                                        const std::string &public_name,
                                        ImportedNamespace::Sym sym);

    private:

        /// Borrow checker compile-time.  Mantiene estado de borrows
        /// activos durante el chequeo de una funcion.  Se resetea al
        /// entrar a cada funcion.
        BorrowChecker borrow_checker_{diags_};

        /// F1 NLL - contador de stmt durante el chequeo del cuerpo de
        /// una funcion.  Incrementado en cada stmt; el borrow checker
        /// consulta @c last_use_idx vs current_stmt_idx_ para dropear
        /// borrows tras su ultimo uso (Non-Lexical Lifetimes).
        uint32_t current_stmt_idx_ = 0;

        /// F1 NLL - pre-pase: walk del body de la funcion en DFS order,
        /// asignando stmt_idx a cada statement y registrando el stmt_idx
        /// maximo en que cada nombre aparece referenciado.  El resultado
        /// se entrega al @c borrow_checker_ via @c set_last_use antes
        /// de empezar el chequeo del body.
        void compute_borrow_last_uses(ast::Stmt *body);

        // Tabla de alias de tipo (introducidos por typedef / using).  Mapea
        // el nombre alias al Type ya resuelto al tipo subyacente; alias
        // anidados (a -> b -> u32) se aplanan en collect_globals.
        std::unordered_map<std::string, Type> type_aliases_;

        // Newtypes (typedef T name new) -- mapa nombre -> tipo underlying
        // (sin nominal_id ni is_opaque) para responder a la introspeccion
        // `X typedef is T` y al cast explicito `(T)x` que cruza la barrera
        // nominal.  Solo poblado para aliases con @c is_newtype = true.
        std::unordered_map<std::string, Type> newtype_underlying_;

    public:
        /// @brief Conversion declarada via @c "explicit from T;" o
        /// @c "explicit to T;" en el bloque del typedef.  @c is_public
        /// indica si la conversion es accesible desde otros ficheros
        /// (default: privado al fichero donde se declaro el typedef).
        struct ExplicitConv {
            Type type;
            bool is_public = false;
        };

        /// @brief Metadata por newtype: conversiones declaradas + fichero
        /// donde se declaro el typedef.  El fichero se usa para imponer
        /// module-privacy: una conversion sin @c public solo aplica en el
        /// mismo fichero.
        struct NewtypeInfo {
            std::vector<ExplicitConv> from_conversions;
            std::vector<ExplicitConv> to_conversions;
            std::string               source_file;
        };

    private:
        // Mapa de metadata por nombre de newtype.  Vacio para newtypes
        // sin bloque @c {explicit from/to T;} declarado.
        std::unordered_map<std::string, NewtypeInfo> newtype_info_;

        // Counter global de IDs nominales asignados a newtypes.  Empieza
        // en 0; el ID 0 esta reservado para "no es newtype" (alias clasico).
        uint32_t newtype_counter_ = 0;

    public:
        /// @brief Acceso de solo lectura al underlying de un newtype.
        /// Usado por el lowering para emitir el cast explicito (T)x
        /// preservando bits.  Devuelve nullptr si @p name no es un newtype.
        const Type *newtype_underlying(const std::string &name) const noexcept {
            auto it = newtype_underlying_.find(name);
            return (it != newtype_underlying_.end()) ? &it->second : nullptr;
        }

        /// @brief Metadata de conversiones de un newtype (puede ser nullptr).
        const NewtypeInfo *newtype_info(const std::string &name) const noexcept {
            auto it = newtype_info_.find(name);
            return (it != newtype_info_.end()) ? &it->second : nullptr;
        }

    private:

        // Tabla de structs declarados con su layout pre-calculado (offsets
        // de campos y tamano total).  Acceso O(1) por nombre desde
        // type_from_node y desde el lowering (a traves de la API publica).
        std::unordered_map<std::string, StructLayout> struct_layouts_;

        // Tabla de clases declaradas con su layout pre-calculado.  Una
        // clase tiene los mismos fields que un struct mas una vtable y
        // una superclase opcional; el frontend baja a defclass +
        // deffield* + defmethod* en el __module_init.
        std::unordered_map<std::string, ClassLayout> class_layouts_;

        // tabla de enums declarados en el modulo.  El nombre
        // de la variante esta calificado por el nombre del enum (e.g.
        // un enum @c Color con variante @c Red se accede via
        // @c enum_layouts_["Color"].variants[0]).  Los nombres de variante
        // son unicos dentro de su enum pero pueden colisionar entre
        // enums distintos (no hay namespace global de variantes).
        std::unordered_map<std::string, EnumLayout> enum_layouts_;

        // Plantillas genericas (clases con type_params no vacios).  No se
        // procesan como clases concretas; cada instanciado las
        // monomorphiza.  Mapea nombre del template -> indice en
        // mod_.decls (para clonar el ClassDecl original).
        std::unordered_map<std::string, size_t> generic_templates_;
        /// L2.3: templates de enum genericos.  Mapea template_name (sin
        /// args) -> indice en mod_.decls.  Cada uso `Maybe<i32>` se
        /// monomorphiza on demand via monomorphize_enum().
        std::unordered_map<std::string, size_t> generic_enum_templates_;
        /// L2.3: stack para inferir el mangled de variantes sin payload
        /// (e.g. `Maybe<i32> a = Maybe.None`).  Pair = (template_name,
        /// mangled_name).
        std::vector<std::pair<std::string, std::string>> expected_enum_stack_;

        // Cache de monomorphizaciones: clave = "Box<i32>" mangled como
        // "Box_i32"; valor = true si ya esta generada.  Evita regenerar
        // la misma instanciacion mas de una vez.
        std::unordered_map<std::string, bool> monomorphized_;

    public:
        /**
         * @brief Provenance de una clase monomorphizada.
         *        Conocer el template + los args concretos permite que
         *        el JIT/AOT identifique instanciaciones, deduplique
         *        especializaciones, y emita stack traces legibles
         *        ("Box<i32>" en vez de "Box_i32").
         */
        struct MonomorphInfo {
            std::string              template_name;  ///< "Box"
            std::vector<std::string> type_args;       ///< ["i32"] (legibles)
        };

        /**
         * @brief Devuelve el provenance de una clase monomorphizada,
         *        o @c nullptr si @p mangled no es una instanciacion.
         *
         * Lo consulta @c Lowering al generar IrFunction para los
         * metodos de la clase: rellena @c IrFunction::generic_template_name
         * y @c generic_type_args para que el IR lleve el contract.
         */
        const MonomorphInfo *monomorph_info(const std::string &mangled) const noexcept {
            auto it = monomorph_info_.find(mangled);
            return (it == monomorph_info_.end()) ? nullptr : &it->second;
        }

        // ---- Phase M.2: interop con .vexi (interfaces compiladas) ----
        //
        // El TypeChecker puede exportar sus simbolos publicos a un
        // descriptor @c vex::VexiModule para que el emitter del .vexi
        // los serialize.  Tambien puede inyectar simbolos importados
        // desde un .vexi parseado.  Las firmas precisas viven en
        // @c module_interop.cpp para no forzar include de vexi_format.h
        // aqui (forward declaramos VexiModule mas abajo via traits).

        /// @brief Entry para la lista de simbolos en @c only.
        struct VexiOnlyEntry {
            std::string name;       // nombre original en el modulo
            std::string rename;     // nombre local (vacio = mismo)
        };

        /// @brief Resolver de typenames canonicos -> @c Type.  Re-parsea
        /// strings tipo @c "i32", @c "u64*", @c "Optional<i32>",
        /// @c "fn(i32) -> i64".  Devuelve @c Type{} (kind=VOID) si el
        /// typename es desconocido en el contexto actual.  Usado por el
        /// cargador de .vexi para resolver firmas de funciones y tipos
        /// de fields cross-module.
        Type resolve_type_string(const std::string &type_str) const;

        /// @brief Acceso const al mapa de aliases (transparente + newtype).
        const std::unordered_map<std::string, Type> &type_aliases() const noexcept {
            return type_aliases_;
        }

        /// @brief Map de nombre -> sig_index (para iterar funciones top-level).
        /// Usado por el emitter del .vexi para enumerar funciones con
        /// sus firmas sin necesidad de mantener un getter por funcion.
        const std::unordered_map<std::string, uint32_t> &function_names() const noexcept {
            return sig_by_name_;
        }

        /// @brief Reserva un nominal_id univoco para un nuevo newtype
        /// (cuando se inyecta desde .vexi).  Cada llamada devuelve un id
        /// distinto.  No tiene efectos secundarios.
        uint32_t allocate_nominal_id() noexcept {
            return ++newtype_counter_;
        }

        // -- Registradores usados solo por el cargador de .vexi (M2.d).
        // Insertan en las tablas internas SIN re-validar (asumen que el
        // .vexi de origen es valido).  Si el nombre ya existe, no se
        // sobreescribe (idempotente).

        void register_imported_type_alias(const std::string &name, Type t) {
            type_aliases_.emplace(name, std::move(t));
        }
        void register_imported_newtype(const std::string &name, Type underlying) {
            newtype_underlying_.emplace(name, std::move(underlying));
        }
        /// Phase M.L8: registra el bloque @c {explicit from/to T;} de un
        /// newtype importado.  Las conversiones se almacenan en
        /// @c newtype_info_ y participan en @c check_cast como
        /// allow-list para casts cross-module.
        void register_imported_newtype_info(const std::string &name,
                                             NewtypeInfo ni) {
            newtype_info_.emplace(name, std::move(ni));
        }
        void register_imported_struct(const std::string &name, StructLayout L) {
            // Phase M.fix-classfield: overwrite (no emplace) para soportar
            // pre-registro de skeleton seguido de fill cross-type within
            // mismo dep modulo.
            struct_layouts_[name] = std::move(L);
        }
        void register_imported_class(const std::string &name, ClassLayout L) {
            class_layouts_[name] = std::move(L);
        }
        void register_imported_enum(const std::string &name, EnumLayout L) {
            enum_layouts_[name] = std::move(L);
        }
        void register_imported_function(const std::string &name, FunctionSig sig) {
            const uint32_t idx = static_cast<uint32_t>(function_sigs_.size());
            function_sigs_.push_back(std::move(sig));
            sig_by_name_.emplace(name, idx);
            // Encolar para que `run()` declare el Symbol en el scope global
            // tras el push_scope inicial.  Sin esto, el lookup en
            // `lookup_with_depth` no encuentra la funcion importada.
            pending_imported_fn_names_.push_back({name, idx});
        }
        /// Phase M.L7: registra una variable global importada de otro
        /// modulo via @c .vexi.  Igual que las funciones, se encola para
        /// que @c run() la declare en el scope global tras el
        /// @c push_scope inicial.  Si @c has_init_value es @c true y la
        /// global es @c const , el lowering del consumidor puede
        /// inline-ar el valor literal.
        void register_imported_global(const std::string &name,
                                      Type type,
                                      bool is_const,
                                      bool has_init_value = false,
                                      uint64_t init_value = 0) {
            PendingGlobal pg;
            pg.name           = name;
            pg.type           = std::move(type);
            pg.is_const       = is_const;
            pg.has_init_value = has_init_value;
            pg.init_value     = init_value;
            pending_imported_globals_.push_back(std::move(pg));
        }

        /// v4: registra un comptime const string importado cross-module.
        /// El lowering puede materializar el StringObject al primer uso via
        /// `STRMAKE` con los bytes guardados en @c imported_global_consts_.
        void register_imported_global_str(const std::string &name,
                                            Type type,
                                            std::string str_value) {
            PendingGlobal pg;
            pg.name      = name;
            pg.type      = std::move(type);
            pg.is_const  = true;
            pg.is_str    = true;
            pg.str_value = std::move(str_value);
            pending_imported_globals_.push_back(std::move(pg));
        }

        /// Phase M.L7: tabla de globals const importadas con valor
        /// literal embedded.  El lowering la consulta en @c lower_ident
        /// para inline-ar `CONST <value>` directamente cuando el ident
        /// resuelve a uno de estos nombres.  Poblada al final de run()
        /// drenando @c pending_imported_globals_.  El struct esta
        /// declarado arriba en la primera seccion public anidada.
        const std::unordered_map<std::string, ImportedGlobalConst> &
        imported_global_consts() const noexcept {
            return imported_global_consts_;
        }

        /// Phase M6.a L.3: setea visibilidad de un simbolo top-level.
        /// El TypeChecker llama esto al procesar cada decl.
        void set_function_visibility(const std::string &name, bool is_pub) {
            function_is_public_[name] = is_pub;
        }
        void set_global_visibility(const std::string &name, bool is_pub) {
            global_is_public_[name] = is_pub;
        }
        void set_typedef_visibility(const std::string &name, bool is_pub) {
            typedef_is_public_[name] = is_pub;
        }
        /// Acceso de solo lectura.  @c true si el simbolo es publico (o
        /// no fue registrado; default permisivo).
        bool is_function_public(const std::string &name) const noexcept {
            auto it = function_is_public_.find(name);
            return (it == function_is_public_.end()) || it->second;
        }
        bool is_global_public(const std::string &name) const noexcept {
            auto it = global_is_public_.find(name);
            return (it == global_is_public_.end()) || it->second;
        }
        bool is_typedef_public(const std::string &name) const noexcept {
            auto it = typedef_is_public_.find(name);
            return (it == typedef_is_public_.end()) || it->second;
        }

    private:

        // Tabla paralela a @c monomorphized_ que ademas guarda el
        // template_name + lista legible de type_args.  Util para
        // pasar al IR y para tools.
        std::unordered_map<std::string, MonomorphInfo> monomorph_info_;

        // Nombre de la clase contenedora durante la verificacion de un
        // metodo de instancia (vacio fuera de un metodo).  Lo usa
        // check_this y la resolucion de nombres no calificados que
        // refieren a campos/metodos de la propia clase.
        std::string current_class_;

        // Flag activo cuando el metodo en chequeo es static.  Se usa para
        // rechazar @c this dentro de su body con un mensaje claro.
        bool current_method_is_static_ = false;

        // tipo de retorno de la funcion / metodo en chequeo.
        // Lo usa @c check_match para que los @c return dentro de las
        // arms del match validen contra el return type real (no contra
        // VOID).  Se settea al entrar en @c check_function /
        // @c check_class_method y se restaura al salir.
        Type current_fn_return_type_{PrimitiveKind::VOID};

        // Bug fix 2026-05-23 (audit optres infer): tipo Result<V,E> esperado
        // en el contexto actual (caller del check_expr).  Se setea en
        // check_return / check_var_decl antes de check_expr cuando el destino
        // es un Result<V,E>; @c check_call lo consulta al ver Ok(v) o Err(e)
        // y usa V/E del contexto en lugar de los placeholders (Result<V, i64>
        // / Result<i64, E>).  Resuelve `return Err("lit")` con string E y
        // `Ok(i32)` que se promocionaria a i64 sin contexto.
        Type expected_result_type_{};
        // Idem para Optional<T>: cuando el destino es Optional<T>, propagar
        // T al Some(v).
        Type expected_optional_type_{};

        /// BugFix R8: indica si estamos en el body de un @Macro.  Cuando
        /// es true, check_var_decl trata las var-decls como
        /// `comptime const` automaticamente para que los IdentExpr
        /// posteriores sean resoluble por el comptime evaluator.
        bool current_fn_is_macro_ = false;

        // Conteo de errores al inicio del run() para detectar exito.
        size_t initial_errors_ = 0;

        /**
         * @brief contexto activo por cada lambda anidada.
         *
         * Cuando se entra a chequear el body de una @c LambdaExpr,
         * empujamos un @c LambdaCtx que registra (a) el puntero al
         * @c LambdaExpr para acumular captures y (b) el numero de scopes
         * que existian ANTES de la lambda (su @c outer_depth).
         *
         * Cualquier @c IdentExpr resuelto en check_ident comprueba: si la
         * lambda esta activa y el indice del scope donde resolvio es
         * @c < outer_depth, entonces ese identificador es una variable
         * del entorno exterior y debe capturarse.  Lo registramos en
         * @c LambdaExpr::captures (sin duplicados) y guardamos su tipo
         * en @c LambdaExpr::capture_types para que el lowering decida el
         * ancho del LOAD/STORE en el env block.
         *
         * Anidamiento: dos lambdas anidadas producen dos entradas en el
         * stack.  La lambda interior captura del depth exterior a la
         * suya, no del global, asi que cada nivel mantiene su propio
         * outer_depth.  Captures transitivas (la lambda interior usa una
         * captura de la exterior) se manejan por composicion: cuando la
         * lambda interior captura, agrega el nombre a su lista; cuando
         * la lambda exterior se chequea, ese nombre tambien se resolvera
         * via captures de la exterior si era ajeno a su scope.
         */
        struct LambdaCtx {
            ast::LambdaExpr *expr;        ///< Donde acumular captures.
            size_t           outer_depth; ///< scopes_.size() antes de push del scope de la lambda.
        };
        std::vector<LambdaCtx> lambda_stack_;
    };

} // namespace vex

#endif // VEX_TYPE_CHECKER_H
