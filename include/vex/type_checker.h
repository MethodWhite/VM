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
