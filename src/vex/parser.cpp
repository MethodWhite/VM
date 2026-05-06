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
 * @file parser.cpp
 * @brief Implementacion del parser recursivo descendente de Vex.
 *
 * Notas de rendimiento:
 *  - Cada nivel de precedencia se inlinea bien porque las funciones son
 *    pequenyas y se llaman desde un unico llamador (la siguiente capa).
 *  - El loop tipico es del estilo:
 *        auto lhs = parse_lower();
 *        while (binop matches) lhs = combine(lhs, parse_lower());
 *    Esto evita recursion profunda en expresiones largas como
 *    a + b + c + d + ...  (aprovecha la asociatividad por la izquierda).
 *  - synchronize() avanza tokens hasta el siguiente sentinel para
 *    evitar avalanchas de errores tras un fallo.
 */

#include "vex/parser.h"

#include <utility>

namespace vex {

    // ---------------------------------------------------------------------
    // Constructor.
    // ---------------------------------------------------------------------

    Parser::Parser(Lexer &lex, Diagnostics &diags)
        : lex_(lex), diags_(diags), current_(lex.next()) {
        // current_ se carga con el primer token al construir.  A partir
        // de aqui consume() avanza siempre.
    }

    // ---------------------------------------------------------------------
    // Helpers basicos.
    // ---------------------------------------------------------------------

    Token Parser::consume() {
        Token t = std::move(current_);
        current_ = lex_.next();
        return t;
    }

    bool Parser::match(TokenKind k) {
        if (current_.kind == k) {
            (void)consume();
            return true;
        }
        return false;
    }

    Token Parser::expect(TokenKind k, const char *msg) {
        if (current_.kind == k) return consume();
        // Reportar y devolver un placeholder (UNKNOWN) sin avanzar:
        // dejamos que el caller decida si quiere sincronizar o seguir.
        error_here(msg);
        Token bad;
        bad.kind = TokenKind::UNKNOWN;
        bad.loc  = current_.loc;
        return bad;
    }

    void Parser::error_here(const char *msg) {
        diags_.error(current_.loc, msg);
    }

    void Parser::error_at(const Token &tok, const char *msg) {
        diags_.error(tok.loc, msg);
    }

    void Parser::synchronize() {
        // Avanza hasta el siguiente punto de "respiracion": fin de
        // statement, cierre de bloque, o el inicio de una declaracion
        // de top-level.  Esto evita reportar 50 errores cuando solo hubo 1.
        while (current_.kind != TokenKind::END_OF_FILE) {
            if (current_.kind == TokenKind::SEMICOLON) {
                (void)consume();
                return;
            }
            switch (current_.kind) {
                case TokenKind::RBRACE:
                case TokenKind::KW_IF:
                case TokenKind::KW_WHILE:
                case TokenKind::KW_DO:
                case TokenKind::KW_FOR:
                case TokenKind::KW_RETURN:
                case TokenKind::KW_BREAK:
                case TokenKind::KW_CONTINUE:
                case TokenKind::KW_CLASS:
                case TokenKind::KW_STRUCT:
                case TokenKind::KW_IMPORT:
                case TokenKind::KW_CONST:
                case TokenKind::KW_FN:
                    return;
                default:
                    (void)consume();
            }
        }
    }

    // ---------------------------------------------------------------------
    // Punto de entrada: parse_program.
    // ---------------------------------------------------------------------

    std::unique_ptr<ast::ModuleNode> Parser::parse_program() {
        auto mod = std::make_unique<ast::ModuleNode>();
        mod->loc.file = lex_.filename();
        mod->loc.line = 1;
        mod->loc.column = 1;

        while (current_.kind != TokenKind::END_OF_FILE) {
            // bloque `extern "lib.dll" { fn ...; fn ...; }` produce
            // N decls (uno por funcion).  Caso especial porque el resto de
            // top-level decls produce 1 nodo y parse_top_level_decl tiene
            // esa firma.
            if (current_.kind == TokenKind::KW_EXTERN) {
                parse_extern_block(*mod);
                continue;
            }
            auto decl = parse_top_level_decl();
            if (decl) {
                mod->decls.push_back(std::move(decl));
            } else {
                // El parser ya reporto el error; intentar seguir.
                synchronize();
            }
        }
        return mod;
    }

    // ---------------------------------------------------------------------
    // extern "lib.dll" { fn name(params) -> ret; ... }
    //
    // Cada `fn` se traduce a un ExternFnDecl independiente con el campo
    // @c lib copiado del bloque.  El type checker los registra como
    // Symbol::Function con @c FunctionSig::extern_lib = lib; el lowering
    // emite @c CALLN @Method("<lib>:<name>") en vez de CALLVM.
    // ---------------------------------------------------------------------
    void Parser::parse_extern_block(ast::ModuleNode &mod) {
        const SourceLoc block_loc = current_.loc;
        if (expect(TokenKind::KW_EXTERN, "se esperaba 'extern'").kind == TokenKind::UNKNOWN) return;

        if (current_.kind != TokenKind::STRING_LIT
         && current_.kind != TokenKind::RAW_STRING_LIT) {
            error_here("se esperaba el nombre de la libreria como string literal "
                       "tras 'extern' (e.g. \"user32.dll\")");
            synchronize();
            return;
        }
        const std::string lib = current_.str_val; // sin comillas, escapes resueltos
        (void)consume();

        if (expect(TokenKind::LBRACE, "se esperaba '{' tras el nombre de libreria").kind == TokenKind::UNKNOWN) return;

        while (current_.kind != TokenKind::RBRACE
            && current_.kind != TokenKind::END_OF_FILE) {
            // Cada fn: `fn <name>(<params>) -> <ret>;`
            const SourceLoc fn_loc = current_.loc;
            if (expect(TokenKind::KW_FN, "se esperaba 'fn' al inicio de declaracion extern").kind == TokenKind::UNKNOWN) {
                synchronize();
                continue;
            }
            if (current_.kind != TokenKind::IDENTIFIER) {
                error_here("se esperaba el nombre de la funcion tras 'fn'");
                synchronize();
                continue;
            }
            const std::string fn_name = current_.lexeme;
            (void)consume();

            if (expect(TokenKind::LPAREN, "se esperaba '(' tras el nombre de la funcion").kind == TokenKind::UNKNOWN) {
                synchronize();
                continue;
            }
            std::vector<std::unique_ptr<ast::ParamDecl>> params;
            if (current_.kind != TokenKind::RPAREN) {
                while (true) {
                    // Param: `<type> [<name>]` (nombre opcional para parecerse
                    // a declaraciones C de cabecera).
                    auto p_type = parse_type_node();
                    if (!p_type) { synchronize(); break; }
                    auto pd = std::make_unique<ast::ParamDecl>();
                    pd->loc = current_.loc;
                    pd->type = std::move(p_type);
                    if (current_.kind == TokenKind::IDENTIFIER) {
                        pd->name = current_.lexeme;
                        (void)consume();
                    } else {
                        // Sin nombre explicito: sintetizamos uno para que
                        // el resto del frontend (type checker, lowering)
                        // pueda referirlo.  Los externs no se ejecutan en
                        // Vex (solo son marcas), asi que el nombre interno
                        // no aparece nunca.
                        pd->name = "__arg" + std::to_string(params.size());
                    }
                    params.push_back(std::move(pd));
                    if (!match(TokenKind::COMMA)) break;
                }
            }
            if (expect(TokenKind::RPAREN, "se esperaba ')' tras los parametros").kind == TokenKind::UNKNOWN) {
                synchronize();
                continue;
            }
            // Tipo de retorno: `-> <type>` o ausencia (= void).
            std::unique_ptr<ast::TypeNode> ret_type;
            if (match(TokenKind::ARROW)) {
                ret_type = parse_type_node();
                if (!ret_type) { synchronize(); continue; }
            } else {
                // Sin '->': retorno void.
                auto pn = std::make_unique<ast::PrimitiveTypeNode>();
                pn->loc  = fn_loc;
                pn->prim = PrimitiveKind::VOID;
                ret_type = std::move(pn);
            }
            if (expect(TokenKind::SEMICOLON, "se esperaba ';' tras la firma extern").kind == TokenKind::UNKNOWN) {
                synchronize();
                continue;
            }
            auto efd = std::make_unique<ast::ExternFnDecl>();
            efd->loc         = fn_loc;
            efd->lib         = lib;
            efd->return_type = std::move(ret_type);
            efd->name        = fn_name;
            efd->params      = std::move(params);
            mod.decls.push_back(std::move(efd));
        }
        if (expect(TokenKind::RBRACE, "se esperaba '}' al final del bloque extern").kind == TokenKind::UNKNOWN) {
            (void)block_loc;
            return;
        }
    }

    // ---------------------------------------------------------------------
    // Top-level: distinguir entre funcion y variable global.
    //
    // Forma:
    //   [const]? <type> <ident>  '(' params ')' '{' ... '}'    -> FunctionDecl
    //   [const]? <type> <ident>  ('=' expr)? ';'               -> GlobalVarDecl
    // ---------------------------------------------------------------------

    std::unique_ptr<ast::Node> Parser::parse_top_level_decl() {
        // typedef <tipo> <nombre> ;
        if (current_.kind == TokenKind::KW_TYPEDEF) {
            // typedef struct/enum C-style.
            // Si tras typedef viene `struct` o `enum`, parseamos como
            // StructDecl/EnumDecl con name al final.  Sin esto solo se
            // soportaba `typedef i32 Foo;` (alias de tipo basico).
            const TokenKind nk = lex_.peek_at(0).kind;
            if (nk == TokenKind::KW_STRUCT || nk == TokenKind::KW_ENUM) {
                return parse_typedef_struct_or_enum();
            }
            return parse_typedef_decl();
        }
        // using <nombre> = <tipo> ;
        if (current_.kind == TokenKind::KW_USING) {
            return parse_using_decl();
        }
        // Anotaciones top-level que preceden a una clase o funcion:
        //   @Aspect: clase de aspectos
        //   @Async:  funcion async, transformada a wrapper future + spawn 
        //   Otras se aceptan y se ignoran silenciosamente.
        bool top_is_aspect = false;
        bool top_is_async  = false;
        while (current_.kind == TokenKind::AT) {
            (void)consume();
            if (current_.kind == TokenKind::IDENTIFIER) {
                if (current_.lexeme == "Aspect") top_is_aspect = true;
                else if (current_.lexeme == "Async") top_is_async = true;
                (void)consume();
                if (current_.kind == TokenKind::LPAREN) {
                    int depth = 0;
                    do {
                        if (current_.kind == TokenKind::LPAREN) ++depth;
                        else if (current_.kind == TokenKind::RPAREN) --depth;
                        (void)consume();
                    } while (depth > 0
                          && current_.kind != TokenKind::END_OF_FILE);
                }
            } else {
                error_here("se esperaba el nombre de la anotacion tras '@'");
                break;
            }
        }
        // struct <nombre> { ... }
        if (current_.kind == TokenKind::KW_STRUCT) {
            return parse_struct_decl();
        }
        // class <nombre> { ... }
        if (current_.kind == TokenKind::KW_CLASS) {
            auto cd = parse_class_decl();
            if (cd && top_is_aspect) cd->is_aspect = true;
            return cd;
        }
        // interface <nombre> { metodos abstractos }
        // Reusa parse_class_decl marcando is_interface=true; el parser de
        // metodos acepta `;` en lugar de body para metodos abstractos.
        if (current_.kind == TokenKind::KW_INTERFACE) {
            auto cd = parse_interface_decl();
            return cd;
        }
        // ADTs: enum <nombre> { Variante1, Variante2(T1, T2), ... }
        if (current_.kind == TokenKind::KW_ENUM) {
            return parse_enum_decl();
        }

        // Manejar 'const' opcional al principio.
        bool is_const = false;
        if (match(TokenKind::KW_CONST)) is_const = true;

        if (!starts_type()) {
            error_here("se esperaba un tipo al inicio de la declaracion top-level");
            return nullptr;
        }

        const SourceLoc loc = current_.loc;
        auto type_node = parse_type_node();
        if (!type_node) return nullptr;

        if (current_.kind != TokenKind::IDENTIFIER) {
            error_here("se esperaba un nombre tras el tipo");
            return nullptr;
        }
        std::string name = consume().lexeme;

        if (current_.kind == TokenKind::LPAREN) {
            // Es una funcion.  Las funciones no admiten 'const' delante;
            // si lo hubo, reportar warning pero seguir.
            if (is_const) {
                diags_.warning(loc, "'const' ignorado en declaracion de funcion");
            }
            auto fd = parse_function_decl(std::move(type_node), std::move(name), loc);
            // propagar @Async leida en el bucle de annotations.
            // El lowering trata estas funciones distinto: las envuelve en
            // future_alloc + spawn { msgrecv handle + body + fulfill } y
            // devuelve el handle del future al caller.
            if (fd && top_is_async) fd->is_async = true;
            return fd;
        }
        // Es una variable global.
        return parse_global_var_decl(std::move(type_node), std::move(name), loc, is_const);
    }

    // ---------------------------------------------------------------------
    // FunctionDecl: '(' params? ')' block
    // ---------------------------------------------------------------------

    std::unique_ptr<ast::FunctionDecl>
    Parser::parse_function_decl(std::unique_ptr<ast::TypeNode> ret_type,
                                std::string name,
                                SourceLoc loc) {
        auto fn = std::make_unique<ast::FunctionDecl>();
        fn->loc         = loc;
        fn->return_type = std::move(ret_type);
        fn->name        = std::move(name);

        (void)expect(TokenKind::LPAREN, "se esperaba '(' tras el nombre de la funcion");

        // Lista de parametros vacia o coma-separada.
        if (current_.kind != TokenKind::RPAREN) {
            while (true) {
                auto p = parse_param();
                if (p) fn->params.push_back(std::move(p));
                if (!match(TokenKind::COMMA)) break;
            }
        }
        (void)expect(TokenKind::RPAREN, "se esperaba ')' al cerrar la lista de parametros");

        // Cuerpo: bloque obligatorio en A.1 (no hay extern aun).
        if (current_.kind != TokenKind::LBRACE) {
            error_here("se esperaba '{' para abrir el cuerpo de la funcion");
            return fn;
        }
        fn->body = parse_block();
        return fn;
    }

    std::unique_ptr<ast::ParamDecl> Parser::parse_param() {
        if (!starts_type()) {
            error_here("se esperaba un tipo en parametro");
            return nullptr;
        }
        auto p = std::make_unique<ast::ParamDecl>();
        p->loc = current_.loc;
        p->type = parse_type_node();
        // `T !!name` en posicion de parametro fuerza no-null en
        // entry: el lowering inyecta un `unwrap r_param, r_param` al
        // inicio del cuerpo para que la funcion falle pronto si se le
        // pasa null.  El type checker puede confiar en que el param
        // es no-null en el body.
        if (current_.kind == TokenKind::BANG_BANG) {
            (void)consume();
            if (p->type) p->type->is_nonnull = true;
        }
        if (current_.kind != TokenKind::IDENTIFIER) {
            error_here("se esperaba un nombre tras el tipo del parametro");
            return p;
        }
        p->name = consume().lexeme;
        return p;
    }

    // ---------------------------------------------------------------------
    // GlobalVarDecl: ('=' expr)? ';'
    // ---------------------------------------------------------------------

    std::unique_ptr<ast::GlobalVarDecl>
    Parser::parse_global_var_decl(std::unique_ptr<ast::TypeNode> type,
                                  std::string name,
                                  SourceLoc loc,
                                  bool is_const) {
        auto gv = std::make_unique<ast::GlobalVarDecl>();
        gv->loc      = loc;
        gv->type     = std::move(type);
        gv->name     = std::move(name);
        gv->is_const = is_const;
        if (match(TokenKind::ASSIGN)) {
            gv->init = parse_expr();
        }
        (void)expect(TokenKind::SEMICOLON, "se esperaba ';' al final de la declaracion");
        return gv;
    }

    // ---------------------------------------------------------------------
    // Tipos.
    //
    // En A.1 solo primitivos.  Punteros, arrays, generics se anyaden en
    // hitos posteriores (esto se quedara como caso por defecto del switch).
    // ---------------------------------------------------------------------

    bool Parser::starts_type() const noexcept {
        // Cualquier keyword que sea tipo primitivo, o un identificador
        // seguido de uno o mas '*' (cero permitidos) y luego otro
        // identificador (caso "Edad x = ..." o "Punto* p = ...").  Si el
        // siguiente token tras la cadena de '*' no es IDENTIFIER (e.g.
        // 'x = 2' o 'a * b + c'), tratamos esto como expresion.
        //
        // Generics: tambien aceptamos `Cls<T> name` y
        // `Cls<T1, T2> name`.  Lookahead saltando un par balanceado de
        // angle brackets, luego `*`* y un IDENT.
        if (primitive_kind_from_token(current_.kind) != PrimitiveKind::COUNT)
            return true;
        // nonnull: `nonnull T name = ...` empieza un type-decl.
        if (current_.kind == TokenKind::KW_NONNULL) return true;
        // closures: `fn(T...) -> R name = ...` empieza un type-decl.
        // Solo reconocemos `fn` seguido de `(` para no chocar con un
        // hipotetico identificador que empezara por "fn"; KW_FN siempre
        // es el keyword reservado.
        if (current_.kind == TokenKind::KW_FN) return true;
        if (current_.kind != TokenKind::IDENTIFIER) return false;

        Lexer &mut_lex = const_cast<Lexer &>(lex_);
        size_t off = 0;
        // Optional: skip generic angle-brackets `<...>` con balance.
        if (mut_lex.peek_at(off).kind == TokenKind::LT) {
            int depth = 1;
            ++off;
            const size_t MAX_LOOKAHEAD = 64;  // cota dura
            while (depth > 0 && off < MAX_LOOKAHEAD) {
                TokenKind k = mut_lex.peek_at(off).kind;
                if (k == TokenKind::END_OF_FILE) return false;
                if (k == TokenKind::LT) ++depth;
                else if (k == TokenKind::GT) --depth;
                ++off;
            }
            if (depth != 0) return false;
        }
        // Saltar `*`s.
        while (mut_lex.peek_at(off).kind == TokenKind::STAR) ++off;
        // aceptar `T !!name` (BANG_BANG entre tipo y nombre).
        if (mut_lex.peek_at(off).kind == TokenKind::BANG_BANG) ++off;
        return mut_lex.peek_at(off).kind == TokenKind::IDENTIFIER;
    }

    std::unique_ptr<ast::TypeNode> Parser::parse_type_node() {
        // nonnull: prefijo opcional `nonnull T` que marca el tipo
        // como no-null.  Solo afecta semantica del type checker
        // (rechazo de null literal); el lowering trata el tipo igual
        // que una referencia normal.
        bool nonnull = false;
        if (current_.kind == TokenKind::KW_NONNULL) {
            nonnull = true;
            (void)consume();
        }
        std::unique_ptr<ast::TypeNode> base;
        // closures: `fn(T1, T2, ...) -> R` produce un FunctionTypeNode.
        // Si el usuario omite la flecha @c -> el return_type queda como
        // PrimitiveTypeNode(VOID) para mantener un tipo siempre presente
        // (simplifica el type checker, que no tiene que manejar null).
        if (current_.kind == TokenKind::KW_FN) {
            const SourceLoc loc = current_.loc;
            (void)consume(); // 'fn'
            (void)expect(TokenKind::LPAREN, "se esperaba '(' tras 'fn' en tipo de funcion");
            auto fn = std::make_unique<ast::FunctionTypeNode>();
            fn->loc = loc;
            // Parametros: lista de tipos separados por coma.  Vacio para
            // `fn() -> R`.  No se admiten nombres aqui (un type-node solo
            // describe la firma, no introduce parametros con nombre).
            while (current_.kind != TokenKind::RPAREN
                && current_.kind != TokenKind::END_OF_FILE) {
                auto pt = parse_type_node();
                if (!pt) break;
                fn->param_types.push_back(std::move(pt));
                if (!match(TokenKind::COMMA)) break;
            }
            (void)expect(TokenKind::RPAREN, "se esperaba ')' al cerrar los parametros del tipo funcion");
            // Flecha de retorno opcional.  Si no esta, asumimos void.
            if (current_.kind == TokenKind::ARROW) {
                (void)consume(); // '->'
                fn->return_type = parse_type_node();
                if (!fn->return_type) {
                    error_here("se esperaba un tipo tras '->' en tipo funcion");
                    return nullptr;
                }
            } else {
                // Insertar VOID por defecto para no propagar nulls al type checker.
                auto v = std::make_unique<ast::PrimitiveTypeNode>();
                v->loc  = loc;
                v->prim = PrimitiveKind::VOID;
                fn->return_type = std::move(v);
            }
            base = std::move(fn);
        }
        // Caso 1: tipo primitivo via keyword (i32, u8, f64, ...).
        else if (const PrimitiveKind k = primitive_kind_from_token(current_.kind);
                 k != PrimitiveKind::COUNT) {
            auto pt = std::make_unique<ast::PrimitiveTypeNode>();
            pt->loc  = current_.loc;
            pt->prim = k;
            (void)consume();
            // generics para colecciones primitivas.  Aceptamos
            // type args opcionales tras el keyword de tipo coleccion:
            //   ArrayList<T>     prim=ARRAYLIST, type_args=[T]
            //   HashMap<K,V>     prim=HASHMAP,   type_args=[K,V]
            //   etc.
            // El type checker propaga estos type args al `Type` semantico
            // como `pointee` (1er arg) y `pointee2` (2do arg) -- mismo
            // mecanismo que Optional<T> y Result<V,E>.
            if (current_.kind == TokenKind::LT) {
                // Solo aceptamos type args para tipos de coleccion.
                const bool is_col = (k == PrimitiveKind::ARRAYLIST
                                  || k == PrimitiveKind::HASHMAP
                                  || k == PrimitiveKind::HASHSET
                                  || k == PrimitiveKind::QUEUE
                                  || k == PrimitiveKind::DEQUE
                                  || k == PrimitiveKind::TREEMAP
                                  || k == PrimitiveKind::TREESET
                                  || k == PrimitiveKind::STACK);
                if (is_col) {
                    (void)consume(); // '<'
                    while (current_.kind != TokenKind::GT
                        && current_.kind != TokenKind::END_OF_FILE) {
                        auto ta = parse_type_node();
                        if (!ta) break;
                        pt->type_args.push_back(std::move(ta));
                        if (!match(TokenKind::COMMA)) break;
                    }
                    (void)expect(TokenKind::GT,
                        "se esperaba '>' al cerrar argumentos de tipo");
                }
                // Si no es coleccion, dejamos el '<' para que el caller
                // lo trate como BinaryOp comparison (ej. en `i32 a = ...; a < b`).
            }
            base = std::move(pt);
        } else if (current_.kind == TokenKind::IDENTIFIER
                && current_.lexeme == "VirtualPtr") {
            // Caso especial: VirtualPtr<T> es una direccion VM al contenido T.
            // Lo desazucaramos a PointerTypeNode con `is_virtual=true`.  Sin
            // anyadir keyword nuevo: el lexer trata VirtualPtr como un
            // identificador comun, y parse_type lo intercepta aqui antes de
            // la rama generica de NamedTypeNode.
            const SourceLoc vloc = current_.loc;
            (void)consume(); // VirtualPtr
            (void)expect(TokenKind::LT, "se esperaba '<' tras VirtualPtr");
            auto inner = parse_type_node();
            if (!inner) {
                error_here("se esperaba un tipo dentro de VirtualPtr<...>");
                return nullptr;
            }
            (void)expect(TokenKind::GT, "se esperaba '>' al cerrar VirtualPtr<...>");
            auto pn = std::make_unique<ast::PointerTypeNode>();
            pn->loc        = vloc;
            pn->pointee    = std::move(inner);
            pn->is_virtual = true;
            base = std::move(pn);
        } else if (current_.kind == TokenKind::IDENTIFIER) {
            // Caso 2: tipo nombrado via identificador (alias de typedef/using
            // o struct).  La resolucion al tipo subyacente la hace el type
            // checker; aqui solo guardamos el nombre tal cual.
            auto nt = std::make_unique<ast::NamedTypeNode>();
            nt->loc  = current_.loc;
            nt->name = consume().lexeme;
            // si despues del nombre viene `<`, parseamos
            // type args.  En contexto de tipo no hay ambiguedad: `<` solo
            // puede iniciar argumentos de tipo aqui.  Aceptamos uno o mas
            // tipos separados por coma, terminados en `>`.
            if (current_.kind == TokenKind::LT) {
                (void)consume(); // '<'
                while (current_.kind != TokenKind::GT
                    && current_.kind != TokenKind::END_OF_FILE) {
                    auto ta = parse_type_node();
                    if (!ta) break;
                    nt->type_args.push_back(std::move(ta));
                    if (!match(TokenKind::COMMA)) break;
                }
                (void)expect(TokenKind::GT,
                             "se esperaba '>' al cerrar argumentos de tipo");
            }
            base = std::move(nt);
        } else {
            error_here("se esperaba un tipo");
            return nullptr;
        }
        // Postfix: cada '*' apila un PointerTypeNode adicional.
        // Ejemplo: 'i32**' -> Pointer(Pointer(Primitive(i32))).
        while (current_.kind == TokenKind::STAR) {
            const SourceLoc loc = current_.loc;
            (void)consume(); // '*'
            auto pn = std::make_unique<ast::PointerTypeNode>();
            pn->loc     = loc;
            pn->pointee = std::move(base);
            base = std::move(pn);
        }
        // Postfix '[N]' o '[]': arrays nativos.  Aceptamos solo literales
        // enteros (positivos) como tamano fijo; expresiones constantes mas
        // generales llegaran cuando exista un evaluador de constantes.
        // T[] (sin numero) representa un array sin tamano (decay-to-ptr,
        // tipico de parametros de funcion).  Permitimos encadenar para
        // formar i32[3][4] (matriz de 4 filas de 3 columnas... C-style).
        while (current_.kind == TokenKind::LBRACKET) {
            const SourceLoc loc = current_.loc;
            (void)consume(); // '['
            auto an = std::make_unique<ast::ArrayTypeNode>();
            an->loc          = loc;
            an->element_type = std::move(base);
            if (current_.kind != TokenKind::RBRACKET) {
                an->size_expr = parse_expr();
            }
            (void)expect(TokenKind::RBRACKET, "se esperaba ']' al cerrar el tamano del array");
            base = std::move(an);
        }
        if (base) base->is_nonnull = nonnull;
        return base;
    }

    // -----------------------------------------------------------------
    // typedef y using: alias de tipos.
    //
    // typedef:  typedef <tipo> <nombre> ;     (estilo C clasico)
    // using:    using   <nombre> = <tipo> ;   (estilo C++ moderno)
    //
    // Ambos producen el mismo AST node TypeAliasDecl.  La forma se
    // preserva en is_using_form solo para diagnosticos.
    // -----------------------------------------------------------------

    std::unique_ptr<ast::Node> Parser::parse_typedef_struct_or_enum() {
        // Sintaxis C: `typedef struct { ... } Name;` y
        // `typedef enum { ... } Name;`.  Tag opcional tras struct/enum:
        // `typedef struct Tag { ... } Name;` (Tag se ignora; usamos Name).
        const SourceLoc loc_td = current_.loc;
        (void)consume();   // 'typedef'

        const bool is_struct = (current_.kind == TokenKind::KW_STRUCT);
        (void)consume();   // 'struct' o 'enum'

        // Tag opcional (ignorado; el name real va al final).
        if (current_.kind == TokenKind::IDENTIFIER
         && lex_.peek_at(0).kind == TokenKind::LBRACE) {
            (void)consume();   // skip tag
        }

        if (current_.kind != TokenKind::LBRACE) {
            error_here("se esperaba '{' tras typedef struct/enum");
            return nullptr;
        }
        (void)consume();   // '{'

        if (is_struct) {
            auto s = std::make_unique<ast::StructDecl>();
            s->loc  = loc_td;
            // Reusar logica de parse_struct_decl (cuerpo del struct).
            while (current_.kind != TokenKind::RBRACE
                && current_.kind != TokenKind::END_OF_FILE) {
                if (!starts_type()) {
                    error_here("se esperaba un tipo de campo dentro del struct");
                    synchronize();
                    continue;
                }
                ast::StructFieldDecl f;
                f.loc  = current_.loc;
                f.type = parse_type_node();
                if (!f.type) { synchronize(); continue; }
                if (current_.kind != TokenKind::IDENTIFIER) {
                    error_here("se esperaba un nombre de campo tras el tipo");
                    synchronize();
                    continue;
                }
                f.name = consume().lexeme;
                if (current_.kind == TokenKind::COLON) {
                    (void)consume();
                    if (current_.kind == TokenKind::INT_LIT) {
                        f.bit_width = (uint8_t)current_.int_val;
                        (void)consume();
                    }
                }
                (void)expect(TokenKind::SEMICOLON, "se esperaba ';' al final del campo");
                s->fields.push_back(std::move(f));
            }
            (void)expect(TokenKind::RBRACE, "se esperaba '}' al cerrar el struct");
            if (current_.kind != TokenKind::IDENTIFIER) {
                error_here("se esperaba el nombre del typedef tras '}'");
                return nullptr;
            }
            s->name = consume().lexeme;
            (void)expect(TokenKind::SEMICOLON, "se esperaba ';' al final del typedef");
            return s;
        }
        // typedef enum { ... } Name;
        auto e = std::make_unique<ast::EnumDecl>();
        e->loc = loc_td;
        while (current_.kind != TokenKind::RBRACE
            && current_.kind != TokenKind::END_OF_FILE) {
            ast::EnumVariantDecl v;
            v.loc = current_.loc;
            if (current_.kind != TokenKind::IDENTIFIER) {
                error_here("se esperaba el nombre de una variante de enum");
                synchronize();
                continue;
            }
            v.name = consume().lexeme;
            if (current_.kind == TokenKind::LPAREN) {
                (void)consume();
                while (current_.kind != TokenKind::RPAREN
                    && current_.kind != TokenKind::END_OF_FILE) {
                    auto pt = parse_type_node();
                    if (!pt) break;
                    v.field_types.push_back(std::move(pt));
                    if (!match(TokenKind::COMMA)) break;
                }
                (void)expect(TokenKind::RPAREN, "se esperaba ')' al cerrar payload");
            }
            e->variants.push_back(std::move(v));
            if (!match(TokenKind::COMMA)) break;
        }
        (void)expect(TokenKind::RBRACE, "se esperaba '}' al cerrar el enum");
        if (current_.kind != TokenKind::IDENTIFIER) {
            error_here("se esperaba el nombre del typedef tras '}'");
            return nullptr;
        }
        e->name = consume().lexeme;
        (void)expect(TokenKind::SEMICOLON, "se esperaba ';' al final del typedef");
        return e;
    }

    std::unique_ptr<ast::TypeAliasDecl> Parser::parse_typedef_decl() {
        auto a = std::make_unique<ast::TypeAliasDecl>();
        a->loc            = current_.loc;
        a->is_using_form  = false;
        (void)consume(); // 'typedef'

        if (!starts_type()) {
            error_here("se esperaba un tipo tras 'typedef'");
            return nullptr;
        }
        a->aliased = parse_type_node();
        if (!a->aliased) return nullptr;

        if (current_.kind != TokenKind::IDENTIFIER) {
            error_here("se esperaba un nombre tras el tipo en 'typedef'");
            return nullptr;
        }
        a->name = consume().lexeme;
        (void)expect(TokenKind::SEMICOLON, "se esperaba ';' al final de 'typedef'");
        return a;
    }

    std::unique_ptr<ast::TypeAliasDecl> Parser::parse_using_decl() {
        auto a = std::make_unique<ast::TypeAliasDecl>();
        a->loc            = current_.loc;
        a->is_using_form  = true;
        (void)consume(); // 'using'

        if (current_.kind != TokenKind::IDENTIFIER) {
            error_here("se esperaba un nombre tras 'using'");
            return nullptr;
        }
        a->name = consume().lexeme;
        (void)expect(TokenKind::ASSIGN, "se esperaba '=' tras el nombre en 'using'");
        if (!starts_type()) {
            error_here("se esperaba un tipo tras '=' en 'using'");
            return nullptr;
        }
        a->aliased = parse_type_node();
        if (!a->aliased) return nullptr;

        (void)expect(TokenKind::SEMICOLON, "se esperaba ';' al final de 'using'");
        return a;
    }

    // -----------------------------------------------------------------
    // struct: declaracion de tipo agregado (value type, sin metodos).
    //
    // Forma:  struct <nombre> { <tipo> <campo>; <tipo> <campo>; ... }
    // -----------------------------------------------------------------

    // -----------------------------------------------------------------
    // parse_enum_decl + parse_match_expr.
    //
    // Forma del enum:
    //   enum Name {
    //       Variant1,
    //       Variant2(T),
    //       Variant3(T1, T2),
    //       ...                   (coma trailing opcional)
    //   }
    //
    // No se admiten valor por defecto ni herencia: los enums Vex son
    // tipos de datos algebraicos planos.  Los tags se asignan
    // implicitamente como el indice 0..N-1 de la variante en el bloque.
    // -----------------------------------------------------------------

    std::unique_ptr<ast::EnumDecl> Parser::parse_enum_decl() {
        auto e = std::make_unique<ast::EnumDecl>();
        e->loc = current_.loc;
        (void)consume(); // 'enum'

        if (current_.kind != TokenKind::IDENTIFIER) {
            error_here("se esperaba un nombre tras 'enum'");
            return nullptr;
        }
        e->name = consume().lexeme;
        (void)expect(TokenKind::LBRACE, "se esperaba '{' al abrir el cuerpo del enum");

        while (current_.kind != TokenKind::RBRACE
            && current_.kind != TokenKind::END_OF_FILE) {
            ast::EnumVariantDecl v;
            v.loc = current_.loc;
            if (current_.kind != TokenKind::IDENTIFIER) {
                error_here("se esperaba el nombre de una variante de enum");
                synchronize();
                continue;
            }
            v.name = consume().lexeme;
            // Payload opcional: lista de tipos entre parentesis.
            if (current_.kind == TokenKind::LPAREN) {
                (void)consume(); // '('
                while (current_.kind != TokenKind::RPAREN
                    && current_.kind != TokenKind::END_OF_FILE) {
                    auto pt = parse_type_node();
                    if (!pt) break;
                    v.field_types.push_back(std::move(pt));
                    if (!match(TokenKind::COMMA)) break;
                }
                (void)expect(TokenKind::RPAREN, "se esperaba ')' al cerrar payload de variante");
            }
            e->variants.push_back(std::move(v));
            // Coma separadora (con coma trailing opcional gracias al check
            // de RBRACE en la condicion del while).
            if (!match(TokenKind::COMMA)) break;
        }
        (void)expect(TokenKind::RBRACE, "se esperaba '}' al cerrar el enum");
        return e;
    }

    // ------------------------------------------------------------------
    // parse_match_expr: punto de entrada cuando el parser ve KW_MATCH.
    //
    //   match scrutinee {
    //       case Variant            => stmt
    //       case Variant(a, b)      => { stmts; }
    //       case _                  => default_stmt
    //   }
    //
    // Cada arm termina con `;` (o con `}` final para la ultima arm de
    // bloque); las arms se consumen hasta encontrar `}` del match.
    // ------------------------------------------------------------------
    std::unique_ptr<ast::Expr> Parser::parse_match_expr() {
        const SourceLoc loc = current_.loc;
        (void)consume(); // 'match'
        auto m = std::make_unique<ast::MatchExpr>();
        m->loc = loc;

        // Scrutinee: aceptamos cualquier expresion.  No requerimos
        // parentesis (estilo Rust).  Esto permite tanto:
        //   match x { ... }
        //   match (x + 1) { ... }
        m->scrutinee = parse_expr();
        if (!m->scrutinee) return nullptr;

        (void)expect(TokenKind::LBRACE, "se esperaba '{' tras el scrutinee del match");
        while (current_.kind != TokenKind::RBRACE
            && current_.kind != TokenKind::END_OF_FILE) {
            if (current_.kind != TokenKind::KW_CASE) {
                error_here("se esperaba 'case' al iniciar una arm del match");
                synchronize();
                continue;
            }
            (void)consume(); // 'case'
            ast::MatchArm arm;
            arm.loc = current_.loc;
            // Patron del arm.  Aceptamos:
            //   - Identificador "_" (catchall).
            //   - Identificador (variant simple, p.ej. "Red").
            //   - Identificador "(" bind1, bind2, ... ")" (variant con bindings).
            // Para variantes calificadas tipo `Color.Red` aceptamos tambien
            // el patron `Color.Red(...)`, pero internamente almacenamos solo
            // la parte tras el ultimo `.` ya que el match scrutinee fija el
            // tipo enum y el variant_name es suficiente.
            if (current_.kind != TokenKind::IDENTIFIER) {
                error_here("se esperaba un nombre de variante o '_' tras 'case'");
                synchronize();
                continue;
            }
            std::string ident = consume().lexeme;
            // Forma calificada `Color.Red`: descartamos el qualifier.
            while (current_.kind == TokenKind::DOT) {
                (void)consume();
                if (current_.kind != TokenKind::IDENTIFIER) {
                    error_here("se esperaba un identificador tras '.' en patron de match");
                    break;
                }
                ident = consume().lexeme;
            }
            arm.variant_name = ident;
            // Payload bindings opcionales.
            if (current_.kind == TokenKind::LPAREN) {
                (void)consume(); // '('
                while (current_.kind != TokenKind::RPAREN
                    && current_.kind != TokenKind::END_OF_FILE) {
                    if (current_.kind != TokenKind::IDENTIFIER) {
                        error_here("se esperaba un nombre de binding o '_' en patron");
                        break;
                    }
                    arm.bindings.push_back(consume().lexeme);
                    if (!match(TokenKind::COMMA)) break;
                }
                (void)expect(TokenKind::RPAREN, "se esperaba ')' al cerrar bindings del patron");
            }
            (void)expect(TokenKind::FAT_ARROW, "se esperaba '=>' tras el patron del case");
            // Body del arm: aceptamos cualquier statement (bloque,
            // return, throw, asignacion, expression, ...).  Esto
            // permite `case Red => return 10;` igual que `case Red => { ... }`.
            // parse_statement consume el `;` cuando aplica (returns,
            // expression-stmts), por lo que NO comemos un `;` extra aqui.
            // Para uniformidad, todas las arms se almacenan envueltas en
            // un BlockStmt: simplifica el lowering ya que siempre tiene
            // un solo punto de entrada.
            if (current_.kind == TokenKind::LBRACE) {
                arm.body = parse_block();
            } else {
                auto stmt = parse_statement();
                if (!stmt) { synchronize(); continue; }
                auto blk = std::make_unique<ast::BlockStmt>();
                blk->loc = arm.loc;
                blk->body.push_back(std::move(stmt));
                arm.body = std::move(blk);
            }
            m->arms.push_back(std::move(arm));
        }
        (void)expect(TokenKind::RBRACE, "se esperaba '}' al cerrar el match");
        return m;
    }

    std::unique_ptr<ast::StructDecl> Parser::parse_struct_decl() {
        auto s = std::make_unique<ast::StructDecl>();
        s->loc = current_.loc;
        (void)consume(); // 'struct'

        if (current_.kind != TokenKind::IDENTIFIER) {
            error_here("se esperaba un nombre tras 'struct'");
            return nullptr;
        }
        s->name = consume().lexeme;
        (void)expect(TokenKind::LBRACE, "se esperaba '{' al abrir el cuerpo del struct");

        while (current_.kind != TokenKind::RBRACE
            && current_.kind != TokenKind::END_OF_FILE) {
            if (!starts_type()) {
                error_here("se esperaba un tipo de campo dentro del struct");
                synchronize();
                continue;
            }
            ast::StructFieldDecl f;
            f.loc  = current_.loc;
            f.type = parse_type_node();
            if (!f.type) { synchronize(); continue; }
            if (current_.kind != TokenKind::IDENTIFIER) {
                error_here("se esperaba un nombre de campo tras el tipo");
                synchronize();
                continue;
            }
            f.name = consume().lexeme;
            // Bit field width: `i32 flag : 3;`.  El bit_width
            // se guarda en el AST y el type checker calcula el packing.
            if (current_.kind == TokenKind::COLON) {
                (void)consume();
                if (current_.kind != TokenKind::INT_LIT) {
                    error_here("se esperaba un literal entero tras ':' (bit width)");
                } else {
                    const int64_t w = (int64_t)current_.int_val;
                    if (w <= 0 || w > 64) {
                        error_here("bit width debe estar en rango 1..64");
                    } else {
                        f.bit_width = (uint8_t)w;
                    }
                    (void)consume();
                }
            }
            (void)expect(TokenKind::SEMICOLON, "se esperaba ';' al final del campo");
            s->fields.push_back(std::move(f));
        }
        (void)expect(TokenKind::RBRACE, "se esperaba '}' al cerrar el struct");
        return s;
    }

    // -----------------------------------------------------------------
    // class: declaracion de tipo POO (reference type con vtable).
    //
    // Forma minima
    //   class <nombre> [':' <super>]? '{'
    //       <tipo> <campo> ';'
    //       ...
    //       <tipo> <metodo>(<params>) '{' <body> '}'   // metodo
    //       <nombre>(<params>) '{' <body> '}'           // constructor
    //   '}'
    //
    // -----------------------------------------------------------------

    std::unique_ptr<ast::ClassDecl> Parser::parse_class_decl() {
        auto c = std::make_unique<ast::ClassDecl>();
        c->loc = current_.loc;
        (void)consume(); // 'class'

        if (current_.kind != TokenKind::IDENTIFIER) {
            error_here("se esperaba un nombre tras 'class'");
            return nullptr;
        }
        c->name = consume().lexeme;

        // Generics A.8: parametros de tipo opcionales `<T>`, `<K, V>`.
        // Cada parametro es un identificador simple; la clase se trata
        // como plantilla y no se procesa como clase concreta hasta que
        // se instancie via `Box<i32>`.
        if (current_.kind == TokenKind::LT) {
            (void)consume(); // '<'
            while (current_.kind == TokenKind::IDENTIFIER) {
                c->type_params.push_back(consume().lexeme);
                if (!match(TokenKind::COMMA)) break;
            }
            (void)expect(TokenKind::GT,
                         "se esperaba '>' al cerrar parametros de tipo");
        }

        // Superclase opcional via ':'.
        if (current_.kind == TokenKind::COLON) {
            (void)consume();
            if (current_.kind != TokenKind::IDENTIFIER) {
                error_here("se esperaba un nombre de superclase tras ':'");
                return nullptr;
            }
            c->super_name = consume().lexeme;
            // Soporte de interfaces multiples llega en A.5: aqui ignoramos
            // tokens adicionales ',' <ident> y los reportamos como no soportados.
            while (current_.kind == TokenKind::COMMA) {
                (void)consume();
                if (current_.kind == TokenKind::IDENTIFIER) {
                    c->interface_names.push_back(consume().lexeme);
                } else break;
            }
        }

        (void)expect(TokenKind::LBRACE, "se esperaba '{' al abrir el cuerpo de la clase");

        // Cuerpo: secuencia de campos y metodos hasta '}'.  Cada miembro
        // puede llevar modificadores opcionales en cualquier orden:
        //   public | private | protected | static | final
        // Se aceptan combinaciones razonables; no se valida que tengan
        // sentido (eso lo hace el type checker en hitos posteriores).
        while (current_.kind != TokenKind::RBRACE
            && current_.kind != TokenKind::END_OF_FILE) {

            // Parsear anotaciones prefijas (@Name o @Name(args...)).
            // Reconocidas con efecto semantico:
            //   @Override                -> marca el metodo como override
            //   @Inline                  -> marca el metodo para inlining en call sites
            //   @Before("Cls.metodo")    -> registra advice BEFORE en __module_init
            //   @After("Cls.metodo")     -> registra advice AFTER
            //   @Around("Cls.metodo")    -> registra advice AROUND (no implementado en exec)
            // El resto se aceptan silenciosamente.
            bool        annot_override     = false;
            bool        annot_inline       = false;
            uint8_t     annot_advice_kind  = 0;  // 0=ninguno, 1=BEFORE, 2=AFTER, 3=AROUND
            std::string annot_advice_target;
            while (current_.kind == TokenKind::AT) {
                (void)consume();  // '@'
                if (current_.kind == TokenKind::IDENTIFIER) {
                    const std::string aname = current_.lexeme;
                    (void)consume();
                    // Detectar kind de advice por nombre.
                    uint8_t this_kind = 0;
                    if      (aname == "Before") this_kind = 1;
                    else if (aname == "After")  this_kind = 2;
                    else if (aname == "Around") this_kind = 3;
                    if (aname == "Override") annot_override = true;
                    if (aname == "Inline")   annot_inline   = true;

                    if (current_.kind == TokenKind::LPAREN) {
                        (void)consume();  // '('
                        // Si es un advice y el primer arg es STRING_LIT,
                        // capturarlo como pointcut.  Para otras anotaciones
                        // o args adicionales, comemos sin almacenar.
                        if (this_kind != 0
                         && current_.kind == TokenKind::STRING_LIT) {
                            annot_advice_kind   = this_kind;
                            annot_advice_target = current_.str_val; // valor sin comillas
                            (void)consume();
                        }
                        // Avanzar hasta el ')' coincidente (saltando args adicionales).
                        int depth = 1;
                        while (depth > 0
                            && current_.kind != TokenKind::END_OF_FILE) {
                            if (current_.kind == TokenKind::LPAREN) ++depth;
                            else if (current_.kind == TokenKind::RPAREN) {
                                --depth;
                                if (depth == 0) { (void)consume(); break; }
                            }
                            (void)consume();
                        }
                    }
                } else {
                    error_here("se esperaba el nombre de la anotacion tras '@'");
                    break;
                }
            }

            // Parsear modificadores prefijos.
            uint8_t access     = 0;     // 0 = default/public, 1 = private, 2 = protected
            bool    is_static  = false;
            bool    is_final   = false;
            bool    saw_access = false;
            for (;;) {
                if (current_.kind == TokenKind::KW_PUBLIC) {
                    if (saw_access) error_here("modificador de acceso duplicado");
                    access = 0;
                    saw_access = true;
                    (void)consume();
                } else if (current_.kind == TokenKind::KW_PRIVATE) {
                    if (saw_access) error_here("modificador de acceso duplicado");
                    access = 1;
                    saw_access = true;
                    (void)consume();
                } else if (current_.kind == TokenKind::KW_PROTECTED) {
                    if (saw_access) error_here("modificador de acceso duplicado");
                    access = 2;
                    saw_access = true;
                    (void)consume();
                } else if (current_.kind == TokenKind::KW_STATIC) {
                    is_static = true;
                    (void)consume();
                } else if (current_.kind == TokenKind::KW_FINAL) {
                    is_final = true;
                    (void)consume();
                } else {
                    break;
                }
            }

            // Caso 0: setter de propiedad: `set name(T v) { ... }`.  No
            // lleva tipo de retorno (void implicito).  Se detecta primero
            // porque KW_SET no es un tipo.  El metodo se almacena con
            // nombre `set_<name>` y `property_kind=2` para que el frontend
            // reescriba luego `obj.name = X` a `obj.set_name(X)`.
            if (current_.kind == TokenKind::KW_SET) {
                const SourceLoc sloc = current_.loc;
                (void)consume();  // 'set'
                if (current_.kind != TokenKind::IDENTIFIER) {
                    error_here("se esperaba el nombre de la propiedad tras 'set'");
                    synchronize();
                    continue;
                }
                std::string prop = consume().lexeme;
                auto m = std::make_unique<ast::ClassMethodDecl>();
                m->loc           = sloc;
                m->name          = std::string("set_") + prop;
                m->return_type   = nullptr;          // void implicito
                m->access        = access;
                m->is_static     = is_static;
                m->is_final      = is_final;
                m->is_override   = annot_override;
                m->is_inline     = annot_inline;
                m->property_kind = 2;
                m->property_name = prop;
                (void)expect(TokenKind::LPAREN, "se esperaba '(' tras nombre del setter");
                if (current_.kind != TokenKind::RPAREN) {
                    auto p  = std::make_unique<ast::ParamDecl>();
                    p->loc  = current_.loc;
                    p->type = parse_type_node();
                    if (!p->type) { synchronize(); continue; }
                    if (current_.kind != TokenKind::IDENTIFIER) {
                        error_here("se esperaba el nombre del parametro del setter");
                        synchronize();
                        continue;
                    }
                    p->name = consume().lexeme;
                    m->params.push_back(std::move(p));
                }
                (void)expect(TokenKind::RPAREN, "se esperaba ')' al cerrar parametros del setter");
                m->body = parse_method_body(/*is_void=*/true);
                c->methods.push_back(std::move(m));
                continue;
            }

            // destructor: `~ClassName() { ... }`.  El parser detecta
            // TILDE seguido del nombre de la clase + '('.  Sin parametros.
            // Sin tipo de retorno (void implicito).
            if (current_.kind == TokenKind::TILDE
             && lex_.peek_at(0).kind == TokenKind::IDENTIFIER
             && lex_.peek_at(0).lexeme == c->name
             && lex_.peek_at(1).kind == TokenKind::LPAREN) {
                (void)consume(); // '~'
                auto m = std::make_unique<ast::ClassMethodDecl>();
                m->loc            = current_.loc;
                // Nombre interno: __dtor (sin '~' para que el label
                // emitido por el lowering -- ClassName__<name> -- sea
                // valido en el ensamblador, que rechaza '~' en symbol).
                // El campo @c is_destructor permite reidentificar.
                (void)consume(); // class name
                m->name           = "__dtor";
                m->is_destructor  = true;
                m->return_type    = nullptr; // void implicito
                m->access         = access;
                m->is_static      = false;
                m->is_final       = is_final;
                m->is_override    = annot_override;
                (void)expect(TokenKind::LPAREN, "se esperaba '(' tras nombre del destructor");
                if (current_.kind != TokenKind::RPAREN) {
                    error_here("destructor no acepta parametros");
                    synchronize();
                    continue;
                }
                (void)expect(TokenKind::RPAREN, "se esperaba ')' tras destructor");
                m->body = parse_method_body(/*is_void=*/true);
                c->methods.push_back(std::move(m));
                continue;
            }

            // Caso 1: constructor - el nombre del miembro coincide con el
            // de la clase y va inmediatamente seguido de '('.  Los
            // modificadores `static` no aplican al constructor; el type
            // checker reporta luego.
            if (current_.kind == TokenKind::IDENTIFIER
             && current_.lexeme == c->name
             && lex_.peek_at(0).kind == TokenKind::LPAREN) {
                auto m = std::make_unique<ast::ClassMethodDecl>();
                m->loc            = current_.loc;
                m->name           = consume().lexeme;
                m->is_constructor = true;
                m->return_type    = nullptr; // void implicito
                m->access         = access;
                m->is_static      = is_static;
                m->is_final       = is_final;
                m->is_override    = annot_override;
                (void)expect(TokenKind::LPAREN, "se esperaba '(' tras nombre del constructor");
                while (current_.kind != TokenKind::RPAREN
                    && current_.kind != TokenKind::END_OF_FILE) {
                    auto p = std::make_unique<ast::ParamDecl>();
                    p->loc  = current_.loc;
                    p->type = parse_type_node();
                    if (!p->type) { synchronize(); break; }
                    if (current_.kind != TokenKind::IDENTIFIER) {
                        error_here("se esperaba el nombre del parametro");
                        synchronize();
                        break;
                    }
                    p->name = consume().lexeme;
                    m->params.push_back(std::move(p));
                    if (!match(TokenKind::COMMA)) break;
                }
                (void)expect(TokenKind::RPAREN, "se esperaba ')' al cerrar parametros del constructor");
                // Constructor admite cuerpo de bloque o expression-bodied
                // (=> expr ;) que se traduce a `{ this(args via expr); }` no es
                // util para ctor; lo permitimos solo para metodos no-ctor.
                m->body = parse_method_body(/*is_void=*/true);
                c->methods.push_back(std::move(m));
                continue;
            }

            // Caso 2: campo o metodo normal.  Ambos comienzan con un tipo.
            if (!starts_type()) {
                error_here("se esperaba un tipo de campo o metodo dentro de la clase");
                synchronize();
                continue;
            }
            const SourceLoc mloc = current_.loc;
            auto type_node = parse_type_node();
            if (!type_node) { synchronize(); continue; }

            // Caso 2.5: getter de propiedad: `T get name => expr;` o
            // `T get name { return expr; }`.  No lleva parametros; el
            // metodo se almacena con nombre `get_<name>` y
            // `property_kind=1`.
            //
            // Distinguir contextualmente entre property getter y un
            // metodo llamado "get": si tras 'get' viene un IDENTIFIER es
            // property getter (`T get name => ...`); si viene '(' es un
            // metodo normal (`T get(args) { body }`).  Sin esto el
            // parser entraba en bucle al encontrar `i32 get() {...}`
            // (consume 'get', falla en IDENTIFIER, synchronize avanza
            // dentro del body y luego no podia recuperar al inicio del
            // siguiente miembro).  El parser ya consume 'get' como
            // KW_GET aunque sea nombre de metodo: hay que mirar el
            // segundo token sin consumir antes de decidir.
            if (current_.kind == TokenKind::KW_GET
             && lex_.peek_at(0).kind == TokenKind::IDENTIFIER) {
                (void)consume();  // 'get'
                if (current_.kind != TokenKind::IDENTIFIER) {
                    error_here("se esperaba el nombre de la propiedad tras 'get'");
                    synchronize();
                    continue;
                }
                std::string prop = consume().lexeme;
                auto m = std::make_unique<ast::ClassMethodDecl>();
                m->loc           = mloc;
                m->name          = std::string("get_") + prop;
                m->return_type   = std::move(type_node);
                m->access        = access;
                m->is_static     = is_static;
                m->is_final      = is_final;
                m->is_override   = annot_override;
                m->is_inline     = annot_inline;
                m->property_kind = 1;
                m->property_name = prop;
                m->body = parse_method_body(/*is_void=*/false);
                c->methods.push_back(std::move(m));
                continue;
            }

            // Aceptar KW_GET / KW_SET como nombres de miembro normales:
            // si llegamos aqui es porque NO seguia el patron de property
            // (`get name => ...` o `set name(T v) ...`), por tanto el
            // usuario quiere un metodo/campo llamado literalmente "get"
            // o "set".  Sin esto el parser entraba en bucle eterno al
            // ver `i32 get() { ... }` (KW_GET no es IDENTIFIER asi que
            // error_here + synchronize, y synchronize re-entraba en el
            // siguiente miembro produciendo el mismo error).
            if (current_.kind != TokenKind::IDENTIFIER
             && current_.kind != TokenKind::KW_GET
             && current_.kind != TokenKind::KW_SET) {
                error_here("se esperaba el nombre del miembro");
                synchronize();
                continue;
            }
            std::string member_name = consume().lexeme;

            // Distinguir campo (siguiente '=' o ';') vs metodo (siguiente '(').
            if (current_.kind == TokenKind::LPAREN) {
                // Metodo de instancia o estatico.
                auto m = std::make_unique<ast::ClassMethodDecl>();
                m->loc         = mloc;
                m->name        = std::move(member_name);
                m->return_type   = std::move(type_node);
                m->access        = access;
                m->is_static     = is_static;
                m->is_final      = is_final;
                m->is_override   = annot_override;
                m->is_inline     = annot_inline;
                m->advice_kind   = annot_advice_kind;
                m->advice_target = annot_advice_target;
                (void)consume(); // '('
                while (current_.kind != TokenKind::RPAREN
                    && current_.kind != TokenKind::END_OF_FILE) {
                    auto p = std::make_unique<ast::ParamDecl>();
                    p->loc  = current_.loc;
                    p->type = parse_type_node();
                    if (!p->type) { synchronize(); break; }
                    if (current_.kind != TokenKind::IDENTIFIER) {
                        error_here("se esperaba el nombre del parametro");
                        synchronize();
                        break;
                    }
                    p->name = consume().lexeme;
                    m->params.push_back(std::move(p));
                    if (!match(TokenKind::COMMA)) break;
                }
                (void)expect(TokenKind::RPAREN, "se esperaba ')' al cerrar parametros del metodo");
                m->body = parse_method_body(/*is_void=*/false);
                c->methods.push_back(std::move(m));
            } else {
                // Campo.  Init opcional con '='.
                ast::ClassFieldDecl f;
                f.loc       = mloc;
                f.type      = std::move(type_node);
                f.name      = std::move(member_name);
                f.access    = access;
                f.is_static = is_static;
                f.is_final  = is_final;
                if (match(TokenKind::ASSIGN)) {
                    f.init = parse_expr();
                }
                (void)expect(TokenKind::SEMICOLON, "se esperaba ';' al final del campo");
                c->fields.push_back(std::move(f));
            }
        }
        (void)expect(TokenKind::RBRACE, "se esperaba '}' al cerrar la clase");
        return c;
    }

    // -----------------------------------------------------------------
    // parse_interface_decl
    //
    // Sintaxis:
    //   interface IFoo {
    //       i32 metodo(i32 arg);
    //       void otro();
    //   }
    //
    // Cada metodo termina en `;` (sin body); no se permiten campos.  El
    // type checker valida que ninguna clase implementadora deje fuera
    // ningun metodo y que las firmas sean compatibles.  El lowering
    // emite defclass para la interfaz (sin defmethod en __module_init,
    // ya que no hay code_vaddr) para que sea localizable via findclass.
    // -----------------------------------------------------------------
    std::unique_ptr<ast::ClassDecl> Parser::parse_interface_decl() {
        auto c = std::make_unique<ast::ClassDecl>();
        c->loc          = current_.loc;
        c->is_interface = true;
        (void)consume(); // 'interface'

        if (current_.kind != TokenKind::IDENTIFIER) {
            error_here("se esperaba un nombre tras 'interface'");
            return nullptr;
        }
        c->name = consume().lexeme;

        // Una interfaz puede extender otras interfaces via `:`.  Aceptamos
        // la misma sintaxis que class para uniformidad: `interface I : J, K`.
        if (current_.kind == TokenKind::COLON) {
            (void)consume();
            if (current_.kind == TokenKind::IDENTIFIER) {
                c->super_name = consume().lexeme;
            }
            while (current_.kind == TokenKind::COMMA) {
                (void)consume();
                if (current_.kind == TokenKind::IDENTIFIER) {
                    c->interface_names.push_back(consume().lexeme);
                } else break;
            }
        }

        (void)expect(TokenKind::LBRACE,
                     "se esperaba '{' al abrir el cuerpo de la interfaz");

        while (current_.kind != TokenKind::RBRACE
            && current_.kind != TokenKind::END_OF_FILE) {
            // Las interfaces solo declaran metodos (no fields, no init).
            // Modificadores de acceso: aceptamos `public` (default) por
            // claridad, pero todos los metodos son implicitamente publicos.
            if (current_.kind == TokenKind::KW_PUBLIC) (void)consume();
            // Tipo de retorno.
            if (!starts_type()) {
                error_here("se esperaba un tipo de retorno en metodo de interfaz");
                synchronize();
                continue;
            }
            const SourceLoc mloc = current_.loc;
            auto rettype = parse_type_node();
            if (!rettype) { synchronize(); continue; }
            if (current_.kind != TokenKind::IDENTIFIER) {
                error_here("se esperaba el nombre del metodo de interfaz");
                synchronize();
                continue;
            }
            std::string mname = consume().lexeme;
            if (current_.kind != TokenKind::LPAREN) {
                error_here("se esperaba '(' tras nombre del metodo de interfaz");
                synchronize();
                continue;
            }
            auto m = std::make_unique<ast::ClassMethodDecl>();
            m->loc            = mloc;
            m->name           = std::move(mname);
            m->return_type    = std::move(rettype);
            m->access         = 0;
            m->is_static      = false;
            m->is_final       = false;
            m->is_constructor = false;
            (void)consume(); // '('
            while (current_.kind != TokenKind::RPAREN
                && current_.kind != TokenKind::END_OF_FILE) {
                auto p = std::make_unique<ast::ParamDecl>();
                p->loc  = current_.loc;
                p->type = parse_type_node();
                if (!p->type) { synchronize(); break; }
                if (current_.kind != TokenKind::IDENTIFIER) {
                    error_here("se esperaba el nombre del parametro");
                    synchronize();
                    break;
                }
                p->name = consume().lexeme;
                m->params.push_back(std::move(p));
                if (!match(TokenKind::COMMA)) break;
            }
            (void)expect(TokenKind::RPAREN,
                         "se esperaba ')' al cerrar parametros del metodo");
            (void)expect(TokenKind::SEMICOLON,
                         "se esperaba ';' al final del metodo abstracto");
            // body queda nullptr -> metodo abstracto.
            c->methods.push_back(std::move(m));
        }
        (void)expect(TokenKind::RBRACE,
                     "se esperaba '}' al cerrar el cuerpo de la interfaz");
        return c;
    }

    /**
     * @brief Parsea el cuerpo de un metodo (cuerpo de bloque o
     *        expression-bodied @c => expr ;).
     *
     * - Bloque: @c { stmts... }, retorno explicito en stmts.
     * - Expression-bodied: @c => expr ; equivale a @c { return expr; }
     *   (o solo @c expr; si @p is_void).  Util para metodos cortos.
     */
    std::unique_ptr<ast::BlockStmt> Parser::parse_method_body(bool is_void) {
        if (current_.kind == TokenKind::FAT_ARROW) {
            const SourceLoc loc = current_.loc;
            (void)consume(); // '=>'
            auto block = std::make_unique<ast::BlockStmt>();
            block->loc = loc;
            auto expr = parse_expr();
            if (is_void) {
                // Tratar como ExprStmt: ejecutar la expresion y descartar.
                auto es  = std::make_unique<ast::ExprStmt>();
                es->loc  = loc;
                es->expr = std::move(expr);
                block->body.push_back(std::move(es));
            } else {
                // Wrap como `return expr;`.
                auto rs   = std::make_unique<ast::ReturnStmt>();
                rs->loc   = loc;
                rs->value = std::move(expr);
                block->body.push_back(std::move(rs));
            }
            (void)expect(TokenKind::SEMICOLON, "se esperaba ';' tras expression-bodied '=>'");
            return block;
        }
        return parse_block();
    }

    // ---------------------------------------------------------------------
    // Bloques y statements.
    // ---------------------------------------------------------------------

    std::unique_ptr<ast::BlockStmt> Parser::parse_block() {
        auto b = std::make_unique<ast::BlockStmt>();
        b->loc = current_.loc;
        (void)expect(TokenKind::LBRACE, "se esperaba '{' al abrir bloque");
        while (current_.kind != TokenKind::RBRACE
            && current_.kind != TokenKind::END_OF_FILE) {
            auto s = parse_statement();
            if (s) b->body.push_back(std::move(s));
            else   synchronize();
        }
        (void)expect(TokenKind::RBRACE, "se esperaba '}' al cerrar bloque");
        return b;
    }

    std::unique_ptr<ast::Stmt> Parser::parse_statement() {
        // `label:` -- declaracion de etiqueta para `goto`.  Detectada
        // como IDENT seguido inmediatamente de COLON (sin espacio
        // semantico en medio).  La etiqueta se modela como un statement
        // separado; el siguiente statement se procesa normalmente.
        if (current_.kind == TokenKind::IDENTIFIER) {
            Lexer &mut_lex = const_cast<Lexer &>(lex_);
            if (mut_lex.peek_at(0).kind == TokenKind::COLON) {
                auto lab = std::make_unique<ast::LabelStmt>();
                lab->loc  = current_.loc;
                lab->name = consume().lexeme;     // IDENT
                (void)consume();                   // ':'
                return lab;
            }
        }
        switch (current_.kind) {
            case TokenKind::LBRACE:        return parse_block();
            case TokenKind::KW_IF:         return parse_if_stmt();
            case TokenKind::KW_WHILE:      return parse_while_stmt();
            case TokenKind::KW_DO:         return parse_do_while_stmt();
            case TokenKind::KW_FOR:        return parse_for_stmt();
            case TokenKind::KW_RETURN:     return parse_return_stmt();
            case TokenKind::KW_CONST: {
                (void)consume();
                return parse_var_decl_stmt(true);
            }
            case TokenKind::KW_BREAK: {
                auto s = std::make_unique<ast::BreakStmt>();
                s->loc = current_.loc;
                (void)consume();
                (void)expect(TokenKind::SEMICOLON, "se esperaba ';' tras 'break'");
                return s;
            }
            case TokenKind::KW_CONTINUE: {
                auto s = std::make_unique<ast::ContinueStmt>();
                s->loc = current_.loc;
                (void)consume();
                (void)expect(TokenKind::SEMICOLON, "se esperaba ';' tras 'continue'");
                return s;
            }
            case TokenKind::KW_GOTO: {
                auto s = std::make_unique<ast::GotoStmt>();
                s->loc = current_.loc;
                (void)consume(); // 'goto'
                if (current_.kind != TokenKind::IDENTIFIER) {
                    error_here("se esperaba un identificador (label) tras 'goto'");
                    return nullptr;
                }
                s->label = consume().lexeme;
                (void)expect(TokenKind::SEMICOLON, "se esperaba ';' tras 'goto label'");
                return s;
            }
            case TokenKind::KW_TRY:        return parse_try_stmt();
            case TokenKind::KW_THROW:      return parse_throw_stmt();
            case TokenKind::KW_SYNCHRONIZED: return parse_synchronized_stmt();
            case TokenKind::KW_MATCH: {
                // A.11 ADTs: match como statement.  El parser de
                // expresion ya sabe parsear MatchExpr (rama en
                // parse_primary), pero como statement queremos que NO
                // exija un `;` final (igual que if/while/for).
                auto e = parse_match_expr();
                if (!e) return nullptr;
                auto es = std::make_unique<ast::ExprStmt>();
                es->loc  = e->loc;
                es->expr = std::move(e);
                // `;` opcional tras `}`.
                if (current_.kind == TokenKind::SEMICOLON) (void)consume();
                return es;
            }
            default:
                if (starts_type()) return parse_var_decl_stmt(false);
                return parse_expr_stmt();
        }
    }

    std::unique_ptr<ast::Stmt> Parser::parse_var_decl_stmt(bool is_const) {
        auto vd = std::make_unique<ast::VarDeclStmt>();
        vd->loc      = current_.loc;
        vd->is_const = is_const;
        vd->type     = parse_type_node();
        // azucar: `T !!name = init;` equivale a
        // `nonnull T name = !!init;`.  El `!!` entre tipo y nombre
        // marca el tipo como no-null y envuelve el inicializador con
        // unwrap para insertar el check runtime + assert compile-time.
        bool inline_nonnull = false;
        if (current_.kind == TokenKind::BANG_BANG) {
            inline_nonnull = true;
            (void)consume();
            if (vd->type) vd->type->is_nonnull = true;
        }
        if (current_.kind != TokenKind::IDENTIFIER) {
            error_here("se esperaba un nombre tras el tipo");
            return nullptr;
        }
        vd->name = consume().lexeme;
        if (match(TokenKind::ASSIGN)) {
            vd->init = parse_expr();
            // Si la sintaxis fue `T !!name = init`, envolvemos el init
            // con un `!!` automatico para que el runtime falle pronto si
            // init resulta null.  Si el usuario ya escribio `!!init`, el
            // doble unwrap es idempotente (segundo unwrap sobre valor no
            // null = valor mismo).
            if (inline_nonnull && vd->init) {
                auto un = std::make_unique<ast::UnaryExpr>();
                un->loc     = vd->init->loc;
                un->op      = ast::UnOp::Unwrap;
                un->operand = std::move(vd->init);
                vd->init    = std::move(un);
            }
        }
        (void)expect(TokenKind::SEMICOLON, "se esperaba ';' al final de la declaracion");
        return vd;
    }

    std::unique_ptr<ast::Stmt> Parser::parse_if_stmt() {
        auto s = std::make_unique<ast::IfStmt>();
        s->loc = current_.loc;
        (void)consume(); // 'if'
        (void)expect(TokenKind::LPAREN, "se esperaba '(' tras 'if'");
        s->cond = parse_expr();
        (void)expect(TokenKind::RPAREN, "se esperaba ')' tras la condicion");
        s->then_branch = parse_statement();
        if (match(TokenKind::KW_ELSE)) {
            s->else_branch = parse_statement();
        }
        return s;
    }

    std::unique_ptr<ast::Stmt> Parser::parse_while_stmt() {
        auto s = std::make_unique<ast::WhileStmt>();
        s->loc = current_.loc;
        (void)consume(); // 'while'
        (void)expect(TokenKind::LPAREN, "se esperaba '(' tras 'while'");
        s->cond = parse_expr();
        (void)expect(TokenKind::RPAREN, "se esperaba ')' tras la condicion");
        s->body = parse_statement();
        return s;
    }

    std::unique_ptr<ast::Stmt> Parser::parse_do_while_stmt() {
        // Forma: do <stmt> while ( <expr> ) ;
        // En el lowering tiene un manejador dedicado (lower_do_while)
        // que baja directamente el patron CFG sin duplicar el body en el AST.
        auto s = std::make_unique<ast::DoWhileStmt>();
        s->loc = current_.loc;
        (void)consume(); // 'do'
        s->body = parse_statement();
        (void)expect(TokenKind::KW_WHILE, "se esperaba 'while' tras el cuerpo de 'do'");
        (void)expect(TokenKind::LPAREN, "se esperaba '(' tras 'while'");
        s->cond = parse_expr();
        (void)expect(TokenKind::RPAREN, "se esperaba ')' tras la condicion");
        (void)expect(TokenKind::SEMICOLON, "se esperaba ';' al final de 'do-while'");
        return s;
    }

    std::unique_ptr<ast::Stmt> Parser::parse_for_stmt() {
        const SourceLoc for_loc = current_.loc;
        (void)consume(); // 'for'
        (void)expect(TokenKind::LPAREN, "se esperaba '(' tras 'for'");

        // Disambiguacion entre foreach y counted-for.
        //   foreach: `for (T NAME : EXPR) body`
        //   counted: `for (init? ; cond? ; step?) body`
        // Ambos empiezan con un tipo opcional; la diferencia es lo que
        // viene tras el primer identificador.  Hacemos lookahead: si
        // encontramos `:` despues de `T NAME`, vamos por la rama
        // foreach; de lo contrario reusamos parse_var_decl_stmt.
        if (starts_type()) {
            // Save state to allow rollback if not foreach.
            // Parseamos el tipo (ya valido por starts_type).
            auto type_node = parse_type_node();
            if (!type_node) {
                error_here("tipo invalido en for");
                return nullptr;
            }
            // Tras el tipo: identificador.
            if (current_.kind == TokenKind::IDENTIFIER) {
                std::string name = consume().lexeme;
                if (current_.kind == TokenKind::COLON) {
                    // foreach
                    (void)consume(); // ':'
                    auto fe = std::make_unique<ast::ForEachStmt>();
                    fe->loc       = for_loc;
                    fe->iter_type = std::move(type_node);
                    fe->iter_name = std::move(name);
                    fe->iter_expr = parse_expr();
                    (void)expect(TokenKind::RPAREN, "se esperaba ')' tras for-each");
                    fe->body = parse_statement();
                    return fe;
                }
                // No es foreach: reconstruimos el VarDeclStmt manualmente
                // (no podemos retroceder tokens facilmente).
                auto s = std::make_unique<ast::ForStmt>();
                s->loc = for_loc;
                auto vd = std::make_unique<ast::VarDeclStmt>();
                vd->loc  = for_loc;
                vd->type = std::move(type_node);
                vd->name = std::move(name);
                if (match(TokenKind::ASSIGN)) {
                    vd->init = parse_expr();
                }
                (void)expect(TokenKind::SEMICOLON, "se esperaba ';' tras init del 'for'");
                s->init = std::move(vd);
                if (current_.kind != TokenKind::SEMICOLON) s->cond = parse_expr();
                (void)expect(TokenKind::SEMICOLON, "se esperaba ';' tras la condicion del 'for'");
                if (current_.kind != TokenKind::RPAREN) s->step = parse_expr();
                (void)expect(TokenKind::RPAREN, "se esperaba ')' al cerrar 'for'");
                s->body = parse_statement();
                return s;
            }
            error_here("se esperaba un identificador tras el tipo en for");
            return nullptr;
        }

        // No tipo al inicio: counted-for sin init de tipo o vacio.
        auto s = std::make_unique<ast::ForStmt>();
        s->loc = for_loc;
        if (!match(TokenKind::SEMICOLON)) {
            s->init = parse_expr_stmt();
        }
        if (current_.kind != TokenKind::SEMICOLON) s->cond = parse_expr();
        (void)expect(TokenKind::SEMICOLON, "se esperaba ';' tras la condicion del 'for'");
        if (current_.kind != TokenKind::RPAREN) s->step = parse_expr();
        (void)expect(TokenKind::RPAREN, "se esperaba ')' al cerrar 'for'");
        s->body = parse_statement();
        return s;
    }

    std::unique_ptr<ast::Stmt> Parser::parse_return_stmt() {
        auto s = std::make_unique<ast::ReturnStmt>();
        s->loc = current_.loc;
        (void)consume(); // 'return'
        if (current_.kind != TokenKind::SEMICOLON) {
            s->value = parse_expr();
        }
        (void)expect(TokenKind::SEMICOLON, "se esperaba ';' tras 'return'");
        return s;
    }

    std::unique_ptr<ast::Stmt> Parser::parse_expr_stmt() {
        auto s = std::make_unique<ast::ExprStmt>();
        s->loc = current_.loc;
        s->expr = parse_expr();
        (void)expect(TokenKind::SEMICOLON, "se esperaba ';' al final del statement");
        return s;
    }

    // -----------------------------------------------------------------
    // try { ... } catch (T e) { ... } catch (...) { ... } finally { ... }
    //
    // Sintaxis
    //   - El catch puede omitir el binding: `catch (T) { ... }`.
    //   - El catch sin tipo `catch { ... }` o `catch (e) { ... }` es
    //     catch-all (el tipo se trata como nullptr en bytecode).
    //   - finally es opcional (en MVP se acepta sintacticamente y se
    //     emite como bloque post-catch).
    // -----------------------------------------------------------------
    std::unique_ptr<ast::Stmt> Parser::parse_try_stmt() {
        auto s = std::make_unique<ast::TryStmt>();
        s->loc = current_.loc;
        (void)consume(); // 'try'
        s->body = parse_block();
        if (!s->body) return nullptr;
        while (current_.kind == TokenKind::KW_CATCH) {
            ast::CatchClause cc;
            cc.loc = current_.loc;
            (void)consume(); // 'catch'
            (void)expect(TokenKind::LPAREN, "se esperaba '(' tras 'catch'");
            // Aceptamos: `catch (T e)`, `catch (T)`, `catch ()`.  El primer
            // identificador es el tipo si hay dos, o el binding si hay uno.
            if (current_.kind == TokenKind::IDENTIFIER) {
                std::string first = consume().lexeme;
                if (current_.kind == TokenKind::IDENTIFIER) {
                    cc.exc_class_name = std::move(first);
                    cc.var_name       = consume().lexeme;
                } else {
                    // Una sola identifier: la tratamos como tipo (sin binding).
                    cc.exc_class_name = std::move(first);
                }
            }
            (void)expect(TokenKind::RPAREN, "se esperaba ')' tras catch parametro");
            cc.body = parse_block();
            if (!cc.body) return nullptr;
            s->catches.push_back(std::move(cc));
        }
        if (current_.kind == TokenKind::KW_FINALLY) {
            (void)consume();
            s->finally_body = parse_block();
        }
        if (s->catches.empty() && !s->finally_body) {
            error_here("'try' requiere al menos un 'catch' o 'finally'");
        }
        return s;
    }

    std::unique_ptr<ast::Stmt> Parser::parse_throw_stmt() {
        auto s = std::make_unique<ast::ThrowStmt>();
        s->loc = current_.loc;
        (void)consume(); // 'throw'
        s->value = parse_expr();
        (void)expect(TokenKind::SEMICOLON, "se esperaba ';' tras 'throw <expr>'");
        return s;
    }

    /**
     * @brief Parsea @c synchronized @c (expr) @c { @c body @c }.
     *
     * Sintaxis: paren obligatorios alrededor de la expresion-target,
     * llaves obligatorias alrededor del body (no admitimos el body
     * como statement suelto, porque querramos ejecutar mas de una
     * instruccion casi siempre).
     */
    std::unique_ptr<ast::Stmt> Parser::parse_synchronized_stmt() {
        auto s = std::make_unique<ast::SynchronizedStmt>();
        s->loc = current_.loc;
        (void)consume(); // 'synchronized'
        (void)expect(TokenKind::LPAREN, "se esperaba '(' tras 'synchronized'");
        s->target = parse_expr();
        (void)expect(TokenKind::RPAREN, "se esperaba ')' tras 'synchronized (expr'");
        if (current_.kind != TokenKind::LBRACE) {
            error_here("se esperaba '{' tras 'synchronized (expr)'");
            return nullptr;
        }
        s->body = parse_block();
        if (!s->body) return nullptr;
        return s;
    }

    // ---------------------------------------------------------------------
    // Expresiones (cascada de precedencias).
    // ---------------------------------------------------------------------

    std::unique_ptr<ast::Expr> Parser::parse_expr() {
        return parse_assignment();
    }

    std::unique_ptr<ast::Expr> Parser::parse_assignment() {
        // Asociatividad por la derecha.  Parseamos el lado izquierdo
        // como una expresion or-logico y, si vemos un operador de
        // asignacion, recursamos para el lado derecho.
        auto lhs = parse_logical_or();
        ast::AssignOp op;
        if (ast::assignop_from_token(current_.kind, op)) {
            const SourceLoc loc = current_.loc;
            (void)consume();
            auto rhs = parse_assignment();
            auto a = std::make_unique<ast::AssignExpr>();
            a->loc    = loc;
            a->op     = op;
            a->target = std::move(lhs);
            a->value  = std::move(rhs);
            return a;
        }
        return lhs;
    }

    // Helper: macro-like factory para los niveles binarios izquierda-asociativos.
    // Lo escribimos a mano en cada nivel en lugar de via macro/template para que
    // el optimizador inlinee con visibilidad completa.
    static std::unique_ptr<ast::Expr> make_binop(ast::BinOp op,
                                                 std::unique_ptr<ast::Expr> lhs,
                                                 std::unique_ptr<ast::Expr> rhs,
                                                 SourceLoc loc) {
        auto b = std::make_unique<ast::BinaryExpr>();
        b->loc = loc;
        b->op  = op;
        b->lhs = std::move(lhs);
        b->rhs = std::move(rhs);
        return b;
    }

    std::unique_ptr<ast::Expr> Parser::parse_logical_or() {
        auto lhs = parse_logical_and();
        while (current_.kind == TokenKind::OR_OR) {
            const SourceLoc loc = current_.loc;
            (void)consume();
            auto rhs = parse_logical_and();
            lhs = make_binop(ast::BinOp::LogicalOr, std::move(lhs), std::move(rhs), loc);
        }
        return lhs;
    }

    std::unique_ptr<ast::Expr> Parser::parse_logical_and() {
        auto lhs = parse_bitwise_or();
        while (current_.kind == TokenKind::AND_AND) {
            const SourceLoc loc = current_.loc;
            (void)consume();
            auto rhs = parse_bitwise_or();
            lhs = make_binop(ast::BinOp::LogicalAnd, std::move(lhs), std::move(rhs), loc);
        }
        return lhs;
    }

    std::unique_ptr<ast::Expr> Parser::parse_bitwise_or() {
        auto lhs = parse_bitwise_xor();
        while (current_.kind == TokenKind::PIPE) {
            const SourceLoc loc = current_.loc;
            (void)consume();
            auto rhs = parse_bitwise_xor();
            lhs = make_binop(ast::BinOp::BitOr, std::move(lhs), std::move(rhs), loc);
        }
        return lhs;
    }

    std::unique_ptr<ast::Expr> Parser::parse_bitwise_xor() {
        auto lhs = parse_bitwise_and();
        while (current_.kind == TokenKind::CARET) {
            const SourceLoc loc = current_.loc;
            (void)consume();
            auto rhs = parse_bitwise_and();
            lhs = make_binop(ast::BinOp::BitXor, std::move(lhs), std::move(rhs), loc);
        }
        return lhs;
    }

    std::unique_ptr<ast::Expr> Parser::parse_bitwise_and() {
        auto lhs = parse_equality();
        while (current_.kind == TokenKind::AMP) {
            const SourceLoc loc = current_.loc;
            (void)consume();
            auto rhs = parse_equality();
            lhs = make_binop(ast::BinOp::BitAnd, std::move(lhs), std::move(rhs), loc);
        }
        return lhs;
    }

    std::unique_ptr<ast::Expr> Parser::parse_equality() {
        auto lhs = parse_relational();
        while (current_.kind == TokenKind::EQ || current_.kind == TokenKind::NEQ) {
            const ast::BinOp op = (current_.kind == TokenKind::EQ)
                                  ? ast::BinOp::Eq : ast::BinOp::Neq;
            const SourceLoc loc = current_.loc;
            (void)consume();
            auto rhs = parse_relational();
            lhs = make_binop(op, std::move(lhs), std::move(rhs), loc);
        }
        return lhs;
    }

    std::unique_ptr<ast::Expr> Parser::parse_relational() {
        auto lhs = parse_shift();
        while (current_.kind == TokenKind::LT || current_.kind == TokenKind::LE
            || current_.kind == TokenKind::GT || current_.kind == TokenKind::GE)
        {
            ast::BinOp op = ast::BinOp::Lt;
            switch (current_.kind) {
                case TokenKind::LT: op = ast::BinOp::Lt; break;
                case TokenKind::LE: op = ast::BinOp::Le; break;
                case TokenKind::GT: op = ast::BinOp::Gt; break;
                case TokenKind::GE: op = ast::BinOp::Ge; break;
                default: break;
            }
            const SourceLoc loc = current_.loc;
            (void)consume();
            auto rhs = parse_shift();
            lhs = make_binop(op, std::move(lhs), std::move(rhs), loc);
        }
        return lhs;
    }

    std::unique_ptr<ast::Expr> Parser::parse_shift() {
        auto lhs = parse_additive();
        while (current_.kind == TokenKind::SHL || current_.kind == TokenKind::SHR) {
            const ast::BinOp op = (current_.kind == TokenKind::SHL)
                                  ? ast::BinOp::Shl : ast::BinOp::Shr;
            const SourceLoc loc = current_.loc;
            (void)consume();
            auto rhs = parse_additive();
            lhs = make_binop(op, std::move(lhs), std::move(rhs), loc);
        }
        return lhs;
    }

    std::unique_ptr<ast::Expr> Parser::parse_additive() {
        auto lhs = parse_multiplicative();
        while (current_.kind == TokenKind::PLUS || current_.kind == TokenKind::MINUS) {
            const ast::BinOp op = (current_.kind == TokenKind::PLUS)
                                  ? ast::BinOp::Add : ast::BinOp::Sub;
            const SourceLoc loc = current_.loc;
            (void)consume();
            auto rhs = parse_multiplicative();
            lhs = make_binop(op, std::move(lhs), std::move(rhs), loc);
        }
        return lhs;
    }

    std::unique_ptr<ast::Expr> Parser::parse_multiplicative() {
        auto lhs = parse_unary();
        while (current_.kind == TokenKind::STAR
            || current_.kind == TokenKind::SLASH
            || current_.kind == TokenKind::PERCENT)
        {
            ast::BinOp op = ast::BinOp::Mul;
            switch (current_.kind) {
                case TokenKind::STAR:    op = ast::BinOp::Mul; break;
                case TokenKind::SLASH:   op = ast::BinOp::Div; break;
                case TokenKind::PERCENT: op = ast::BinOp::Mod; break;
                default: break;
            }
            const SourceLoc loc = current_.loc;
            (void)consume();
            auto rhs = parse_unary();
            lhs = make_binop(op, std::move(lhs), std::move(rhs), loc);
        }
        return lhs;
    }

    std::unique_ptr<ast::Expr> Parser::parse_unary() {
        // Unarios prefijo: ! ~ - + ++ -- & * await
        // El '&' produce AddrOf (toma direccion) y el '*' produce Deref
        // (lectura via puntero).  'await' (KW_AWAIT) bloquea hasta que el
        // future operando se resuelva.  Todos comparten precedencia.
        if (current_.kind == TokenKind::KW_AWAIT) {
            const SourceLoc loc = current_.loc;
            (void)consume();
            auto operand = parse_unary();
            auto u = std::make_unique<ast::UnaryExpr>();
            u->loc     = loc;
            u->op      = ast::UnOp::Await;
            u->operand = std::move(operand);
            return u;
        }
        switch (current_.kind) {
            case TokenKind::BANG:
            case TokenKind::BANG_BANG:
            case TokenKind::TILDE:
            case TokenKind::MINUS:
            case TokenKind::PLUS:
            case TokenKind::PLUS_PLUS:
            case TokenKind::MINUS_MINUS:
            case TokenKind::AMP:
            case TokenKind::STAR: {
                const SourceLoc loc = current_.loc;
                ast::UnOp op = ast::UnOp::Neg;
                switch (current_.kind) {
                    case TokenKind::BANG:        op = ast::UnOp::LogicalNot; break;
                    case TokenKind::BANG_BANG:   op = ast::UnOp::Unwrap;     break;
                    case TokenKind::TILDE:       op = ast::UnOp::BitNot;     break;
                    case TokenKind::MINUS:       op = ast::UnOp::Neg;        break;
                    case TokenKind::PLUS:        op = ast::UnOp::Pos;        break;
                    case TokenKind::PLUS_PLUS:   op = ast::UnOp::PreInc;     break;
                    case TokenKind::MINUS_MINUS: op = ast::UnOp::PreDec;     break;
                    case TokenKind::AMP:         op = ast::UnOp::AddrOf;     break;
                    case TokenKind::STAR:        op = ast::UnOp::Deref;      break;
                    default: break;
                }
                (void)consume();
                auto operand = parse_unary();
                auto u = std::make_unique<ast::UnaryExpr>();
                u->loc     = loc;
                u->op      = op;
                u->operand = std::move(operand);
                return u;
            }
            default:
                return parse_postfix();
        }
    }

    std::unique_ptr<ast::Expr> Parser::parse_postfix() {
        // Postfix: x++, x--, x(args).  En A.1 solo estos tres.
        auto expr = parse_primary();
        while (true) {
            switch (current_.kind) {
                case TokenKind::PLUS_PLUS:
                case TokenKind::MINUS_MINUS: {
                    const SourceLoc loc = current_.loc;
                    const ast::UnOp op = (current_.kind == TokenKind::PLUS_PLUS)
                                         ? ast::UnOp::PostInc : ast::UnOp::PostDec;
                    (void)consume();
                    auto u = std::make_unique<ast::UnaryExpr>();
                    u->loc     = loc;
                    u->op      = op;
                    u->operand = std::move(expr);
                    expr = std::move(u);
                    break;
                }
                case TokenKind::LPAREN: {
                    const SourceLoc loc = current_.loc;
                    (void)consume();
                    auto call = std::make_unique<ast::CallExpr>();
                    call->loc    = loc;
                    call->callee = std::move(expr);
                    if (current_.kind != TokenKind::RPAREN) {
                        while (true) {
                            auto arg = parse_expr();
                            if (arg) call->args.push_back(std::move(arg));
                            if (!match(TokenKind::COMMA)) break;
                        }
                    }
                    (void)expect(TokenKind::RPAREN, "se esperaba ')' al cerrar la llamada");
                    expr = std::move(call);
                    break;
                }
                case TokenKind::DOT: {
                    const SourceLoc loc = current_.loc;
                    (void)consume(); // '.'
                    // Aceptar tambien KW_GET / KW_SET como nombres de
                    // campo o metodo: el usuario puede haber definido
                    // un metodo llamado 'get' o 'set' (no son property
                    // accessors aqui, son nombres normales).  Sin esto
                    // `obj.get(...)` fallaba al ver KW_GET tras DOT.
                    if (current_.kind != TokenKind::IDENTIFIER
                     && current_.kind != TokenKind::KW_GET
                     && current_.kind != TokenKind::KW_SET) {
                        error_here("se esperaba un nombre de campo tras '.'");
                        return expr;
                    }
                    auto fa = std::make_unique<ast::FieldAccessExpr>();
                    fa->loc        = loc;
                    fa->base       = std::move(expr);
                    fa->field_name = consume().lexeme;
                    expr = std::move(fa);
                    break;
                }
                case TokenKind::LBRACKET: {
                    // Subscript: base[index].  En A.3.4.b se restringe a
                    // base de tipo puntero; el type checker valida.
                    const SourceLoc loc = current_.loc;
                    (void)consume(); // '['
                    auto idx = std::make_unique<ast::IndexExpr>();
                    idx->loc   = loc;
                    idx->base  = std::move(expr);
                    idx->index = parse_expr();
                    (void)expect(TokenKind::RBRACKET, "se esperaba ']' al cerrar el subindice");
                    expr = std::move(idx);
                    break;
                }
                default:
                    return expr;
            }
        }
    }

    std::unique_ptr<ast::Expr> Parser::parse_primary() {
        const SourceLoc loc = current_.loc;

        // InitListExpr: `{ e0, e1, ... }` o
        // `{ .field = e0, ... }`.  Solo se acepta como expr en contexto
        // de inicializador (var-decl init y campos anidados).  El parser
        // no distingue contexto aqui -- el type checker valida que solo
        // aparezca en init list correctos.
        if (current_.kind == TokenKind::LBRACE) {
            auto e = std::make_unique<ast::InitListExpr>();
            e->loc = loc;
            (void)consume();   // '{'
            // Lista vacia: { } valida.
            while (current_.kind != TokenKind::RBRACE
                && current_.kind != TokenKind::END_OF_FILE) {
                std::string fname;
                bool desig = false;
                if (current_.kind == TokenKind::DOT) {
                    desig = true;
                    e->is_designated = true;
                    (void)consume();   // '.'
                    if (current_.kind != TokenKind::IDENTIFIER) {
                        error_here("se esperaba el nombre del campo tras '.'");
                        synchronize();
                        continue;
                    }
                    fname = consume().lexeme;
                    if (!match(TokenKind::ASSIGN)) {
                        error_here("se esperaba '=' tras '.field'");
                        synchronize();
                        continue;
                    }
                }
                auto val = parse_expr();
                if (!val) { synchronize(); continue; }
                if (desig) e->field_names.push_back(std::move(fname));
                else if (e->is_designated) {
                    error_here("no se puede mezclar '.field=' y posicional");
                }
                e->elements.push_back(std::move(val));
                if (!match(TokenKind::COMMA)) break;
            }
            (void)expect(TokenKind::RBRACE, "se esperaba '}' al cerrar init list");
            // Si is_designated, field_names debe tener N entradas.
            if (e->is_designated
             && e->field_names.size() != e->elements.size()) {
                diags_.error(loc,
                    "init list designado debe usar '.field=' en TODOS los elementos");
            }
            return e;
        }

        switch (current_.kind) {
            case TokenKind::INT_LIT: {
                auto e = std::make_unique<ast::IntLitExpr>();
                e->loc   = loc;
                e->value = current_.int_val;
                (void)consume();
                return e;
            }
            case TokenKind::FLOAT_LIT: {
                auto e = std::make_unique<ast::FloatLitExpr>();
                e->loc   = loc;
                e->value = current_.flt_val;
                (void)consume();
                return e;
            }
            case TokenKind::TRUE_KW:
            case TokenKind::FALSE_KW: {
                auto e = std::make_unique<ast::BoolLitExpr>();
                e->loc   = loc;
                e->value = current_.kind == TokenKind::TRUE_KW;
                (void)consume();
                return e;
            }
            case TokenKind::NULL_KW: {
                auto e = std::make_unique<ast::NullLitExpr>();
                e->loc = loc;
                (void)consume();
                return e;
            }
            case TokenKind::CHAR_LIT: {
                auto e = std::make_unique<ast::CharLitExpr>();
                e->loc       = loc;
                e->codepoint = (uint32_t)current_.int_val;
                (void)consume();
                return e;
            }
            case TokenKind::STRING_LIT:
            case TokenKind::RAW_STRING_LIT: {
                auto e = std::make_unique<ast::StringLitExpr>();
                e->loc    = loc;
                e->is_raw = (current_.kind == TokenKind::RAW_STRING_LIT);
                e->value  = consume().str_val;
                return e;
            }
            case TokenKind::ISTR_BEGIN: {
                // Interpolacion ${expr}: consumir secuencia de tokens
                // ISTR_BEGIN, [ISTR_TEXT, [ISTR_EXPR_BEGIN ... ISTR_EXPR_END,
                // [ISTR_TEXT,]]*]?  ISTR_END.  Construir un StringLitExpr
                // con interp_parts + interp_exprs.  Garantia del lexer:
                // las parts y exprs estan intercaladas correctamente y
                // siempre cierra con ISTR_END.
                auto e = std::make_unique<ast::StringLitExpr>();
                e->loc    = loc;
                e->is_raw = false;
                (void)consume();   // ISTR_BEGIN

                // Acumulador del proximo "part" literal.  Si no hay texto
                // antes de la primera expresion, se anade "" para mantener
                // el invariante parts.size() == exprs.size() + 1.
                std::string pending_text;
                bool has_pending_text = false;

                while (current_.kind != TokenKind::ISTR_END
                    && current_.kind != TokenKind::END_OF_FILE) {
                    if (current_.kind == TokenKind::ISTR_TEXT) {
                        pending_text += current_.str_val;
                        has_pending_text = true;
                        (void)consume();
                        continue;
                    }
                    if (current_.kind == TokenKind::ISTR_EXPR_BEGIN) {
                        // Consolidar el texto pendiente como part anterior.
                        e->interp_parts.push_back(std::move(pending_text));
                        pending_text.clear();
                        has_pending_text = false;
                        (void)consume();   // ISTR_EXPR_BEGIN

                        // Parsear UNA expresion (asignaciones permitidas).
                        auto expr = parse_expr();
                        if (!expr) {
                            diags_.error(current_.loc,
                                "expresion vacia dentro de ${...}");
                            // Insertar placeholder para mantener layout valido.
                            auto placeholder = std::make_unique<ast::IntLitExpr>();
                            placeholder->loc = current_.loc;
                            placeholder->value = 0;
                            e->interp_exprs.push_back(std::move(placeholder));
                        } else {
                            e->interp_exprs.push_back(std::move(expr));
                        }

                        // Consumir ISTR_EXPR_END (cierre del lexer).
                        if (current_.kind == TokenKind::ISTR_EXPR_END) {
                            (void)consume();
                        } else {
                            diags_.error(current_.loc,
                                "esperado '}' al cerrar interpolacion ${...}");
                        }
                        continue;
                    }
                    // Token inesperado dentro del string interpolado: error.
                    diags_.error(current_.loc,
                        "token inesperado dentro de string interpolado");
                    break;
                }
                // Cerrar: ultimo part = pending_text (puede estar vacio).
                e->interp_parts.push_back(std::move(pending_text));

                if (current_.kind == TokenKind::ISTR_END) {
                    (void)consume();
                }
                return e;
            }
            case TokenKind::IDENTIFIER: {
                auto e = std::make_unique<ast::IdentExpr>();
                e->loc  = loc;
                e->name = consume().lexeme;
                return e;
            }
            case TokenKind::KW_THIS: {
                // Receptor implicito de un metodo de instancia.  El type
                // checker valida que aparece dentro del cuerpo de un
                // metodo no estatico y resuelve su tipo a la clase
                // contenedora.
                auto e = std::make_unique<ast::ThisExpr>();
                e->loc = loc;
                (void)consume();
                return e;
            }
            case TokenKind::KW_NEW: {
                // Creacion de instancia: 'new' <ClassName> '(' args... ')'.
                (void)consume();
                if (current_.kind != TokenKind::IDENTIFIER) {
                    error_here("se esperaba un nombre de clase tras 'new'");
                    return nullptr;
                }
                auto e = std::make_unique<ast::NewExpr>();
                e->loc        = loc;
                e->class_name = consume().lexeme;
                // Generics A.8: argumentos de tipo opcionales `<T>` para
                // `new Box<i32>(42)`.
                if (current_.kind == TokenKind::LT) {
                    (void)consume(); // '<'
                    while (current_.kind != TokenKind::GT
                        && current_.kind != TokenKind::END_OF_FILE) {
                        auto ta = parse_type_node();
                        if (!ta) break;
                        e->type_args.push_back(std::move(ta));
                        if (!match(TokenKind::COMMA)) break;
                    }
                    (void)expect(TokenKind::GT,
                                 "se esperaba '>' al cerrar argumentos de tipo en new");
                }
                (void)expect(TokenKind::LPAREN, "se esperaba '(' tras el nombre de la clase");
                while (current_.kind != TokenKind::RPAREN
                    && current_.kind != TokenKind::END_OF_FILE) {
                    auto arg = parse_expr();
                    if (arg) e->args.push_back(std::move(arg));
                    if (!match(TokenKind::COMMA)) break;
                }
                (void)expect(TokenKind::RPAREN, "se esperaba ')' al cerrar argumentos de 'new'");
                return e;
            }
            case TokenKind::KW_SPAWN: {
                // spawn { body } -- arranca proceso hijo, devuelve PID.
                // El body es un bloque sin parametros; se compila como
                // funcion sintetica __spawn_<N> en el lowering.  No hay
                // captura lexica en MVP (closures con captura llegan en B).
                //
                // spawn admite hint de placement opcional:
                //   spawn { body }            -- Auto (round-robin entre schedulers)
                //   spawn here { body }       -- Here (mismo scheduler que el padre)
                //   spawn on(expr) { body }   -- Pinned (scheduler = expr % num_schedulers)
                // `here` y `on` se reconocen contextualmente como identificadores
                // tras KW_SPAWN para evitar reservar mas keywords globales.
                (void)consume();
                auto e = std::make_unique<ast::SpawnExpr>();
                e->loc = loc;
                if (current_.kind == TokenKind::IDENTIFIER) {
                    if (current_.lexeme == "here") {
                        (void)consume();
                        e->policy = ast::SpawnExpr::Policy::Here;
                    } else if (current_.lexeme == "on") {
                        (void)consume();
                        (void)expect(TokenKind::LPAREN,
                            "se esperaba '(' tras 'on' en spawn on(expr)");
                        e->sched_idx = parse_expr();
                        if (!e->sched_idx) return nullptr;
                        (void)expect(TokenKind::RPAREN,
                            "se esperaba ')' al cerrar 'on(expr)'");
                        e->policy = ast::SpawnExpr::Policy::Pinned;
                    }
                }
                if (current_.kind != TokenKind::LBRACE) {
                    error_here("se esperaba '{' tras 'spawn' (cuerpo del proceso)");
                    return nullptr;
                }
                e->body = parse_block();
                if (!e->body) return nullptr;
                return e;
            }
            case TokenKind::KW_RSPAWN: {
                // rspawn(node_idx) { body } -- spawn distribuido.
                // Devuelve i64 (Future handle).  El body se ejecuta en el
                // nodo remoto y su valor de retorno (via `return X`) se
                // captura en R0 + hlt; el runtime remoto envia X de vuelta
                // como fulfill del Future.  El caller hace `await fut` para
                // obtener X.
                (void)consume();
                auto e = std::make_unique<ast::RSpawnExpr>();
                e->loc = loc;
                (void)expect(TokenKind::LPAREN,
                    "se esperaba '(' tras 'rspawn' para indicar el nodo remoto");
                e->node_idx = parse_expr();
                if (!e->node_idx) return nullptr;
                (void)expect(TokenKind::RPAREN,
                    "se esperaba ')' al cerrar 'rspawn(node_idx)'");
                if (current_.kind != TokenKind::LBRACE) {
                    error_here("se esperaba '{' tras 'rspawn(node)' (cuerpo del proceso remoto)");
                    return nullptr;
                }
                e->body = parse_block();
                if (!e->body) return nullptr;
                return e;
            }
            case TokenKind::KW_MATCH:
                // match expr { case Variant(b) => body; ... }
                return parse_match_expr();
            case TokenKind::LPAREN: {
                // distinguir entre expresion parentizada y
                // lambda `(args) => expr/{...}`.  Lookahead profundo: contar
                // los parentesis hasta cerrar el grupo y mirar si lo
                // siguiente es FAT_ARROW.  El lexer expone @c peek_at sin
                // consumir, asi que no necesitamos backtracking del parser.
                if (is_lambda_start()) {
                    return parse_lambda_expr();
                }
                (void)consume();
                auto inner = parse_expr();
                (void)expect(TokenKind::RPAREN, "se esperaba ')' al cerrar la expresion entre parentesis");
                return inner;
            }
            default:
                error_here("se esperaba una expresion primaria");
                (void)consume();
                return nullptr;
        }
    }

    // -----------------------------------------------------------------
    // closures: parseo de lambdas inline y disambiguacion vs
    // expresion parentizada.
    // -----------------------------------------------------------------

    bool Parser::is_lambda_start() const noexcept {
        // Precondicion: current_ es LPAREN.  Recorremos los tokens
        // siguientes via peek_at hasta cerrar el grupo de parentesis.
        // Si el primer token tras el RPAREN matching es FAT_ARROW
        // (=>), estamos viendo una lambda; si no, es una expresion.
        //
        // Limite duro: 256 tokens de lookahead.  Los argumentos de
        // lambdas reales nunca llegan a tanto, y poner una cota
        // garantiza tiempo lineal acotado en el peor caso (codigo
        // sintacticamente invalido sin RPAREN cerrando).
        Lexer &mut_lex = const_cast<Lexer &>(lex_);
        size_t off  = 0;       // distancia desde current_+1 (peek_at(0))
        int    depth = 1;      // ya estamos dentro del primer LPAREN
        const size_t MAX_LOOKAHEAD = 256;
        while (depth > 0 && off < MAX_LOOKAHEAD) {
            const TokenKind k = mut_lex.peek_at(off).kind;
            if (k == TokenKind::END_OF_FILE) return false;
            if (k == TokenKind::LPAREN) ++depth;
            else if (k == TokenKind::RPAREN) --depth;
            ++off;
        }
        if (depth != 0) return false;
        // off apunta ahora al primer token TRAS el RPAREN matching.
        // Solo si es FAT_ARROW se trata de una lambda.  Cualquier otra
        // cosa (operador binario, llamada, indexacion, etc.) significa
        // que era una expresion parentizada.
        return mut_lex.peek_at(off).kind == TokenKind::FAT_ARROW;
    }

    std::unique_ptr<ast::Expr> Parser::parse_lambda_expr() {
        const SourceLoc loc = current_.loc;
        // Consume el LPAREN inicial.  is_lambda_start ya garantizo que
        // existe un FAT_ARROW tras el RPAREN matching.
        (void)expect(TokenKind::LPAREN, "se esperaba '(' al iniciar lambda");
        auto lam = std::make_unique<ast::LambdaExpr>();
        lam->loc = loc;

        // Lista de parametros.  Aceptamos dos formas:
        //   (T1 name1, T2 name2, ...)   ->  con tipos explicitos
        //   (name1, name2, ...)         ->  tipos inferidos del contexto
        // No mezclamos las dos formas en una sola lambda (inferiremos o
        // el usuario tipa todos).  El distinguir se hace en cada elemento:
        // si el primer token del parametro es un tipo (starts_type()),
        // parseamos `tipo nombre`; en caso contrario solo `nombre`.
        while (current_.kind != TokenKind::RPAREN
            && current_.kind != TokenKind::END_OF_FILE) {
            auto p = std::make_unique<ast::ParamDecl>();
            p->loc = current_.loc;
            if (starts_type()) {
                // Forma con tipo: "T name".
                p->type = parse_type_node();
                if (!p->type) return nullptr;
                if (current_.kind != TokenKind::IDENTIFIER) {
                    error_here("se esperaba el nombre del parametro tras el tipo en lambda");
                    return nullptr;
                }
                p->name = consume().lexeme;
            } else if (current_.kind == TokenKind::IDENTIFIER) {
                // Forma sin tipo: "name" -> p->type = null y el type
                // checker lo deducira del contexto (asignacion a fn(T)).
                p->name = consume().lexeme;
                p->type = nullptr;
            } else {
                error_here("se esperaba un parametro en la lambda");
                return nullptr;
            }
            lam->params.push_back(std::move(p));
            if (!match(TokenKind::COMMA)) break;
        }
        (void)expect(TokenKind::RPAREN, "se esperaba ')' al cerrar parametros de lambda");
        (void)expect(TokenKind::FAT_ARROW, "se esperaba '=>' tras los parametros de lambda");

        // Cuerpo: dos formas.
        //   - Block:        () => { stmts; return X; }
        //   - Expression:   () => expr     -> reescribir a { return expr; }
        if (current_.kind == TokenKind::LBRACE) {
            lam->body = parse_block();
            if (!lam->body) return nullptr;
        } else {
            // Expression-bodied: envolvemos en un ReturnStmt + BlockStmt.
            // La SourceLoc del block hereda la posicion de la lambda para
            // mantener buenos diagnosticos.
            auto e = parse_expr();
            if (!e) return nullptr;
            auto ret = std::make_unique<ast::ReturnStmt>();
            ret->loc   = e->loc;
            ret->value = std::move(e);
            auto blk = std::make_unique<ast::BlockStmt>();
            blk->loc = loc;
            blk->body.push_back(std::move(ret));
            lam->body = std::move(blk);
        }
        return lam;
    }

} // namespace vex
