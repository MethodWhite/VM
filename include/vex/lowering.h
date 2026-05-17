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
 * @file lowering.h
 * @brief Pase de bajada AST de Vex -> ir::IrModule (SSA).
 *
 * Subset cubierto en(intencionalmente reducido para validar el
 * pipeline end-to-end pronto):
 *
 *   - Declaracion de funciones top-level con parametros y retorno.
 *   - Variables locales con inicializador (UNA SOLA asignacion).
 *   - Expresiones aritmeticas, logicas, bitwise, comparacion.
 *   - if/else.
 *   - Llamadas a otras funciones del mismo modulo.
 *   - Recursion permitida.
 *
 * Diferido a hitos posteriores:
 *
 *   - while / for / do-while con estado mutable (requiere phi nodes en
 *     el bloque header, algoritmo de construccion de Braun et al.).
 *   - Asignacion a variables locales (idem: cada nueva asignacion crea
 *     un IrValueId distinto, hay que recolectar phi en bloques merge).
 *   - ++ y -- (variante del anterior).
 *   - Variables globales con estado mutable.
 *
 * Si el AST contiene una construccion no soportada, el lowering emite
 * un diagnostico explicito y aborta.  Esto evita generar IR incorrecto
 * silenciosamente.
 *
 * Decisiones de hardware:
 *   - Scope chain con std::vector<unordered_map>: lookup O(N_scopes)
 *     amortizado, dominado por la cache hot del scope actual.
 *   - Las constantes se rematerializan en cada uso (no se cachean).
 *     El optimizador IR (O1+) hace common subexpression elimination
 *     en la fase posterior, asi que duplicacion aqui es gratuita.
 *   - El lowering recorre el AST UNA sola vez; el type checker ya
 *     dejo el campo result_type relleno, evitando recomputos.
 */

#ifndef VEX_LOWERING_H
#define VEX_LOWERING_H

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ir/ssa_ir.h"
#include "vex/ast.h"
#include "vex/diagnostic.h"
#include "vex/type_checker.h"

namespace vex {

    /**
     * @class Lowering
     * @brief Convierte un ModuleNode AST en un ir::IrModule SSA.
     *
     * Uso:
     * @code
     *   Lowering low(mod, tc, diags);
     *   ir::IrModule irmod;
     *   if (low.run(irmod, "modname")) { ... usar irmod ... }
     * @endcode
     */
    class Lowering {
    public:
        /**
         * @brief Construye el lowering sobre un modulo AST y un diags.
         *
         * @param mod   AST a bajar.  El campo result_type de cada Expr
         *              debe estar relleno (ejecutar TypeChecker antes).
         * @param tc    TypeChecker que ya corrio sobre @p mod; se usa
         *              para consultar StructLayout y resolver tamanos
         *              de variables tipo struct.
         * @param diags Sumidero de errores.
         */
        Lowering(ast::ModuleNode &mod, const TypeChecker &tc, Diagnostics &diags);

        /**
         * @brief Ejecuta el pase y rellena @p out_module.
         *
         * @param out_module Modulo IR de salida.  Se sobreescriben todos
         *                   sus campos.
         * @param module_name Nombre logico del modulo (acaba en @c IrModule::name).
         * @return @c true si no hubo errores; el modulo es valido para emitir.
         */
        bool run(ir::IrModule &out_module, const std::string &module_name);

        /**
         * @brief Habilita instrumentacion para debugging.  Cuando esta
         * activa, el lowering emite @c CALLN sinteticas a
         * @c "vex_trace:enter" al inicio y a @c "vex_trace:exit" antes
         * de cada @c RET de cada funcion del usuario.  La instrumentacion
         * vive en el IR -> todos los backends (bytecode VM, JIT, port C,
         * futuros ports) la heredan automaticamente.
         *
         * @param mode @c "none" (default, sin instrumentacion),
         *             @c "trace" (enter/exit con nombre + depth),
         *             @c "profile" (timing per-funcion).
         */
        void set_instrument_mode(const std::string &mode) {
            instrument_mode_ = mode;
        }

    private:
        // -----------------------------------------------------------------
        // Helpers de tipo y constante.
        // -----------------------------------------------------------------

        /**
         * @brief Convierte un PrimitiveKind del frontend a ir::IrType.
         */
        static ir::IrType ir_type_from_primitive(PrimitiveKind p) noexcept;

        /**
         * @brief Genera una instruccion CONST en el bloque actual.
         */
        ir::IrValueId emit_const(ir::IrType t, uint64_t imm, uint32_t source_line);

        /**
         * @brief Emite GETPROC y devuelve el SSA value (PTR al ProcessVM).
         *        Usado por las variantes GC-aware de las colecciones (A.30).
         */
        ir::IrValueId emit_getproc(uint32_t source_line);

        /**
         * @brief Inserta una conversion de tipo si difiere; identidad si igual.
         *
         * @param is_explicit Si false (por defecto), emite un warning cuando
         *        la conversion es potencialmente perdida (narrowing,
         *        float<->int, etc.).  Las llamadas desde @c lower_cast_expr
         *        pasan true para silenciar el warning porque el usuario
         *        opto explicitamente por el cast.
         */
        ir::IrValueId cast_if_needed(ir::IrValueId v, ir::IrType from, ir::IrType to,
                                     uint32_t source_line,
                                     bool is_explicit = false);

        // -----------------------------------------------------------------
        // Lowering por categoria.
        // -----------------------------------------------------------------

        void           lower_function(ast::FunctionDecl *fd, ir::IrModule &out);

        /**
         * @brief Lowering de funciones marcadas con @Async.
         *
         * Genera DOS funciones IR a partir de la `FunctionDecl @Async`:
         *
         *   1. Wrapper publico con el nombre original (`fd->name`):
         *      - alloca un Future via @c future_alloc()
         *      - spawnea un child con la funcion sintetica `__async_<name>`
         *      - msgsend(child, fut_handle)
         *      - return fut_handle (i64)
         *
         *   2. Spawn helper sintetica `__async_<name>`:
         *      - msgrecv() -> handle del Future en SSA value (`async_fut_id_`)
         *      - body lowered del usuario; cada `return X` se intercepta en
         *        @c lower_return: emite `fulfill(async_fut_id_, X) + hlt`
         *        en lugar de RET.
         *      - Si el body cae naturalmente sin return: emite
         *        `fulfill(async_fut_id_, 0) + hlt` al final.
         *
         * El usuario llama `i64 fut = fn(); i64 r = await fut;` con la
         * misma firma que el wrapper expone.
         */
        void           lower_async_function(ast::FunctionDecl *fd, ir::IrModule &out);
        void           lower_block(ast::BlockStmt *b);
        void           lower_stmt(ast::Stmt *s);
        void           lower_var_decl(ast::VarDeclStmt *vd);
        void           lower_if(ast::IfStmt *s);
        void           lower_return(ast::ReturnStmt *s);
        void           lower_while(ast::WhileStmt *s);
        void           lower_do_while(ast::DoWhileStmt *s);
        void           lower_for(ast::ForStmt *s);
        void           lower_try(ast::TryStmt *s);
        void           lower_throw(ast::ThrowStmt *s);
        void           lower_foreach(ast::ForEachStmt *s);
        void           lower_synchronized(ast::SynchronizedStmt *s);

        ir::IrValueId  lower_expr(ast::Expr *e);
        ir::IrValueId  lower_binary(ast::BinaryExpr *e);
        ir::IrValueId  lower_unary(ast::UnaryExpr *e);
        ir::IrValueId  lower_call(ast::CallExpr *e);
        ir::IrValueId  lower_ident(ast::IdentExpr *e);
        ir::IrValueId  lower_string_lit(ast::StringLitExpr *e);
        ir::IrValueId  lower_assign(ast::AssignExpr *e);
        ir::IrValueId  lower_field_access(ast::FieldAccessExpr *e);
        ir::IrValueId  lower_index(ast::IndexExpr *e);

        /**
         * @brief Lowering de @c spawn @c { @c body @c }.
         *
         * Genera una funcion sintetica de nombre @c __spawn_<N>(void) que
         * contiene el body, terminada con @c hlt (no @c ret) porque los
         * procesos hijo terminan, no retornan.  En el sitio del spawn,
         * emite @c mov @c rN, @c @Absolute("code.__spawn_N") + @c spawn @c rN
         * y captura el PID encoded del hijo (que el opcode deposita en R0).
         *
         * @return SSA value con el PID encoded del hijo (i64).
         */
        ir::IrValueId  lower_spawn_expr(ast::SpawnExpr *e);

        /**
         * @brief Genera la funcion sintetica del body de un @c spawn.
         *
         * Compila @p body como funcion top-level con nombre
         * @c __spawn_<N> donde N es @c spawn_func_counter_++.  Termina
         * con @c hlt.  La nueva @c IrFunction se agrega al modulo via
         * @c out_mod_->add_function.
         *
         * @return Nombre de la funcion generada (para usar en @c @Absolute).
         */
        std::string    generate_spawn_helper(ast::BlockStmt *body, const SourceLoc &loc);

        /**
         * @brief lowering de @c rspawn(node) { body }.
         *
         * Genera helper @c __rspawn_<N> con `is_rspawn_body_=true` (cualquier
         * @c return X dentro del body se intercepta -> @c mov r0, X + hlt).
         * En el caller emite la instruccion bytecode @c rspawn r_fn, r_node y
         * captura R0 al SSA value (GcHandle del Future).
         *
         * @return SSA value con el GcHandle del Future (i64).
         */
        ir::IrValueId  lower_rspawn_expr(ast::RSpawnExpr *e);

        /**
         * @brief genera la funcion sintetica del body de un @c rspawn.
         *
         * Identico a @c generate_spawn_helper salvo por el flag
         * @c is_rspawn_body_ activado durante el lowering del body.  El name
         * es @c __rspawn_<N>.
         */
        std::string    generate_rspawn_helper(ast::BlockStmt *body, const SourceLoc &loc);

        /**
         * @brief closures: lowering de una @c LambdaExpr inline.
         *
         * Pipeline de tres fases:
         *   1. Genera la funcion sintetica @c __lambda_<N> via
         *      @c generate_lambda_helper, que toma los params declarados
         *      como parametros normales y los captures como prologue
         *      implicito que carga desde @c R14 + offset.
         *   2. Aloca un env block en la pila del caller (@c subsp rsp,
         *      8*nCaptures) y escribe los valores capturados ahi.  Si la
         *      lambda no captura nada (n=0), env_addr = 0 (sentinela).
         *   3. Aloca un slot de 16 bytes para el "function value" tipo
         *      @c fn(T) -> R: `[+0 fn_addr][+8 env_addr]`.  Devuelve el
         *      SSA value con la direccion de ese slot.
         *
         * Coste: con N captures, la creacion de la closure cuesta:
         *   - 1 ALLOCA de @c 8*N bytes (env block)
         *   - N STOREs (un qword por captura)
         *   - 1 ALLOCA de 16 bytes (function value slot)
         *   - 1 RAW_ASM @c mov rN, @c \@Absolute(label)
         *   - 2 STOREs (fn_addr + env_addr en el slot)
         * Total: ~5 + 2N instrucciones bytecode.  Cero allocaciones de
         * heap; cero overhead GC.
         *
         * Limitacion MVP: el env vive en la pila del caller, asi que la
         * closure solo es valida dentro del scope que la creo.  Para
         * closures que escapen (e.g. devolver una closure de una funcion)
         * habra que detectar el escape y promover el env a heap GC.
         *
         * @return SSA value con la direccion del function value (16 bytes).
         */
        ir::IrValueId  lower_lambda_expr(ast::LambdaExpr *e);

        /**
         * @brief construye un function value (16 bytes) que
         *        apunta a una funcion top-level con env_addr=0.
         *
         * Mismo layout que @c lower_lambda_expr (slot de 16 bytes en
         * stack: fn_addr en +0, env_addr en +8) pero @c fn_addr es
         * @c @Absolute("code.<fn_name>") y env_addr es la constante 0.
         * Permite pasar funciones declaradas en top-level como argumento
         * a parametros tipados @c fn(...) -> ... sin requerir el wrapping
         * manual a una lambda local.
         *
         * @param fn_name Nombre de la funcion top-level.
         * @param line    Linea fuente para diagnostico.
         * @return SSA value PTR con la direccion del slot de 16 bytes.
         */
        ir::IrValueId  emit_topfn_value(const std::string &fn_name, int line);

        /**
         * @brief Promueve un string literal a @c StringObject GC-managed.
         *
         * Emite la secuencia: STR_LIT_ADDR -> SSA value PTR; CONST(len) ->
         * SSA value I64; RAW_ASM "strmake {dst}, {src0}, {src1}" -> SSA
         * value I64 (GcHandle).  El handle se devuelve y puede bindearse
         * a una variable de tipo @c string.  Cero overhead vs el modelo
         * antiguo cuando NO se promueve (literal sigue siendo PTR puro).
         *
         * @param slit  Literal de string sin interpolacion.
         * @return SSA value I64 con el GcHandle del StringObject.
         */
        ir::IrValueId  lower_string_literal_to_string_object(
                                ast::StringLitExpr *slit);

        /**
         * @brief closures: genera la @c IrFunction sintetica para
         *        el body de una lambda.
         *
         * El helper se llama @c __lambda_<N> donde N es @c lambda_counter_++.
         * Su firma es la de la lambda (mismos params), pero el body lleva
         * un prologue que carga cada captura desde @c [r14 + 8*i] a un
         * SSA value local con el nombre del capture.  De esa forma el
         * resto del body (que el type checker validO referencias a
         * capturas como si fueran locales) puede usar las capturas
         * naturalmente sin distinguirlas de los params.
         *
         * Reusa el mismo patron de save/restore de scope que
         * @c generate_spawn_helper.
         *
         * @return Nombre del helper generado (para @c \@Absolute).
         */
        std::string    generate_lambda_helper(ast::LambdaExpr *e);

        /**
         * @brief ADTs: lowering de un constructor de variante de
         *        enum (`Color.Green(42)` o `Color.Red`).
         *
         * Estrategia:
         *   1. Aloca un slot en pila del caller con
         *      @c size_bytes = 8 + 8*max_payload_fields del enum.  Igual
         *      que para structs, usamos ALLOCA del IR -> @c subsp rsp, N.
         *   2. STORE i64 del @c tag en offset 0.
         *   3. Para cada payload arg: STORE i64 del valor en offset
         *      @c 8 + 8*i.  El valor se promociona a i64 si es mas
         *      estrecho (uniformidad del slot).
         *   4. Devuelve el SSA value con la direccion del slot (PTR).
         *
         * El call site del lowering reconoce este patron mirando
         * @c FieldAccessExpr::property_kind == 99 (marcado por el type
         * checker).  Para variantes sin payload se entra con
         * @c CallExpr::args vacio o directamente desde un FieldAccessExpr.
         *
         * @param enum_name      Nombre del enum (e.g. "Color").
         * @param variant_name   Nombre de la variante (e.g. "Green").
         * @param args           Args del CallExpr (vacio si sin payload).
         * @param loc            Para diagnosticos / metadata IR.
         * @return SSA value PTR al slot del enum recien construido.
         */
        ir::IrValueId  lower_enum_constructor(const std::string &enum_name,
                                              const std::string &variant_name,
                                              const std::vector<std::unique_ptr<ast::Expr>> &args,
                                              const SourceLoc &loc);

        /**
         * @brief ADTs: lowering de un @c MatchExpr.
         *
         * Estrategia:
         *   1. Lower del scrutinee -> SSA value PTR al slot del enum.
         *   2. LOAD i64 del tag (offset 0).
         *   3. Construye en stack una jumptable: array de @c uint64
         *      (8 bytes/entry) con la direccion de cada arm en orden de
         *      tag.  Para variantes no cubiertas en el match, la entry
         *      es la direccion del arm @c _ (default) o la de la
         *      siguiente instruccion tras el match (fall-through) si no
         *      hay default.
         *   4. RAW_ASM @c jumptable r_tag, r_table, count -> dispatch O(1).
         *   5. Para cada arm: emite un nuevo IrBlock que (a) lee los
         *      payloads del slot del scrutinee como SSA values nuevos,
         *      (b) los bindea con los nombres del patron, (c) lowers el
         *      body, (d) salta al merge_block.
         *   6. merge_block: continuacion tras el match.
         *
         * MVP simplificado: en lugar de @c jumptable bytecode (que
         * requiere ALLOCA + escritura de la tabla en stack + cuidado
         * con relocations), usamos una cadena de @c cmp + @c jmp.jeq al
         * label de cada arm.  Es O(N) en el numero de variantes pero
         * mas simple y robusto para empezar.  Optimizacion a jumptable
         * 0x27 queda como mejora futura (importante para enums grandes).
         *
         * @return @c IR_NO_VALUE (match es statement-like en MVP).
         */
        ir::IrValueId  lower_match_expr(ast::MatchExpr *e);

        /**
         * @brief Lower @c CastExpr `(T) operand`.
         *
         * Casos cubiertos:
         *  - num <-> num (primitivos): delega en @c cast_if_needed.
         *  - PTR <-> PTR (incluyendo @c VirtualPtr<X> <-> @c X*): bitcast,
         *    el valor SSA mantiene el bit-pattern, pero @c is_host_ptr y
         *    @c pointee_is_host_ptr se ajustan al destino para que LOAD
         *    y STORE posteriores emitan @c mov vs @c movh segun la
         *    naturaleza del destino.
         *  - PTR <-> int (i64/u64) y viceversa: BITCAST IR op (preserva
         *    bits sin conversion numerica).
         *  - ARRAY -> PTR: decay (mismo bit-pattern); ARRAY <-> ARRAY:
         *    bitcast con propagacion de @c is_host_ptr.
         */
        ir::IrValueId  lower_cast_expr(ast::CastExpr *e);

        // -----------------------------------------------------------------
        // POO: clases, new, this, getfield/setfield/callvirt sobre CLASS.
        // -----------------------------------------------------------------

        /**
         * @brief Compila los metodos de una clase como funciones IR
         *        independientes con nombre @c <Class>__<method> y un
         *        primer parametro implicito @c this de tipo PTR.
         *
         * Ademas registra en out_module la metadata necesaria para que
         * el module init pueda llamar @c defmethod con la direccion del
         * metodo (via label).
         */
        void           lower_class_methods(ast::ClassDecl *cd, ir::IrModule &out);

        /**
         * @brief Genera el bloque @c __module_init que registra todas las
         *        clases del modulo.  Se invoca al inicio de @c main.
         *
         * Construye la cadena RAW_ASM con la secuencia
         * @c findclass / @c defclass / @c deffield / @c defmethod usando
         * convenciones de registro fijas (r12-r15 reservados).  Los
         * nombres de clase/field/method se registran como bytes estaticos
         * via @c IrModule::intern_static_data.
         */
        std::string    build_module_init_asm(ir::IrModule &out_module);

        /**
         * @brief Lower de @c new ClassName(args) -> CALL a la funcion
         *        auxiliar @c __new_<ClassName> generada por el frontend.
         *        Esa funcion encapsula findclass + newobj + callvirt 0.
         */
        ir::IrValueId  lower_new_expr(ast::NewExpr *e);

        /**
         * @brief Genera la IrFunction auxiliar @c __new_<Class>(args) para
         *        cada clase declarada en el modulo y la anyade a out.
         *        El cuerpo es un bloque RAW_ASM con findclass + newobj +
         *        callvirt 0 (ctor) + return GcHandle.
         */
        void           generate_new_helpers(ir::IrModule &out);

        /**
         * @brief Genera la IrFunction @c __module_init que registra todas
         *        las clases del modulo via defclass / deffield / defmethod.
         */
        void           generate_module_init_function(ir::IrModule &out);

        /**
         * @brief Exporta @c TypeChecker::class_layouts_ al @c IrModule::classes.
         *
         * Convierte el modelo interno del frontend Vex a la representacion
         * portable del IR.  Cada @c ClassLayout produce un @c ir::IrClass
         * con sus fields/methods/super/interfaces.  Esta info la consumen
         * los transpilers (port-C, port-Java, ...) para emitir POO eficiente
         * sin tener que reconstruir el modelo desde @c __module_init.
         *
         * Clases @c is_runtime_predefined (FatalError, etc.) se omiten:
         * el runtime las define y los transpilers no deben re-emitirlas.
         */
        void           export_classes_to_ir(ir::IrModule &out);

        /**
         * @brief Lower de @c this -> primer parametro del metodo en curso.
         */
        ir::IrValueId  lower_this_expr(ast::ThisExpr *e);

        /**
         * @brief Lower de @c obj.field (lectura) cuando @c obj es CLASS.
         *        Emite GETFIELD con el offset del ClassLayout.
         */
        ir::IrValueId  lower_class_field_load(ast::FieldAccessExpr *e);

        /**
         * @brief Lower de @c obj.field = v cuando @c obj es CLASS.
         *        Emite SETFIELD.
         */
        ir::IrValueId  lower_class_field_store(ast::FieldAccessExpr *target,
                                                ir::IrValueId rhs,
                                                const SourceLoc &loc);

        /**
         * @brief Lower de @c obj.method(args) cuando @c obj es CLASS.
         *        Emite CALLVIRT con el indice del metodo en la vtable.
         */
        ir::IrValueId  lower_class_method_call(ast::CallExpr *e);

        /**
         * @brief Calcula el puntero al elemento indexado (base + i*sizeof(*base)).
         *
         * Helper compartido por @c lower_index (lectura) y la rama IndexExpr
         * de @c lower_assign (escritura).  Devuelve un IrValueId tipo PTR.
         */
        ir::IrValueId  lower_index_addr(ast::IndexExpr *e);

        /**
         * @brief Tamano en bytes del tipo Vex (consulta layout para STRUCT).
         *
         * @return Tamano del tipo, o 0 si no se puede determinar (e.g. void
         *         o struct desconocido).
         */
        size_t         size_of_type(const Type &t) const;

        /**
         * @brief Calcula el IrValueId del puntero al campo @c e->field_name.
         *
         * Helper compartido por @c lower_field_access (lectura) y por la
         * rama FieldAccessExpr de @c lower_assign (escritura).  Suma el
         * offset del campo (resuelto via @c TypeChecker::struct_layouts())
         * al puntero base del struct.
         *
         * @return IrValueId de tipo @c PTR apuntando al campo, o
         *         @c IR_NO_VALUE si la base no era un struct valido.
         */
        ir::IrValueId  lower_field_addr(ast::FieldAccessExpr *e);

        /**
         * @brief Construye un IrInstr binario en el bloque actual.
         *
         * Helper compartido por lower_binary() y lower_assign() (compound
         * assignments).  Decide el opcode por categoria flotante/integral y
         * con/sin signo, alineando el comportamiento entre asignaciones
         * compuestas y operaciones binarias normales.
         *
         * @param op       Opcode aritmetico/bitwise (mapeado a ADD/FADD/AND/etc.).
         * @param lhs_val  Valor izquierdo (ya bajado).
         * @param rhs_val  Valor derecho (ya bajado).
         * @param common   Tipo comun resultante (ya promovido).
         * @param loc      SourceLoc para anotacion de linea.
         * @return IrValueId del resultado.
         */
        ir::IrValueId  emit_binop_ir(ast::BinOp op, ir::IrValueId lhs_val,
                                      ir::IrValueId rhs_val, PrimitiveKind common,
                                      const SourceLoc &loc);

        /**
         * @brief Si @p name es un builtin (println, print) emite la llamada
         *        FFI correspondiente a vesta_io y devuelve el IrValueId del
         *        retorno (IR_NO_VALUE si la firma es void).
         *
         * Devuelve un valor distinto de "false" mediante un puntero al
         * IrValueId asignado solo cuando se reconocio el builtin; en otro
         * caso devuelve @c std::nullopt.  Los argumentos ya estan en el AST
         * no bajados; el helper se encarga de emitir todo (inclusive la
         * pre-bajada de cada arg y el registro de los datos estaticos / imports).
         *
         * @return IR_NO_VALUE si el builtin coincide y se emitio (caso typico
         *         println/print devuelven void) o un IrValueId valido si en
         *         el futuro hay builtins que devuelven valor.  Devuelve
         *         IR_NO_VALUE - 1 (centinela) si el nombre NO es un builtin
         *         y el caller debe seguir con la ruta normal de CALL.
         */
        bool try_lower_builtin_call(ast::CallExpr *e, ir::IrValueId &out_value);

        // -----------------------------------------------------------------
        // Tabla de simbolos para variables locales y parametros.
        // -----------------------------------------------------------------

        void           push_scope();
        void           pop_scope();
        void           bind(const std::string &name, ir::IrValueId v);
        ir::IrValueId  lookup(const std::string &name) const;

        /**
         * @brief Actualiza el valor SSA asociado a @p name en el scope mas
         *        cercano que ya lo tenga definido.
         *
         * Si la variable no existe en ningun scope, ejecuta @c bind() en el
         * scope mas interno como fallback (no deberia ocurrir si el type
         * checker corrio antes; protege en caso de programas malformados).
         *
         * Es la primitiva basica del modelo SSA-construction de Braun:
         * para cada asignacion `x = e` anotamos un nuevo IrValueId como
         * "current value de x" en el scope donde x esta declarada.  Al
         * leer x mas tarde, lookup() devolvera ese ultimo valor.  Los
         * loops insertan PHI nodes en sus bloques header al sellar.
         */
        void           update_scope(const std::string &name, ir::IrValueId v);

        /**
         * @brief Recorre el cuerpo de una funcion buscando @c &x donde x es
         *        un IdentExpr local, y rellena @c address_taken_locals_.
         *
         * Se llama una vez al inicio de @c lower_function antes de bajar el
         * body.  El conjunto resultante guia las decisiones de @c lower_var_decl
         * (ALLOCA en lugar de SSA), @c read_local y @c write_local
         * (LOAD/STORE en lugar de scope-update).
         */
        void           scan_address_taken(ast::Stmt *s);

        /**
         * @brief Lectura de una variable local respetando promocion address-taken.
         *
         * Si @p name esta marcada como address-taken (@c address_taken_locals_),
         * emite un LOAD desde la direccion guardada en scope.  En caso
         * contrario devuelve el IrValueId SSA actual (lookup directo).
         *
         * @param name        Nombre de la variable.
         * @param ir_ty       Tipo IR esperado (para el LOAD; usado solo si
         *                    address-taken).
         * @param source_line Linea fuente para anotar la instruccion.
         * @return IrValueId con el valor leido.  IR_NO_VALUE si la variable
         *         no esta declarada.
         */
        ir::IrValueId  read_local(const std::string &name, ir::IrType ir_ty,
                                  uint32_t source_line);

        /**
         * @brief Escritura a una variable local respetando promocion address-taken.
         *
         * Si address-taken: emite STORE a la direccion del ALLOCA y deja
         * el value SSA del scope intacto (sigue apuntando a la addr).
         * Si no address-taken: actualiza el scope con el nuevo IrValueId.
         */
        void           write_local(const std::string &name, ir::IrValueId v,
                                   ir::IrType ir_ty, uint32_t source_line);

        // Helper para reportar features no soportadas en A.1.
        void           unsupported(SourceLoc loc, const char *feature);

        void           error_at(SourceLoc loc, std::string msg);

        // -----------------------------------------------------------------
        // Datos.
        // -----------------------------------------------------------------

        ast::ModuleNode    &mod_;
        const TypeChecker  &tc_;
        Diagnostics        &diags_;

        // Nombre del fichero fuente actual (para warnings que solo
        // tienen un source_line).  Se infiere del primer AST node con
        // loc no vacio durante run() y se mantiene durante todo el
        // lowering del modulo.  Si no se puede inferir, queda vacio
        // y los warnings se imprimen sin prefijo de fichero (siguen
        // siendo utiles porque contienen line:col).
        std::string current_file_;

        // Estado por funcion en curso.
        ir::IrModule   *out_mod_       = nullptr;  ///< Modulo IR de salida (para static_data e imports).
        ir::IrFunction *fn_            = nullptr;  ///< Funcion en construccion.
        ir::IrBlockId   current_block_ = ir::IR_NO_BLOCK; ///< Bloque actual donde insertar.
        bool            block_terminated_ = false; ///< true si current_block_ ya tiene terminador.

        // Tabla de simbolos local: cada scope mapea nombre -> IrValueId.
        std::vector<std::unordered_map<std::string, ir::IrValueId>> scopes_;

        // Para CALL necesitamos saber el tipo de retorno de cada funcion;
        // el type checker ya valido las llamadas, asi que aqui solo
        // mantenemos un cache nombre -> IrType.
        std::unordered_map<std::string, ir::IrType> fn_return_types_;

        /// FFI declarativo: nombre de funcion -> libreria nativa
        /// (e.g. "user32.dll", "kernel32.dll" o "stdlib/native/io/vesta_io").
        /// Se llena en el pase 1 al recorrer @c ExternFnDecl.  Si una entrada
        /// existe para el callee de @c lower_call, emitimos
        /// @c CALLN @Method("<lib>:<name>") con args en R1..RN y registramos
        /// el import via @c out_mod_->register_native_import.  Sin entry,
        /// el flujo normal CALLVM (funcion Vex local) sigue intacto.
        std::unordered_map<std::string, std::string> extern_lib_by_fn_name_;

        /// ADTs: nombre del enum que la funcion retorna (vacio si
        /// no retorna enum declarado).  Se usa en lower_call para
        /// alocar el retbuf con @c enum_layouts_[name].size_bytes y en
        /// lower_function para configurar @c sret_active_/@c sret_buf_size_.
        std::unordered_map<std::string, std::string> fn_ret_enum_name_;

        /// (gap O cerrado): conjunto de funciones que retornan un
        /// valor de tipo FUNCTION (function value).  Se trata como SRET
        /// con buf_size=16 (mismo layout que el slot de lambda: fn_addr
        /// en +0, env_addr en +8).  El env block apuntado por env_addr
        /// se aloca via RAW_ALLOC (heap) en lugar de ALLOCA (stack)
        /// cuando estamos dentro de una de estas funciones, asi el env
        /// sobrevive al RET y la closure retornada es invocable por el
        /// caller sin use-after-free.
        std::unordered_set<std::string> fn_returns_function_;

        /// Funciones cuyo return type es @c unique<T> o @c shared<T>.
        /// Igual que @c fn_returns_function_ pero con buffer SRET de 8
        /// bytes (slot del smart pointer).  El @c return p en el body
        /// copia los 8 bytes del slot local al retbuf del caller; el
        /// caller bindea la variable receptora directamente al retbuf,
        /// donde ya viven los datos correctos.  Cleanup del local NO
        /// se emite (escape detection lo detecta via "return ident"),
        /// pero el caller registra cleanup sobre el retbuf.
        std::unordered_set<std::string> fn_returns_smartptr_;

        /// indicador activo durante el lowering del body de una
        /// funcion que retorna FUNCTION.  Disparado en @c lower_function
        /// y consultado por @c lower_lambda_expr para alocar el env
        /// block en heap raw en lugar de stack.  Se restaura al salir.
        bool current_fn_returns_function_ = false;

        /// Indica que la funcion actual declara devolver `string` a nivel
        /// fuente.  El IR tipo es I64 (handle a StringObject) por lo que
        /// el flag es necesario para auto-promover en `return "..."` el
        /// literal a StringObject via STRMAKE (mismo patron que
        /// `lower_var_decl` para `string s = "lit"`).  Se restaura al
        /// salir de `lower_function`.
        bool current_fn_returns_string_ = false;

        /// true si la funcion actual contiene algun `try { } catch`
        /// statement.  Se rellena con un pre-pase simple en lower_function.
        /// Cuando es true, NO emitimos el cleanup automatico release_handle
        /// para CLASS sin destructor: el bytecode tryenter/throw no preserva
        /// los GP regs, asi que un cleanup que lea un reg con el binding del
        /// local podria leer garbage si el catch handler corrio antes.
        /// Para funciones SIN try el cleanup es seguro y libera handles
        /// deterministicamente al exit del scope.
        bool current_fn_has_try_ = false;

        /// true si la funcion actual contiene algun loop
        /// (while/for/do-while).  Se rellena con un pre-pase en
        /// lower_function.  Cuando es true, el lower_var_decl marca
        /// AUTOMATICAMENTE como address-taken las vars CLASS / I64-GC
        /// declaradas en el top-level del body (depth scope <= 2).
        ///
        /// Razon: el regalloc puede clobbar el reg de una var GC del
        /// outer scope durante el body del loop (e.g. `newobj r1` reusa
        /// r1 que tenia `owned`).  Address-taken fuerza ALLOCA + STORE/
        /// LOAD por uso, garantizando que el binding sobreviva al loop.
        ///
        /// Esto permite que el cleanup CALL_DTOR / RAW_ASM al RET de la
        /// funcion lea del stack en lugar de un reg potencialmente
        /// corrupto.  Tambien habilita que el cleanup scope-local sea
        /// seguro.
        ///
        /// Coste: 1 STORE inicial + 1 LOAD por uso (~2 instr extra).
        /// Solo aplica a vars CLASS/GC en funciones con loops; el resto
        /// del codigo no tiene overhead.
        bool current_fn_has_loops_ = false;

        /// sret en call sites: cache nombre-de-funcion -> PrimitiveKind
        /// del tipo de retorno semantico (antes de la transformacion sret).
        /// Solo nos interesa distinguir OPTIONAL / RESULT del resto, porque
        /// solo esos dos kinds disparan el alocado del retbuf en el caller.
        /// Las claves que NO estan en el mapa o cuyo valor no sea
        /// OPTIONAL/RESULT corresponden a calls "normales" (la firma IR del
        /// callee usa @c fn_return_types_ y el resultado del CALL es el
        /// valor devuelto directamente).
        std::unordered_map<std::string, PrimitiveKind> fn_ret_kind_;

        /// Variables locales cuya direccion se ha tomado con '&' en alguna
        /// parte de la funcion actual.  Se rellena con scan_address_taken al
        /// inicio de lower_function y se limpia al terminarla.  Las entradas
        /// disparan ALLOCA en lower_var_decl y LOAD/STORE en read/write_local.
        std::unordered_set<std::string> address_taken_locals_;

        /// locales cuyo handle escapa del scope: se devuelven via
        /// @c return id, se asignan a un campo (@c this.x = id, @c obj.x = id,
        /// @c *p = id) o a un slot de array (@c arr[i] = id).  Para esos
        /// locales NO registramos auto-free al exit del scope porque el
        /// caller (o el padre) toma posesion del handle y debera liberarlo
        /// (o el GC lo gestionara cuando integremos GC roots en A.27.3+).
        ///
        /// Conservador: si algun uso del local podria escapar segun los
        /// patrones detectados por @c scan_escaping_locals, se marca como
        /// escaping y queda fuera del cleanup automatico.  Falsos positivos
        /// (escape detectado pero no real) producen un leak intencional
        /// que el programador debe liberar via dispose() explicito.
        std::unordered_set<std::string> escaping_locals_;

        /// pre-pase ejecutado al inicio de @c lower_function que
        /// rellena @c escaping_locals_ recorriendo el body.  Reusable como
        /// helper de futuras analizadores de escape mas precisas.
        void scan_escaping_locals(ast::Stmt *body);

        /// Bug D fix: propagar @c is_gc_object a traves de todos los PHI
        /// nodes de la funcion hasta punto fijo.  Llamado al final de cada
        /// lowering de funcion (top-level, class methods, helpers
        /// sintetizados) justo antes de @c IrModule::add_function.
        void propagate_is_gc_object_through_phis(ir::IrFunction &fn);

        /// Limitacion A (cerrada): subset de @c address_taken_locals_ cuyo
        /// contenido es un puntero a memoria HOST (resultado de malloc o
        /// derivado).  Lo registra @c write_local cada vez que el valor
        /// escrito tiene @c is_host_ptr = true.  @c read_local consulta el
        /// set para propagar el bit al SSA value resultante del LOAD; sin
        /// esto, un LOAD de un local address-taken con tipo @c T* siempre
        /// emite @c mov (memoria VM) y corrompe la heap host.
        ///
        /// Es un best-effort sticky: si el local pasa por una asignacion
        /// con @c is_host_ptr = true se queda marcado para siempre.
        /// Asignaciones posteriores con valores VM no lo desmarcan.  En
        /// la practica los locales mantienen su naturaleza host/VM a lo
        /// largo de su vida, asi que esta semantica conservadora basta.
        ///
        /// NO cubre el caso indirecto @c i32** pp = &p; **pp = v.  Para
        /// ese caso haria falta un bit @c pointee_is_host_ptr en
        /// IrValue propagado a traves de @c &x.  Documentado como gap
        /// remanente; requiere acuerdo de diseno antes de implementarse.
        std::unordered_set<std::string> host_bearing_locals_;

        /// Mapa de variables locales tipo `Class` cuyo origen es un
        /// `Class.forName("X")` con literal X.  Permite que el lowering
        /// de `cls.newInstance()` detecte la clase concreta en compile
        /// time y emita `new X()` (que SI llama al constructor via
        /// `__new_<X>` synthetic) en lugar del NEWOBJ raw que no llama
        /// al ctor.  Coste: cero (el helper `__new_<X>` ya existia).
        ///
        /// Llave: nombre del local Class.  Valor: nombre de la clase X
        /// que el local apunta.  Las re-asignaciones desde fuentes no
        /// trackeable (e.g. `cls = otroFn()`) borran la entrada.
        std::unordered_map<std::string, std::string> class_origin_of_local_;

        /// Mapa SSA value -> clase concreta cuando el valor proviene de un
        /// @c new Class() (o cadena MOV/PHI desde ese origen).  Permite
        /// devirtualizar en tiempo de compilacion las llamadas via
        /// interface receiver cuando el tipo concreto es estaticamente
        /// conocido: el dispatch baja a CALLVIRT directo con el vtable_idx
        /// del metodo en la CLASE concreta, sin necesidad del trio
        /// findclass+findmethod+callm runtime.
        ///
        /// Coste: cero runtime; ~50 LOC en el lowering para mantenerlo.
        /// Beneficio: tanto port C como JIT obtienen output devirtualizado
        /// para el caso comun de `Iface x = new Impl(); x.metodo()`.
        ///
        /// Llave: IrValueId.  Valor: nombre de clase concreta.  Vacio = no
        /// se conoce el tipo concreto estatico.
        std::unordered_map<ir::IrValueId, std::string> ssa_concrete_class_;

        /// Modo de instrumentacion: "none", "trace", "profile".  Cuando
        /// no es "none", el lowering envuelve cada funcion usuario con
        /// CALLs a @c vex_trace:enter y @c vex_trace:exit (o equivalente).
        std::string instrument_mode_ = "none";

        /// Helper: emite CALLN sintetica a @c "vex_trace:enter" con
        /// argumento puntero al string literal del nombre de la funcion.
        /// Usa @c out_mod_->intern_static_data para internar el nombre.
        void emit_instrument_enter(const std::string &fn_name, uint32_t line);

        /// Helper: emite CALLN sintetica a @c "vex_trace:exit" con
        /// argumentos (fn_name_ptr, return_value).  Si @c v_ret es
        /// @c IR_NO_VALUE (funcion void), se pasa @c 0.
        void emit_instrument_exit(const std::string &fn_name,
                                    ir::IrValueId v_ret, uint32_t line);

        /// Spill slots activos durante el body y catches de un try.
        /// Para cada variable del scope outer que se asigna dentro del try,
        /// reservamos un slot (8 bytes en stack) y mantenemos un STORE
        /// duplicado en cada @c write_local.  El @c throw salta sin pop
        /// pero el slot vive en stack VM mas alla del rsp restore.  En el
        /// merge_bb hacemos LOAD del slot y bindeamos el nombre al SSA
        /// resultante -- asi el valor visible tras el try es siempre el
        /// ultimo escrito (body o catch) sin importar registros corruptos.
        ///
        /// Estructura: name -> SSA value PTR del slot.  Vacio fuera de un
        /// try.  El lower_try lo llena pre-body, lo limpia post-merge.
        std::unordered_map<std::string, ir::IrValueId> try_spill_slots_;

        /// stack de acciones de cleanup activas en el flujo actual.
        /// Cada entrada es codigo RAW_ASM que debe ejecutarse al SALIR del
        /// scope que la registro (sea por flujo normal, return o break).
        /// Lo usa @c synchronized para emitir @c tryleave + @c monexit
        /// cuando el body hace @c return temprano.  Las excepciones NO
        /// pasan por aqui: las maneja el @c tryenter+handler clasico.
        ///
        /// Pila LIFO: el cleanup mas reciente se ejecuta primero (orden
        /// inverso a su registro).  @c emit_cleanups_all() recorre la
        /// pila de tope a fondo, emitiendo cada bloque RAW_ASM en el
        /// bloque actual sin modificar el stack (para que el caller que
        /// abrio el scope haga su pop normal).
        struct CleanupAction {
            /// tipo de accion:
            ///   RAW_ASM: emitir un bloque @c IrOp::RAW_ASM con @c asm_text y
            ///            @c operands.  El regalloc trata el bloque como
            ///            opaco (no preserva regs caller-saved), asi que es
            ///            adecuado solo para cleanups que NO clobreen regs
            ///            de valores vivos del scope (e.g. monexit).
            ///   CALL_DTOR: emitir un @c IrOp::CALLVIRT real que el regalloc
            ///              trata como CALL normal (preserva los regs vivos
            ///              automaticamente).  Necesario para destructores
            ///              de clase y auto-free de colecciones cuando hay
            ///              valor de retorno vivo en el RET (lower_return
            ///              spills regs al stack alrededor del cleanup).
            ///   CALLN_FREE: emitir un @c IrOp::CALLN para liberar una
            ///               coleccion primitiva.  Usa @c func_name para el
            ///               nombre del simbolo nativo y @c needs_proc para
            ///               decidir si emite GETPROC + lo prepende como
            ///               primer argumento (variantes @c *_free_gc de A.30).
            ///               El regalloc trata el CALLN como cualquier call,
            ///               preservando regs caller-saved automaticamente.
            ///   SMARTPTR_FREE: libera un @c unique<T> al exit del scope.
            ///                  Sigue el patron: cargar ptr del slot stack,
            ///                  saltar si es 0 (moved), invocar deleter
            ///                  literal (free, fclose, ~T()).  Para Tier 0
            ///                  el deleter es CALLN a @c free.  Para Tier 1
            ///                  con deleter custom usa el field +8 del slot.
            ///   SHAREDPTR_REL: decremento del refcount de un @c shared<T> al
            ///                  exit del scope.  Si llega a 0 invoca el
            ///                  deleter sobre payload (ctrl_block + 16).
            enum class Kind { RAW_ASM, CALL_DTOR, CALLN_FREE, SMARTPTR_FREE, SHAREDPTR_REL };
            Kind                         kind = Kind::RAW_ASM;
            // --- Comun ---
            std::vector<ir::IrValueId>   operands;     ///< valores SSA referenciados
            uint32_t                     source_line;
            /// si != "", @c emit_cleanups_all hace @c lookup(refresh_name)
            /// y reemplaza @c operands[0] con el SSA value ACTUAL del binding.
            std::string                  refresh_name;
            // --- RAW_ASM ---
            std::string                  asm_text;     ///< plantilla con {src0..}
            // --- CALL_DTOR ---
            uint32_t                     dtor_vtable_index = 0;
            // --- CALLN_FREE / SMARTPTR_FREE / SHAREDPTR_REL ---
            std::string                  func_name;    ///< "lib:symbol" para CALLN
            bool                         needs_proc = false; ///< prepend GETPROC
            // --- SMARTPTR_FREE / SHAREDPTR_REL ---
            /// SSA value del PTR al slot del smart pointer (donde vive el ptr).
            /// Usado para LOAD del valor actual y CMP_EQ 0 (skip si moved).
            ir::IrValueId                slot_addr = 0;
            /// Nombre del deleter.  Tres formatos:
            ///   "free"                  -> emite IrOp::RAW_FREE directo
            ///                              (deleter por defecto de unique_box).
            ///   "<vesta_fn_name>"       -> emite CALLVM @Method al simbolo
            ///                              Vesta declarado por el usuario.
            ///                              Cero overhead: 1 LOAD + 1 CALLVM.
            ///   "@extern:<lib>:<fn>"    -> emite CALLN al simbolo nativo
            ///                              de la libreria.  El prefijo "@extern:"
            ///                              discrimina extern vs Vesta.
            std::string                  literal_deleter;
            /// Tamano del slot del smart pointer (8 para Tier 0).
            uint32_t                     slot_size = 8;
        };
        std::vector<CleanupAction> cleanup_stack_;
        /// Contador para etiquetas unicas en cleanups que emiten labels
        /// (smart pointer cleanups con branches internos).  Cada invocacion
        /// de emit_cleanups consume el siguiente valor; garantiza que dos
        /// cleanups no colisionen en el mismo label dentro de la misma
        /// funcion (caso comun: 2 vars unique<T> en el mismo scope).
        uint32_t                   cleanup_label_seq_ = 0;
        /// Canal compartido entre @c try_lower_builtin_call (unique_with /
        /// shared_with) y @c lower_var_decl para pasar el nombre del deleter
        /// custom al cleanup pendiente.  Vacio = usar el deleter por
        /// defecto ("free").  Se limpia tras consumirse en lower_var_decl.
        std::string                pending_smartptr_deleter_;

        /// Stack de targets de break/continue para los loops anidados.
        /// Cada vez que entramos a un while/for/do-while se hace push de
        /// {continue_bb, break_bb}; al salir, pop.  BreakStmt emite
        /// `br break_bb` y ContinueStmt emite `br continue_bb` del top.
        ///
        /// continue_bb apunta al header del while (la cond se re-evalua) o
        /// al step_bb del for (ejecuta step y luego cond).  break_bb apunta
        /// al exit_bb del loop.  Vacio fuera de loops (BreakStmt y
        /// ContinueStmt en ese contexto son error de compilacion).
        struct LoopTargets {
            ir::IrBlockId continue_bb;
            ir::IrBlockId break_bb;
            /// Lista de bloques predecesores que saltaron al `continue_bb`
            /// via la sentencia `continue`.  Se rellena cada vez que el
            /// lowering procesa un `ContinueStmt` dentro del loop.  Al
            /// cerrar el loop, lower_while/for itera estos preds para
            /// completar los PHI nodes del header con los SSA values
            /// del scope al momento del continue (sin esto, las
            /// variables modificadas en el body antes del continue
            /// no se propagan correctamente al header del loop).
            std::vector<ir::IrBlockId> continue_preds;
            /// Snapshot del scope (todos los niveles) capturado en el
            /// instante de cada `continue`.  Indice paralelo a
            /// continue_preds.
            std::vector<std::vector<std::unordered_map<std::string, ir::IrValueId>>>
                continue_scopes;
        };
        std::vector<LoopTargets> loop_targets_;

        /// Mapa global de `goto` labels declaradas en la funcion actual.
        /// Cada `label:` declara un bloque con ese nombre; cada `goto label`
        /// emite BR al bloque resuelto via este mapa.  Se vacia al cerrar
        /// la funcion.  Si una label se usa antes de declararse, se
        /// crea el bloque en el primer goto y se reusa al verla.
        struct GotoEntry {
            ir::IrBlockId block;
            bool          declared = false;   // true tras encontrar `label:`
            SourceLoc     first_use_loc;       // para diagnostico de undefined
        };
        std::unordered_map<std::string, GotoEntry> goto_labels_;

        /// contador monotono para nombrar funciones sinteticas
        /// generadas por @c spawn @c { @c body @c }.  Cada spawn produce
        /// @c __spawn_<N>; reset al inicio del modulo (no por funcion).
        size_t spawn_func_counter_ = 0;

        /// contador para nombres unicos de helpers
        /// @c __lambda_<N>.  Crece junto con cada @c LambdaExpr lowered;
        /// el reseteo es per-modulo (no per-funcion) para garantizar que
        /// dos lambdas distintas en funciones distintas tengan nombres
        /// diferentes.  Reusa @c pending_spawn_helpers_ para encolar los
        /// helpers; el flush a @c out_mod_ pasa al final de @c run().
        size_t lambda_counter_ = 0;

        /// helpers de spawn pendientes de anyadir al modulo.  Las
        /// funciones se acumulan aqui durante el lowering del padre y se
        /// vuelcan a @c out_mod_ al final de @c run() para preservar el
        /// orden de funciones (main primero -> emisor IR la marca como
        /// entry point con @c hlt; spawn helpers despues -> @c ret).
        std::vector<ir::IrFunction> pending_spawn_helpers_;

        /// si !=IR_NO_VALUE estamos bajando el body de una
        /// funcion @Async como spawn helper.  El SSA value contiene el
        /// handle del Future (resultado de msgrecv al inicio del helper).
        /// `lower_return` consulta este flag: si esta set, en vez de emitir
        /// RET, emite `fulfill(async_fut_id_, value) + hlt`.  Asi cada
        /// return del body resuelve el future del caller.
        ir::IrValueId async_fut_id_ = ir::IR_NO_VALUE;

        /// si true, estamos lowering el body de un `rspawn(node) { ... }`
        /// helper.  En este caso `lower_return` intercepta `return X` y emite
        /// `mov r0, X + hlt`.  El runtime distribuido captura R0 al detectar
        /// HALT en un proceso con `rspawn_future_id != 0` y envia VDP_FUTURE_FULFILL
        /// al nodo origen con ese valor.  No usa async_fut_id_ porque el future
        /// vive en el nodo CALLER, no en el remoto.
        bool is_rspawn_body_ = false;

        /// Emite todos los cleanups activos del @c cleanup_stack_ en orden
        /// inverso (LIFO).  No modifica el stack.  Usado por @c lower_return
        /// para garantizar que las acciones de salida (e.g. monexit) corran
        /// antes del RET.
        void emit_cleanups_all();

        /**
         * @brief Emite los cleanups del rango [start, end) del @c cleanup_stack_
         *        en orden inverso (mas reciente primero).
         *
         * Variante de @c emit_cleanups_all que solo procesa una ventana del
         * stack.  Util para scope-local cleanup desde @c lower_block: el
         * scope recuerda la altura del stack al entrar (start = mark) y al
         * salir emite los cleanups que se registraron en su propio cuerpo
         * (end = stack actual), sin tocar los outer.  Asi un destructor
         * dentro de un loop body se ejecuta al exit de cada iteracion.
         *
         * NO modifica el cleanup_stack_; el caller hace @c resize(start)
         * tras esta llamada para popear las entradas ya emitidas.
         *
         * @param start Indice inicial (inclusive).
         * @param end   Indice final (exclusive); debe ser >= start.
         */
        void emit_cleanups_range(size_t start, size_t end);

        /// sret: si la funcion actual declara devolver Optional<T> o
        /// Result<V,E>, el tipo de retorno se transforma en void y el
        /// caller pasa un buffer hidden (retbuf) como primer argumento.
        /// @c sret_active_ indica si el patron esta en uso, y
        /// @c sret_retbuf_ guarda el SSA value del param hidden.  El
        /// lowering de @c return en estas funciones copia el contenido
        /// del Optional/Result construido al retbuf en vez de devolverlo
        /// como valor.  El de las funciones que NO devuelven Optional/
        /// Result es false / IR_NO_VALUE.
        bool            sret_active_ = false;
        ir::IrValueId   sret_retbuf_ = ir::IR_NO_VALUE;
        /// Tamano del Optional/Result que se devuelve (16 o 24).  Se
        /// usa para emitir el MEMCPY al retbuf en lower_return.
        uint64_t        sret_buf_size_ = 0;

        /// Nombre de la clase contenedora durante el lowering de un metodo
        /// de instancia.  Lo consulta @c lower_class_field_load /
        /// @c lower_class_field_store para resolver offsets cuando el
        /// receptor de @c .field es @c this (caso comun) o cualquier
        /// expresion con tipo CLASS.  Vacio fuera de metodos.
        std::string current_class_lowering_;
    };

} // namespace vex

#endif // VEX_LOWERING_H
