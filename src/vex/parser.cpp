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

#include <iostream>
#include <string>
#include <unordered_set>
#include <utility>

namespace vex {

    /**
     * @brief Reconoce nombres de builtins comptime de introspection.
     *
     * Solo cuando el parser ve un IdentExpr cuyo nombre esta en este set
     * y va seguido de @c <, consume los type args como parte de un
     * CallExpr generico.  Sin esta restriccion, @c LT en posicion postfix
     * seria ambiguo con operadores de comparacion (@c foo < bar).
     *
     * Para anyadir nuevos builtins comptime: insertar el nombre aqui y en
     * el dispatcher del type checker.  El parser solo necesita el set
     * (un name no listado se trata como llamada normal sin type args, lo
     * cual no rompe codigo existente -- LT pasa al binary expr parser).
     */
    static bool is_comptime_builtin_name(const std::string &name) {
        static const std::unordered_set<std::string> set = {
            /* queries atomicas */
            "sizeof", "alignof", "typename", "type_id", "kind",
            /* queries de fields/methods */
            "offsetof", "has_field", "has_method",
            "field_count", "method_count",
            "field_name", "field_type",
            "is_subtype", "is_same",
            "is_class", "is_struct", "is_primitive",
            "is_newtype", "is_opaque", "underlying_of",
            /* iteracion + acceso directo */
            "field_get", "field_set",
            "for_each_field", "for_each_method",
            /* Type-as-first-class-value + builtins composables */
            "comptime_type", "parent_class", "element_type",
            "error_type", "field_type_at",
            "method_name", "method_return_type",
            /* string ops comptime (sin <T>) */
            /* Estos NO toman type_args, pero los meto aqui solo para
             * documentar que son builtins reconocidos.  El parser no los
             * usa para nada especial (no consume LT). */
        };
        return set.count(name) > 0;
    }


    // ---------------------------------------------------------------------
    // Constructor.
    // ---------------------------------------------------------------------

    Parser::Parser(Lexer &lex, Diagnostics &diags)
        : lex_(lex), diags_(diags), current_(lex.next()) {
        // current_ se carga con el primer token al construir.  A partir
        // de aqui consume() avanza siempre.
    }


    // Phase M.L24: skip una decl top-level cuando @Target no matchea.
    // Consume tokens hasta el final natural de la decl: para decls con
    // cuerpo `{ ... }`, hasta cerrar el `}` matching; para decls simples
    // (typedef, using, global var), hasta el siguiente `;` top-level.
    void Parser::skip_target_skipped_decl() {
        int brace_depth = 0;
        bool entered_body = false;
        while (current_.kind != TokenKind::END_OF_FILE) {
            const auto k = current_.kind;
            if (k == TokenKind::LBRACE) {
                ++brace_depth;
                entered_body = true;
                (void)consume();
            } else if (k == TokenKind::RBRACE) {
                if (brace_depth > 0) --brace_depth;
                (void)consume();
                if (entered_body && brace_depth == 0) break;
            } else if (k == TokenKind::SEMICOLON && brace_depth == 0
                       && !entered_body) {
                (void)consume();
                break;
            } else {
                (void)consume();
            }
        }
    }

    // Phase M.L24: evalua un spec de @Target("k:v") contra los build
    // tags del binario actual.  Tags soportados:
    //   os:windows / os:linux / os:macos / os:posix
    //   arch:x86_64 / arch:arm64 / arch:x86
    // Match exacto; si el spec usa ! como prefijo, se niega.
    static bool target_matches_(const std::string &spec_in) noexcept {
        if (spec_in.empty()) return true;
        std::string spec = spec_in;
        bool negate = false;
        if (spec[0] == '!') {
            negate = true;
            spec.erase(0, 1);
        }
        // Tags compilados desde macros del compilador host.
        std::vector<std::string> tags;
#if defined(_WIN32)
        tags.push_back("os:windows");
#elif defined(__APPLE__)
        tags.push_back("os:macos");
        tags.push_back("os:posix");
#elif defined(__linux__)
        tags.push_back("os:linux");
        tags.push_back("os:posix");
#endif
#if defined(__x86_64__) || defined(_M_X64)
        tags.push_back("arch:x86_64");
#elif defined(__aarch64__) || defined(_M_ARM64)
        tags.push_back("arch:arm64");
#elif defined(__i386__) || defined(_M_IX86)
        tags.push_back("arch:x86");
#endif
        bool match = false;
        for (const auto &t : tags) {
            if (t == spec) { match = true; break; }
        }
        return negate ? !match : match;
    }

    // Phase M6.a L.3: aplica @c pending_visibility_ al nodo si soporta
    // @c is_public.  Limpia el flag al final para que sub-decls nested
    // no hereden la visibilidad del top-level que los envuelve.
    void Parser::apply_pending_visibility(ast::Node *n) noexcept {
        if (n == nullptr || pending_visibility_ == 0) return;
        const bool is_public = (pending_visibility_ == 1);
        switch (n->kind) {
            case ast::NodeKind::FunctionDecl:
                static_cast<ast::FunctionDecl*>(n)->is_public = is_public;
                break;
            case ast::NodeKind::GlobalVarDecl:
                static_cast<ast::GlobalVarDecl*>(n)->is_public = is_public;
                break;
            case ast::NodeKind::TypeAliasDecl:
                static_cast<ast::TypeAliasDecl*>(n)->is_public = is_public;
                break;
            case ast::NodeKind::StructDecl:
                static_cast<ast::StructDecl*>(n)->is_public = is_public;
                break;
            case ast::NodeKind::EnumDecl:
                static_cast<ast::EnumDecl*>(n)->is_public = is_public;
                break;
            case ast::NodeKind::ClassDecl:
                static_cast<ast::ClassDecl*>(n)->is_public = is_public;
                break;
            default: break;
        }
        // Limpiar para no propagar a nested decls.
        pending_visibility_ = 0;
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

    Token Parser::expect_close_angle(const char *msg) {
        // Caso comun: el token actual es ya un `>` (GT).  Consumir y
        // listo.
        if (current_.kind == TokenKind::GT) return consume();
        // Caso del lexer: `>>` se tokeniza como un solo SHR.  Cuando
        // aparece cerrando un argumento de tipo anidado (e.g.
        // `VirtualPtr<VirtualPtr<i64>>`), el parser quiere cerrar UN
        // solo `>` y dejar el otro disponible para el caller exterior.
        // Partimos el token: devolvemos un GT sintetico y mutamos
        // current_ para que sea un GT con loc avanzada un caracter.
        if (current_.kind == TokenKind::SHR) {
            Token first;
            first.kind   = TokenKind::GT;
            first.lexeme = ">";
            first.loc    = current_.loc;
            // Avanzar la columna del token restante.  El campo de linea
            // no cambia: `>>` siempre cabe en una linea.
            current_.kind   = TokenKind::GT;
            current_.lexeme = ">";
            ++current_.loc.column;
            ++current_.loc.offset;
            current_.loc.length = 1;
            return first;
        }
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
        //
        // GARANTIA DE PROGRESO: consume SIEMPRE al menos un token antes
        // de chequear sync points.  Sin esto, si el parser fallo
        // dejando current_ sobre un sync-point keyword (e.g. KW_FN),
        // parse_program quedaria en bucle infinito: parse_top_level_decl
        // falla -> synchronize ve KW_FN -> retorna sin consumir -> retry.
        // Bug observado: `fn my_release(p: i64) { }` con sintaxis Rust-style
        // a nivel top-level (Vex usa C-style `T name(T param)`) causaba
        // 5+ GB de RAM al crecer indefinidamente el AST.
        if (current_.kind == TokenKind::END_OF_FILE) return;
        (void)consume();   // forzar progreso
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
                case TokenKind::KW_NAMESPACE:
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
            } else if (last_decl_was_target_skip_) {
                // L.24: skip intencional via @Target no matcheado.
                // El skip_target_skipped_decl ya consumio la decl
                // completa.  NO sincronizar (el siguiente token ya es
                // el inicio de la siguiente decl valida).
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
        // namespace foo { ... }  (Phase M.7.c, inline namespace estilo C++).
        if (current_.kind == TokenKind::KW_NAMESPACE) {
            return parse_namespace_decl();
        }
        // import "path" [as alias] [only A, B];  (Phase M sistema de modulos).
        if (current_.kind == TokenKind::KW_IMPORT) {
            return parse_import_decl(/*is_public_reexport=*/false);
        }
        // public import "x";  (re-export transitivo).
        if (current_.kind == TokenKind::KW_PUBLIC
         && lex_.peek_at(0).kind == TokenKind::KW_IMPORT) {
            (void)consume(); // 'public'
            return parse_import_decl(/*is_public_reexport=*/true);
        }
        // Phase M6.a L.3: visibilidad de top-level decl.  Capturamos
        // `public`/`private` y guardamos en pending_visibility_; cada
        // sub-parser que produzca un decl top-level consulta el flag al
        // final (helper @c apply_pending_visibility_) y lo limpia.  Sin
        // keyword: el nodo conserva su default (is_public=true) -- compat
        // con codigo existente.  Un futuro sprint M.future flipeara el
        // default a privado tras migrar stdlib + editor.
        if (current_.kind == TokenKind::KW_PUBLIC) {
            (void)consume();
            pending_visibility_   = 1;     // 1 = public explicito
        } else if (current_.kind == TokenKind::KW_PRIVATE) {
            (void)consume();
            pending_visibility_   = 2;     // 2 = private explicito
        }
        // Cleanup garantizado al salir de parse_top_level_decl.
        struct VisGuard {
            uint8_t &flag;
            ~VisGuard() { flag = 0; }
        } guard{pending_visibility_};
        // typedef <tipo> <nombre> ;
        if (current_.kind == TokenKind::KW_TYPEDEF) {
            // typedef struct/enum C-style.
            // Si tras typedef viene `struct` o `enum`, parseamos como
            // StructDecl/EnumDecl con name al final.  Sin esto solo se
            // soportaba `typedef i32 Foo;` (alias de tipo basico).
            const TokenKind nk = lex_.peek_at(0).kind;
            if (nk == TokenKind::KW_STRUCT || nk == TokenKind::KW_ENUM) {
                auto n = parse_typedef_struct_or_enum();
                apply_pending_visibility(n.get());
                return n;
            }
            auto n = parse_typedef_decl();
            apply_pending_visibility(n.get());
            return n;
        }
        // using <nombre> = <tipo> ;
        if (current_.kind == TokenKind::KW_USING) {
            auto n = parse_using_decl();
            apply_pending_visibility(n.get());
            return n;
        }
        // Anotaciones top-level que preceden a una clase o funcion:
        //   @Aspect:     clase de aspectos
        //   @Async:      funcion async, transformada a wrapper future + spawn
        //   @Introspect: clase/struct/enum runtime-introspectable (Sprint 4 A.37.s4)
        //   @Target("os:linux"): Phase M.L24 - compilacion condicional;
        //                la decl se descarta si no matchea el target actual.
        //   Otras se aceptan y se ignoran silenciosamente.
        bool top_is_aspect     = false;
        bool top_is_async      = false;
        bool top_is_introspect = false;
        bool top_is_macro      = false;  /* A.43.16: @Macro */
        bool top_is_pure       = false;  /* A.43.20: @Pure -- memoizable */
        bool top_target_skip   = false;  /* L.24: @Target no matchea */
        // v4: atributos para comptime const a nivel modulo.
        bool     top_attr_hot   = false;
        bool     top_attr_cold  = false;
        uint16_t top_attr_align = 0;
        std::string top_attr_section;
        while (current_.kind == TokenKind::AT) {
            (void)consume();
            if (current_.kind == TokenKind::IDENTIFIER) {
                const bool is_target = (current_.lexeme == "Target");
                if (current_.lexeme == "Aspect") top_is_aspect = true;
                else if (current_.lexeme == "Async") top_is_async = true;
                else if (current_.lexeme == "Introspect") top_is_introspect = true;
                else if (current_.lexeme == "Macro") top_is_macro = true;
                else if (current_.lexeme == "Pure")  top_is_pure  = true;
                // v4: atributos para comptime const.
                const bool is_align   = (current_.lexeme == "align");
                const bool is_hot     = (current_.lexeme == "hot");
                const bool is_cold    = (current_.lexeme == "cold");
                const bool is_section = (current_.lexeme == "section");
                (void)consume();
                if (is_hot)  { top_attr_hot  = true; continue; }
                if (is_cold) { top_attr_cold = true; continue; }
                if (is_align) {
                    (void)expect(TokenKind::LPAREN, "se esperaba '(' tras @align");
                    if (current_.kind != TokenKind::INT_LIT) {
                        error_here("@align(N) requiere un entero literal");
                    } else {
                        const int64_t n = current_.int_val;
                        (void)consume();
                        if (n <= 0 || n > 4096
                         || (n & (n - 1)) != 0) {
                            error_here("@align(N): N debe ser potencia de 2 en [1,4096]");
                        } else {
                            top_attr_align = static_cast<uint16_t>(n);
                        }
                    }
                    (void)expect(TokenKind::RPAREN, "se esperaba ')' tras N en @align(N)");
                    continue;
                }
                if (is_section) {
                    (void)expect(TokenKind::LPAREN, "se esperaba '(' tras @section");
                    if (current_.kind != TokenKind::STRING_LIT) {
                        error_here("@section requiere un string literal");
                    } else {
                        top_attr_section = current_.str_val;
                        (void)consume();
                    }
                    (void)expect(TokenKind::RPAREN, "se esperaba ')' tras @section");
                    continue;
                }
                if (is_target) {
                    // L.24: @Target("os:linux"|"arch:x86_64"|...).  Si la
                    // condicion NO matchea con los build tags actuales,
                    // marcamos top_target_skip para descartar la decl.
                    (void)expect(TokenKind::LPAREN, "se esperaba '(' tras @Target");
                    std::string spec;
                    if (current_.kind == TokenKind::STRING_LIT) {
                        spec = current_.str_val;
                        (void)consume();
                    } else {
                        error_here("@Target requiere un string literal");
                    }
                    (void)expect(TokenKind::RPAREN, "se esperaba ')' tras @Target");
                    if (!target_matches_(spec)) {
                        top_target_skip = true;
                    }
                } else if (current_.kind == TokenKind::LPAREN) {
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
        // Phase M.L24: si @Target no matcheo, descartar la decl completa
        // SIN parsearla.  Esto evita diagnosticos espurios por simbolos
        // que solo existen en el otro target.  La pending_visibility se
        // limpia automaticamente via VisGuard al salir.
        if (top_target_skip) {
            // L.24: indicamos "skip intencional" para que parse_program no
            // ejecute synchronize() (que descartaria tokens validos de la
            // siguiente decl).
            last_decl_was_target_skip_ = true;
            skip_target_skipped_decl();
            return nullptr;
        }
        last_decl_was_target_skip_ = false;
        // L.24: si las annotations consumieron tokens y el siguiente
        // token es `public`/`private` (caso `@Target("..") public i32 fn`),
        // re-chequear visibilidad aqui.  El bloque de KW_PUBLIC original
        // esta ANTES del while AT, por lo que las decls con annotations
        // +visibility necesitan este segundo chequeo.
        if (pending_visibility_ == 0
         && (current_.kind == TokenKind::KW_PUBLIC
          || current_.kind == TokenKind::KW_PRIVATE)) {
            pending_visibility_ = (current_.kind == TokenKind::KW_PUBLIC) ? 1 : 2;
            (void)consume();
        }
        // struct <nombre> { ... }
        if (current_.kind == TokenKind::KW_STRUCT) {
            auto sd = parse_struct_decl();
            if (sd && top_is_introspect) sd->is_introspect = true;
            apply_pending_visibility(sd.get());
            return sd;
        }
        // class <nombre> { ... }
        if (current_.kind == TokenKind::KW_CLASS) {
            auto cd = parse_class_decl();
            if (cd && top_is_aspect)     cd->is_aspect     = true;
            if (cd && top_is_introspect) cd->is_introspect = true;
            apply_pending_visibility(cd.get());
            return cd;
        }
        // interface <nombre> { metodos abstractos }
        // Reusa parse_class_decl marcando is_interface=true; el parser de
        // metodos acepta `;` en lugar de body para metodos abstractos.
        if (current_.kind == TokenKind::KW_INTERFACE) {
            auto cd = parse_interface_decl();
            if (cd && top_is_introspect) cd->is_introspect = true;
            apply_pending_visibility(cd.get());
            return cd;
        }
        // ADTs: enum <nombre> { Variante1, Variante2(T1, T2), ... }
        if (current_.kind == TokenKind::KW_ENUM) {
            auto ed = parse_enum_decl();
            if (ed && top_is_introspect) ed->is_introspect = true;
            apply_pending_visibility(ed.get());
            return ed;
        }

        // Sintaxis canonica (post-hard-break v4):
        //   comptime T NAME = expr;          -- const inmutable comptime
        //   comptime var T NAME = init;      -- mutable (solo usable en eval AST)
        //   comptime auto NAME = expr;       -- inferido inmutable
        //   comptime <T,U> RET fn(...) {...} -- funcion comptime generica
        //   comptime T fn(...) {...}         -- funcion comptime no generica
        //
        // `comptime const T X = ...;` (forma vieja con `const` redundante)
        // se REJECTA con error claro -- el const sobra porque comptime
        // implica constancia.  Hard break v4.
        bool is_comptime_const = false;     // canonico: `comptime T X = ...`
        bool is_comptime_var   = false;
        bool is_comptime_fn    = false;
        std::vector<std::string> comptime_type_params;
        if (current_.kind == TokenKind::IDENTIFIER
         && current_.lexeme == "comptime") {
            Lexer &mut_lex = const_cast<Lexer &>(lex_);
            // LANG.fix-2: `comptime { stmts }` a nivel modulo.  Permite
            // metaprogramacion que muta `comptime var` globales, llena
            // arrays con loops, etc.  Antes solo se aceptaba dentro de
            // funciones (parse_statement).  El cuerpo se ejecuta en el
            // TypeChecker via comptime_eval_stmt sobre los globales.
            if (mut_lex.peek_at(0).kind == TokenKind::LBRACE) {
                const SourceLoc cb_loc = current_.loc;
                (void)consume();                            // 'comptime'
                (void)expect(TokenKind::LBRACE,
                             "se esperaba '{' tras 'comptime'");
                auto cb = std::make_unique<ast::ComptimeBlockStmt>();
                cb->loc = cb_loc;
                while (current_.kind != TokenKind::RBRACE
                    && current_.kind != TokenKind::END_OF_FILE) {
                    /* sugar: `NAME = expr;` dentro del bloque NO se
                     * trata como decl comptime const (a nivel modulo el
                     * usuario raramente quiere eso, suele querer mutar
                     * un global existente).  Solo parse_statement
                     * normal.  Si quieres una const local al bloque,
                     * escribe `comptime T NAME = expr;` explicito. */
                    auto inner = parse_statement();
                    if (inner) cb->stmts.push_back(std::move(inner));
                    else synchronize();
                }
                (void)expect(TokenKind::RBRACE,
                             "se esperaba '}' al cerrar comptime block");
                return cb;
            }
            if (mut_lex.peek_at(0).kind == TokenKind::KW_CONST) {
                // Forma vieja `comptime const T X = ...`: hard error v4.
                const SourceLoc bad_loc = current_.loc;
                (void)consume();   /* 'comptime' */
                (void)consume();   /* 'const' redundante */
                diags_.error(bad_loc,
                    "'comptime const' es redundante; usa 'comptime T X = ...' (comptime ya implica const, salvo 'comptime var')");
                is_comptime_const = true;
            } else if (mut_lex.peek_at(0).kind == TokenKind::IDENTIFIER
                    && (mut_lex.peek_at(0).lexeme == "var"
                     || mut_lex.peek_at(0).lexeme == "auto")) {
                /* `comptime auto X` y `comptime var X` aceptados
                 * como alias.  Soportan DOS modos:
                 *   (a) `comptime var T NAME = init;` -- tipo explicito.
                 *   (b) `comptime var NAME = init;`   -- inferencia.
                 *   (c) `comptime auto NAME = init;`  -- idem (b).
                 */
                const SourceLoc sugar_loc = current_.loc;
                (void)consume();   /* 'comptime' */
                (void)consume();   /* 'var' o 'auto' */
                /* Detectar modo (b)/(c): siguiente token es IDENT seguido
                 * de `=`.  Si si, build GlobalVarDecl con type=nullptr +
                 * is_const=false + is_comptime=true + infer. */
                if (current_.kind == TokenKind::IDENTIFIER
                 && mut_lex.peek_at(0).kind == TokenKind::ASSIGN) {
                    auto gv = std::make_unique<ast::GlobalVarDecl>();
                    gv->loc         = sugar_loc;
                    gv->name        = consume().lexeme;
                    gv->is_const    = false;       /* mutable */
                    gv->is_comptime = true;
                    gv->type        = nullptr;     /* infer */
                    (void)expect(TokenKind::ASSIGN,
                                 "se esperaba '=' tras 'comptime var/auto' + nombre");
                    gv->init = parse_expr();
                    (void)expect(TokenKind::SEMICOLON,
                                 "se esperaba ';' al final de la decl comptime var");
                    return gv;
                }
                is_comptime_var = true;
            } else if (mut_lex.peek_at(0).kind == TokenKind::IDENTIFIER
                    && mut_lex.peek_at(1).kind == TokenKind::ASSIGN) {
                /* sugar: `comptime NAME = expr;` -> equivale a
                 * `comptime const auto NAME = expr;` con inferencia.
                 * Reduce el ruido al construir cadenas de macros + types
                 * en compile-time (que repetian `comptime const ...` por
                 * todas partes).  Solo aplica cuando el siguiente al
                 * IDENT es `=` (sin tipo entre medias). */
                const SourceLoc sugar_loc = current_.loc;
                (void)consume();   /* 'comptime' */
                std::string nm = consume().lexeme;        /* NAME */
                (void)expect(TokenKind::ASSIGN,
                             "se esperaba '=' tras 'comptime' + nombre");
                auto gv = std::make_unique<ast::GlobalVarDecl>();
                gv->loc         = sugar_loc;
                gv->name        = std::move(nm);
                gv->is_const    = true;
                gv->is_comptime = true;
                gv->type        = nullptr;   /* infer desde init */
                gv->init        = parse_expr();
                (void)expect(TokenKind::SEMICOLON,
                             "se esperaba ';' al final de la decl comptime");
                return gv;
            } else {
                /* `comptime <TYPE> ...` -- puede ser:
                 *   (a) `comptime T NAME = expr;`  -> comptime const var (v4 canonico)
                 *   (b) `comptime T fn(args) {...}` -> comptime function
                 *   (c) `comptime <T,U> RET fn(args) {...}` -> generic comptime fn
                 *
                 * El parser consume `comptime`, captura type-params opcionales,
                 * y deja que el flujo regular (type + name + `=` vs `(`) decida
                 * si es var o fn.  Si es var, marcamos como comptime const en
                 * el cierre.  Si es fn, ya marcamos `is_comptime_fn`. */
                (void)consume();   /* 'comptime' */
                /* A.41: type params opcionales `<T, U, ...>` para
                 * comptime fn genericos.  Solo aplica a funciones; si
                 * aparecen tras `comptime` directamente, marcamos como fn
                 * temprano porque solo funciones tienen type params. */
                if (current_.kind == TokenKind::LT) {
                    is_comptime_fn = true;
                    (void)consume();   /* '<' */
                    while (current_.kind == TokenKind::IDENTIFIER) {
                        comptime_type_params.push_back(consume().lexeme);
                        if (current_.kind == TokenKind::COMMA) {
                            (void)consume();
                            continue;
                        }
                        break;
                    }
                    (void)expect_close_angle("se esperaba '>' al cerrar type params");
                } else {
                    /* Sin `<>`: la diferencia entre var y fn la decide la
                     * forma posterior (`= expr;` vs `(args) {...}`).  Marcamos
                     * tentativamente como comptime_const; si luego resulta
                     * que es funcion (`(` tras el nombre), revertimos el flag
                     * antes de devolver el FunctionDecl. */
                    is_comptime_const = true;
                }
            }
        }

        // Manejar 'const' opcional al principio.  En v4, comptime ya implica
        // const, asi que el `const` aparece solo en declaraciones runtime.
        bool is_const = false;
        if (match(TokenKind::KW_CONST)) is_const = true;

        // `static_assert(cond, "msg");` a nivel modulo.  Lo
        // parseamos como una ExprStmt envuelto en un GlobalVarDecl
        // dummy con type=void + init=ese CallExpr.  El type checker lo
        // procesa en la pasada de globales y emite error si la cond es
        // false; el lowering lo trata como void global (no genera codigo).
        if (current_.kind == TokenKind::IDENTIFIER
         && current_.lexeme == "static_assert") {
            Lexer &mut_lex = const_cast<Lexer &>(lex_);
            if (mut_lex.peek_at(0).kind == TokenKind::LPAREN) {
                const SourceLoc sa_loc = current_.loc;
                auto call_expr = parse_expr();              /* static_assert(...) */
                (void)expect(TokenKind::SEMICOLON,
                             "se esperaba ';' tras static_assert(...)");
                auto gv = std::make_unique<ast::GlobalVarDecl>();
                gv->loc  = sa_loc;
                gv->name = std::string("__static_assert_")
                         + std::to_string(static_assert_counter_++);
                auto tn = std::make_unique<ast::PrimitiveTypeNode>();
                tn->prim = PrimitiveKind::VOID;
                gv->type = std::move(tn);
                gv->init = std::move(call_expr);
                return gv;
            }
        }

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
            // v4: si veniamos marcados como `comptime const` (caso tentativo
            // sin type-params) y resulto ser una funcion, promocionar a
            // comptime fn.
            if (is_comptime_const && !is_comptime_fn) {
                is_comptime_fn   = true;
                is_comptime_const = false;
            }
            auto fd = parse_function_decl(std::move(type_node), std::move(name), loc);
            // propagar @Async leida en el bucle de annotations.
            // El lowering trata estas funciones distinto: las envuelve en
            // future_alloc + spawn { msgrecv handle + body + fulfill } y
            // devuelve el handle del future al caller.
            if (fd && top_is_async)     fd->is_async = true;
            if (fd && is_comptime_fn)   fd->is_comptime = true;
            if (fd && top_is_macro)     fd->is_macro = true;
            if (fd && top_is_pure)      fd->is_pure  = true;
            if (fd && is_comptime_fn)   fd->type_params = std::move(comptime_type_params);
            // Registrar posiciones de params @c expr para que el parser sepa
            // hacer raw-text capture en los call sites de este @Macro.  Solo
            // se honra el flag cuando la funcion es @Macro: en otro contexto
            // produciria un mensaje de error en el type checker.
            if (fd && top_is_macro && !fd->params.empty()) {
                std::vector<int> positions;
                for (size_t i = 0; i < fd->params.size(); ++i) {
                    if (fd->params[i] && fd->params[i]->is_expr_capture) {
                        positions.push_back(static_cast<int>(i));
                    }
                }
                if (!positions.empty()) {
                    macro_expr_params_[fd->name] = std::move(positions);
                }
            }
            apply_pending_visibility(fd.get());
            return fd;
        }
        // `comptime T NAME = expr;` (forma canonica v4) -> comptime const
        // (inmutable, exportable, va al .vexi).
        // `comptime var T NAME = init;` -> comptime mutable (local al eval AST).
        // v4: si veniamos como `is_comptime_const` (set tentativamente cuando
        // se vio `comptime` sin `var`/`auto`/`<>`), forzamos @c is_const=true
        // porque comptime SIEMPRE implica const en var-decls top-level.
        if (is_comptime_const) {
            is_const = true;
        }
        auto gv = parse_global_var_decl(std::move(type_node), std::move(name), loc, is_const);
        if (gv && (is_comptime_const || is_comptime_var)) gv->is_comptime = true;
        if (gv) {
            // v4: propagar atributos de usuario (@hot, @cold, @align, @section).
            gv->attr_hot     = top_attr_hot;
            gv->attr_cold    = top_attr_cold;
            gv->attr_align   = top_attr_align;
            gv->attr_section = std::move(top_attr_section);
        }
        apply_pending_visibility(gv.get());
        return gv;
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
            // Cota dura para evitar loops infinitos por bugs en parse_param.
            // En la practica nunca se declara >256 params; 1024 es defensivo.
            constexpr size_t MAX_PARAMS = 1024;
            size_t           param_count = 0;
            while (true) {
                if (current_.kind == TokenKind::END_OF_FILE
                 || current_.kind == TokenKind::LBRACE
                 || current_.kind == TokenKind::RBRACE) {
                    error_here("se esperaba ')' antes de fin de archivo o '{'");
                    break;
                }
                if (++param_count > MAX_PARAMS) {
                    error_here("demasiados parametros (>1024); error de sintaxis no recuperable");
                    break;
                }
                auto p = parse_param();
                if (p) fn->params.push_back(std::move(p));
                if (!match(TokenKind::COMMA)) break;
            }
        }
        (void)expect(TokenKind::RPAREN, "se esperaba ')' al cerrar la lista de parametros");

        // Cuerpo: bloque obligatorio para funciones Vex; las funciones
        // sin cuerpo (FFI extern) se modelan via @c ExternFnDecl aparte.
        // Bug fix 2026-05-23: forward declaration `i32 fn(args);` -- si
        // vemos `;` en lugar de `{`, registramos la firma sin body.  El
        // type checker debe ver una definicion (con body) mas adelante o
        // reportar error de simbolo sin definicion.
        if (current_.kind == TokenKind::SEMICOLON) {
            (void)consume(); // ';'
            fn->body = nullptr;
            fn->is_forward_decl = true;
            return fn;
        }
        if (current_.kind != TokenKind::LBRACE) {
            error_here("se esperaba '{' para abrir el cuerpo de la funcion");
            return fn;
        }
        fn->body = parse_block();
        return fn;
    }

    std::unique_ptr<ast::ParamDecl> Parser::parse_param() {
        // Caso especial `expr name`: tipo contextual valido SOLO en params
        // de @Macro.  Lo materializamos como STRING para el body del macro;
        // la captura raw se hace en el call site (parse_postfix con
        // raw-text slicing).  Marcamos @c is_expr_capture para que el
        // registro posterior (en parse_function_decl) lo recolecte.
        if (current_.kind == TokenKind::IDENTIFIER && current_.lexeme == "expr") {
            auto p = std::make_unique<ast::ParamDecl>();
            p->loc = current_.loc;
            (void)consume();  // consume 'expr'
            auto tn = std::make_unique<ast::PrimitiveTypeNode>();
            tn->loc = p->loc;
            tn->prim = PrimitiveKind::STRING;
            p->type = std::move(tn);
            p->is_expr_capture = true;
            if (current_.kind != TokenKind::IDENTIFIER) {
                error_here("se esperaba un nombre tras el tipo 'expr' del parametro");
                return p;
            }
            p->name = consume().lexeme;
            return p;
        }
        // Z.6: aceptar `shared T name` en params (con disambiguation vs
        // `shared<T>` smart pointer).  Hoy es sugar documental; el type
        // system trata `T` y `shared T` como mismo tipo en parametros
        // (la sharing-ness vive en el HANDLE, no en el TIPO referenciado).
        // Util para que el autor de la libreria documente intent.
        if (current_.kind == TokenKind::KW_SHARED
         && lex_.peek_at(0).kind != TokenKind::LT) {
            (void)consume();  // 'shared' modificador documental
            // No registramos un flag en ParamDecl: la sharing-ness se
            // determina dinamicamente del bit 31 del handle en runtime.
        }
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
    // Cobertura actual: primitivos, punteros @c T*, arrays @c T[N] y
    // @c T[], @c VirtualPtr<T>, generics @c Cls<T1, T2>, tipos de
    // funcion @c fn(T) -> R, y @c nonnull T.  La rama del switch que
    // no matche cae al case por defecto, que reporta error claro.
    // ---------------------------------------------------------------------

    bool Parser::looks_like_cast() const noexcept {
        // Precondition: current_ es LPAREN.  Comprobamos si la
        // secuencia tras `(` forma un type-node valido seguido de `)`.
        // Para evitar conflictos con expresiones agrupadas, exigimos
        // que el PRIMER token sea un type-starter inequivoco:
        // primitivo (i32, u8, ...), VirtualPtr, fn, nonnull.  Tipos
        // nombrados via identifier (typedef sin marca clara) requieren
        // que el usuario escriba `(VirtualPtr<...>)x` o `(T*)x` para
        // desambiguar.
        Lexer &mut_lex = const_cast<Lexer &>(lex_);

        size_t off = 0;
        // Prefijo opcional `nonnull`.
        if (mut_lex.peek_at(off).kind == TokenKind::KW_NONNULL) ++off;

        const Token &first = mut_lex.peek_at(off);
        const TokenKind first_kind = first.kind;
        const bool is_type_starter =
                primitive_kind_from_token(first_kind) != PrimitiveKind::COUNT
                || first_kind == TokenKind::KW_FN
                || (first_kind == TokenKind::IDENTIFIER
                    && first.lexeme == "VirtualPtr")
                // Item 19: identifier declarado como typedef/using
                // tambien es un type-starter valido para casts.
                // Single-pass: el alias debe estar declarado antes
                // del cast en el archivo.
                || (first_kind == TokenKind::IDENTIFIER
                    && declared_aliases_.count(first.lexeme) > 0);
        if (!is_type_starter) return false;
        ++off;

        // Saltar argumentos genericos `<...>` con balance, tratando
        // `>>` (SHR) como dos GTs.
        if (mut_lex.peek_at(off).kind == TokenKind::LT) {
            int depth = 1;
            ++off;
            const size_t MAX_LOOKAHEAD = 64;
            while (depth > 0 && off < MAX_LOOKAHEAD) {
                TokenKind k = mut_lex.peek_at(off).kind;
                if (k == TokenKind::END_OF_FILE) return false;
                if (k == TokenKind::LT) ++depth;
                else if (k == TokenKind::GT) --depth;
                else if (k == TokenKind::SHR) depth -= 2;
                ++off;
            }
            if (depth != 0) return false;
        }
        // Saltar `*`s (punteros).
        while (mut_lex.peek_at(off).kind == TokenKind::STAR) ++off;
        // Saltar `[N]` o `[]` (arrays nativos).  Cierre exacto con `]`.
        while (mut_lex.peek_at(off).kind == TokenKind::LBRACKET) {
            int depth = 1;
            ++off;
            const size_t MAX = 64;
            while (depth > 0 && off < MAX) {
                TokenKind k = mut_lex.peek_at(off).kind;
                if (k == TokenKind::END_OF_FILE) return false;
                if (k == TokenKind::LBRACKET) ++depth;
                else if (k == TokenKind::RBRACKET) --depth;
                ++off;
            }
            if (depth != 0) return false;
        }
        // Tras todo, debe haber `)`.
        if (mut_lex.peek_at(off).kind != TokenKind::RPAREN) return false;
        // El siguiente token debe poder iniciar una expresion (de lo
        // contrario `(i32)` aislado seria ambiguo y la heuristica
        // genera un cast espureo).  Excluye operadores binarios (que
        // empezarian una expresion infix sin operando izquierdo).
        ++off;
        const TokenKind after = mut_lex.peek_at(off).kind;
        switch (after) {
            case TokenKind::IDENTIFIER:
            case TokenKind::INT_LIT:
            case TokenKind::FLOAT_LIT:
            case TokenKind::CHAR_LIT:
            case TokenKind::STRING_LIT:
            case TokenKind::RAW_STRING_LIT:
            case TokenKind::TRUE_KW:
            case TokenKind::FALSE_KW:
            case TokenKind::NULL_KW:
            case TokenKind::KW_THIS:
            case TokenKind::KW_NEW:
            case TokenKind::KW_AWAIT:
            case TokenKind::ISTR_BEGIN:
            case TokenKind::LPAREN:
            case TokenKind::AMP:
            case TokenKind::STAR:
            case TokenKind::PLUS:
            case TokenKind::MINUS:
            case TokenKind::BANG:
            case TokenKind::BANG_BANG:
            case TokenKind::TILDE:
            case TokenKind::PLUS_PLUS:
            case TokenKind::MINUS_MINUS:
                return true;
            default:
                return false;
        }
    }

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
        /* `auto NAME = init;` y `var NAME = init;` cuentan como
         * inicio de var-decl (con inferencia local de tipo).  `auto`/`var`
         * NO son keywords reservadas; solo se interpretan asi cuando van
         * seguidas inmediatamente por otro IDENTIFIER (el nombre). */
        if ((current_.lexeme == "auto" || current_.lexeme == "var")) {
            Lexer &mut_lex = const_cast<Lexer &>(lex_);
            // peek_at(0) es el SIGUIENTE token, no el actual.  El check es
            // "el token despues de `auto`/`var` debe ser IDENT (el nombre)".
            const Token &nx = mut_lex.peek_at(0);
            if (nx.kind == TokenKind::IDENTIFIER) {
                return true;
            }
        }

        Lexer &mut_lex = const_cast<Lexer &>(lex_);
        size_t off = 0;
        // Phase M.7.c: namespace qualified type `ui.Button name`.
        // Saltar pares `DOT IDENT` antes del check de generics.
        while (mut_lex.peek_at(off).kind == TokenKind::DOT
            && mut_lex.peek_at(off + 1).kind == TokenKind::IDENTIFIER) {
            off += 2;
        }
        // Optional: skip generic angle-brackets `<...>` con balance.
        // El lexer tokeniza `>>` como un solo SHR (mismo problema que en
        // C++ <17): aqui tratamos SHR como dos GTs cerrados a la vez.
        // Sin esto, `Cls<Inner<T>>` no se reconoce como tipo y el parser
        // bajaria a expr-stmt, fallando al ver el nombre de la variable.
        if (mut_lex.peek_at(off).kind == TokenKind::LT) {
            int depth = 1;
            ++off;
            const size_t MAX_LOOKAHEAD = 64;  // cota dura
            while (depth > 0 && off < MAX_LOOKAHEAD) {
                TokenKind k = mut_lex.peek_at(off).kind;
                if (k == TokenKind::END_OF_FILE) return false;
                if (k == TokenKind::LT) ++depth;
                else if (k == TokenKind::GT) --depth;
                else if (k == TokenKind::SHR) depth -= 2;
                ++off;
            }
            if (depth != 0) return false;
        }
        // Saltar `*`s.
        while (mut_lex.peek_at(off).kind == TokenKind::STAR) ++off;
        /* saltar `[N]` o `[]` -- postfix de arrays nativos.
         * Permite que `Point[3] arr = ...` y `Cls<T>[5] v = ...` se
         * reconozcan como type-decl. */
        const size_t MAX_LOOKAHEAD = 64;
        while (mut_lex.peek_at(off).kind == TokenKind::LBRACKET) {
            ++off;
            int b_depth = 1;
            size_t guard = 0;
            while (b_depth > 0 && off < MAX_LOOKAHEAD && guard++ < MAX_LOOKAHEAD) {
                TokenKind k = mut_lex.peek_at(off).kind;
                if (k == TokenKind::END_OF_FILE) return false;
                if (k == TokenKind::LBRACKET) ++b_depth;
                else if (k == TokenKind::RBRACKET) --b_depth;
                ++off;
            }
            if (b_depth != 0) return false;
            /* Permitir tambien `*` o mas `[]` tras el array. */
            while (mut_lex.peek_at(off).kind == TokenKind::STAR) ++off;
        }
        // aceptar `T !!name` (BANG_BANG entre tipo y nombre).
        if (mut_lex.peek_at(off).kind == TokenKind::BANG_BANG) ++off;
        const TokenKind nm = mut_lex.peek_at(off).kind;
        // El nombre del campo/metodo/var puede ser un IDENTIFIER o un
        // keyword contextual usado como nombre.  `get` y `set` son
        // contextuales (solo aplican a properties), asi que tambien pueden
        // ser nombres de metodo normales (ej. `HashMap.get`, `Queue.set`).
        // Sin esto, `public V get(K key)` no se reconocia como inicio de
        // metodo porque `get` no era IDENTIFIER.
        return nm == TokenKind::IDENTIFIER
            || nm == TokenKind::KW_GET
            || nm == TokenKind::KW_SET;
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
                // Solo aceptamos type args para tipos de coleccion o smart
                // pointers (unique<T> / shared<T>).
                const bool is_col = (k == PrimitiveKind::ARRAYLIST
                                  || k == PrimitiveKind::HASHMAP
                                  || k == PrimitiveKind::HASHSET
                                  || k == PrimitiveKind::QUEUE
                                  || k == PrimitiveKind::DEQUE
                                  || k == PrimitiveKind::TREEMAP
                                  || k == PrimitiveKind::TREESET
                                  || k == PrimitiveKind::STACK);
                const bool is_smart_ptr = (k == PrimitiveKind::UNIQUE_PTR
                                        || k == PrimitiveKind::SHARED_PTR);
                const bool is_borrow    = (k == PrimitiveKind::BORROW
                                        || k == PrimitiveKind::BORROW_MUT);
                if (is_col || is_smart_ptr || is_borrow) {
                    (void)consume(); // '<'
                    while (current_.kind != TokenKind::GT
                        && current_.kind != TokenKind::END_OF_FILE) {
                        auto ta = parse_type_node();
                        if (!ta) break;
                        pt->type_args.push_back(std::move(ta));
                        if (!match(TokenKind::COMMA)) break;
                    }
                    (void)expect_close_angle(
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
            (void)expect_close_angle("se esperaba '>' al cerrar VirtualPtr<...>");
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
            // Phase M.7.c: namespace qualified type (`ui.Button`).
            // Si seguidos vienen `.IDENT`, concatenamos al nombre con
            // separador `.` que el type checker traduce a mangled
            // `ui__Button` al resolver via imported_namespaces_.
            while (current_.kind == TokenKind::DOT
                && lex_.peek_at(0).kind == TokenKind::IDENTIFIER) {
                (void)consume(); // '.'
                nt->name += ".";
                nt->name += consume().lexeme;
            }
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
                (void)expect_close_angle(
                    "se esperaba '>' al cerrar argumentos de tipo");
            }
            base = std::move(nt);
        } else {
            error_here("se esperaba un tipo");
            // Sincronizacion: consumir el token de error para que el caller
            // no vuelva a procesarlo y entre en bucle infinito (caso clasico:
            // `fn name(p: i64)` con sintaxis Rust-style en params).  Sin
            // esto, parse_param/etc llaman a parse_type_node repetidamente
            // sobre el mismo token y el vector de params crece sin limite
            // -> RAM exhaustion.
            if (current_.kind != TokenKind::END_OF_FILE
             && current_.kind != TokenKind::RBRACE
             && current_.kind != TokenKind::RPAREN
             && current_.kind != TokenKind::SEMICOLON
             && current_.kind != TokenKind::COMMA) {
                (void)consume();
            }
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
        // Newtype + opaque + align + explicit from/to (extension 2026-05-23):
        //   typedef u64 ptr new;
        //   typedef u64 ptr new @opaque;
        //   typedef u8  v   new @align(16);            // SIMD alignment
        //   typedef u64 fd  new @opaque {              // bloque de conversiones
        //       explicit from u64;                     // cast (fd) raw -- privado al fichero
        //       public explicit to u64;                // cast (u64) f  -- cross-file ok
        //   }
        // El `new` reutiliza el keyword existente.  `@opaque` y `@align(N)`
        // se parsean como anotaciones contextuales en cualquier orden.
        if (current_.kind == TokenKind::KW_NEW) {
            (void)consume();
            a->is_newtype = true;
            // Loop sobre anotaciones contextuales @opaque / @align(N).
            while (current_.kind == TokenKind::AT) {
                (void)consume(); // '@'
                if (current_.kind != TokenKind::IDENTIFIER) {
                    error_here("se esperaba nombre de anotacion tras '@'");
                    break;
                }
                const std::string ann = current_.lexeme;
                (void)consume();
                if (ann == "opaque") {
                    a->is_opaque = true;
                } else if (ann == "align") {
                    (void)expect(TokenKind::LPAREN, "se esperaba '(' tras '@align'");
                    if (current_.kind != TokenKind::INT_LIT) {
                        error_here("@align(N) requiere un entero literal");
                        break;
                    }
                    const uint64_t n = current_.int_val;
                    (void)consume();
                    (void)expect(TokenKind::RPAREN, "se esperaba ')' tras N en @align(N)");
                    // Validar potencia de 2 en [1, 4096].
                    if (n == 0 || n > 4096 || (n & (n - 1)) != 0) {
                        error_here("@align(N): N debe ser potencia de 2 en [1, 4096]");
                    } else {
                        a->align_override = static_cast<uint16_t>(n);
                    }
                } else {
                    error_here(("anotacion desconocida tras 'new': '@"
                                + ann + "'; esperaba '@opaque' o '@align(N)'").c_str());
                    break;
                }
            }
            // Bloque opcional de conversiones explicit from/to.
            if (current_.kind == TokenKind::LBRACE) {
                (void)consume(); // '{'
                while (current_.kind != TokenKind::RBRACE
                    && current_.kind != TokenKind::END_OF_FILE) {
                    bool is_public = false;
                    if (current_.kind == TokenKind::KW_PUBLIC) {
                        (void)consume();
                        is_public = true;
                    }
                    if (current_.kind != TokenKind::IDENTIFIER
                     || current_.lexeme != "explicit") {
                        error_here("se esperaba 'explicit' [from|to] T;"
                                   " dentro del bloque de typedef");
                        break;
                    }
                    (void)consume(); // 'explicit'
                    if (current_.kind != TokenKind::IDENTIFIER
                     || (current_.lexeme != "from"
                       && current_.lexeme != "to")) {
                        error_here("se esperaba 'from' o 'to' tras 'explicit'");
                        break;
                    }
                    const bool is_from = (current_.lexeme == "from");
                    (void)consume(); // 'from'|'to'
                    if (!starts_type()) {
                        error_here("se esperaba un tipo tras 'from'/'to'");
                        break;
                    }
                    auto tn = parse_type_node();
                    (void)expect(TokenKind::SEMICOLON,
                        "se esperaba ';' al final de 'explicit from/to T'");
                    ast::TypeAliasDecl::ExplicitConv ec;
                    ec.type      = std::move(tn);
                    ec.is_public = is_public;
                    if (is_from) a->explicit_from.push_back(std::move(ec));
                    else         a->explicit_to.push_back(std::move(ec));
                }
                (void)expect(TokenKind::RBRACE, "se esperaba '}' tras el bloque del typedef");
                // Bloque sin punto y coma final: la sintaxis es similar
                // a class/struct.  Aceptar `}` solo sin ';' obligatorio.
                if (current_.kind == TokenKind::SEMICOLON) {
                    (void)consume();
                }
                // Item 19 + extension: registrar el alias antes del return
                declared_aliases_.insert(a->name);
                return a;
            }
        }
        (void)expect(TokenKind::SEMICOLON, "se esperaba ';' al final de 'typedef'");
        // Item 19: registrar alias para que `looks_like_cast` lo reconozca
        // en `(MyTypedef) x`.
        declared_aliases_.insert(a->name);
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
        declared_aliases_.insert(a->name);
        return a;
    }

    // -----------------------------------------------------------------
    // import: declaracion de importacion de modulo (Phase M).
    //
    // Sintaxis aceptada:
    //   import "path";
    //   import "path" as alias;
    //   import "path" only A, B;
    //   import "path" only A as A2, B;
    //   import "path" as alias only A, B;     <- alias para namespace y only
    //                                            para seleccion no se anyaden
    //                                            al mismo namespace; el alias
    //                                            queda inactivo si hay only
    //   public import "path" [as alias] [only ...];
    //
    // El path es siempre un string literal sin interpolacion.  Por
    // consistencia con extern "lib.dll", loadmodule(path), y @Method.
    // El sufijo .vex se anyade automaticamente al resolver.
    // -----------------------------------------------------------------
    std::unique_ptr<ast::ImportDecl> Parser::parse_import_decl(bool is_public_reexport) {
        auto im = std::make_unique<ast::ImportDecl>();
        im->loc                 = current_.loc;
        im->is_public_reexport  = is_public_reexport;

        (void)consume(); // 'import'

        // El path debe ser un literal string puro.  Strings interpolados
        // arrancan con ISTR_BEGIN (no STRING_LIT) -- el check con
        // STRING_LIT/RAW_STRING_LIT garantiza implicitamente que no hay
        // interpolacion en la ruta.
        if (current_.kind != TokenKind::STRING_LIT
         && current_.kind != TokenKind::RAW_STRING_LIT) {
            error_here("se esperaba un literal string como ruta del modulo, "
                       "e.g. import \"editor/buffer\";");
            return nullptr;
        }
        im->path = current_.str_val;
        (void)consume();

        // Opcional: as alias  (contextual 'as').
        if (current_.kind == TokenKind::IDENTIFIER
         && current_.lexeme == "as") {
            (void)consume(); // 'as'
            if (current_.kind != TokenKind::IDENTIFIER) {
                error_here("se esperaba un identificador para el alias tras 'as'");
                return nullptr;
            }
            im->alias = consume().lexeme;
        }

        // Opcional: only A [as A2], B [as B2], ...  (contextual 'only').
        if (current_.kind == TokenKind::IDENTIFIER
         && current_.lexeme == "only") {
            (void)consume(); // 'only'
            for (;;) {
                if (current_.kind != TokenKind::IDENTIFIER) {
                    error_here("se esperaba un identificador en la lista 'only'");
                    return nullptr;
                }
                ast::ImportDecl::OnlySymbol os;
                os.name = consume().lexeme;
                if (current_.kind == TokenKind::IDENTIFIER
                 && current_.lexeme == "as") {
                    (void)consume(); // 'as'
                    if (current_.kind != TokenKind::IDENTIFIER) {
                        error_here("se esperaba un identificador tras 'as' en 'only'");
                        return nullptr;
                    }
                    os.rename = consume().lexeme;
                }
                im->only_symbols.push_back(std::move(os));
                if (current_.kind == TokenKind::COMMA) {
                    (void)consume();
                    continue;
                }
                break;
            }
        }

        (void)expect(TokenKind::SEMICOLON, "se esperaba ';' al final de 'import'");
        return im;
    }

    // -----------------------------------------------------------------
    // namespace: agrupacion inline estilo C++ (Phase M.7.c).
    //
    //   namespace foo {
    //       class Button { ... }
    //       struct Point { ... }
    //       i32 helper() { ... }
    //   }
    //
    // Soporta anidamiento:
    //   namespace a { namespace b { class C {} } }
    //
    // El contenido se parsea con parse_top_level_decl recursivamente; el
    // pre-pass de mangling (compiler_project.cpp::mangle_top_level_)
    // recorrera el AST anyadiendo el prefijo `foo__` a todos los nombres.
    // -----------------------------------------------------------------
    std::unique_ptr<ast::NamespaceDecl> Parser::parse_namespace_decl() {
        auto ns = std::make_unique<ast::NamespaceDecl>();
        ns->loc = current_.loc;
        (void)consume(); // 'namespace'

        if (current_.kind != TokenKind::IDENTIFIER) {
            error_here("se esperaba el nombre del namespace tras 'namespace'");
            return nullptr;
        }
        ns->name = consume().lexeme;

        (void)expect(TokenKind::LBRACE, "se esperaba '{' tras el nombre del namespace");

        while (current_.kind != TokenKind::RBRACE
            && current_.kind != TokenKind::END_OF_FILE) {
            auto inner = parse_top_level_decl();
            if (!inner) {
                // Error de parse en una decl interna -- skipear hasta el
                // siguiente token aprovechable para no quedarnos en bucle.
                synchronize();
                continue;
            }
            ns->decls.push_back(std::move(inner));
        }
        (void)expect(TokenKind::RBRACE,
            "se esperaba '}' al final del namespace");
        return ns;
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

        // L2.3: generics opcionales `<T>`, `<K, V>` tras el nombre del enum.
        // Mismo patron que parse_class_decl: cada parametro es un identificador
        // simple; el enum se trata como plantilla y se monomorphiza en cada
        // uso `Maybe<i32>` en el type checker.
        if (current_.kind == TokenKind::LT) {
            (void)consume(); // '<'
            while (current_.kind == TokenKind::IDENTIFIER) {
                e->type_params.push_back(consume().lexeme);
                if (!match(TokenKind::COMMA)) break;
            }
            (void)expect_close_angle("se esperaba '>' al cerrar parametros de tipo");
        }

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
            // Bug fix 2026-05-23: match guards `case Pat if cond =>`.
            // Tras parsear el patron (con o sin bindings), aceptar `if expr`
            // opcional antes del `=>`.  La expr se guarda en @c arm.guard
            // y se evalua DESPUES del tag match en runtime.
            if (current_.kind == TokenKind::KW_IF) {
                (void)consume(); // 'if'
                arm.guard = parse_expr();
                if (!arm.guard) {
                    error_here("se esperaba una expresion para el guard del case");
                }
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

        // Generics: parametros de tipo opcionales `<T>`, `<K, V>`.
        // Cada parametro es un identificador simple; la clase se trata
        // como plantilla y no se procesa como clase concreta hasta que
        // se instancie via `Box<i32>`.
        if (current_.kind == TokenKind::LT) {
            (void)consume(); // '<'
            while (current_.kind == TokenKind::IDENTIFIER) {
                c->type_params.push_back(consume().lexeme);
                if (!match(TokenKind::COMMA)) break;
            }
            (void)expect_close_angle("se esperaba '>' al cerrar parametros de tipo");
        }

        // Superclase opcional via ':'.
        if (current_.kind == TokenKind::COLON) {
            (void)consume();
            if (current_.kind != TokenKind::IDENTIFIER) {
                error_here("se esperaba un nombre de superclase tras ':'");
                return nullptr;
            }
            c->super_name = consume().lexeme;
            // Despues de la superclase, una lista opcional de interfaces
            // separadas por coma: @c class X : Base, IFoo, IBar.  El type
            // checker valida que sean efectivamente interfaces.
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
                    // Bug fix 2026-05-23: @AfterReturning alias para @After
                    // que recibe el valor de retorno como ultimo arg.  El
                    // codigo runtime de AOP chain trata kind=2 (AFTER) igual
                    // que kind=4 (AFTER_RETURNING); el frontend baja un
                    // load extra del r0 del callee para el param result.
                    uint8_t this_kind = 0;
                    if      (aname == "Before")          this_kind = 1;
                    else if (aname == "After")           this_kind = 2;
                    else if (aname == "Around")          this_kind = 3;
                    else if (aname == "AfterReturning")  this_kind = 4;
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
            // `comptime if (cond) { ... }` -- el if se evalua
            // 100% en compile-time y solo la rama tomada se baja a IR.
            // Detectamos contextualmente para no reservar `comptime` como
            // keyword global (puede usarse como nombre de variable).
            if (current_.lexeme == "comptime"
             && mut_lex.peek_at(0).kind == TokenKind::KW_IF) {
                (void)consume();                   // 'comptime' (IDENT)
                auto s = parse_if_stmt();           // parsea como if normal
                if (s && s->kind == ast::NodeKind::IfStmt) {
                    static_cast<ast::IfStmt *>(s.get())->is_comptime = true;
                }
                return s;
            }
            // A.39: `comptime const T NAME = expr;` local.
            if (current_.lexeme == "comptime"
             && mut_lex.peek_at(0).kind == TokenKind::KW_CONST) {
                (void)consume();                   // 'comptime'
                (void)consume();                   // 'const'
                auto vd = parse_var_decl_stmt(true);
                if (vd && vd->kind == ast::NodeKind::VarDeclStmt) {
                    auto *v = static_cast<ast::VarDeclStmt *>(vd.get());
                    v->is_comptime = true;
                }
                return vd;
            }
            /* sugar local: `comptime NAME = expr;` ->
             * equivale a `comptime const auto NAME = expr;` con
             * inferencia.  Reduce el ruido en cadenas largas de
             * macros + concat de strings comptime. */
            if (current_.lexeme == "comptime"
             && mut_lex.peek_at(0).kind == TokenKind::IDENTIFIER
             && mut_lex.peek_at(0).lexeme != "var"
             && mut_lex.peek_at(1).kind == TokenKind::ASSIGN) {
                const SourceLoc sugar_loc = current_.loc;
                (void)consume();                   /* 'comptime' */
                auto vd = std::make_unique<ast::VarDeclStmt>();
                vd->loc         = sugar_loc;
                vd->name        = consume().lexeme;  /* NAME */
                vd->is_const    = true;
                vd->is_comptime = true;
                vd->infer_type  = true;
                vd->type        = nullptr;
                (void)expect(TokenKind::ASSIGN,
                             "se esperaba '=' tras 'comptime' + nombre");
                vd->init = parse_expr();
                (void)expect(TokenKind::SEMICOLON,
                             "se esperaba ';' al final de la decl comptime");
                return vd;
            }
            // `comptime var T NAME = expr;` local mutable.
            // Convencion: el modificador `var` (IDENT contextual) hace
            // explicito que la variable es comptime-mutable, paralelo a
            // `comptime const` para inmutable.
            // : `comptime auto X` aceptado como alias.  Tambien
            // soporta `comptime var/auto NAME = init;` (sin tipo) con
            // inferencia local.
            if (current_.lexeme == "comptime"
             && mut_lex.peek_at(0).kind == TokenKind::IDENTIFIER
             && (mut_lex.peek_at(0).lexeme == "var"
              || mut_lex.peek_at(0).lexeme == "auto")) {
                const SourceLoc sugar_loc = current_.loc;
                (void)consume();                   // 'comptime'
                (void)consume();                   // 'var' o 'auto'
                /* Modo inferencia: `comptime var NAME = init;` sin tipo. */
                if (current_.kind == TokenKind::IDENTIFIER
                 && mut_lex.peek_at(0).kind == TokenKind::ASSIGN) {
                    auto vd = std::make_unique<ast::VarDeclStmt>();
                    vd->loc         = sugar_loc;
                    vd->name        = consume().lexeme;
                    vd->is_const    = false;   /* mutable */
                    vd->is_comptime = true;
                    vd->infer_type  = true;
                    vd->type        = nullptr;
                    (void)expect(TokenKind::ASSIGN,
                                 "se esperaba '=' tras 'comptime var/auto' + nombre");
                    vd->init = parse_expr();
                    (void)expect(TokenKind::SEMICOLON,
                                 "se esperaba ';' al final de la decl comptime var");
                    return vd;
                }
                auto vd = parse_var_decl_stmt(false);
                if (vd && vd->kind == ast::NodeKind::VarDeclStmt) {
                    auto *v = static_cast<ast::VarDeclStmt *>(vd.get());
                    v->is_comptime = true;
                }
                return vd;
            }
            // `comptime { stmts }` bloque scope para comptime const +
            // comptime for + static_assert.  El bloque NO emite codigo
            // runtime.
            // sugar: dentro del bloque, `NAME = expr;` (sin
            // 'comptime const' explicito) cuenta como nueva decl
            // `comptime const NAME = expr;` con inferencia.  Reduce
            // ruido en macros donde cada decl repetia `comptime const`.
            if (current_.lexeme == "comptime"
             && mut_lex.peek_at(0).kind == TokenKind::LBRACE) {
                const SourceLoc loc = current_.loc;
                (void)consume();                   // 'comptime'
                (void)expect(TokenKind::LBRACE,
                             "se esperaba '{' tras 'comptime'");
                auto cb = std::make_unique<ast::ComptimeBlockStmt>();
                cb->loc = loc;
                while (current_.kind != TokenKind::RBRACE
                    && current_.kind != TokenKind::END_OF_FILE) {
                    /* sugar: detectar `NAME = expr;` directo
                     * antes del parse_statement normal.  Solo aplica
                     * cuando el siguiente al IDENT es ASSIGN -- otros
                     * IdentExpr (e.g., expr-stmt funcional) caen al
                     * parser normal. */
                    if (current_.kind == TokenKind::IDENTIFIER
                     && mut_lex.peek_at(0).kind == TokenKind::ASSIGN) {
                        const SourceLoc decl_loc = current_.loc;
                        auto vd = std::make_unique<ast::VarDeclStmt>();
                        vd->loc         = decl_loc;
                        vd->name        = consume().lexeme;
                        vd->is_const    = true;
                        vd->is_comptime = true;
                        vd->infer_type  = true;
                        vd->type        = nullptr;
                        (void)expect(TokenKind::ASSIGN,
                                     "se esperaba '=' tras nombre en comptime block");
                        vd->init = parse_expr();
                        (void)expect(TokenKind::SEMICOLON,
                                     "se esperaba ';' al final de la decl comptime");
                        cb->stmts.push_back(std::move(vd));
                        continue;
                    }
                    auto inner = parse_statement();
                    if (inner) cb->stmts.push_back(std::move(inner));
                    else synchronize();
                }
                (void)expect(TokenKind::RBRACE,
                             "se esperaba '}' al cerrar comptime block");
                return cb;
            }
            // `comptime for (i in lo..hi) { body }` unrolled.
            // Sintaxis: `comptime for (IDENT in EXPR `..` EXPR) { body }`
            // o `..=` para rango inclusivo.
            if (current_.lexeme == "comptime"
             && mut_lex.peek_at(0).kind == TokenKind::KW_FOR) {
                const SourceLoc loc = current_.loc;
                (void)consume();                   // 'comptime'
                (void)consume();                   // 'for'
                (void)expect(TokenKind::LPAREN,
                             "se esperaba '(' tras 'comptime for'");
                if (current_.kind != TokenKind::IDENTIFIER) {
                    error_here(
                        "se esperaba nombre del index tras 'comptime for ('");
                    synchronize();
                    return nullptr;
                }
                auto cf = std::make_unique<ast::ComptimeForStmt>();
                cf->loc      = loc;
                cf->var_name = consume().lexeme;
                if (current_.kind != TokenKind::KW_IN) {
                    error_here(
                        "se esperaba 'in' tras el nombre del index");
                } else {
                    (void)consume();
                }
                cf->lo_expr = parse_expr();
                /* `..` o `..=` para rango. */
                if (current_.kind == TokenKind::DOTDOT) {
                    (void)consume();
                    cf->inclusive = false;
                } else if (current_.kind == TokenKind::DOTDOTEQ) {
                    (void)consume();
                    cf->inclusive = true;
                } else {
                    error_here(
                        "se esperaba '..' o '..=' en el rango de comptime for");
                }
                cf->hi_expr = parse_expr();
                (void)expect(TokenKind::RPAREN,
                             "se esperaba ')' tras el rango de comptime for");
                cf->body = parse_statement();
                return cf;
            }
            // BugFix R6: `foreach (T x : col) body` como alias de
            // `for (T x : col) body`.  `foreach` no es keyword reservada;
            // se reconoce contextualmente solo cuando aparece en posicion
            // de statement seguido de `(`.  parse_for_stmt acepta ambos
            // tokens (KW_FOR o IDENT("foreach")) al inicio.
            if (current_.lexeme == "foreach"
             && mut_lex.peek_at(0).kind == TokenKind::LPAREN) {
                return parse_for_stmt();
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
                // Match como statement (destructuring de ADT como
                // sentencia de control de flujo).  El parser de
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
        /* Z.6: modificador `shared` en var-decl marca el storage class.
         * Disambiguacion con el smart pointer `shared<T>`: si tras `shared`
         * viene `<`, NO es modificador (es el tipo smart pointer); si
         * viene cualquier otro starter de tipo (identificador, keyword
         * primitivo, etc.), SI es modificador y consumimos.  El parser
         * de @c parse_type_node luego ve el tipo "limpio" sin shared. */
        if (current_.kind == TokenKind::KW_SHARED
         && lex_.peek_at(0).kind != TokenKind::LT) {
            (void)consume();                  /* descartar 'shared' modifier */
            vd->is_shared = true;
        }
        /* `auto NAME = init;` o `var NAME = init;` -- inferencia
         * local de tipo desde el init.  `auto`/`var` se reconocen como
         * IDENTIFIER contextual seguido de OTRO IDENTIFIER (el nombre);
         * asi NO los reservamos como keywords y codigo existente con
         * variables llamadas `auto`/`var` sigue funcionando salvo en
         * posicion de tipo en var-decl. */
        if (current_.kind == TokenKind::IDENTIFIER
         && (current_.lexeme == "auto" || current_.lexeme == "var")
         && lex_.peek_at(0).kind == TokenKind::IDENTIFIER) {
            (void)consume();          /* descartar 'auto' o 'var' */
            vd->type        = nullptr;
            vd->infer_type  = true;
        } else {
            vd->type     = parse_type_node();
        }
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
        // Sintaxis C-style: `T name[N]` -> wrappear el tipo base en
        // ArrayTypeNode(N).  Acepta tambien `T name[]` (sin tamano,
        // tipico de parametros con decay-to-ptr).  Cadena permitida
        // para matrices: `T name[N][M]`.
        //
        // Bug fix 2026-05-23: para matrices `T name[N][M][K]`, la dimension
        // MAS A LA IZQUIERDA es la EXTERIOR (igual que C).  Sea result =
        // T[N][M][K] significa: array de N de array de M de array de K de T.
        // El orden de los `[N]`, `[M]`, `[K]` en el wrap es:
        //   outer = N -> element = (array M de (array K de T)).
        // El wrap NAIVE (siguiente bracket envuelve al previo) invierte el
        // orden y produce T[K][M][N].  Coleccionamos los tamanyos en
        // vector y wrappeamos de DERECHA a IZQUIERDA.
        if (current_.kind == TokenKind::LBRACKET) {
            std::vector<std::pair<SourceLoc, std::unique_ptr<ast::Expr>>> dims;
            while (current_.kind == TokenKind::LBRACKET) {
                const SourceLoc abr_loc = current_.loc;
                (void)consume(); // '['
                std::unique_ptr<ast::Expr> sz;
                if (current_.kind != TokenKind::RBRACKET) {
                    sz = parse_expr();
                }
                (void)expect(TokenKind::RBRACKET,
                             "se esperaba ']' al cerrar el tamano del array");
                dims.emplace_back(abr_loc, std::move(sz));
            }
            // Wrap de derecha a izquierda: la ULTIMA dimension envuelve al
            // tipo base; cada dimension anterior envuelve la previa.  Asi
            // T[N][M][K] -> ArrayType(N, ArrayType(M, ArrayType(K, T))).
            for (auto it = dims.rbegin(); it != dims.rend(); ++it) {
                auto an = std::make_unique<ast::ArrayTypeNode>();
                an->loc          = it->first;
                an->element_type = std::move(vd->type);
                an->size_expr    = std::move(it->second);
                vd->type         = std::move(an);
            }
        }
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
        // Aceptar tanto KW_FOR como IDENT("foreach") contextual.  Ambos
        // delegan al mismo handler que detecta automaticamente la
        // sintaxis foreach (`T x : col`) vs counted-for (`init; cond; step`).
        (void)consume(); // 'for' o 'foreach'
        (void)expect(TokenKind::LPAREN, "se esperaba '(' tras 'for'/'foreach'");

        // aceptar `comptime const/var T NAME = expr` como init del for.
        // Esto permite usar el counter como comptime value dentro del body
        // si el resto del for esta en contexto comptime (e.g. dentro de
        // comptime fn body).  Sintaxis: `for (comptime var i64 i = 0; ...)`.
        // El parser detecta `comptime` aqui y construye un VarDeclStmt con
        // is_comptime=true; el resto del for se procesa normalmente.
        bool init_is_comptime = false;
        bool init_is_comptime_const = false;
        if (current_.kind == TokenKind::IDENTIFIER
         && current_.lexeme == "comptime") {
            Lexer &mut_lex = const_cast<Lexer &>(lex_);
            if (mut_lex.peek_at(0).kind == TokenKind::KW_CONST) {
                (void)consume(); /* comptime */
                (void)consume(); /* const */
                init_is_comptime = true;
                init_is_comptime_const = true;
            } else if (mut_lex.peek_at(0).kind == TokenKind::IDENTIFIER
                    && mut_lex.peek_at(0).lexeme == "var") {
                (void)consume(); /* comptime */
                (void)consume(); /* var */
                init_is_comptime = true;
            }
        }

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
                /* `for (comptime var/const T NAME = expr; ...)` */
                vd->is_comptime = init_is_comptime;
                vd->is_const    = init_is_comptime_const;
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
        // como una expresion ternaria y, si vemos un operador de
        // asignacion, recursamos para el lado derecho.
        auto lhs = parse_ternary();
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

    /**
     * @brief operador ternario `cond ? then : else`.
     *
     * Precedencia: entre assignment (mas baja) y logical_or (mas alta).
     * Asociatividad por la derecha (igual que C/C++/Java).  El COLON
     * separa las dos ramas; ambas se parsean como `parse_assignment`
     * para que se acepten asignaciones en las ramas:
     *   `flag ? x = 1 : x = 2`  -- legal aunque inusual
     *
     * Cuidado con `match`: el COLON tambien aparece en arms de match
     * y en labels.  Pero parse_ternary solo consume `?` -> cualquier
     * COLON sin `?` previo no afecta a este path.
     */
    std::unique_ptr<ast::Expr> Parser::parse_ternary() {
        auto cond = parse_logical_or();
        if (current_.kind != TokenKind::QUESTION) return cond;
        const SourceLoc loc = current_.loc;
        (void)consume();                                /* '?' */
        auto then_expr = parse_assignment();
        (void)expect(TokenKind::COLON,
                     "se esperaba ':' en la expresion ternaria");
        auto else_expr = parse_assignment();
        auto t = std::make_unique<ast::TernaryExpr>();
        t->loc       = loc;
        t->cond      = std::move(cond);
        t->then_expr = std::move(then_expr);
        t->else_expr = std::move(else_expr);
        return t;
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
        // Cast C-style `(T) expr`.  Comprobamos antes de los demas
        // unarios porque el cast tambien empieza con `(` y queremos
        // reconocerlo antes de caer al patron `(expr)`.
        if (current_.kind == TokenKind::LPAREN && looks_like_cast()) {
            const SourceLoc loc = current_.loc;
            (void)consume(); // '('
            auto type_node = parse_type_node();
            (void)expect(TokenKind::RPAREN, "se esperaba ')' al cerrar el cast");
            auto operand = parse_unary();
            auto ce = std::make_unique<ast::CastExpr>();
            ce->loc          = loc;
            ce->target_type  = std::move(type_node);
            ce->operand      = std::move(operand);
            return ce;
        }
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
        // Operadores postfix soportados: x++, x--, x(args), x[i],
        // x.field, x?.field, x?.[i], x?
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

                case TokenKind::QUESTION: {
                    // P2: operador postfix `?` para Result -- early-return.
                    // Disambiguacion con el ternario `cond ? a : b`:
                    //   - ternario: '?' seguido de algo que empieza una
                    //     expresion (ident, literal, '(', '!', '-', etc.).
                    //   - postfix-?: '?' seguido de un token que no puede
                    //     empezar una expresion (';', ',', ')', ']', '}',
                    //     binop, etc.).
                    // Si NO es postfix-?, retornamos sin consumir el '?'
                    // para que parse_ternary lo procese arriba.
                    Lexer &mut_lex = const_cast<Lexer &>(lex_);
                    // peek_at(0) es el token DESPUES del actual (current_ ya
                    // contiene el `?`, asi que peek_at(0) es lo que viene
                    // tras el `?`).
                    const Token &next = mut_lex.peek_at(0);
                    bool is_postfix_q = false;
                    switch (next.kind) {
                        case TokenKind::SEMICOLON:
                        case TokenKind::COMMA:
                        case TokenKind::RPAREN:
                        case TokenKind::RBRACKET:
                        case TokenKind::RBRACE:
                        // binops + asignacion + comparacion + dot (chain)
                        case TokenKind::PLUS:
                        case TokenKind::STAR:
                        case TokenKind::SLASH:
                        case TokenKind::PERCENT:
                        case TokenKind::AMP:
                        case TokenKind::PIPE:
                        case TokenKind::CARET:
                        case TokenKind::AND_AND:
                        case TokenKind::OR_OR:
                        case TokenKind::SHL:
                        case TokenKind::SHR:
                        case TokenKind::ASSIGN:
                        case TokenKind::EQ:
                        case TokenKind::NEQ:
                        case TokenKind::LE:
                        case TokenKind::GE:
                        case TokenKind::PLUS_ASSIGN:
                        case TokenKind::MINUS_ASSIGN:
                        case TokenKind::STAR_ASSIGN:
                        case TokenKind::SLASH_ASSIGN:
                        case TokenKind::DOT:
                        case TokenKind::QUESTION:    // chain a?b? -> primero postfix
                        case TokenKind::END_OF_FILE:
                            is_postfix_q = true;
                            break;
                        default:
                            // MINUS y LT/GT son ambiguos pero por defecto
                            // se quedan como ternario (mas comun el ternario).
                            // Si el usuario quiere postfix-? en estos casos,
                            // puede agrupar: (expr?) + x.
                            is_postfix_q = false;
                            break;
                    }
                    if (!is_postfix_q) {
                        return expr;  // dejar el '?' para parse_ternary
                    }
                    const SourceLoc loc = current_.loc;
                    (void)consume();  // consume '?'
                    auto t = std::make_unique<ast::TryExpr>();
                    t->loc     = loc;
                    t->operand = std::move(expr);
                    expr = std::move(t);
                    break;
                }
                case TokenKind::LT: {
                    /* Soporte para builtins comptime con type args:
                     * @c sizeof<T>(), @c offsetof<T>("field"), etc.
                     * Solo se activa cuando @c expr es un IdentExpr cuyo
                     * nombre matchea un builtin comptime conocido.  Sin
                     * esta restriccion, @c LT en posicion postfix seria
                     * ambiguo con operadores de comparacion (@c foo < bar).
                     * Si el nombre no es builtin comptime, salimos del
                     * switch para que el LT lo procese binary_expr. */
                    if (expr->kind != ast::NodeKind::IdentExpr) {
                        return expr;
                    }
                    const auto *id_chk = static_cast<const ast::IdentExpr *>(expr.get());
                    /* para nombres no-builtin, hacemos un lookahead
                     * permisivo: si el patron es `name<TYPE_STUFF>(` lo
                     * tratamos como type-args (probable comptime fn
                     * generica).  Esto evita reservar nombres conocidos
                     * y permite que usuarios definan sus propias
                     * generic comptime fns sin tocar la whitelist. */
                    if (!is_comptime_builtin_name(id_chk->name)) {
                        /* Lookahead: tras `<` esperamos un token que
                         * arranque un tipo y eventualmente `>` + `(`.
                         * Si no encontramos `(` tras el `>` (o `>>`) en
                         * un rango razonable, fallback a binary `<`. */
                        Lexer &mut_lex = const_cast<Lexer &>(lex_);
                        bool looks_like_type_args = false;
                        int depth = 0;
                        for (size_t k = 0; k < 32; ++k) {
                            const auto &tk = mut_lex.peek_at(k);
                            if (tk.kind == TokenKind::LT) { depth++; continue; }
                            if (tk.kind == TokenKind::GT) {
                                if (depth == 0) {
                                    const auto &after = mut_lex.peek_at(k + 1);
                                    looks_like_type_args =
                                        (after.kind == TokenKind::LPAREN);
                                    break;
                                }
                                depth--; continue;
                            }
                            if (tk.kind == TokenKind::SHR) {
                                if (depth <= 1) {
                                    const auto &after = mut_lex.peek_at(k + 1);
                                    looks_like_type_args =
                                        (after.kind == TokenKind::LPAREN);
                                    break;
                                }
                                depth -= 2; continue;
                            }
                            if (tk.kind == TokenKind::SEMICOLON
                             || tk.kind == TokenKind::LBRACE
                             || tk.kind == TokenKind::RBRACE
                             || tk.kind == TokenKind::END_OF_FILE) break;
                        }
                        if (!looks_like_type_args) {
                            return expr;
                        }
                    }
                    const SourceLoc loc = current_.loc;
                    (void)consume();  /* '<' */
                    std::vector<std::unique_ptr<ast::TypeNode>> tas;
                    while (current_.kind != TokenKind::GT
                        && current_.kind != TokenKind::SHR
                        && current_.kind != TokenKind::END_OF_FILE) {
                        auto ta = parse_type_node();
                        if (!ta) break;
                        tas.push_back(std::move(ta));
                        if (!match(TokenKind::COMMA)) break;
                    }
                    (void)expect_close_angle(
                        "se esperaba '>' al cerrar type args de builtin comptime");
                    /* Tras los type args DEBE venir '(' para los args runtime
                     * del builtin (incluso si es ()). */
                    if (current_.kind != TokenKind::LPAREN) {
                        error_here("se esperaba '(' tras type args de builtin comptime");
                        break;
                    }
                    (void)consume();  /* '(' */
                    auto call = std::make_unique<ast::CallExpr>();
                    call->loc    = loc;
                    call->callee = std::move(expr);
                    call->type_args = std::move(tas);
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
                case TokenKind::LPAREN: {
                    const SourceLoc loc = current_.loc;
                    (void)consume();
                    auto call = std::make_unique<ast::CallExpr>();
                    call->loc    = loc;
                    /* Detectar @Macro con params @c expr: si el callee es un
                     * IdentExpr cuyo nombre esta en @c macro_expr_params_,
                     * para cada arg en posicion marcada hacemos raw-text
                     * capture en lugar de parsear como expresion.  Las demas
                     * posiciones siguen el flujo normal. */
                    const std::vector<int> *expr_positions = nullptr;
                    if (expr && expr->kind == ast::NodeKind::IdentExpr) {
                        const auto *id = static_cast<const ast::IdentExpr *>(expr.get());
                        auto it = macro_expr_params_.find(id->name);
                        if (it != macro_expr_params_.end()) {
                            expr_positions = &it->second;
                        }
                    }
                    call->callee = std::move(expr);
                    if (current_.kind != TokenKind::RPAREN) {
                        int arg_idx = 0;
                        while (true) {
                            bool is_raw = false;
                            if (expr_positions) {
                                for (int pos : *expr_positions) {
                                    if (pos == arg_idx) { is_raw = true; break; }
                                }
                            }
                            if (is_raw) {
                                /* Raw-text capture: leer source desde el
                                 * offset del token actual hasta el siguiente
                                 * COMMA o RPAREN a depth 0.  El lexer ya tiene
                                 * el offset absoluto en current_.loc.offset. */
                                const std::string &src = lex_.source_buffer();
                                const uint32_t start_off = current_.loc.offset;
                                int paren_depth = 0;
                                int brack_depth = 0;
                                int brace_depth = 0;
                                uint32_t end_off = start_off;
                                /* Consumir tokens hasta el siguiente COMMA
                                 * o RPAREN a depth 0.  Tracking paren/bracket/
                                 * brace para no cortar dentro de subexprs. */
                                while (current_.kind != TokenKind::END_OF_FILE) {
                                    if (paren_depth == 0 && brack_depth == 0
                                        && brace_depth == 0) {
                                        if (current_.kind == TokenKind::COMMA
                                            || current_.kind == TokenKind::RPAREN) {
                                            end_off = current_.loc.offset;
                                            break;
                                        }
                                    }
                                    switch (current_.kind) {
                                        case TokenKind::LPAREN:   ++paren_depth; break;
                                        case TokenKind::RPAREN:   --paren_depth; break;
                                        case TokenKind::LBRACKET: ++brack_depth; break;
                                        case TokenKind::RBRACKET: --brack_depth; break;
                                        case TokenKind::LBRACE:   ++brace_depth; break;
                                        case TokenKind::RBRACE:   --brace_depth; break;
                                        default: break;
                                    }
                                    (void)consume();
                                }
                                if (end_off == start_off) {
                                    /* No avanzamos: arg vacio.  Construimos
                                     * un StringLit vacio. */
                                    end_off = start_off;
                                }
                                /* Trim trailing whitespace del span. */
                                while (end_off > start_off
                                    && end_off <= src.size()
                                    && (src[end_off - 1] == ' '
                                     || src[end_off - 1] == '\t'
                                     || src[end_off - 1] == '\n'
                                     || src[end_off - 1] == '\r')) {
                                    --end_off;
                                }
                                std::string captured;
                                if (start_off < src.size() && end_off >= start_off
                                    && end_off <= src.size()) {
                                    captured = src.substr(start_off, end_off - start_off);
                                }
                                auto slit = std::make_unique<ast::StringLitExpr>();
                                slit->loc = loc;
                                slit->value = std::move(captured);
                                call->args.push_back(std::move(slit));
                            } else {
                                auto arg = parse_expr();
                                if (arg) call->args.push_back(std::move(arg));
                            }
                            ++arg_idx;
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
                    // Subscript: base[index].  El type checker restringe la
                    // base a tipo puntero o array (operacion de indexacion).
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

                        // Formato opcional `${expr:fmt}` (lexer emite
                        // ISTR_EXPR_FMT con la cadena de formato en
                        // str_val).  Capturar y guardar; si no existe,
                        // insertar string vacio para mantener
                        // interp_formats[i] paralelo a interp_exprs[i].
                        std::string fmt;
                        if (current_.kind == TokenKind::ISTR_EXPR_FMT) {
                            fmt = std::move(current_.str_val);
                            (void)consume();
                        }
                        e->interp_formats.push_back(std::move(fmt));

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
            case TokenKind::KW_SUPER: {
                // BugFix R1: `super(args)` para delegacion al ctor de la
                // superclase, o `super.method(args)` para llamar al metodo
                // del super sin dispatch virtual.
                (void)consume();  // 'super'
                if (current_.kind == TokenKind::LPAREN) {
                    // super(args) -- delegacion ctor
                    auto e = std::make_unique<ast::SuperCallExpr>();
                    e->loc = loc;
                    (void)consume();  // '('
                    while (current_.kind != TokenKind::RPAREN
                        && current_.kind != TokenKind::END_OF_FILE) {
                        auto arg = parse_expr();
                        if (arg) e->args.push_back(std::move(arg));
                        if (!match(TokenKind::COMMA)) break;
                    }
                    (void)expect(TokenKind::RPAREN, "se esperaba ')' tras args de super(...)");
                    return e;
                }
                if (current_.kind == TokenKind::DOT) {
                    // super.method(args)
                    (void)consume();  // '.'
                    if (current_.kind != TokenKind::IDENTIFIER) {
                        error_here("se esperaba nombre de metodo tras 'super.'");
                        return nullptr;
                    }
                    auto e = std::make_unique<ast::SuperMethodCallExpr>();
                    e->loc         = loc;
                    e->method_name = consume().lexeme;
                    if (current_.kind != TokenKind::LPAREN) {
                        error_here("se esperaba '(' tras 'super.<metodo>'");
                        return nullptr;
                    }
                    (void)consume();  // '('
                    while (current_.kind != TokenKind::RPAREN
                        && current_.kind != TokenKind::END_OF_FILE) {
                        auto arg = parse_expr();
                        if (arg) e->args.push_back(std::move(arg));
                        if (!match(TokenKind::COMMA)) break;
                    }
                    (void)expect(TokenKind::RPAREN, "se esperaba ')' tras args de super.method(...)");
                    return e;
                }
                error_here("'super' debe ir seguido de '(' (delegacion ctor) o '.metodo(' (llamada no-virtual)");
                return nullptr;
            }
            case TokenKind::KW_NEW: {
                // Creacion de instancia: 'new' <ClassName> '(' args... ')'.
                // bug4: tambien 'new T[N]' donde T puede ser primitivo
                // (i32/i64/string/bool/etc) o nombre de clase/enum/struct.
                (void)consume();
                auto e = std::make_unique<ast::NewExpr>();
                e->loc        = loc;
                // Aceptar IDENTIFIER (clases, enums, structs, typedefs) o
                // primitive_kind_from_token (i32, i64, string, bool, f32...).
                if (current_.kind == TokenKind::IDENTIFIER) {
                    e->class_name = consume().lexeme;
                    // Phase M.7.c: namespace qualified `new ui.Button(...)`.
                    // Concatenamos con `.` igual que en parse_type_node;
                    // el TypeChecker traduce a mangled label.
                    while (current_.kind == TokenKind::DOT
                        && lex_.peek_at(0).kind == TokenKind::IDENTIFIER) {
                        (void)consume(); // '.'
                        e->class_name += ".";
                        e->class_name += consume().lexeme;
                    }
                } else if (primitive_kind_from_token(current_.kind) != PrimitiveKind::COUNT) {
                    // Captura lexeme del primitivo para que el lowering
                    // pueda mapearlo a IrType correcto.
                    e->class_name = consume().lexeme;
                } else {
                    error_here("se esperaba un nombre de tipo tras 'new'");
                    return nullptr;
                }
                // Argumentos de tipo opcionales `<T1, T2, ...>` para
                // instanciaciones genericas: @c new Box<i32>(42).
                if (current_.kind == TokenKind::LT) {
                    (void)consume(); // '<'
                    while (current_.kind != TokenKind::GT
                        && current_.kind != TokenKind::END_OF_FILE) {
                        auto ta = parse_type_node();
                        if (!ta) break;
                        e->type_args.push_back(std::move(ta));
                        if (!match(TokenKind::COMMA)) break;
                    }
                    (void)expect_close_angle(
                        "se esperaba '>' al cerrar argumentos de tipo en new");
                }
                // bug4: array allocation `new T[N]`.  En lugar de `(args)`,
                // si vemos `[<expr>]` parseamos como array_size.  Resultado
                // semantico: aloca N * sizeof(T) bytes en host heap y
                // devuelve un host_ptr al primer elemento (decay-to-T*).
                if (current_.kind == TokenKind::LBRACKET) {
                    (void)consume(); // '['
                    e->array_size = parse_expr();
                    if (!e->array_size) {
                        error_here("se esperaba expresion del tamano del array en 'new T[N]'");
                    }
                    (void)expect(TokenKind::RBRACKET,
                        "se esperaba ']' al cerrar el tamano del array en 'new T[N]'");
                    return e;
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
