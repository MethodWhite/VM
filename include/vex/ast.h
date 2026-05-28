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
 * @file ast.h
 * @brief Arbol de Sintaxis Abstracta (AST) para el lenguaje Vex.
 *
 * Modelo de jerarquia plana con discriminador (NodeKind) + downcast
 * controlado.  Cada nodo lleva su SourceLoc para reportes y la propiedad
 * por @c std::unique_ptr garantiza ownership claro y zero-cost frente a
 * @c shared_ptr (sin contadores atomicos).
 *
 * Cobertura actual del AST:
 *   - ModuleNode (raiz): lista de FunctionDecl, GlobalVarDecl, ClassDecl,
 *                       StructDecl, EnumDecl, ExternFnDecl.
 *   - Statements: Block, VarDecl, ExprStmt, If, While, DoWhile, For (C),
 *                 ForEach (Vex), Return, Break, Continue, Goto, Label,
 *                 Try/Catch/Finally, Synchronized, Throw, Match.
 *   - Expressions: literales (int/float/bool/null/char/string con
 *                  interpolacion), Ident, Binary, Unary, Assign, Call,
 *                  New, Spawn, RSpawn, Lambda, Match, Cast, FieldAccess,
 *                  Index, This, InitList.
 *   - Type AST: PrimitiveTypeNode + PointerTypeNode + ArrayTypeNode +
 *               NamedTypeNode (con generics) + FunctionTypeNode.
 *
 * Decisiones de hardware:
 *   - NodeKind cabe en 1 byte; el discriminador se inlinea en cada nodo.
 *   - Los enum BinOp/UnOp/AssignOp tambien son uint8_t para minimizar
 *     padding cuando el AST se aloja en alguna arena futura.
 *   - Los downcasts en lowering / type checker se hacen con static_cast
 *     tras chequear @c kind, NO con dynamic_cast (cero RTTI overhead).
 */

#ifndef VEX_AST_H
#define VEX_AST_H

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "vex/diagnostic.h"
#include "vex/types.h"

namespace vex::ast {

    // -------------------------------------------------------------------
    // Discriminadores: NodeKind, BinOp, UnOp, AssignOp.
    // -------------------------------------------------------------------

    /**
     * @enum NodeKind
     * @brief Identifica la categoria concreta de cada nodo del AST.
     *
     * Agrupados por seccion (decls, stmts, exprs, types).  Los rangos
     * son contiguos para que predicados como is_expr() puedan implementarse
     * con una comparacion numerica en lugar de un switch.
     */
    enum class NodeKind : uint8_t {
        // ----- Declaraciones (top-level y locales) -----
        Module          = 0,
        FunctionDecl,
        ParamDecl,
        GlobalVarDecl,
        TypeAliasDecl,
        StructDecl,
        ClassDecl,
        EnumDecl,        ///< Declaracion de tipo algebraico (enum + variantes con/sin payload).
        ExternFnDecl,    ///< @c extern "lib.dll" fn name(params) -> ret; (FFI declarativo, 0 overhead).
        ImportDecl,      ///< @c import "path" [as alias] [only A, B];  (Phase M sistema de modulos).
        NamespaceDecl,   ///< @c namespace foo { decls }  (Phase M.7.c, inline namespace estilo C++).

        // ----- Statements -----
        BlockStmt,
        VarDeclStmt,
        ExprStmt,
        IfStmt,
        WhileStmt,
        DoWhileStmt,
        ForStmt,
        ReturnStmt,
        BreakStmt,
        ContinueStmt,
        GotoStmt,
        LabelStmt,
        TryStmt,         ///< try { ... } catch (T e) { ... }
        ThrowStmt,       ///< throw expr;
        ForEachStmt,     ///< for (T x : col) body
        SynchronizedStmt,///< synchronized (obj) { ... }  (monenter/monexit con cleanup implicito en cada via de salida)
        ComptimeBlockStmt, ///< A.39: comptime { ... } scope para comptime const + for + asserts
        ComptimeForStmt,   ///< A.39: comptime for (i in lo..hi) { body } -- unrolled

        // ----- Expressions -----
        IntLitExpr,
        FloatLitExpr,
        BoolLitExpr,
        NullLitExpr,
        CharLitExpr,
        StringLitExpr,
        IdentExpr,
        FieldAccessExpr,
        BinaryExpr,
        UnaryExpr,
        AssignExpr,
        TernaryExpr,   ///< cond ? then : else (A.38)
        TryExpr,       ///< expr? (P2: early-return para Result<V,E>)
        CallExpr,
        IndexExpr,
        ThisExpr,
        NewExpr,
        SpawnExpr,    ///< spawn { body }: arranca proceso hijo, devuelve PID encoded
        RSpawnExpr,   ///< rspawn(node) { body }: spawn cross-node, devuelve Future handle
        LambdaExpr,   ///< (args) => expr  o  (args) => { stmts }: closure inline
        MatchExpr,    ///< ADTs: match scrutinee { case Variant(bindings) => body; ... }
        SuperCallExpr,        ///< super(args) -- delegacion ctor (R1)
        SuperMethodCallExpr,  ///< super.method(args) -- callsuper no-virtual (R1)
        InitListExpr, ///< { 1, 2, 3 } o { .x = 1, .y = 2 } como
                      ///< inicializador de array o struct.  Solo valido en
                      ///< var-decl o como init de campo nested.  Lowering
                      ///< asigna cada elemento al slot correspondiente del
                      ///< destino (no construye un valor temporal).
        CastExpr,     ///< (T)expr: cast C-style.  El parser solo reconoce
                      ///< el patron `(<type>) <unary>` cuando el contenido
                      ///< del parentesis es un type-node (no una expresion
                      ///< que produce valor).  Cubre tanto conversiones
                      ///< numericas como bitcasts de punteros entre
                      ///< VirtualPtr<T> y T*, ptr<->int, etc.

        // ----- Types AST -----
        PrimitiveTypeNode,
        NamedTypeNode,
        PointerTypeNode,
        ArrayTypeNode,
        FunctionTypeNode, ///< fn(T1, T2) -> R: tipo de variable / parametro / retorno closure

        // Sentinela.
        COUNT
    };

    /**
     * @enum BinOp
     * @brief Operadores binarios reconocidos por el parser.
     *
     * Mapeo 1-a-1 con TokenKind operador, mantenido en parse_binary_op_from_token.
     */
    enum class BinOp : uint8_t {
        Add, Sub, Mul, Div, Mod,
        Eq, Neq, Lt, Le, Gt, Ge,
        LogicalAnd, LogicalOr,
        BitAnd, BitOr, BitXor,
        Shl, Shr,
    };

    /**
     * @enum UnOp
     * @brief Operadores unarios prefijo y postfijo.
     *
     * La distincion prefijo/postfijo (++x vs x++) se codifica como dos
     * variantes separadas para que el lowering vea exactamente la
     * semantica deseada sin necesidad de una flag aparte.
     */
    enum class UnOp : uint8_t {
        Neg,        ///< -x
        Pos,        ///< +x (raramente util, pero el parser lo acepta)
        LogicalNot, ///< !x
        BitNot,     ///< ~x
        PreInc,     ///< ++x
        PreDec,     ///< --x
        PostInc,    ///< x++
        PostDec,    ///< x--
        AddrOf,     ///< &x (toma direccion de un lvalue; el lvalue se promociona a ALLOCA)
        Deref,      ///< *p (lectura a traves de puntero; tipo = *p->pointee)
        Unwrap,     ///< !!x  (Optional: assert non-null; lanza NPE si nulo)
        Await,      ///< await fut (Future: bloquea hasta resuelto, devuelve valor)
    };

    /**
     * @enum AssignOp
     * @brief Variantes del operador de asignacion (simple o compuesto).
     */
    enum class AssignOp : uint8_t {
        Assign,         ///< =
        AddAssign,      ///< +=
        SubAssign,      ///< -=
        MulAssign,      ///< *=
        DivAssign,      ///< /=
        ModAssign,      ///< %=
        BitAndAssign,   ///< &=
        BitOrAssign,    ///< |=
        BitXorAssign,   ///< ^=
        ShlAssign,      ///< <<=
        ShrAssign,      ///< >>=
    };

    // -------------------------------------------------------------------
    // Nodo base.
    // -------------------------------------------------------------------

    /**
     * @struct Node
     * @brief Raiz de la jerarquia AST.
     *
     * Toda la informacion comun (discriminador + posicion) vive aqui;
     * los campos especificos viven en las clases derivadas.  El destructor
     * es virtual para que @c unique_ptr<Node> libere correctamente el
     * subtipo, pero NO se confia en RTTI para el dispatch (vease NodeKind).
     */
    struct Node {
        NodeKind  kind = NodeKind::COUNT;
        SourceLoc loc;

        Node() = default;
        explicit Node(NodeKind k) : kind(k) {}
        virtual ~Node() = default;

        // Prohibimos copia para forzar el move-only de uniquept owners.
        Node(const Node &)            = delete;
        Node &operator=(const Node &) = delete;
        Node(Node &&)                 = default;
        Node &operator=(Node &&)      = default;
    };

    // Forward declarations utiles para signatures cruzadas.
    struct Expr;
    struct Stmt;
    struct TypeNode;
    struct ParamDecl;  ///< Necesaria para LambdaExpr antes de su definicion.
    struct BlockStmt;  ///< Necesaria para LambdaExpr antes de su definicion.

    /**
     * @brief @c true si k representa una expresion.
     */
    constexpr bool is_expr_kind(NodeKind k) noexcept {
        // Rango contiguo: cualquier NodeKind entre IntLitExpr y CastExpr
        // (la ultima expresion definida) es una expresion.
        return (uint8_t)k >= (uint8_t)NodeKind::IntLitExpr
            && (uint8_t)k <= (uint8_t)NodeKind::CastExpr;
    }
    /**
     * @brief @c true si k representa un statement.
     */
    constexpr bool is_stmt_kind(NodeKind k) noexcept {
        return (uint8_t)k >= (uint8_t)NodeKind::BlockStmt
            && (uint8_t)k <= (uint8_t)NodeKind::SynchronizedStmt;
    }

    // -------------------------------------------------------------------
    // Tipos en AST (aun sin punteros / arrays / generics).
    // -------------------------------------------------------------------

    /**
     * @struct TypeNode
     * @brief Nodo abstracto raiz para nodos de tipo.
     *
     * @c is_nonnull se activa cuando el usuario escribio `nonnull T`
     * antes del tipo, indicando que la variable / parametro / campo
     * NO puede ser null en compile time.  El type checker rechaza
     * asignaciones de @c null o de tipos nullable sin un `!!` de por
     * medio.  Solo es semanticamente significativo para reference
     * types (CLASS / PTR); para primitivos (i32, etc) no aplica.
     */
    struct TypeNode : Node {
        bool is_nonnull = false;
        explicit TypeNode(NodeKind k) : Node(k) {}
    };

    /**
     * @struct PrimitiveTypeNode
     * @brief Nodo de tipo primitivo (i32, u8, f64, ...).
     *
     * La categoria primitiva se almacena ya canonicalizada (Vex acepta
     * ambos estilos i32 / int32_t y aqui se guarda solo la forma canonica).
     */
    struct PrimitiveTypeNode : TypeNode {
        PrimitiveKind prim = PrimitiveKind::VOID;
        /// type args para colecciones genericas.
        /// `ArrayList<T>` -> prim=ARRAYLIST + type_args=[T].
        /// `HashMap<K,V>` -> prim=HASHMAP   + type_args=[K, V].
        /// El type checker resuelve estos al `pointee`/`pointee2` del
        /// `Type` semantico final, que el lowering consulta para
        /// despachar a las variantes gc-aware del plugin (vcol_*_gc)
        /// cuando el tipo de elemento es GC (string, class, ...).
        std::vector<std::unique_ptr<TypeNode>> type_args;

        PrimitiveTypeNode() : TypeNode(NodeKind::PrimitiveTypeNode) {}
    };

    /**
     * @struct NamedTypeNode
     * @brief Referencia a un tipo por nombre.
     *
     * Aparece cuando el usuario menciona un tipo introducido por
     * @c typedef / @c using o por @c struct.  El type checker lo
     * resuelve consultando la tabla de aliases / structs registrada
     * a nivel de modulo.
     */
    struct NamedTypeNode : TypeNode {
        std::string name;
        /// Argumentos de tipo de un instanciado generico (ej. `Box<i32>`
        /// produce name="Box" + type_args=[PrimitiveTypeNode(i32)]).
        /// Vacio para referencias no genericas.  El type checker
        /// monomorphiza la clase generica al ver el primer instanciado y
        /// reemplaza la referencia por NamedTypeNode con name="Box_i32".
        std::vector<std::unique_ptr<TypeNode>> type_args;
        NamedTypeNode() : TypeNode(NodeKind::NamedTypeNode) {}
    };

    /**
     * @struct PointerTypeNode
     * @brief Tipo puntero raw: T*.
     *
     * El parser apila estos nodos cada vez que ve un '*' tras un tipo
     * (postfix), por lo que T** se representa como PointerTypeNode envolviendo
     * a otro PointerTypeNode envolviendo a T.  El type checker desenvuelve la
     * cadena para producir un @c Type plano (con shared_ptr<Type> como
     * pointee).  Punteros raw NO son rastreados por el GC y NO tienen
     * verificacion de null en deref (solo el tipo @c nonnull T la fuerza).
     */
    struct PointerTypeNode : TypeNode {
        std::unique_ptr<TypeNode> pointee; ///< Tipo apuntado (puede ser otro puntero).
        /// true si el parser vio @c VirtualPtr<T> (direccion de memoria VM);
        /// false si vio @c T* (direccion HOST por defecto).  El type checker
        /// propaga al campo @c is_virtual del @c Type resultante.
        bool is_virtual = false;
        PointerTypeNode() : TypeNode(NodeKind::PointerTypeNode) {}
    };

    /**
     * @struct ArrayTypeNode
     * @brief Tipo array nativo: T[N] (size fijo) o T[] (size variable / decay-to-ptr).
     *
     * En Vex los arrays nativos son simplemente bloques contiguos de
     * memoria; @c element_type indica el tipo de cada elemento y
     * @c size_expr el numero de elementos.  Cuando @c size_expr es nulo
     * el array es @c T[] (sin tamano conocido en compile time, util como
     * tipo de parametro de funcion: equivale a T*).  El type checker
     * exige que @c size_expr, si presente, sea un literal entero positivo
     * (las constantes mas elaboradas llegaran en hitos posteriores).
     */
    struct ArrayTypeNode : TypeNode {
        std::unique_ptr<TypeNode> element_type;
        std::unique_ptr<Expr>     size_expr; ///< nullopt => T[] (decay)
        ArrayTypeNode() : TypeNode(NodeKind::ArrayTypeNode) {}
    };

    /**
     * @struct FunctionTypeNode
     * @brief Tipo de funcion / closure: @c fn(T1, T2, ...) -> R.
     *
     * Permite declarar variables, parametros y campos cuyo tipo es una
     * funcion / closure.  El parser lo construye al ver el keyword @c fn
     * seguido de la lista de tipos de parametros entre parentesis y el
     * @c -> con el tipo de retorno opcional (si se omite, se asume void).
     *
     * Ejemplo de sintaxis aceptada:
     * @code
     *   fn(i32, i32) -> i32 sumador = (a, b) => a + b;
     *   fn() -> void log = () => println("hi");
     * @endcode
     *
     * El type checker resuelve el nodo a @c Type{FUNCTION, params, ret}
     * y lo usa para validar la asignacion de lambdas y las llamadas.
     */
    struct FunctionTypeNode : TypeNode {
        /// Tipos de los parametros, en orden.  Vacio para @c fn() -> R.
        std::vector<std::unique_ptr<TypeNode>> param_types;
        /// Tipo de retorno.  Nunca null (el parser inserta VOID si el
        /// usuario omite la flecha @c ->).
        std::unique_ptr<TypeNode>              return_type;
        FunctionTypeNode() : TypeNode(NodeKind::FunctionTypeNode) {}
    };

    // -------------------------------------------------------------------
    // Expresiones.
    // -------------------------------------------------------------------

    /**
     * @struct Expr
     * @brief Nodo abstracto raiz para todas las expresiones.
     *
     * El campo @c result_type es el tipo deducido por el type checker;
     * el parser lo deja en su valor por defecto y el type checker lo
     * fija despues.  Mantenerlo en el nodo evita una tabla auxiliar
     * en lowering y reduce pointer chasing.
     */
    struct Expr : Node {
        Type result_type{}; ///< Inicialmente VOID; el type checker lo rellena.

        /// Borrow checker (F4 - lifetime elision): si esta Expr produce
        /// un borrow<T>/borrow_mut<T> derivado de algun owner trackeable
        /// (var local, param, etc.), aqui se guarda el nombre de ese
        /// owner.  Vacio si no aplica o si la fuente no es trackeable.
        /// Usado para propagar la "lifetime source" a traves de:
        ///   - lend(x) / lend_mut(x): borrow_owner_source = x
        ///   - identity_borrow(p): si la firma tiene 1 input borrow, el
        ///     resultado hereda el source del arg
        ///   - reborrow lend(b): borrow_owner_source = b's source
        std::string borrow_owner_source;

        /// Borrow checker (F3 ext - suspend semantics): si esta Expr es
        /// un lend()/lend_mut() cuya fuente es un borrow_mut, aqui se
        /// guarda el nombre de la fuente para que @c check_var_decl
        /// pueda llamar @c mark_as_reborrow tras registrar el binding.
        std::string borrow_reborrow_source_name;
        /// Flag asociado: true si la fuente era borrow_mut (necesita
        /// restore al drop).  False si era borrow shared o owner directo.
        bool        borrow_reborrow_source_is_mut = false;

        /// si esta Expr es un IdentExpr que resuelve a un comptime
        /// const (global de A.38 o local de A.39), el type checker
        /// graba aqui el valor para que el lowering lo inline como CONST
        /// directo.  Sin esto, los comptime const LOCALES se perderian
        /// al pop_comptime_scope() del type checker antes del lowering.
        bool        comptime_const_resolved = false;
        bool        comptime_const_is_str = false;
        int64_t     comptime_const_int = 0;
        std::string comptime_const_str;

        explicit Expr(NodeKind k) : Node(k) {}
    };

    struct IntLitExpr : Expr {
        uint64_t value = 0;
        IntLitExpr() : Expr(NodeKind::IntLitExpr) {}
    };

    struct FloatLitExpr : Expr {
        double value = 0.0;
        FloatLitExpr() : Expr(NodeKind::FloatLitExpr) {}
    };

    struct BoolLitExpr : Expr {
        bool value = false;
        BoolLitExpr() : Expr(NodeKind::BoolLitExpr) {}
    };

    struct NullLitExpr : Expr {
        NullLitExpr() : Expr(NodeKind::NullLitExpr) {}
    };

    struct CharLitExpr : Expr {
        uint32_t codepoint = 0;
        CharLitExpr() : Expr(NodeKind::CharLitExpr) {}
    };

    /**
     * @struct StringLitExpr
     * @brief Literal de cadena.
     *
     * Almacena los bytes ya resueltos (post-escapes para "...", verbatim
     * para r"...").  El tipo deducido por el type checker es
     * @c PTR (puntero a los bytes en la seccion de datos del modulo).
     * El lowering registra los bytes en @c IrModule::static_data y emite
     * un @c IrOp::STR_LIT_ADDR con el indice resultante.
     */
    struct StringLitExpr : Expr {
        std::string value; ///< Bytes del literal (sin las comillas ni escapes textuales).
                           ///< Para STRINGS SIMPLES contiene todo el contenido.
                           ///< Para STRINGS INTERPOLADOS queda vacio y los datos
                           ///< viven en @c interp_parts + @c interp_exprs.
        bool        is_raw = false;

        /// Interpolacion ${expr}.  Layout intercalado:
        ///   parts[0] + expr[0] + parts[1] + expr[1] + ... + parts[N]
        /// Si hay N expresiones, hay N+1 parts (algunas pueden ser "").
        /// Los vectores quedan vacios para strings sin interpolacion.
        std::vector<std::string>           interp_parts;
        std::vector<std::unique_ptr<Expr>> interp_exprs;

        /// Formato opcional por interpolacion: @c ${expr:fmt}.  Vector
        /// paralelo a @c interp_exprs (tambien tamano N).  Cadena vacia
        /// = sin formato (default).  El formato es una mini-DSL parseada
        /// en lowering: kinds (`hex`, `bin`, `oct`, `dec`, `ptr`, `gc`,
        /// `char`, `bool`) y alineacion (`>N` right, `<N` left, opcional
        /// fill char tras el ancho).  Multiples specs se separan por `:`.
        std::vector<std::string>           interp_formats;

        /// @c true si el string contiene al menos una expresion ${...}.
        bool is_interpolated() const noexcept {
            return !interp_exprs.empty();
        }

        StringLitExpr() : Expr(NodeKind::StringLitExpr) {}
    };

    struct IdentExpr : Expr {
        std::string name;
        IdentExpr() : Expr(NodeKind::IdentExpr) {}
    };

    /**
     * @struct FieldAccessExpr
     * @brief Acceso a un campo: base.field.
     *
     * @c base es la expresion que evalua al struct (lvalue o rvalue
     * de tipo STRUCT); @c field_name es el nombre del campo.  El type
     * checker valida que @c base es un struct y que @c field_name
     * existe en su layout, y rellena @c result_type con el tipo del
     * campo.  El lowering calcula el offset desde el puntero base
     * y emite LOAD (lectura) o STORE (asignacion) sobre la VA
     * resultante.
     */
    struct FieldAccessExpr : Expr {
        std::unique_ptr<Expr> base;
        std::string           field_name;
        /// @brief Si !=0, el acceso resuelve a un accesor de
        /// propiedad y el lowering emite la llamada en vez de un
        /// getfield directo.  1 = getter (`obj.prop`), 2 = setter (lhs
        /// de un AssignExpr; @ref AssignExpr::is_property_set se marca
        /// en el padre).  3 = static field de clase (Counter.count).
        /// 4 = simbolo de namespace importado (Phase M.7, `lib_a.valor_a`).
        /// El metodo se llama `get_<field_name>` o
        /// `set_<field_name>` segun el caso.
        uint8_t               property_kind = 0;
        /// Phase M.7: cuando @c property_kind == 4 este campo guarda
        /// el indice del namespace en @c TypeChecker::imported_namespaces_
        /// para que el lowering pueda resolver el mangled_label sin
        /// depender de la pila de scopes (que ya esta vacia al lower).
        /// Sentinel: UINT32_MAX = no resuelto.
        uint32_t              ns_index = 0xFFFFFFFFu;
        FieldAccessExpr() : Expr(NodeKind::FieldAccessExpr) {}
    };

    struct BinaryExpr : Expr {
        BinOp                 op;
        std::unique_ptr<Expr> lhs;
        std::unique_ptr<Expr> rhs;
        BinaryExpr() : Expr(NodeKind::BinaryExpr) {}
    };

    struct UnaryExpr : Expr {
        UnOp                  op;
        std::unique_ptr<Expr> operand;
        UnaryExpr() : Expr(NodeKind::UnaryExpr) {}
    };

    /**
     * @struct TernaryExpr
     * @brief Operador ternario `cond ? then : else` (A.38).
     *
     * Equivalente semantico a `if (cond) then_value else else_value` pero
     * como expresion.  El type checker deduce el tipo resultado como el
     * "common type" entre @c then_expr y @c else_expr (mismo
     * @c types_assignable que usa AssignExpr).  El lowering emite el patron
     * `cond -> br_cond %c, then_bb, else_bb; bb_then -> ...; bb_else -> ...;
     * merge -> phi.t [then_val, then_bb] [else_val, else_bb]`.
     *
     * Asociativo por la derecha: `a ? b : c ? d : e` == `a ? b : (c ? d : e)`.
     * Cortocircuita: solo se evalua la rama elegida (importante si las
     * sub-expresiones tienen efectos secundarios como calls).
     */
    struct TernaryExpr : Expr {
        std::unique_ptr<Expr> cond;
        std::unique_ptr<Expr> then_expr;
        std::unique_ptr<Expr> else_expr;
        TernaryExpr() : Expr(NodeKind::TernaryExpr) {}
    };

    /**
     * @struct TryExpr
     * @brief Operador @c `?` postfix para Result -- early-return sintactico.
     *
     * Sintaxis: @c `expr?` -- donde @c expr es de tipo @c Result<V,E>.
     *
     * Equivalente a:
     * @code
     *   let __tmp = expr;
     *   if (isErr(__tmp)) { return __tmp; }
     *   value(__tmp)
     * @endcode
     *
     * Solo valido dentro de funciones cuyo return type sea @c Result<_, E>
     * compatible (mismo @c E o convertible).  El type checker valida estas
     * condiciones; el lowering desugara emitiendo una rama if-error con
     * RET via SRET copy (mismo patron que @c return X cuando la funcion
     * retorna Result).
     *
     * El tipo de la expresion es @c V (el payload Ok del Result).
     */
    struct TryExpr : Expr {
        std::unique_ptr<Expr> operand;  ///< Expresion de tipo Result<V,E>
        TryExpr() : Expr(NodeKind::TryExpr) {}
    };

    struct AssignExpr : Expr {
        AssignOp              op;
        std::unique_ptr<Expr> target;  // lvalue: IdentExpr, FieldAccess, IndexExpr o UnaryExpr Deref
        std::unique_ptr<Expr> value;
        AssignExpr() : Expr(NodeKind::AssignExpr) {}
    };

    struct CallExpr : Expr {
        std::unique_ptr<Expr>              callee;
        std::vector<std::unique_ptr<Expr>> args;
        /// Argumentos de tipo @c <T,U,...> para builtins comptime
        /// (@c sizeof<T>, @c offsetof<T>, etc.).  Vacio para llamadas
        /// normales.  Solo poblado por el parser cuando el callee es un
        /// IdentExpr cuyo nombre matchea un builtin comptime conocido y
        /// va seguido de @c <...>.  El type checker valida que el numero
        /// de type args coincida con la aridad esperada del builtin.
        std::vector<std::unique_ptr<TypeNode>> type_args;
        /// A.43.10: macros Lisp con quote/emit/splicing.  Cuando el type
        /// checker detecta @c comptime_emit_expr("texto"), parsea el
        /// texto como una expresion Vex, lo type-checa en el contexto
        /// actual y guarda el AST resultante aqui.  El lowering, al ver
        /// este campo no-null, baja la expresion sustituida en lugar de
        /// emitir una llamada.  Equivalente al `splice` de Lisp/Scheme
        /// para emitir codigo al AST runtime (no solo eval comptime).
        std::unique_ptr<Expr>              macro_expanded;
        CallExpr() : Expr(NodeKind::CallExpr) {}
    };

    /**
     * @struct ThisExpr
     * @brief Referencia implicita al receptor de un metodo de instancia.
     *
     * Aparece dentro del cuerpo de un metodo de clase (no constructor).  El
     * type checker valida que el contexto es un metodo de instancia y le
     * asigna como tipo la clase receptora.  El lowering lo baja al primer
     * parametro implicito del metodo (slot reservado para el receptor).
     */
    struct ThisExpr : Expr {
        ThisExpr() : Expr(NodeKind::ThisExpr) {}
    };

    /**
     * @struct SuperCallExpr (BugFix R1)
     * @brief @c super(args) -- delegacion al constructor de la superclase
     *        dentro del cuerpo de un constructor.
     *
     * Solo valida dentro del body de un ctor de clase con super_name no
     * vacio.  El type checker resuelve al constructor de la super, valida
     * la aridad y tipos de args.  El lowering emite CALLVIRT a la
     * vtable_index del super_ctor con this como receptor.
     */
    struct SuperCallExpr : Expr {
        std::vector<std::unique_ptr<Expr>> args;
        SuperCallExpr() : Expr(NodeKind::SuperCallExpr) {}
    };

    /**
     * @struct SuperMethodCallExpr (BugFix R1)
     * @brief @c super.method(args) -- llamada a metodo de la superclase
     *        evitando el dispatch virtual.
     *
     * Util para overrides que delegan al super.  El lowering emite
     * @c callsuper (opcode 0xFC) o equivalent que dispatcha a la
     * vtable_index del SUPER directamente, no del receiver dinamico.
     */
    struct SuperMethodCallExpr : Expr {
        std::string                        method_name;
        std::vector<std::unique_ptr<Expr>> args;
        SuperMethodCallExpr() : Expr(NodeKind::SuperMethodCallExpr) {}
    };

    /**
     * @struct NewExpr
     * @brief Creacion de instancia: @c new ClassName(args).
     *
     * El type checker resuelve @c class_name al ClassInfo correspondiente
     * y valida que existe un constructor que encaja con la lista de
     * argumentos.  El lowering emite findclass + newobj + callvirt al
     * indice del constructor en la vtable.
     */
    struct NewExpr : Expr {
        std::string                        class_name;
        std::vector<std::unique_ptr<Expr>> args;
        /// Argumentos de tipo para @c new Box<i32>(42).
        /// Vacio para clases no genericas.  El type checker monomorphiza
        /// la clase plantilla y reemplaza class_name con el nombre del
        /// tipo concreto generado (ej. "Box_i32").
        std::vector<std::unique_ptr<TypeNode>> type_args;
        /// bug4: array allocation `new T[N]`.  Si esta seteado, el
        /// expression aloca un array de N elementos del tipo `class_name`
        /// en host heap (via malloc).  El resultado es un host_ptr al
        /// buffer de N * sizeof(T) bytes.  args queda vacio en este caso.
        std::unique_ptr<Expr> array_size;
        /// Z.6: marca que esta instancia debe alocarse en el SharedHeap
        /// (cross-process visible).  Lo setea @c lower_var_decl cuando el
        /// var-decl padre tiene @c is_shared=true.  El lowering despacha
        /// a @c __new_<Class>_shared (que emite el opcode @c newobjs en
        /// lugar de @c newobj).
        bool                  is_shared = false;
        /// Bug fix 2026-05-23: indica que @c class_name YA fue mutado a su
        /// forma mangled (Node_i32 etc).  Sin este flag, re-llamadas a
        /// @c check_new sobre el mismo NewExpr (e.g. por compound assign
        /// que re-evalua RHS) concatenarian el sufijo otra vez ("Node_i32_i32").
        bool                  is_mangled = false;
        NewExpr() : Expr(NodeKind::NewExpr) {}
    };

    // SpawnExpr se define DESPUES de BlockStmt porque lo referencia.

    /**
     * @struct IndexExpr
     * @brief Subscript: base[index].
     *
     * @c base es siempre un puntero (T*) y @c index un entero.
     * El type checker valida los tipos y rellena @c result_type con el
     * tipo apuntado.  El lowering desugara `p[i]` a `*(p + i*sizeof(*p))`,
     * reutilizando los mismos LOAD/STORE de los punteros.
     *
     * Es un lvalue: `p[i] = v` se acepta como destino de AssignExpr.
     */
    struct IndexExpr : Expr {
        std::unique_ptr<Expr> base;
        std::unique_ptr<Expr> index;
        IndexExpr() : Expr(NodeKind::IndexExpr) {}
    };

    // -------------------------------------------------------------------
    // Statements.
    // -------------------------------------------------------------------

    /**
     * @struct Stmt
     * @brief Nodo abstracto raiz para todos los statements.
     */
    struct Stmt : Node {
        explicit Stmt(NodeKind k) : Node(k) {}
    };

    struct BlockStmt : Stmt {
        std::vector<std::unique_ptr<Stmt>> body;
        BlockStmt() : Stmt(NodeKind::BlockStmt) {}
    };

    /**
     * @struct SpawnExpr
     * @brief @c spawn @c { @c body @c }: arranca un proceso hijo en el scheduler
     *        actual y devuelve su PID encoded como @c i64.
     *
     * El @c body se compila como una funcion sintetica @c __spawn_<N>(void)
     * que termina con @c hlt (no @c ret).  El lowering del @c SpawnExpr
     * emite @c mov + @c spawn con la direccion de esa funcion.  El PID
     * encoded del hijo (devuelto en @c R0 por el opcode @c spawn) se
     * captura en el SSA value de la expresion.
     *
     * No hay captura lexica en MVP: el body solo accede a su propio
     * estado y al pasado via mailbox (@c msgsend / @c msgrecv).  Closures
     * con captura llegan en Phase B.
     */
    struct SpawnExpr : Expr {
        /// @brief politica de placement del proceso hijo.
        ///
        /// - @c Auto    : el VM elige automaticamente el scheduler propietario
        ///                (round-robin entre todos los schedulers cuando hay
        ///                mas de uno; mismo scheduler que el padre si hay uno
        ///                solo).  Sintaxis: `spawn { body }`.
        /// - @c Here    : el hijo se asigna al MISMO scheduler que el padre
        ///                (ruta cooperativa, sin overhead de IPC cross-thread).
        ///                Sintaxis: `spawn here { body }`.
        /// - @c Pinned  : el hijo se fija al scheduler indicado en
        ///                @c sched_idx (modulo num_schedulers para evitar
        ///                fuera de rango).  Sintaxis: `spawn on(expr) { body }`.
        ///
        /// Spawn distribuido cross-node se modela via @c RSpawnExpr en
        /// lugar de extender este enum, porque cambia la semantica del
        /// valor de retorno (Future<T> vs PID).
        enum class Policy : uint8_t {
            Auto   = 0,
            Here   = 1,
            Pinned = 2
        };
        Policy                     policy = Policy::Auto;
        std::unique_ptr<Expr>      sched_idx; ///< Solo @c Pinned: indice del scheduler.
        std::unique_ptr<BlockStmt> body;
        SpawnExpr() : Expr(NodeKind::SpawnExpr) {}
    };

    /**
     * @struct RSpawnExpr
     * @brief rspawn(node_idx) { body } -- spawn distribuido cross-node.
     *
     * Diferencias clave con SpawnExpr:
     *   - Devuelve `i64` que es un GcHandle de FutureObject (NO un PID).
     *   - El body se ejecuta en el nodo remoto especificado por @c node_idx
     *     (indice en el NodeRegistry de la VM, registrado via `--dist-add-node`).
     *   - El valor de @c return en el body se captura en R0 antes de hlt; el
     *     runtime remoto envia ese valor de vuelta via VDP_FUTURE_FULFILL.
     *   - El caller usa `await fut` para bloquear hasta que llegue el valor.
     *
     * Patron tipico:
     *   @code
     *     i64 fut = rspawn(node) { return 42; };
     *     i64 r   = await fut;  // r == 42 cuando el remoto responde
     *   @endcode
     *
     * Si la VM no tiene `dist_runtime->start()` invocado o el node_idx no existe,
     * el bytecode `rspawn` devuelve handle invalido (0xFFFFFFFF) y `await` falla.
     */
    struct RSpawnExpr : Expr {
        std::unique_ptr<Expr>      node_idx; ///< Indice de nodo remoto (entero).
        std::unique_ptr<BlockStmt> body;     ///< Cuerpo a ejecutar remotamente.
        RSpawnExpr() : Expr(NodeKind::RSpawnExpr) {}
    };

    /**
     * @struct LambdaExpr
     * @brief Expresion lambda / closure inline con captura lexica.
     *
     * Sintaxis aceptada por el parser:
     * @code
     *   (i32 x, i32 y) => x + y          // expression-bodied (un solo return)
     *   (x, y)         => x + y          // tipos deducidos por inferencia
     *   ()             => 42             // sin parametros
     *   (i32 n)        => { return n*n; } // bloque (multiples stmts)
     * @endcode
     *
     * Lowering (vease Lowering::lower_lambda_expr):
     *   - Cada lambda distinta produce una funcion sintetica @c __lambda_<N>
     *     en el modulo IR (acumulada en @c pending_spawn_helpers_ junto con
     *     las de @c spawn / @c rspawn).
     *   - Los parametros formales del helper son los mismos que los de la
     *     lambda; el helper recibe ademas los captures via el registro @c R14
     *     (calling convention compartida con @c callrawclosure).
     *   - En el call site donde se construye la lambda, el lowering aloca
     *     un bloque de captures de @c (8 * n_captures) bytes en la pila del
     *     caller (@c subsp), copia los valores capturados ahi y devuelve un
     *     valor SSA "function value" de 16 bytes alocado tambien en la pila:
     *         `[+0 i64 fn_addr][+8 i64 env_addr]`.
     *
     * Para invocar el function value, vease @c CallExpr lowering: carga
     * fn_addr y env_addr del slot de 16 bytes, mete env_addr en R14 y
     * emite @c callvmr fn_addr.  Cero overhead vs llamada directa cuando
     * no hay captures (env_addr = 0, ignorado por el helper).
     *
     * MVP: el env_addr vive en la pila del caller y la lambda solo es valida
     * dentro del scope donde se creo.  Escape detection (closures que
     * sobreviven al scope) y promocion a heap quedan para una fase posterior.
     */
    /**
     * @struct MatchArm
     * @brief Una rama de un @c match: pattern + body (destructuring de ADT).
     *
     * El patron puede ser:
     *   - Variant simple: @c Color.Red    (variant_name="Red", bindings vacio)
     *   - Variant con bindings: @c Color.Green(n)  (variant_name="Green", bindings=["n"])
     *   - Comodin (default): @c _   (variant_name="_", bindings vacio)
     *
     * El body es un Stmt; tipicamente un bloque (`=> { ... }`) o una
     * expresion-bodied (`=> expr;` reescrita por el parser a un return).
     * Los bindings introducen variables nuevas en el scope del body con
     * los tipos declarados de los payload fields de la variante.
     */
    struct MatchArm {
        std::string              variant_name;  ///< "Red", "Green", "_" (default), etc.
        std::vector<std::string> bindings;      ///< Nombres locales para los payload fields.
        std::unique_ptr<Stmt>    body;
        /// Bug fix 2026-05-23: guard opcional `case Pat if expr =>`.  Si
        /// presente, el lowering evalua la expr DESPUES del tag match;
        /// solo entra al body si guard es true.  Si false, continua al
        /// siguiente arm.  La exhaustividad se relaja: la presencia de
        /// guards permite que el match no sea estaticamente exhaustivo.
        std::unique_ptr<Expr>    guard;
        SourceLoc                loc;
    };

    /**
     * @struct MatchExpr
     * @brief @c match expr @c { case Variant(bindings) => body; ... }
     *
     * Estrategia de lowering:
     *   - Carga el tag del scrutinee (i64 en offset 0 del slot enum).
     *   - Si todas las arms son variantes con tag entero contiguo y
     *     conocido en compile time -> emite IrOp::JUMPTABLE para dispatch
     *     O(1) (bytecode 0x27).
     *   - Si hay arms tipo class hierarchy -> usa IrOp::TYPESWITCH (0x28).
     *   - En cada arm: bind de los payload fields como SSA values nuevos
     *     leyendo del slot del scrutinee + offsets fijos, luego lower del
     *     body.  Las arms terminan con @c jmp end_label.
     *   - Si no es exhaustive y no hay default arm, el type checker
     *     reporta error en compile time.
     *
     * El SSA value de la expresion @c match es VOID a nivel global del IR;
     * para usar match como expresion (e.g. asignar el resultado a una
     * variable), se modela en el frontend asignando el resultado dentro
     * de cada arm a una variable comun.  En MVP @c match es un statement-
     * like dentro de @c ExprStmt.
     */
    struct MatchExpr : Expr {
        std::unique_ptr<Expr>  scrutinee;   ///< El valor a inspeccionar (tipo enum).
        std::vector<MatchArm>  arms;        ///< Arms en orden de prioridad.
        MatchExpr() : Expr(NodeKind::MatchExpr) {}
    };

    /**
     * @struct InitListExpr
     * @brief Inicializador de lista C-style: `{ e0, e1, ... }`
     *        o `{ .field = e0, .field2 = e1, ... }` para structs designado.
     *
     * Solo valido como inicializador de var-decl con tipo array, struct, o
     * struct anidado.  Para arrays solo se admite la forma posicional
     * (sin .field).  Para structs se admiten ambas formas: positional (en
     * orden de declaracion) o designado (orden libre, no requiere todos).
     *
     * Si @c is_designated es true, @c field_names tiene N entradas
     * paralelas a @c elements.  Si es false, @c field_names esta vacio.
     */
    struct InitListExpr : Expr {
        std::vector<std::unique_ptr<Expr>> elements;
        std::vector<std::string>           field_names;
        bool                               is_designated = false;
        /// Tipo destino anotado por el type checker desde el contexto.
        /// Para `unique<Punto> p = {.x=10, .y=20}` el desugar marca aqui
        /// "Punto" para que el lowering sepa que el init list construye
        /// un struct Punto y emita los STOREs en posiciones correctas.
        /// Vacio = init list contextual (el type checker resuelve por
        /// uso en var-decl u otros sitios).
        std::string                        target_type_name;
        InitListExpr() : Expr(NodeKind::InitListExpr) {}
    };

    /**
     * @struct CastExpr
     * @brief Cast C-style: `(T) expr`.
     *
     * El parser lo reconoce solo cuando el contenido del parentesis es
     * un type-node valido (incluyendo @c VirtualPtr<T>, @c T*, primitivos
     * y aliases declarados via typedef/using).  Si el parentesis contiene
     * una expresion (e.g. `(a + b)`), el parser usa la rama de expresion
     * agrupada y NO genera @c CastExpr.
     *
     * El type checker valida la conversion: cualquier ptr->ptr es bitcast
     * (mismo bit-pattern), conversiones numericas usan el cast IR
     * apropiado (ZEXT/SEXT/TRUNC/FTOI/...), y el flag @c is_virtual del
     * tipo destino se propaga al SSA value resultante para que el LOAD/
     * STORE posterior emita @c mov vs @c movh correctamente.
     */
    struct CastExpr : Expr {
        std::unique_ptr<TypeNode> target_type;  ///< Tipo destino del cast.
        std::unique_ptr<Expr>     operand;      ///< Expresion a convertir.
        CastExpr() : Expr(NodeKind::CastExpr) {}
    };

    struct LambdaExpr : Expr {
        /// Parametros de la lambda con tipos opcionales (si el parser ve
        /// @c (x, y) => ... sin tipos, el type checker debe inferirlos
        /// del contexto, e.g. de la firma de la variable destino).
        std::vector<std::unique_ptr<ParamDecl>> params;
        /// Tipo de retorno declarado.  null = inferido del @c return del body
        /// (o del tipo del expression-body).
        std::unique_ptr<TypeNode>               return_type;
        /// Cuerpo: siempre un @c BlockStmt.  Las lambdas expression-bodied
        /// @c (x) => expr se reescriben en el parser a @c (x) => { return expr; }.
        std::unique_ptr<BlockStmt>              body;

        // ----- Llenado por el TypeChecker -----
        /// Nombres de las variables del outer scope que el body referencia
        /// y que por tanto deben copiarse al env.  Se computa durante el
        /// type check del body recorriendo IdentExpr y comprobando si el
        /// nombre se resuelve en un scope ANCESTRO del scope de la lambda.
        std::vector<std::string> captures;
        /// Tipos de los captures (paralelo a @c captures).  Lo necesita el
        /// lowering para decidir el ancho del LOAD/STORE en el env block y
        /// asignar offsets correctos.
        std::vector<Type>        capture_types;

        /// Subconjunto de @c captures que aparece en lhs de una
        /// asignacion dentro del body (mutables).  Estas se promueven a
        /// address-taken en el scope outer y el env block guarda su
        /// PUNTERO en lugar del valor: reads/writes en el body son
        /// LOAD/STORE indirectos via el puntero, asi las modificaciones
        /// se ven fuera del scope de la lambda (capture-by-reference).
        /// Captures NO mutadas siguen el modelo by-value (valor copiado
        /// al env, sin overhead de indireccion).
        std::vector<std::string> mutable_captures;

        // ----- Llenado por el Lowering -----
        /// Nombre del helper sintetico generado (@c __lambda_<N>).  El call
        /// site lo usa para emitir @c mov r_fn, @c \@Absolute("code." + name).
        std::string              synthetic_name;
        /// true si el env block se aloca en HEAP RAW
        /// (RAW_ALLOC) en lugar de en STACK (ALLOCA).  Lo activa el
        /// lowering cuando la funcion contenedora retorna FUNCTION; el
        /// helper sintetico marca @c env_ptr.is_host_ptr=true para que
        /// los LOAD del prologue de captures emitan @c movh (host mem)
        /// en lugar de @c mov (vm mem).
        bool                     env_in_heap = false;

        LambdaExpr() : Expr(NodeKind::LambdaExpr) {}
    };

    /**
     * @struct VarDeclStmt
     * @brief Declaracion de variable local (dentro de un bloque o init de for).
     */
    struct VarDeclStmt : Stmt {
        std::unique_ptr<TypeNode> type;
        std::string               name;
        std::unique_ptr<Expr>     init;       ///< null si no hay inicializador.
        bool                      is_const = false;
        /// marca `comptime const NAME = expr;` local.  El type
        /// checker evalua el init y registra en
        /// @c TypeChecker::comptime_const_locals_; el lowering omite la
        /// emision (no genera ALLOCA ni STORE).
        bool                      is_comptime = false;
        /// A.43.7: marca `auto NAME = expr;` o `var NAME = expr;` con
        /// inferencia local de tipo.  El parser deja @c type=nullptr y
        /// setea @c infer_type=true; el type checker computa el tipo del
        /// @c init y lo aplica al binding sin requerir TypeNode en el AST.
        bool                      infer_type = false;
        /// Z.6: marca `shared T name = new T()` -- el storage class
        /// dispatcha al SharedHeap en lugar del gc_heap local.  El handle
        /// resultante tiene bit 31 (SHARED_HANDLE_BIT) set.  Stdlib clases
        /// son agnosticas; solo el var-decl decide.
        bool                      is_shared = false;
        VarDeclStmt() : Stmt(NodeKind::VarDeclStmt) {}
    };

    struct ExprStmt : Stmt {
        std::unique_ptr<Expr> expr;
        ExprStmt() : Stmt(NodeKind::ExprStmt) {}
    };

    /**
     * @struct ComptimeBlockStmt
     * @brief bloque `comptime { ... }` -- scope para comptime const,
     *        comptime for y static_assert.
     *
     * Todo lo declarado dentro vive en un scope comptime nuevo (push_/
     * pop_comptime_scope en el type checker).  El bloque NO emite
     * codigo runtime; el lowering lo trata como no-op.  Runtime stmts
     * dentro son error (no se permite).
     */
    struct ComptimeBlockStmt : Stmt {
        std::vector<std::unique_ptr<Stmt>> stmts;
        ComptimeBlockStmt() : Stmt(NodeKind::ComptimeBlockStmt) {}
    };

    /**
     * @struct ComptimeForStmt
     * @brief `comptime for (i in lo..hi) { body }` unrolled.
     *
     * El type checker evalua @c lo y @c hi en compile-time.  Para cada
     * valor de @c var_name en [lo, hi) (o [lo, hi] si @c inclusive), el
     * lowering CLONA el body con @c var_name bindeado al valor concreto
     * (registrado en @c comptime_const_locals_).  Result: N copias del
     * body emitidas en secuencia, sin loop runtime.  Cuerpo emite
     * codigo runtime normalmente.
     */
    struct ComptimeForStmt : Stmt {
        std::string           var_name;     ///< nombre del index
        std::unique_ptr<Expr> lo_expr;
        std::unique_ptr<Expr> hi_expr;
        bool                  inclusive = false;  ///< true para `..=`
        std::unique_ptr<Stmt> body;        ///< BlockStmt usualmente
        ComptimeForStmt() : Stmt(NodeKind::ComptimeForStmt) {}
    };

    struct IfStmt : Stmt {
        std::unique_ptr<Expr> cond;
        std::unique_ptr<Stmt> then_branch;
        std::unique_ptr<Stmt> else_branch;    ///< null si no hay else.
        /**
         * @brief marca `comptime if (cond) { ... }`.
         *
         * El type checker exige que `cond` se evalue 100% en compile-time
         * (literales + builtins comptime + operadores logicos/aritmeticos
         * sobre constantes).  El lowering descarta la rama no tomada
         * COMPLETAMENTE -- no se baja a IR, no se emite bytecode --
         * eliminando branches y phi nodes.  Util para generic code que
         * elige implementaciones por tipo (sizeof<T>() <= 8 -> by-value
         * vs by-pointer) sin coste runtime.
         */
        bool is_comptime = false;
        IfStmt() : Stmt(NodeKind::IfStmt) {}
    };

    struct WhileStmt : Stmt {
        std::unique_ptr<Expr> cond;
        std::unique_ptr<Stmt> body;
        WhileStmt() : Stmt(NodeKind::WhileStmt) {}
    };

    /**
     * @struct DoWhileStmt
     * @brief Bucle 'do { body } while (cond);' con primera iteracion garantizada.
     *
     * Semantica: ejecuta @c body una vez, evalua @c cond y, si es verdadera,
     * repite la ejecucion del body.  Equivalente a un while que se ejecuta
     * al menos una vez.  El lowering lo baja a un patron CFG dedicado para
     * evitar duplicar el body en el AST: entry -> body -> header -> body|exit.
     */
    struct DoWhileStmt : Stmt {
        std::unique_ptr<Stmt> body;
        std::unique_ptr<Expr> cond;
        DoWhileStmt() : Stmt(NodeKind::DoWhileStmt) {}
    };

    /**
     * @struct ForStmt
     * @brief @c for(init; cond; step) body al estilo C.
     *
     * Cualquiera de los tres campos init/cond/step puede ser nulo:
     *   - init nulo  => no hay inicializador.
     *   - cond nulo  => bucle infinito (semantica de while(true)).
     *   - step nulo  => no hay paso por iteracion.
     */
    struct ForStmt : Stmt {
        std::unique_ptr<Stmt> init;
        std::unique_ptr<Expr> cond;
        std::unique_ptr<Expr> step;
        std::unique_ptr<Stmt> body;
        ForStmt() : Stmt(NodeKind::ForStmt) {}
    };

    struct ReturnStmt : Stmt {
        std::unique_ptr<Expr> value;          ///< null si return sin valor.
        ReturnStmt() : Stmt(NodeKind::ReturnStmt) {}
    };

    /**
     * @struct ForEachStmt
     * @brief @c for (T name : collection) body.
     *
     * Soportamos colecciones que sean arrays nativos con tamano conocido
     * en compile time (T[N]) o decay-to-pointer (T[]).  El lowering
     * desazucara a un counted loop usando un indice oculto.  Para T[N]
     * usamos N como cota; para T[] el caller debe pasar un tamano
     * (limitacion actual: solo @c T[N] inline esta verificado).
     */
    struct ForEachStmt : Stmt {
        std::unique_ptr<TypeNode> iter_type;
        std::string               iter_name;
        std::unique_ptr<Expr>     iter_expr;
        std::unique_ptr<Stmt>     body;
        ForEachStmt() : Stmt(NodeKind::ForEachStmt) {}
    };

    struct BreakStmt    : Stmt { BreakStmt()    : Stmt(NodeKind::BreakStmt)    {} };
    struct ContinueStmt : Stmt { ContinueStmt() : Stmt(NodeKind::ContinueStmt) {} };

    /// `goto label;` -- salto incondicional a una etiqueta declarada en
    /// la funcion actual.  Las labels son visibles en toda la funcion;
    /// se permiten gotos forward o backward.  Igual que en C.
    struct GotoStmt : Stmt {
        std::string label;
        GotoStmt() : Stmt(NodeKind::GotoStmt) {}
    };

    /// `label:` -- declaracion de etiqueta dentro del cuerpo de una
    /// funcion.  El statement que sigue se asocia con la etiqueta.
    /// Multiples labels seguidas son permitidas.
    struct LabelStmt : Stmt {
        std::string name;
        LabelStmt() : Stmt(NodeKind::LabelStmt) {}
    };

    /**
     * @struct CatchClause
     * @brief Una clausula catch dentro de un try.
     *
     * Captura excepciones de la clase @c exc_class_name (o de cualquiera
     * de sus subclases por el dispatcher de @c do_throw).  Si
     * @c exc_class_name esta vacio, es un catch-all.  El parametro
     * @c var_name (puede estar vacio) recibe el objeto excepcion
     * lanzado (tipo equivalente a @c exc_class_name).
     */
    struct CatchClause {
        SourceLoc                  loc;
        std::string                exc_class_name;  ///< vacio = catch-all
        std::string                var_name;        ///< vacio = sin binding
        std::unique_ptr<BlockStmt> body;
    };

    /**
     * @struct TryStmt
     * @brief Statement try/catch (sin finally).
     *
     * Lowering: emite @c tryenter ANTES del body con (handler, type) del
     * primer catch.  El body normal termina con @c tryleave.  El handler
     * binda el objeto excepcion (que do_throw deja en r0) a la variable
     * del catch y ejecuta el body del catch.  Multi-catch se serializa:
     * cada catch tiene su propio tryenter consecutivo.
     */
    struct TryStmt : Stmt {
        std::unique_ptr<BlockStmt>             body;
        std::vector<CatchClause>               catches;
        std::unique_ptr<BlockStmt>             finally_body;  ///< nullptr si no hay finally
        TryStmt() : Stmt(NodeKind::TryStmt) {}
    };

    /**
     * @struct ThrowStmt
     * @brief @c throw expr; lanza una excepcion con el objeto resultante.
     */
    struct ThrowStmt : Stmt {
        std::unique_ptr<Expr> value;
        ThrowStmt() : Stmt(NodeKind::ThrowStmt) {}
    };

    /**
     * @struct SynchronizedStmt
     * @brief @c synchronized (obj) @c { @c ...body... } adquiere el monitor
     *        del objeto @p obj con @c monenter al entrar y lo libera con
     *        @c monexit a la salida (incluyendo salida por excepcion).
     *
     * Lowering: emite @c monenter ANTES del body y @c monexit DESPUES del
     * body con el handle del objeto en un registro local.  Para garantizar
     * la liberacion en caso de excepcion, el lowering envuelve el body en
     * un try/finally implicito: el bloque finally hace @c monexit.
     *
     * Dentro del body, los builtins @c wait(), @c notify() y @c notifyAll()
     * sin argumento operan sobre el monitor de la expresion @p target del
     * synchronized mas cercano (resuelto por el type checker via stack).
     * No los implementamos como tokens dedicados sino como funciones
     * builtin reconocidas en lower_call.
     */
    struct SynchronizedStmt : Stmt {
        std::unique_ptr<Expr>      target;  ///< Objeto cuyo monitor se adquiere.
        std::unique_ptr<BlockStmt> body;
        SynchronizedStmt() : Stmt(NodeKind::SynchronizedStmt) {}
    };

    // -------------------------------------------------------------------
    // Declaraciones.
    // -------------------------------------------------------------------

    /**
     * @struct ParamDecl
     * @brief Declaracion de un parametro de funcion.
     */
    struct ParamDecl : Node {
        std::unique_ptr<TypeNode> type;
        std::string               name;
        /** @c true si el tipo declarado fue `expr` (solo valido en @Macro):
         * el parser captura el texto raw del call site como string en lugar
         * de parsear la expresion. El @c type queda materializado como
         * primitivo STRING para el body del macro. */
        bool                      is_expr_capture = false;
        ParamDecl() : Node(NodeKind::ParamDecl) {}
    };

    /**
     * @struct FunctionDecl
     * @brief Declaracion (o definicion) de una funcion top-level.
     *
     * Si @c body es null, se trata de una declaracion sin definicion
     * (typicamente FFI extern; las funciones extern reales se modelan
     * via @c ExternFnDecl, este caso queda como fallback general).
     */
    struct FunctionDecl : Node {
        std::unique_ptr<TypeNode>               return_type;
        std::string                              name;
        std::vector<std::unique_ptr<ParamDecl>>  params;
        std::unique_ptr<BlockStmt>               body;
        /// Phase M6.a L.3: visibilidad cross-module.  @c true (default) =
        /// publica, exportada al `.vexi` y accesible desde otros modulos.
        /// @c false = privada al modulo (no se exporta).  El parser setea
        /// segun keyword `public`/`private` precedente; sin keyword = true
        /// (default permisivo para compat con codigo existente).
        bool                                     is_public = true;
        bool                                     is_async = false; ///< @Async: el cuerpo se ejecuta en un proceso hijo, devuelve handle de Future
        /// marca `comptime fn` -- la funcion se interpreta en
        /// compile-time, no genera codigo runtime.  Solo puede llamarse
        /// desde contextos comptime (init de comptime const, condicion
        /// de comptime if, args de comptime for, otros comptime fn).
        bool                                     is_comptime = false;
        /// parametros de tipo para comptime fns genericos.
        /// Sintaxis: `comptime <T, U> R name(params) { ... }`.  Permite
        /// type-level metaprogramming -- el body puede usar T como un
        /// tipo (sizeof<T>, kind<T>, etc.) y el call site provee T:
        /// `my_fn<Vec3>(...)`.  Solo aplica a comptime fns.
        std::vector<std::string>                 type_params;
        /// marca `@Macro` -- comptime fn cuyo string de retorno
        /// se INYECTA como codigo Vex en el call site (parse + lower).
        /// Equivalente a invocar `comptime_emit_expr(my_fn(args))`
        /// transparentemente.  Solo aplica a comptime fns que retornan
        /// string.  Permite la sintaxis `i32 r = mi_macro(args);` con
        /// inyeccion automatica del codigo generado.
        bool                                     is_macro = false;
        /// marca `@Pure` -- el cuerpo NO depende ni modifica
        /// state externo (globales mutables, gensym, etc.) -- solo de
        /// los args.  El evaluador comptime puede MEMOIZAR la salida
        /// por (nombre, args).  Llamadas repetidas con mismos args
        /// devuelven el resultado cacheado sin reejecutar el body.
        /// Coste runtime cero (todo compile-time).  Si el usuario
        /// marca @Pure una fn impura, el resultado es indefinido --
        /// la memoizacion no detecta el cheating.
        bool                                     is_pure  = false;
        /// Bug fix 2026-05-23: forward declaration `T fn(args);` sin body.
        /// El parser lo setea cuando ve `;` post-`)`.  El type checker
        /// registra la firma sin requerir body; otra FunctionDecl con el
        /// mismo nombre debe aparecer despues con body, o error.
        bool                                     is_forward_decl = false;
        FunctionDecl() : Node(NodeKind::FunctionDecl) {}
    };

    /**
     * @struct ExternFnDecl
     * @brief FFI declarativo - declaracion `extern "lib.dll" fn name(...) -> R;`
     *
     * Sintaxis Vex:
     * @code
     *     extern "user32.dll" {
     *         fn MessageBoxA(i64 hwnd, ptr text, ptr caption, i32 type) -> i32;
     *     }
     *     extern "kernel32.dll" {
     *         fn Sleep(i32 ms) -> void;
     *         fn GetCurrentProcessId() -> i32;
     *     }
     * @endcode
     *
     * Cada @c fn dentro del bloque produce un @c ExternFnDecl.  El parser
     * desarma el bloque y emite N decls independientes (uno por funcion);
     * todas comparten @c lib.  El type checker registra la funcion como
     * @c Symbol::Function con @c FunctionSig::extern_lib = lib, asi el
     * lowering al ver una llamada al simbolo emite @c CALLN
     * @c @Method("<lib>:<name>") en lugar de @c CALLVM.
     *
     * Cero overhead runtime: el ensamblador resuelve @c "user32.dll" igual
     * que @c "stdlib/native/io/vesta_io" usando el mismo mecanismo
     * @c LoadLibraryA + @c GetProcAddress de @c src/ffi/native_ffi.cpp.
     */
    struct ExternFnDecl : Node {
        std::string                              lib;          ///< nombre de la libreria (e.g. "user32.dll")
        std::unique_ptr<TypeNode>                return_type;
        std::string                              name;         ///< nombre de la funcion nativa
        std::vector<std::unique_ptr<ParamDecl>>  params;
        ExternFnDecl() : Node(NodeKind::ExternFnDecl) {}
    };

    /**
     * @struct GlobalVarDecl
     * @brief Variable declarada a nivel de modulo (top-level).
     */
    struct GlobalVarDecl : Node {
        std::unique_ptr<TypeNode> type;
        std::string               name;
        std::unique_ptr<Expr>     init;
        bool                      is_const = false;
        /// Phase M6.a L.3: visibilidad cross-module (default true).
        bool                      is_public = true;
        ///  marca `comptime const`.  El init debe ser comptime-
        /// evaluable; el type checker guarda el valor en
        /// @c TypeChecker::comptime_const_values_ y los usos posteriores
        /// se baja como @c IrOp::CONST inline (cero overhead).  No genera
        /// global runtime storage.
        bool                      is_comptime = false;
        /// v4: atributos `@hot`/`@cold`/`@align(N)`/`@section("name")`.
        /// Solo aplicables a comptime const (string/array/struct).
        bool                      attr_hot   = false;
        bool                      attr_cold  = false;
        uint16_t                  attr_align = 0;     ///< 0 = default
        std::string               attr_section;        ///< vacio = default
        GlobalVarDecl() : Node(NodeKind::GlobalVarDecl) {}
    };

    /**
     * @struct TypeAliasDecl
     * @brief Declaracion de alias de tipo via @c typedef o @c using.
     *
     * Sintaxis equivalente:
     * @code
     *     typedef u32 Edad;        // estilo C
     *     using   Edad = u32;      // estilo C++ moderno
     * @endcode
     *
     * Pure frontend: el type checker registra (name -> aliased) y al
     * resolver un NamedTypeNode con ese nombre devuelve el tipo
     * subyacente.  No genera codigo.  El campo @c is_using_form se
     * conserva solo para la presentacion de mensajes de error.
     */
    struct TypeAliasDecl : Node {
        std::string               name;
        std::unique_ptr<TypeNode> aliased;
        bool                      is_using_form = false;
        /// Phase M6.a L.3: visibilidad cross-module (default true).
        bool                      is_public = true;
        /// Si @c true, el alias es un NEWTYPE: comparte la representacion
        /// del tipo subyacente pero es nominalmente DISTINTO en el type
        /// checker.  Sintaxis: @c "typedef u64 ptr new;".  El type checker
        /// asigna un @c nominal_id unico que distingue este tipo de su
        /// underlying y de otros newtypes.  Sin conversion implicita;
        /// requiere cast explicito @c (T)x para cruzar al underlying.
        bool                      is_newtype = false;
        /// Si @c true (solo aplica si @c is_newtype), el newtype es
        /// @c @opaque: ademas de la barrera nominal, el usuario NO puede
        /// inicializar con literal del underlying ni hacer cast explicito.
        /// Sintaxis: @c "typedef u64 fd new @opaque;".  Sirve para handles
        /// donde el codigo cliente no debe ver la representacion.
        bool                      is_opaque = false;
        /// Alineacion forzada en bytes via @c "@align(N)" (potencia de 2,
        /// 1..4096).  0 = sin override (alineacion natural del underlying).
        uint16_t                  align_override = 0;
        /// Tipos desde los que se permite construir este newtype via
        /// cast explicito (@c "explicit from T;" en el bloque).  Una entrada
        /// con @c is_public=true permite cross-file; sin esa marca el cast
        /// solo se admite en el mismo fichero donde se declaro el typedef.
        struct ExplicitConv {
            std::unique_ptr<TypeNode> type;
            bool                      is_public = false;
        };
        std::vector<ExplicitConv> explicit_from;
        std::vector<ExplicitConv> explicit_to;
        TypeAliasDecl() : Node(NodeKind::TypeAliasDecl) {}
    };

    /**
     * @struct ImportDecl
     * @brief Declaracion de @c import "path" [as alias] [only A, B];
     *
     * Phase M (sistema de modulos).  El @c path es siempre un string
     * literal por consistencia con @c extern "lib.dll", @c loadmodule(),
     * @c @Method("lib:fn"), etc.  Se resuelve a un fichero @c .vex en
     * el filesystem por el module resolver del compilador.
     *
     * Sin @c as ni @c only, el modulo se accede via su namespace (el
     * ultimo segmento del path: @c "editor/buffer" -> @c buffer).  Con
     * @c as, el namespace se renombra.  Con @c only, los simbolos
     * listados se inyectan al scope local sin necesidad de prefijo.
     *
     * Si @c is_public_reexport es @c true (sintaxis @c "public import"),
     * los simbolos importados se reexportan a su vez para los modulos
     * que importen ESTE modulo.  Sin esa marca, los imports son privados
     * al modulo actual (no se re-exportan transitivamente).
     */
    struct ImportDecl : Node {
        /// Ruta literal tal como aparecio en el source (sin las comillas).
        /// E.g. @c "editor/buffer" o @c "std/io".
        std::string                  path;
        /// Alias opcional para el namespace.  Vacio si no hay @c as.
        std::string                  alias;
        /// Lista de simbolos especificos a importar al scope local
        /// cuando se usa @c "only A, B" o @c "only A as B, C as D".
        /// Vacio significa "todos los publicos accesibles via namespace".
        struct OnlySymbol {
            std::string name;          // nombre original en el modulo
            std::string rename;        // nombre local (vacio = sin rename)
        };
        std::vector<OnlySymbol>      only_symbols;
        /// Si el import es @c "public import "x";" (re-export).  Sin esa
        /// marca, los simbolos importados son privados al modulo actual.
        bool                         is_public_reexport = false;

        ImportDecl() : Node(NodeKind::ImportDecl) {}
    };

    /**
     * @struct NamespaceDecl
     * @brief Declaracion de namespace inline estilo C++:
     *        @c "namespace foo { ... }"
     *
     * Agrupa declaraciones top-level bajo un namespace logico.  Estructura
     * identica a @c ModuleNode (raiz) en cuanto a contenidos, pero anidable
     * dentro del fichero.  Permite tener dos clases / structs / enums /
     * funciones con el mismo nombre en namespaces distintos del mismo
     * modulo (e.g. @c ui.Button vs @c audio.Button).
     *
     * El mangling automatico aplica prefijo @c <ns_name>__ a todas las
     * declaraciones internas, de modo que en el bytecode emitido los
     * labels son @c ui__Button vs @c audio__Button.  Anidamiento se
     * concatena: @c "namespace a { namespace b { class C } }" produce
     * label @c a__b__C.  Cero overhead runtime: el namespace es
     * puramente lexical.
     *
     * Acceso desde fuera del namespace: sintaxis qualified @c foo.X
     * (no @c foo::X -- Vex prefiere el punto sobre el scope-op de C++).
     */
    struct NamespaceDecl : Node {
        std::string                          name;       ///< "ui", "audio", etc.
        std::vector<std::unique_ptr<Node>>   decls;       ///< contenidos top-level
        NamespaceDecl() : Node(NodeKind::NamespaceDecl) {}
    };

    /**
     * @struct StructFieldDecl
     * @brief Campo de un struct: tipo + nombre.
     *
     * Solo lo usa @c StructDecl; no es un nodo de top-level.  No deriva
     * de Node porque vive como elemento de un vector dentro de StructDecl;
     * no tenemos uso polimorfico para campos.
     */
    struct StructFieldDecl {
        std::unique_ptr<TypeNode> type;
        std::string               name;
        SourceLoc                 loc;
        /// Bit field width.  0 = campo normal (byte-aligned).
        /// >0 = bit field con esta cantidad de bits.  El type checker
        /// calcula bit_offset y los empaqueta en storage words del tipo
        /// declarado (i32 -> 32 bits por word, etc.).  Estilo C/C++.
        uint8_t                   bit_width = 0;
    };

    /**
     * @struct StructDecl
     * @brief Declaracion de un @c struct (value type sin herencia).
     *
     * Solo cubre campos; los metodos opcionales llegan en hitos posteriores.  
     * El type checker
     * registra el struct con su layout y permite usarlo via
     * NamedTypeNode.
     */
    struct StructDecl : Node {
        std::string                  name;
        std::vector<StructFieldDecl> fields;
        /// Phase M6.a L.3: visibilidad cross-module (default true).
        bool                         is_public = true;
        /// marca `@Introspect` -- el compilador
        /// emite IntrospectInfo POD en static_data y registra el tipo en
        /// el global `__introspect_registry` para que `find_type("Name")`
        /// runtime lo encuentre.  Sin esta marca, el tipo solo es
        /// introspectable via builtins comptime (sin overhead).
        bool                         is_introspect = false;
        StructDecl() : Node(NodeKind::StructDecl) {}
    };

    /**
     * @struct EnumVariantDecl
     * @brief Variante de un @c enum.
     *
     * Cada variante tiene un nombre y una lista (posiblemente vacia) de
     * tipos para sus payloads.  Variantes sin payload son meros
     * "marcadores" (e.g. @c None, @c Red).  Variantes con payloads se
     * representan en memoria como una tagged union: tag (i64) + payload
     * fields concatenados a partir del offset 8, padded a 8 bytes cada
     * uno para alineacion uniforme.
     *
     * El tag asignado es el indice de la variante en
     * @c EnumDecl::variants (0..N-1), estable entre compilaciones.
     */
    struct EnumVariantDecl {
        std::string                            name;
        std::vector<std::unique_ptr<TypeNode>> field_types; ///< Vacio para variantes sin payload.
        SourceLoc                              loc;
    };

    /**
     * @struct EnumDecl
     * @brief Declaracion de un tipo algebraico.
     *
     * Sintaxis aceptada por el parser:
     * @code
     *   enum Color {
     *       Red,                    // sin payload
     *       Green(i32),             // un payload
     *       Blue(i32, i32)          // multiples payloads
     *   }
     * @endcode
     *
     * Layout en memoria (mismo modelo que @c Optional / @c Result builtins):
     *   `[+0 i64 tag][+8 payload_field_0][+16 payload_field_1] ...`
     * El tamano total de un valor del enum es @c 8 + 8 * max_payload_fields,
     * suficiente para cualquier variante.  Cero alocaciones de heap; cero
     * overhead GC.
     *
     * Construccion: @c Color.Green(42) -> escribe tag=1 + payload[0]=42 en
     * un slot recien alocado en stack del caller.
     *
     * Destructuring: @c match c { case Green(n) => ... } -> el lowering
     * carga el tag, dispatcha via @c jumptable (0x27) al label de la
     * variante, y en el body ya tiene el payload accesible via LOAD del
     * slot del scrutinee a un SSA value bindeado al nombre @c n.
     */
    struct EnumDecl : Node {
        std::string                  name;
        std::vector<EnumVariantDecl> variants;
        /// marca `@Introspect`.  Ver `StructDecl`.
        bool                         is_introspect = false;
        /// Phase M6.a L.3: visibilidad cross-module (default true).
        bool                         is_public = true;
        /// L2.3: parametros de tipo opcionales `enum Maybe<T> { None, Some(T) }`.
        /// Si no esta vacio, el enum es un template y se monomorphiza on demand
        /// en cada uso `Maybe<i32>` (mismo modelo que generic classes A.8).
        std::vector<std::string>     type_params;
        EnumDecl() : Node(NodeKind::EnumDecl) {}
    };

    /**
     * @struct ClassFieldDecl
     * @brief Campo de instancia o estatico de una clase.
     *
     * Estructura ligera (no hereda de Node) usada como elemento de un
     * vector dentro de @c ClassDecl.  Modificadores @c access, @c is_static
     * y @c is_final se llenan por el parser; el type checker los valida
     * y los traduce a flags de @c FieldDecl al registrar la clase.
     */
    struct ClassFieldDecl {
        std::unique_ptr<TypeNode> type;
        std::string               name;
        std::unique_ptr<Expr>     init;       ///< null = sin valor por defecto
        SourceLoc                 loc;
        uint8_t                   access    = 0; ///< 0 = default/public, 1 = private, 2 = protected
        bool                      is_static = false;
        bool                      is_final  = false;
    };

    /**
     * @struct ClassMethodDecl
     * @brief Metodo o constructor de una clase.
     *
     * Reusa la firma de @c FunctionDecl pero anade un primer parametro
     * implicito @c this (no listado en @c params).  El campo
     * @c is_constructor distingue el ctor del resto de metodos: el ctor
     * tiene el mismo nombre que la clase y no devuelve valor.
     */
    struct ClassMethodDecl : Node {
        std::unique_ptr<TypeNode>               return_type; ///< null si es constructor
        std::string                              name;
        std::vector<std::unique_ptr<ParamDecl>>  params;
        std::unique_ptr<BlockStmt>               body;
        uint8_t                                  access         = 0;
        bool                                     is_static      = false;
        bool                                     is_final       = false;
        bool                                     is_override    = false;
        bool                                     is_constructor = false;
        /// true si es destructor `~ClassName()` declarado dentro
        /// del cuerpo de la clase.  Sin parametros (validado en parser).
        /// Body lowereado como un metodo void normal; el frontend invoca
        /// CALLVIRT al destructor automaticamente al exit del scope para
        /// instancias locales que NO escapan (escape detection ya
        /// rastrea esto).
        bool                                     is_destructor  = false;
        /// @brief Si el metodo es un advice AOP.  @c advice_kind:
        /// 0 = no es advice (default), 1 = BEFORE, 2 = AFTER, 3 = AROUND.
        /// @c advice_target tiene la forma "ClassName.methodName" extraida
        /// del argumento de la anotacion (@Before("Animal.sonido")).  El
        /// lowering lo resuelve via findclass/findmethod en __module_init.
        uint8_t                                  advice_kind    = 0;
        std::string                              advice_target;

        /// @brief si !=0, el metodo es accesor de propiedad.
        /// 1 = getter (`public get name => expr;`).  Sin params, devuelve T.
        /// 2 = setter (`public set name(T v) { ... }`).  1 param, retorna void.
        /// El nombre interno almacenado en @c name es `get_<prop>` o
        /// `set_<prop>` para evitar colision con el field del mismo nombre;
        /// el frontend reescribe el acceso `obj.prop`/`obj.prop = X` a la
        /// llamada al metodo accesor correspondiente.
        uint8_t                                  property_kind  = 0;
        std::string                              property_name; ///< nombre logico de la propiedad (e.g. "value")

        /// @brief @Inline marca para inlining en el call site.
        /// El lowering, en lugar de emitir CALLVIRT, sustituye la llamada
        /// por una copia del body del metodo con `this` y los params
        /// resueltos a los SSA values del call site.  MVP: solo metodos
        /// expression-bodied o bloques con 1 statement, sin try/await/spawn.
        bool                                     is_inline      = false;
        ClassMethodDecl() : Node(NodeKind::FunctionDecl) {}
    };

    /**
     * @struct ClassDecl
     * @brief Declaracion de una clase Vex.
     *
     * Reference type con semantica Java: instancias creadas con
     * @c new ClassName(args) viven en heap (NEWOBJ), gestionadas por GC.
     * El type checker registra la clase en su tabla local; el lowering
     * la traduce a un bloque de inicializacion (defclass + deffield* +
     * defmethod*) que se ejecuta una vez por modulo antes de @c main.
     *
     * solo clases sin herencia ni interfaces.  La
     * superclass por defecto es @c null (equivalente a Object una vez
     * exista). Se anaden modificadores y final.
     */
    struct ClassDecl : Node {
        std::string                                       name;
        std::string                                       super_name;   ///< vacio = sin superclase explicita
        std::vector<std::string>                          interface_names;
        std::vector<ClassFieldDecl>                       fields;
        std::vector<std::unique_ptr<ClassMethodDecl>>     methods;
        bool                                              is_final = false;
        /// Phase M6.a L.3: visibilidad cross-module (default true).
        bool                                              is_public = true;
        /// Parametros de tipo (templates).  `class Box<T>` produce
        /// type_params = ["T"].  Vacio para clases no genericas.  El
        /// type checker no procesa la clase como tal sino que la guarda
        /// como "plantilla" en @c generic_class_templates_; cada
        /// instanciado (`Box<i32>`) genera por monomorphizacion una
        /// nueva ClassDecl concreta con T -> i32 y la procesa como una
        /// clase normal (collect_classes + lower_class_methods).
        std::vector<std::string>                          type_params;
        /// @brief Marca de aspecto AOP.  Si es @c true, los metodos con
        /// @Before/@After/@Around dentro de esta clase se registran como
        /// advices en __module_init (ademas de definirse como metodos
        /// normales para que el dispatch directo y la reflexion funcionen).
        bool                                              is_aspect = false;
        /// @brief Marca de interfaz pura.  Si es @c true, esta declaracion
        /// procede del keyword @c interface y todos sus metodos son
        /// abstractos (sin body).  El type checker usa esto para validar
        /// que las clases implementadoras provean los metodos requeridos
        /// con firmas compatibles.  Las interfaces SI se registran en el
        /// ClassRegistry como ClassInfo* (sin instanciar) para que la
        /// reflexion (findclass) y el typeswitch las puedan localizar.
        bool                                              is_interface = false;
        /// marca `@Introspect`.  Ver `StructDecl`.
        bool                                              is_introspect = false;
        ClassDecl() : Node(NodeKind::ClassDecl) {}
    };

    /**
     * @struct ModuleNode
     * @brief Raiz del AST: lista de declaraciones top-level.
     */
    struct ModuleNode : Node {
        std::vector<std::unique_ptr<Node>> decls; ///< FunctionDecl o GlobalVarDecl.
        ModuleNode() : Node(NodeKind::Module) {}
    };

    // -------------------------------------------------------------------
    // Helpers de mapeo desde tokens (usados por el parser).
    // -------------------------------------------------------------------

    /**
     * @brief Convierte un TokenKind operador binario a BinOp.
     *
     * Devuelve @c true si el token es un operador binario reconocido.
     * El BinOp se escribe en @p out.
     */
    bool binop_from_token(TokenKind k, BinOp &out) noexcept;

    /**
     * @brief Convierte un TokenKind operador de asignacion a AssignOp.
     *
     * Devuelve @c true si el token es un operador de asignacion (incluido
     * el simple '=').  El AssignOp se escribe en @p out.
     */
    bool assignop_from_token(TokenKind k, AssignOp &out) noexcept;

} // namespace vex::ast

#endif // VEX_AST_H
