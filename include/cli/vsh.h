/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file vsh.h
 * @brief Motor del lenguaje de scripting VestaShell (.vsh).
 *
 * Define el API publico del interprete: VshInterpreter, Value, VshEnv,
 * el Lexer (VshLexer), el Parser (VshParser), los nodos AST (AstNode),
 * y las senales de control de flujo (ReturnSignal, BreakSignal, etc.).
 *
 * El lenguaje soporta:
 *   - Tipos: null, bool, int(64), float(64), string, list, map, function, class, instance
 *   - Control: if/elif/else, while, for/in, break, continue, return
 *   - Funciones de primera clase con closures
 *   - Manejo de errores con try/catch (con catch tipado) y throw
 *   - Clases y herencia: class Name [: Parent] { fn method(self,...){} }
 *   - Anotaciones de tipo opcionales en let y parametros de funcion
 *   - Importacion de otros .vsh
 *   - String interpolation: "Hola ${nombre}"
 *   - REPL interactivo con run_interactive()
 *   - Integracion con el REPL: comandos del REPL llamables como funciones
 *
 * Uso tipico:
 * @code
 *   vsh::VshInterpreter interp([](const std::string &line) {
 *       return dispatch_table_cmd(line);
 *   });
 *   interp.exec_file("mi_script.vsh");
 * @endcode
 */

#ifndef VSH_H
#define VSH_H

#include <string>
#include <vector>
#include <memory>
#include <variant>
#include <unordered_map>
#include <functional>
#include <stdexcept>
#include <deque>
#include <cstdint>

namespace vsh {

// ============================================================
// Seccion 0: Declaraciones adelantadas (forward declarations)
// necesarias para resolver dependencias circulares entre tipos
// ============================================================

struct VshEnv;       ///< forward: scope / entorno de variables
struct VshFunction;  ///< forward: funcion del lenguaje
struct VshClass;     ///< forward: clase del lenguaje
struct VshInstance;  ///< forward: instancia de clase

// ============================================================
// Seccion 1: Tipos de valor del lenguaje
// ============================================================

/** @brief Categoria de tipo de un valor VSH en tiempo de ejecucion. */
enum class VshType : uint8_t {
    Null,      ///< valor nulo
    Bool,      ///< booleano
    Int,       ///< entero de 64 bits
    Float,     ///< flotante de 64 bits
    String,    ///< cadena de texto
    List,      ///< lista dinamica
    Map,       ///< mapa clave-valor
    Function,  ///< funcion (closure o builtin)
    Class,     ///< descriptor de clase (VshClass)
    Instance   ///< instancia de clase (VshInstance)
};

/**
 * @brief Valor dinamico del lenguaje VestaShell.
 *
 * Los escalares (null, bool, int, float) se almacenan inline sin heap.
 * Los contenedores (string, list, map, function, class, instance) se
 * comparten via shared_ptr para que las asignaciones y los closures no
 * copien datos en profundidad.
 */
struct Value {
    using ListPtr     = std::shared_ptr<std::vector<Value>>;
    using MapPtr      = std::shared_ptr<std::unordered_map<std::string, Value>>;
    using FnPtr       = std::shared_ptr<VshFunction>;
    using ClassPtr    = std::shared_ptr<VshClass>;
    using InstancePtr = std::shared_ptr<VshInstance>;

    std::variant<
        std::monostate,   ///< Null
        bool,             ///< Bool
        int64_t,          ///< Int
        double,           ///< Float
        std::string,      ///< String  (SSO inline para cadenas cortas)
        ListPtr,          ///< List
        MapPtr,           ///< Map
        FnPtr,            ///< Function
        ClassPtr,         ///< Class
        InstancePtr       ///< Instance
    > data;

    // Constructores
    Value() = default;
    explicit Value(bool b)        : data(b) {}
    explicit Value(int64_t i)     : data(i) {}
    explicit Value(int i)         : data(int64_t(i)) {}
    explicit Value(double d)      : data(d) {}
    explicit Value(std::string s) : data(std::move(s)) {}
    explicit Value(const char *s) : data(std::string(s)) {}
    explicit Value(ListPtr lp)    : data(std::move(lp)) {}
    explicit Value(MapPtr mp)     : data(std::move(mp)) {}
    explicit Value(FnPtr fp)      : data(std::move(fp)) {}
    explicit Value(ClassPtr cp)   : data(std::move(cp)) {}
    explicit Value(InstancePtr ip): data(std::move(ip)) {}

    /** @brief Devuelve el tipo del valor segun la variante activa. */
    VshType type() const noexcept;

    bool is_null()     const noexcept { return std::holds_alternative<std::monostate>(data); }
    bool is_bool()     const noexcept { return std::holds_alternative<bool>(data); }
    bool is_int()      const noexcept { return std::holds_alternative<int64_t>(data); }
    bool is_float()    const noexcept { return std::holds_alternative<double>(data); }
    bool is_string()   const noexcept { return std::holds_alternative<std::string>(data); }
    bool is_list()     const noexcept { return std::holds_alternative<ListPtr>(data); }
    bool is_map()      const noexcept { return std::holds_alternative<MapPtr>(data); }
    bool is_function() const noexcept { return std::holds_alternative<FnPtr>(data); }
    bool is_class()    const noexcept { return std::holds_alternative<ClassPtr>(data); }
    bool is_instance() const noexcept { return std::holds_alternative<InstancePtr>(data); }
    bool is_numeric()  const noexcept { return is_int() || is_float(); }

    bool   truthy()    const noexcept; ///< false = null, false, 0, 0.0, "", [], {}
    double as_number() const;          ///< eleva int a double si es necesario
    std::string to_string()  const;    ///< representacion para echo/str()
    std::string to_repr()    const;    ///< representacion con comillas para listas/mapas

    int64_t            as_int()      const { return std::get<int64_t>(data); }
    double             as_float()    const { return std::get<double>(data); }
    bool               as_bool()     const { return std::get<bool>(data); }
    const std::string& as_string()   const { return std::get<std::string>(data); }
    ListPtr            as_list()     const { return std::get<ListPtr>(data); }
    MapPtr             as_map()      const { return std::get<MapPtr>(data); }
    FnPtr              as_fn()       const { return std::get<FnPtr>(data); }
    ClassPtr           as_class()    const { return std::get<ClassPtr>(data); }
    InstancePtr        as_instance() const { return std::get<InstancePtr>(data); }

    bool operator==(const Value &o) const noexcept;
    bool operator!=(const Value &o) const noexcept { return !(*this == o); }
    bool operator< (const Value &o) const;
    bool operator<=(const Value &o) const { return *this < o || *this == o; }
    bool operator> (const Value &o) const { return o < *this; }
    bool operator>=(const Value &o) const { return o <= *this; }
};

/** @brief Crea un Value lista vacia. */
inline Value make_list_val() {
    return Value(std::make_shared<std::vector<Value>>());
}
/** @brief Crea un Value mapa vacio. */
inline Value make_map_val() {
    return Value(std::make_shared<std::unordered_map<std::string, Value>>());
}

// ============================================================
// Seccion 2: Entorno / Scope
// ============================================================

/**
 * @brief Scope encadenado de variables VSH.
 *
 * Cada llamada a funcion, cada bloque if/while/for crea un hijo.
 * Los closures capturan el scope en el momento de la definicion de fn.
 */
struct VshEnv : std::enable_shared_from_this<VshEnv> {
    std::unordered_map<std::string, Value> vars;
    std::shared_ptr<VshEnv>                parent;

    explicit VshEnv(std::shared_ptr<VshEnv> p = nullptr) : parent(std::move(p)) {}

    Value& get(const std::string &name);                  ///< busca en cadena; lanza si no existe
    void   define(const std::string &name, Value v);      ///< define en scope actual (let)
    void   assign(const std::string &name, Value v);      ///< reasigna en el scope donde existe
    bool   has(const std::string &name) const noexcept;   ///< comprueba existencia

    /** @brief Crea un entorno hijo con este como padre. */
    std::shared_ptr<VshEnv> make_child();
};

// ============================================================
// Seccion 3: Funcion VSH
// ============================================================

/**
 * @brief Descriptor de una funcion definida en VSH.
 *
 * Las funciones nativas (built-ins) se almacenan aparte en
 * VshInterpreter::builtins_; esta estructura solo cubre las fn definidas
 * con la sintaxis `fn nombre(...) { ... }` en el script.
 */
struct VshFunction {
    std::string              name;        ///< identificador (vacio para lambdas)
    std::vector<std::string> params;      ///< lista de parametros formales
    std::vector<std::string> param_types; ///< tipo esperado por cada parametro (vacio = sin restriccion)
    std::shared_ptr<void>    body;        ///< nodo AstNode del bloque (erased type)
    std::shared_ptr<VshEnv>  closure_env; ///< scope capturado al definir la funcion
    std::string              doc;         ///< docstring: primera sentencia del cuerpo si es string literal
};

// ============================================================
// Seccion 4: Clase e Instancia
// ============================================================

/**
 * @brief Descriptor de una clase VSH.
 *
 * Una clase tiene nombre, clase padre opcional, mapa de metodos y docstring.
 * Los metodos son funciones VSH (VshFunction) que reciben 'self' como primer
 * parametro por convencion.
 */
struct VshClass {
    std::string                                              name;    ///< nombre de la clase
    std::shared_ptr<VshClass>                                parent;  ///< clase padre o nullptr
    std::unordered_map<std::string, std::shared_ptr<VshFunction>> methods; ///< metodos de la clase
    std::string                                              doc;     ///< docstring de la clase
};

/**
 * @brief Instancia de una clase VSH.
 *
 * Almacena la clase a la que pertenece y el mapa de atributos de instancia.
 * Los atributos se crean dinamicamente al hacer 'self.campo = valor'.
 */
struct VshInstance {
    std::shared_ptr<VshClass>              klass; ///< clase de la que es instancia
    std::unordered_map<std::string, Value> attrs; ///< atributos de instancia
};

/** @brief Crea un Value instancia a partir de un puntero de instancia. */
inline Value make_instance_val(std::shared_ptr<VshInstance> ip) {
    return Value(std::move(ip));
}
/** @brief Crea un Value clase a partir de un puntero de clase. */
inline Value make_class_val(std::shared_ptr<VshClass> cp) {
    return Value(std::move(cp));
}

// ============================================================
// Seccion 5: Excepciones
// ============================================================

/** @brief Error de ejecucion capturable en el script con try/catch. */
struct VshRuntimeError : std::runtime_error {
    int         line, col;
    std::string vsh_msg; ///< mensaje que recibe el bloque catch del script
    VshRuntimeError(std::string msg, int l = 0, int c = 0)
        : std::runtime_error(msg), line(l), col(c), vsh_msg(msg) {}
};

/**
 * @brief Error de ejecucion causado por 'throw instancia'.
 *
 * Ademas del mensaje de texto (para compatibilidad con catch sin tipo),
 * lleva el Value de la instancia lanzada para catch tipado.
 */
struct VshInstanceError : VshRuntimeError {
    Value thrown_val; ///< valor instance lanzado por throw
    VshInstanceError(std::string msg, Value v, int l = 0, int c = 0)
        : VshRuntimeError(std::move(msg), l, c), thrown_val(std::move(v)) {}
};

/** @brief Error de sintaxis (no capturable en el script). */
struct VshParseError : std::runtime_error {
    int line, col;
    VshParseError(const std::string &msg, int l = 0, int c = 0)
        : std::runtime_error(msg), line(l), col(c) {}
};

// ============================================================
// Seccion 6: Senales de control de flujo
// ============================================================

/** @brief Lanzado por `return expr` — capturado en exec_fn_call. */
struct ReturnSignal  { Value val; explicit ReturnSignal(Value v) : val(std::move(v)) {} };
/** @brief Lanzado por `break`     — capturado en exec_while/exec_for_in. */
struct BreakSignal   { int line = 0; };
/** @brief Lanzado por `continue`  — capturado en exec_while/exec_for_in. */
struct ContinueSignal{ int line = 0; };

// ============================================================
// Seccion 7: Tokens
// ============================================================

/** @brief Todos los tipos de token del lexer de VestaShell. */
enum class TK : uint16_t {
    // Literales
    INT_LIT, FLOAT_LIT, STRING_LIT, BOOL_LIT, NULL_LIT, IDENT,
    // Palabras clave existentes
    KW_LET, KW_FN, KW_RETURN, KW_IF, KW_ELIF, KW_ELSE,
    KW_WHILE, KW_FOR, KW_IN, KW_BREAK, KW_CONTINUE,
    KW_TRY, KW_CATCH, KW_IMPORT, KW_AND, KW_OR, KW_NOT,
    // Nuevas palabras clave para OOP y errores tipados
    KW_CLASS,    ///< 'class' — declaracion de clase
    KW_SUPER,    ///< 'super' — referencia a clase padre dentro de un metodo
    KW_THROW,    ///< 'throw' — lanzar un error o instancia
    // Operadores aritmeticos
    PLUS, MINUS, STAR, SLASH, PERCENT, STARSTAR,
    // Asignacion compuesta
    PLUS_EQ, MINUS_EQ, STAR_EQ, SLASH_EQ, PERCENT_EQ,
    // Comparacion
    EQ_EQ, BANG_EQ, LT, GT, LT_EQ, GT_EQ,
    // Asignacion
    EQ,
    // Delimitadores
    LPAREN, RPAREN, LBRACE, RBRACE, LBRACKET, RBRACKET,
    COMMA, COLON, DOT, SEMICOLON,
    // Terminador de sentencia logico
    NEWLINE,
    // String interpolado: el lexer emite estos tokens en secuencia
    ISTR_TEXT,       ///< fragmento literal dentro de "..."
    ISTR_EXPR_BEGIN, ///< encontro `${` dentro de una string
    ISTR_EXPR_END,   ///< `}` de cierre de la expresion embebida
    ISTR_END,        ///< cierre final `"` del string interpolado
    // Especiales
    END_OF_FILE, LEX_ERROR,
};

/** @brief Un token producido por VshLexer. */
struct Token {
    TK          kind    = TK::END_OF_FILE;
    std::string lexeme;          ///< texto original sin procesar
    int         line    = 1;
    int         col     = 1;
    int64_t     int_val = 0;     ///< valor pre-parseado para INT_LIT
    double      flt_val = 0.0;   ///< valor pre-parseado para FLOAT_LIT
};

// ============================================================
// Seccion 8: AST
// ============================================================

/** @brief Tipo de nodo del Arbol de Sintaxis Abstracta de VestaShell. */
enum class AstKind : uint8_t {
    // Expresiones
    Literal,        ///< null/bool/int/float/string ya evaluada
    InterpString,   ///< "Hola ${nombre}" (lista de partes)
    Ident,          ///< referencia a variable
    BinOp,          ///< left OP right
    UnaryOp,        ///< -expr / not expr
    Assign,         ///< target = value
    CompoundAssign, ///< target OP= value
    Call,           ///< callee(args...)
    Index,          ///< expr[expr]
    ListLit,        ///< [e1, e2, ...]
    MapLit,         ///< {"k": v, ...}
    FnExpr,         ///< fn(params){body} (funcion anonima)
    FieldAccess,    ///< obj.field (acceso a campo o metodo de instancia/clase)
    // Sentencias
    ExprStmt,
    LetDecl,
    FnDecl,
    Return,
    Break,
    Continue,
    IfStmt,
    WhileStmt,
    ForInStmt,
    TryCatch,
    Import,
    Block,
    Program,
    ClassDecl,      ///< declaracion de clase
    ThrowStmt,      ///< sentencia throw
};

/** @brief Rama de un if/elif/else; cond=nullptr para else. */
struct IfBranch {
    std::shared_ptr<struct AstNode> cond;
    std::shared_ptr<struct AstNode> body;
};

/**
 * @brief Nodo del AST de VestaShell.
 *
 * Patron "fat node": un unico struct con campos opcionales segun el kind.
 * Evita una jerarquia de clases profunda para un interprete embebido.
 */
struct AstNode {
    AstKind kind = AstKind::Literal;
    int     line = 0;
    int     col  = 0;

    // Campos por kind:
    Value   lit_val;                                         ///< Literal
    std::string name;                                        ///< Ident / LetDecl / FnDecl / catch-var / ForIn-var / Import / ClassDecl / FieldAccess.field
    TK      op = TK::END_OF_FILE;                           ///< BinOp / UnaryOp / CompoundAssign
    std::shared_ptr<AstNode> left;                          ///< BinOp.left / Assign.target / Index.obj / UnaryOp.operand
    std::shared_ptr<AstNode> right;                         ///< BinOp.right / Assign.val / LetDecl.init / Return.val / ThrowStmt.expr
    std::shared_ptr<AstNode> callee;                        ///< Call.callee
    std::vector<std::shared_ptr<AstNode>> args;             ///< Call.args / Block.stmts / Program.stmts / InterpString.parts / ListLit / MapLit(pairs) / ClassDecl.methods
    std::vector<IfBranch>    branches;                      ///< IfStmt
    std::shared_ptr<AstNode> cond;                          ///< WhileStmt.cond / ForIn.iter (reused)
    std::shared_ptr<AstNode> body;                          ///< WhileStmt/ForIn body
    std::vector<std::string> params;                        ///< FnDecl / FnExpr
    std::vector<std::string> param_types;                   ///< tipo de cada parametro (vacio = sin restriccion)
    std::shared_ptr<AstNode> try_body;                      ///< TryCatch
    std::shared_ptr<AstNode> catch_body;                    ///< TryCatch
    std::string type_hint;                                  ///< LetDecl: anotacion de tipo opcional ("int", "str", ...)
    std::string parent_class;                               ///< ClassDecl: nombre de la clase padre (vacio si no hay)
    std::string catch_type_hint;                            ///< TryCatch: tipo esperado en catch (vacio = captura todo)
};

using AstNodePtr = std::shared_ptr<AstNode>;

/** @brief Crea un nodo AST con kind, linea y columna. */
inline AstNodePtr make_node(AstKind k, int line = 0, int col = 0) {
    auto n = std::make_shared<AstNode>();
    n->kind = k; n->line = line; n->col = col;
    return n;
}

// ============================================================
// Seccion 9: Lexer
// ============================================================

/**
 * @brief Analizador lexico de VestaShell.
 *
 * Emite tokens bajo demanda via next()/peek(). Para strings interpoladas
 * produce la secuencia ISTR_TEXT / ISTR_EXPR_BEGIN / ... / ISTR_EXPR_END /
 * ISTR_END antes de que el parser las consuma.
 */
class VshLexer {
public:
    explicit VshLexer(std::string source, std::string filename = "<input>");

    Token       next();
    const Token& peek(int offset = 0); ///< 0 = siguiente sin consumir
    bool        at_end() const;

private:
    std::string      src_, filename_;
    size_t           pos_  = 0;
    int              line_ = 1, col_ = 1;
    std::deque<Token> buf_; ///< buffer para lookahead y tokens de interpolacion

    char   cur()    const;
    char   peek1()  const;
    char   advance();
    bool   match(char c);
    void   skip_ws_and_comments();
    Token  make_tok(TK k, std::string lex = {}) const;
    Token  read_one_token();
    Token  read_number();
    Token  read_string_or_interp();
    Token  read_ident_kw();
    void   push_interp_tokens(const std::string &raw, int base_line, int base_col);
    static const std::unordered_map<std::string, TK>& kw_map();
};

// ============================================================
// Seccion 10: Parser
// ============================================================

/**
 * @brief Analizador sintactico de VestaShell (descenso recursivo).
 *
 * Produce un AstNode de tipo Program a partir del token stream del lexer.
 */
class VshParser {
public:
    explicit VshParser(VshLexer &lex);
    AstNodePtr parse_program();

private:
    VshLexer &lex_;
    Token     cur_, peek_;

    Token      advance();
    Token      expect(TK k, const char *msg);
    bool       check(TK k) const;
    bool       match(TK k);
    void       skip_nl();
    void       end_stmt();

    AstNodePtr stmt();
    AstNodePtr let_decl();
    AstNodePtr fn_decl();
    AstNodePtr class_decl();
    AstNodePtr throw_stmt();
    AstNodePtr if_stmt();
    AstNodePtr while_stmt();
    AstNodePtr for_stmt();
    AstNodePtr try_stmt();
    AstNodePtr ret_stmt();
    AstNodePtr brk_stmt();
    AstNodePtr cnt_stmt();
    AstNodePtr import_stmt();
    AstNodePtr block();
    AstNodePtr expr_stmt();

    AstNodePtr expr();
    AstNodePtr assign_expr();
    AstNodePtr or_expr();
    AstNodePtr and_expr();
    AstNodePtr not_expr();
    AstNodePtr cmp_expr();
    AstNodePtr add_expr();
    AstNodePtr mul_expr();
    AstNodePtr pow_expr();
    AstNodePtr unary_expr();
    AstNodePtr postfix_expr();
    AstNodePtr primary();
    AstNodePtr interp_string();
    AstNodePtr list_lit();
    AstNodePtr map_lit();
    AstNodePtr fn_expr();
    std::vector<AstNodePtr>  args_list();
    /** @brief Parsea lista de parametros y rellena el vector de tipos paralelo en n. */
    std::vector<std::string> params_list(AstNode *n = nullptr);
};

// ============================================================
// Seccion 11: Interprete principal
// ============================================================

/**
 * @brief Interprete de VestaShell.
 *
 * Recibe un callback `ReplDispatch` que apunta a `dispatch_table_cmd`
 * del REPL. Los comandos del REPL son llamables como funciones en el script;
 * si el interprete no encuentra la funcion en el scope ni en los built-ins,
 * construye la linea de comando y la pasa al dispatcher.
 */
class VshInterpreter {
public:
    using ReplDispatch  = std::function<bool(const std::string &)>;
    using NativeFn      = std::function<Value(std::vector<Value>)>;
    using ReadlineFn    = std::function<std::string(const std::string &)>;

    explicit VshInterpreter(ReplDispatch dispatch = nullptr);
    ~VshInterpreter();

    /** @brief Ejecuta un fichero .vsh. */
    void exec_file(const std::string &path);

    /** @brief Ejecuta una cadena de codigo VSH. */
    void exec_string(const std::string &src, const std::string &name = "<string>");

    /**
     * @brief Ejecuta el REPL interactivo de VestaShell.
     *
     * Muestra el prompt ">>> " para nuevas sentencias y "... " para
     * continuaciones. Si readline_fn no es null, la usa para leer lineas;
     * de lo contrario usa std::getline. Las expresiones simples (ExprStmt)
     * imprimen su resultado via to_repr(). Escribe "exit" o "quit" para salir.
     *
     * @param readline_fn  Funcion opcional para leer una linea dado el prompt.
     */
    void run_interactive(ReadlineFn readline_fn = nullptr);

    /** @brief Acceso al scope global (para modo REPL interactivo). */
    std::shared_ptr<VshEnv> global_env() { return global_; }

    /** @brief Registra una funcion nativa adicional. */
    void register_builtin(const std::string &name, NativeFn fn);

    /**
     * @brief Establece la lista ARGV accesible desde el script.
     *
     * Se debe llamar ANTES de exec_file() para que el script vea los
     * argumentos. Convencion estilo Python:
     *   ARGV[0] = ruta del script
     *   ARGV[1..] = argumentos posicionales tras --script <fichero>
     *
     * Si no se llama, ARGV queda como lista vacia.
     */
    void set_argv(const std::vector<std::string> &argv);


private:
    std::shared_ptr<VshEnv>                  global_;
    ReplDispatch                             repl_dispatch_;
    std::unordered_map<std::string, NativeFn> builtins_;
    std::vector<std::string>                 import_stack_;

    /** @brief Registra todas las funciones built-in del lenguaje. */
    void register_builtins();

    Value eval(const AstNodePtr &n, std::shared_ptr<VshEnv> env);
    void  exec(const AstNodePtr &n, std::shared_ptr<VshEnv> env);

    Value eval_literal    (const AstNode &n);
    Value eval_interp_str (const AstNode &n, std::shared_ptr<VshEnv> env);

    Value eval_ident      (const AstNode &n, std::shared_ptr<VshEnv> env);
    Value eval_binop      (const AstNode &n, std::shared_ptr<VshEnv> env);
    Value eval_unary      (const AstNode &n, std::shared_ptr<VshEnv> env);
    Value eval_assign     (const AstNode &n, std::shared_ptr<VshEnv> env);
    Value eval_compound   (const AstNode &n, std::shared_ptr<VshEnv> env);
    Value eval_call       (const AstNode &n, std::shared_ptr<VshEnv> env);
    Value eval_index      (const AstNode &n, std::shared_ptr<VshEnv> env);
    Value eval_list_lit   (const AstNode &n, std::shared_ptr<VshEnv> env);
    Value eval_map_lit    (const AstNode &n, std::shared_ptr<VshEnv> env);
    Value eval_field      (const AstNode &n, std::shared_ptr<VshEnv> env);

    void exec_block     (const AstNode &n, std::shared_ptr<VshEnv> env);
    void exec_let       (const AstNode &n, std::shared_ptr<VshEnv> env);
    void exec_fn_decl   (const AstNode &n, std::shared_ptr<VshEnv> env);
    void exec_class_decl(const AstNode &n, std::shared_ptr<VshEnv> env);
    void exec_if        (const AstNode &n, std::shared_ptr<VshEnv> env);
    void exec_while     (const AstNode &n, std::shared_ptr<VshEnv> env);
    void exec_for_in    (const AstNode &n, std::shared_ptr<VshEnv> env);
    void exec_try       (const AstNode &n, std::shared_ptr<VshEnv> env);
    void exec_import    (const AstNode &n, std::shared_ptr<VshEnv> env);

    /**
     * @brief Resuelve el path de un @c import "ruta" probando varias
     *        ubicaciones canonicas: literal, relativo al script
     *        importador, relativo al directorio del interprete (vm.exe),
     *        relativo a @c <interprete>/stdlib/, y la variable de entorno
     *        @c VESTAVM_VSH_PATH.  Devuelve el path absoluto del primer
     *        candidato existente, o lanza @c VshRuntimeError listando
     *        todos los paths probados.
     *
     * @param path           Argumento literal del import (e.g. "stdlib/vsh/build_lib.vsh").
     * @param importer_file  Path del fichero .vsh que contiene el import,
     *                       o cadena vacia si es el script raiz.
     */
    std::string resolve_import_path(const std::string &path,
                                     const std::string &importer_file) const;

    Value call_fn(const Value &callee, std::vector<Value> args, int line);

    /**
     * @brief Busca un metodo por nombre en la jerarquia de herencia de una clase.
     *
     * @param klass  Clase desde la que iniciar la busqueda.
     * @param name   Nombre del metodo.
     * @return       Puntero al VshFunction encontrado, o nullptr si no existe.
     */
    static std::shared_ptr<VshFunction> find_method(
        const std::shared_ptr<VshClass> &klass, const std::string &name);

    /**
     * @brief Llama a un metodo con self ya incluido y tipos comprobados.
     *
     * @param method     Funcion a invocar.
     * @param self_val   Valor de la instancia (se prepone a los argumentos).
     * @param extra_args Argumentos adicionales (sin self).
     * @param line       Linea de la llamada (para mensajes de error).
     * @return           Valor devuelto por el metodo.
     */
    Value call_method_impl(const std::shared_ptr<VshFunction> &method,
                           const Value &self_val,
                           std::vector<Value> extra_args,
                           int line);

    /**
     * @brief Comprueba que un valor coincide con la anotacion de tipo dada.
     *
     * Lanza VshRuntimeError si el tipo no coincide. Si hint esta vacio, no hace nada.
     *
     * @param hint   Nombre del tipo esperado (null, bool, int, float, str/string, list, map, fn,
     *               o nombre de clase).
     * @param val    Valor a comprobar.
     * @param ctx    Contexto (nombre de variable o parametro) para el mensaje de error.
     * @param line   Linea del script para el mensaje de error.
     * @param env    Entorno en el que resolver nombres de clase (puede ser nullptr).
     */
    void check_type_hint(const std::string &hint, const Value &val,
                         const std::string &ctx, int line,
                         std::shared_ptr<VshEnv> env);
};

} // namespace vsh

#endif // VSH_H
