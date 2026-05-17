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
 * @file parser.h
 * @brief Parser recursivo descendente para el lenguaje Vex.
 *
 * Estrategia:
 *  - Declaraciones, statements y tipos: descenso recursivo clasico.
 *  - Expresiones: cascada de niveles de precedencia (estilo C),
 *    cada nivel implementado como funcion propia.  Esto evita el
 *    coste de hashing/dispatch de un esquema Pratt generico y produce
 *    un binario mas predecible para el branch predictor.
 *  - Errores: sin excepciones.  Los errores se acumulan en Diagnostics
 *    y el parser intenta sincronizar al siguiente ";" o "}" para
 *    seguir reportando.
 *
 * Reglas de precedencia (de menor a mayor):
 *   1.  asignacion         = += -= *= /= %= &= |= ^= <<= >>=  (derecha-asoc)
 *   2.  or logico          ||
 *   3.  and logico         &&
 *   4.  or bit             |
 *   5.  xor bit            ^
 *   6.  and bit            &
 *   7.  igualdad           == !=
 *   8.  relacional         < <= > >=
 *   9.  shift              << >>
 *   10. aditivo            + -
 *   11. multiplicativo     * / %
 *   12. unario prefijo     ! ~ - + ++ --
 *   13. postfijo           ()  ++  --  (solo en A.1)
 *   14. primaria           literales, identificadores, ( expr )
 */

#ifndef VEX_PARSER_H
#define VEX_PARSER_H

#include <memory>

#include "vex/ast.h"
#include "vex/diagnostic.h"
#include "vex/lexer.h"

namespace vex {

    /**
     * @class Parser
     * @brief Construye un AST a partir de los tokens producidos por @c Lexer.
     *
     * Es de un solo uso: una instancia produce un AST llamando a
     * @c parse_program() y luego se descarta.  Mantiene internamente el
     * token actual y acceso al lexer para producir el siguiente.
     */
    class Parser {
    public:
        /**
         * @brief Construye el parser sobre un lexer y un sumidero de diagnosticos.
         *
         * @param lex   Lexer del que se consumen tokens.  Debe sobrevivir al parser.
         * @param diags Sumidero de errores.  Debe sobrevivir al parser.
         */
        Parser(Lexer &lex, Diagnostics &diags);

        /**
         * @brief Parsea el programa completo desde el cursor actual del lexer.
         *
         * @return Un ModuleNode con todas las declaraciones top-level encontradas.
         *         Aun en caso de errores, devuelve un AST parcial; el caller debe
         *         consultar @c Diagnostics::has_errors() antes de bajar a IR.
         */
        std::unique_ptr<ast::ModuleNode> parse_program();

    private:
        // -----------------------------------------------------------------
        // Helpers de gestion de tokens.
        // -----------------------------------------------------------------

        /**
         * @brief Avanza al siguiente token y devuelve el consumido.
         */
        Token consume();

        /**
         * @brief Devuelve el token actual sin consumirlo.
         */
        const Token &peek() const noexcept { return current_; }

        /**
         * @brief @c true si el token actual es del tipo @p k.
         */
        [[nodiscard]] bool check(TokenKind k) const noexcept { return current_.kind == k; }

        /**
         * @brief Si el token actual es @p k, lo consume y devuelve true.
         */
        bool match(TokenKind k);

        /**
         * @brief Exige un token de tipo @p k.  Si no coincide, emite error y devuelve false.
         *
         * El parser no aborta; intenta seguir parseando para detectar mas errores.
         *
         * @param k   Token esperado.
         * @param msg Mensaje a emitir cuando no coincide.
         * @return    Token consumido si coincidio, o un Token UNKNOWN si no.
         */
        Token expect(TokenKind k, const char *msg);

        /**
         * @brief Cierra un grupo de argumentos de tipo `<...>`.
         *
         * Acepta tanto un solo `>` (token GT) como el primer `>` de un
         * `>>` (token SHR), que se "parte" en dos: consume la mitad
         * izquierda y deja la derecha como nuevo current_ (kind=GT).
         * Esto permite tipos genericos anidados como
         * `VirtualPtr<VirtualPtr<i64>>` sin requerir espacios entre los
         * dos cierres (`> >`).
         */
        Token expect_close_angle(const char *msg);

        /**
         * @brief Avanza tokens hasta hallar un punto de sincronizacion.
         *
         * Sincronizadores: ';', '}', EOF, y el inicio de cualquier
         * keyword de declaracion top-level.  Usado tras un error para
         * limitar la cascada de mensajes.
         */
        void synchronize();

        /**
         * @brief Reporta un error en la posicion del token actual.
         */
        void error_here(const char *msg);

        /**
         * @brief Reporta un error en la posicion de un token dado.
         */
        void error_at(const Token &tok, const char *msg);

        // -----------------------------------------------------------------
        // Reglas gramaticales: top-level y declaraciones.
        // -----------------------------------------------------------------

        std::unique_ptr<ast::Node>          parse_top_level_decl();

        /**
         * @brief parsea @c extern @c "lib.dll" @c { @c fn @c name(params) @c -> @c R; @c ... @c }
         *
         * El bloque produce N decls (uno por funcion); el parser los empuja
         * directamente al modulo via @p mod.  Cada @c ExternFnDecl comparte
         * el campo @c lib del bloque.  Sin parseo de nombre de parametro
         * obligatorio: si no hay identificador tras el tipo, sintetiza
         * @c "__arg<i>" para que el resto del frontend pueda referirlo.
         * El tipo de retorno usa @c PrimitiveKind::VOID si no hay @c "->".
         */
        void                                parse_extern_block(ast::ModuleNode &mod);
        std::unique_ptr<ast::FunctionDecl>  parse_function_decl(std::unique_ptr<ast::TypeNode> ret_type,
                                                                std::string name,
                                                                SourceLoc loc);
        std::unique_ptr<ast::GlobalVarDecl> parse_global_var_decl(std::unique_ptr<ast::TypeNode> type,
                                                                  std::string name,
                                                                  SourceLoc loc,
                                                                  bool is_const);
        std::unique_ptr<ast::ParamDecl>     parse_param();
        std::unique_ptr<ast::TypeAliasDecl> parse_typedef_decl();
        std::unique_ptr<ast::TypeAliasDecl> parse_using_decl();
        std::unique_ptr<ast::StructDecl>    parse_struct_decl();
        /// `typedef struct {...} Name;` o
        /// `typedef enum {...} Name;`.  Devuelve StructDecl o EnumDecl.
        std::unique_ptr<ast::Node>          parse_typedef_struct_or_enum();
        std::unique_ptr<ast::ClassDecl>     parse_class_decl();
        std::unique_ptr<ast::ClassDecl>     parse_interface_decl();

        /**
         * @briefparsea @c enum @c Name { @c Variant, @c Variant(T1, T2), ... }
         *
         * El parser reconoce las dos formas de variante:
         *   - Sin payload: solo el identificador de la variante.
         *   - Con payload: identificador seguido de @c (T1, T2, ...).
         * Las variantes se separan por coma; el bloque puede tener una
         * coma trailing opcional.  No se admite valor por defecto ni
         * herencia (los enums son tipos de datos algebraicos planos).
         */
        std::unique_ptr<ast::EnumDecl>      parse_enum_decl();

        /**
         * @brief parsea @c match @c expr @c { @c case ... }
         *
         * Sintaxis aceptada:
         *   match expr {
         *       case Variant            => stmt;
         *       case Variant(b1, b2)    => { stmts; }
         *       case _                  => default_stmt;   // catchall opcional
         *   }
         * El cuerpo de cada arm puede ser una expression-bodied
         * (`=> expr;`) que se reescribe a `{ return expr; }` o un bloque
         * (`=> { ... }`).  Cada arm termina en @c ;
         */
        std::unique_ptr<ast::Expr>          parse_match_expr();
        std::unique_ptr<ast::BlockStmt>     parse_method_body(bool is_void);

        // -----------------------------------------------------------------
        // Reglas gramaticales: tipos.
        // -----------------------------------------------------------------

        std::unique_ptr<ast::TypeNode> parse_type_node();

        // -----------------------------------------------------------------
        // Reglas gramaticales: statements.
        // -----------------------------------------------------------------

        std::unique_ptr<ast::BlockStmt>   parse_block();
        std::unique_ptr<ast::Stmt>        parse_statement();
        std::unique_ptr<ast::Stmt>        parse_var_decl_stmt(bool is_const);
        std::unique_ptr<ast::Stmt>        parse_if_stmt();
        std::unique_ptr<ast::Stmt>        parse_while_stmt();
        std::unique_ptr<ast::Stmt>        parse_do_while_stmt();
        std::unique_ptr<ast::Stmt>        parse_for_stmt();
        std::unique_ptr<ast::Stmt>        parse_return_stmt();
        std::unique_ptr<ast::Stmt>        parse_try_stmt();
        std::unique_ptr<ast::Stmt>        parse_throw_stmt();
        std::unique_ptr<ast::Stmt>        parse_synchronized_stmt();
        std::unique_ptr<ast::Stmt>        parse_expr_stmt();

        // -----------------------------------------------------------------
        // Reglas gramaticales: expresiones, por nivel de precedencia.
        // -----------------------------------------------------------------

        std::unique_ptr<ast::Expr> parse_expr();
        std::unique_ptr<ast::Expr> parse_assignment();
        std::unique_ptr<ast::Expr> parse_logical_or();
        std::unique_ptr<ast::Expr> parse_logical_and();
        std::unique_ptr<ast::Expr> parse_bitwise_or();
        std::unique_ptr<ast::Expr> parse_bitwise_xor();
        std::unique_ptr<ast::Expr> parse_bitwise_and();
        std::unique_ptr<ast::Expr> parse_equality();
        std::unique_ptr<ast::Expr> parse_relational();
        std::unique_ptr<ast::Expr> parse_shift();
        std::unique_ptr<ast::Expr> parse_additive();
        std::unique_ptr<ast::Expr> parse_multiplicative();
        std::unique_ptr<ast::Expr> parse_unary();
        std::unique_ptr<ast::Expr> parse_postfix();
        std::unique_ptr<ast::Expr> parse_primary();

        /**
         * @brief Parsea una lambda @c (args) => expr/{stmts} a partir del LPAREN.
         *
         * El @c current_ debe estar en LPAREN cuando se llama (precondicion
         * que comprueba @c is_lambda_start()).  Consume desde el LPAREN hasta
         * el final del cuerpo (expression-bodied o block).
         */
        std::unique_ptr<ast::Expr> parse_lambda_expr();

        /**
         * @brief Lookahead que determina si lo que sigue al @c current_ LPAREN
         *        es una lambda y no una expresion parentizada.
         *
         * Cuenta parentesis con balance hasta cerrar el grupo; si el siguiente
         * token tras el RPAREN matching es FAT_ARROW, es una lambda.  Sin
         * consumir tokens (usa @c Lexer::peek_at).  Coste O(N) donde N es la
         * longitud del grupo entre parentesis; despreciable para listas de
         * argumentos tipicas.
         *
         * @return true si la siguiente forma sintactica es una lambda.
         */
        [[nodiscard]] bool is_lambda_start() const noexcept;

        /**
         * @brief Decide si el token actual abre un tipo primitivo (i32, ...).
         */
        [[nodiscard]] bool starts_type() const noexcept;

        /**
         * @brief Decide si el `(` actual inicia un cast C-style `(T) expr`.
         *
         * El parser reconoce el patron `(<type>) <unary>` solo cuando el
         * primer token tras `(` es claramente un type-starter (primitivo,
         * @c VirtualPtr, @c fn, @c nonnull) y la secuencia se cierra con
         * un `)` seguido de un token que pueda iniciar una expresion.
         * Sin esto, el parser bajaria a `(expr)` y fallaria al ver el
         * `)` en mitad del tipo.  No aceptamos `(Foo) x` para typedefs
         * de identificadores; el usuario puede escribir
         * `(VirtualPtr<Foo>) x` o `(Foo*) x` que son inequivocos.
         */
        [[nodiscard]] bool looks_like_cast() const noexcept;

        Lexer       &lex_;
        Diagnostics &diags_;
        Token        current_;
    };

} // namespace vex

#endif // VEX_PARSER_H
