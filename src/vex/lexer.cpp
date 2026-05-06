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
 * @file lexer.cpp
 * @brief Implementacion del tokenizador del lenguaje Vex.
 *
 * Optimizaciones aplicadas:
 *  - classify_identifier() despacha por primera letra (switch sobre char,
 *    convertido a jump table por el compilador) y luego por longitud
 *    exacta + memcmp.  No hay hashing, no hay std::unordered_map; la
 *    ruta caliente es totalmente predecible para el branch predictor.
 *  - Los lexemas se construyen con un substring sobre el buffer source_
 *    (un memcpy lineal), nunca char-a-char con push_back.
 *  - skip_trivia() usa loops estrechos sin asignaciones; los comentarios
 *    se descartan in-place avanzando el cursor.
 */

#include "vex/lexer.h"

#include <cctype>
#include <cstring>
#include <string>

namespace vex {

    // -----------------------------------------------------------------------
    // Tabla de nombres de TokenKind para diagnosticos.
    //
    // Es una funcion en lugar de un array para que la imagen del lexer no
    // crezca con datos solo usados al imprimir errores.  Si en el futuro
    // se llama mucho desde error paths, convertirlo a array constexpr
    // indexado por (uint16_t)k.
    // -----------------------------------------------------------------------
    const char *token_kind_name(TokenKind k) noexcept {
        switch (k) {
            case TokenKind::END_OF_FILE:     return "END_OF_FILE";
            case TokenKind::UNKNOWN:         return "UNKNOWN";
            case TokenKind::INT_LIT:         return "INT_LIT";
            case TokenKind::FLOAT_LIT:       return "FLOAT_LIT";
            case TokenKind::CHAR_LIT:        return "CHAR_LIT";
            case TokenKind::STRING_LIT:      return "STRING_LIT";
            case TokenKind::RAW_STRING_LIT:  return "RAW_STRING_LIT";
            case TokenKind::TRUE_KW:         return "true";
            case TokenKind::FALSE_KW:        return "false";
            case TokenKind::NULL_KW:         return "null";
            case TokenKind::ISTR_BEGIN:      return "ISTR_BEGIN";
            case TokenKind::ISTR_TEXT:       return "ISTR_TEXT";
            case TokenKind::ISTR_EXPR_BEGIN: return "ISTR_EXPR_BEGIN";
            case TokenKind::ISTR_EXPR_END:   return "ISTR_EXPR_END";
            case TokenKind::ISTR_END:        return "ISTR_END";
            case TokenKind::IDENTIFIER:      return "IDENTIFIER";
            case TokenKind::KW_VOID:         return "void";
            case TokenKind::KW_BOOL:         return "bool";
            case TokenKind::KW_CHAR:         return "char";
            case TokenKind::KW_INT8:         return "i8";
            case TokenKind::KW_INT16:        return "i16";
            case TokenKind::KW_INT32:        return "i32";
            case TokenKind::KW_INT64:        return "i64";
            case TokenKind::KW_UINT8:        return "u8";
            case TokenKind::KW_UINT16:       return "u16";
            case TokenKind::KW_UINT32:       return "u32";
            case TokenKind::KW_UINT64:       return "u64";
            case TokenKind::KW_INT8_T:       return "int8_t";
            case TokenKind::KW_INT16_T:      return "int16_t";
            case TokenKind::KW_INT32_T:      return "int32_t";
            case TokenKind::KW_INT64_T:      return "int64_t";
            case TokenKind::KW_UINT8_T:      return "uint8_t";
            case TokenKind::KW_UINT16_T:     return "uint16_t";
            case TokenKind::KW_UINT32_T:     return "uint32_t";
            case TokenKind::KW_UINT64_T:     return "uint64_t";
            case TokenKind::KW_F32:          return "f32";
            case TokenKind::KW_F64:          return "f64";
            case TokenKind::KW_FLOAT:        return "float";
            case TokenKind::KW_DOUBLE:       return "double";
            case TokenKind::KW_STRING:       return "string";
            case TokenKind::KW_ARRAYLIST:    return "ArrayList";
            case TokenKind::KW_HASHMAP:      return "HashMap";
            case TokenKind::KW_HASHSET:      return "HashSet";
            case TokenKind::KW_QUEUE:        return "Queue";
            case TokenKind::KW_DEQUE:        return "Deque";
            case TokenKind::KW_TREEMAP:      return "TreeMap";
            case TokenKind::KW_TREESET:      return "TreeSet";
            case TokenKind::KW_STACK:        return "Stack";
            case TokenKind::KW_CONST:        return "const";
            case TokenKind::KW_STATIC:       return "static";
            case TokenKind::KW_FINAL:        return "final";
            case TokenKind::KW_NONNULL:      return "nonnull";
            case TokenKind::KW_TYPEDEF:      return "typedef";
            case TokenKind::KW_USING:        return "using";
            case TokenKind::KW_STRUCT:       return "struct";
            case TokenKind::KW_CLASS:        return "class";
            case TokenKind::KW_INTERFACE:    return "interface";
            case TokenKind::KW_ENUM:         return "enum";
            case TokenKind::KW_FN:           return "fn";
            case TokenKind::KW_PUBLIC:       return "public";
            case TokenKind::KW_PRIVATE:      return "private";
            case TokenKind::KW_PROTECTED:    return "protected";
            case TokenKind::KW_IMPORT:       return "import";
            case TokenKind::KW_EXTERN:       return "extern";
            case TokenKind::KW_GET:          return "get";
            case TokenKind::KW_SET:          return "set";
            case TokenKind::KW_IF:           return "if";
            case TokenKind::KW_ELSE:         return "else";
            case TokenKind::KW_WHILE:        return "while";
            case TokenKind::KW_DO:           return "do";
            case TokenKind::KW_FOR:          return "for";
            case TokenKind::KW_IN:           return "in";
            case TokenKind::KW_BREAK:        return "break";
            case TokenKind::KW_CONTINUE:     return "continue";
            case TokenKind::KW_GOTO:         return "goto";
            case TokenKind::KW_RETURN:       return "return";
            case TokenKind::KW_TRY:          return "try";
            case TokenKind::KW_CATCH:        return "catch";
            case TokenKind::KW_FINALLY:      return "finally";
            case TokenKind::KW_THROW:        return "throw";
            case TokenKind::KW_NEW:          return "new";
            case TokenKind::KW_DELETE:       return "delete";
            case TokenKind::KW_THIS:         return "this";
            case TokenKind::KW_SUPER:        return "super";
            case TokenKind::KW_SYNCHRONIZED: return "synchronized";
            case TokenKind::KW_MONITOR:      return "monitor";
            case TokenKind::KW_AWAIT:        return "await";
            case TokenKind::KW_SPAWN:        return "spawn";
            case TokenKind::KW_RSPAWN:       return "rspawn";
            case TokenKind::KW_ASM:          return "asm";
            case TokenKind::KW_OVERRIDE:     return "override";
            case TokenKind::KW_MATCH:        return "match";
            case TokenKind::KW_CASE:         return "case";
            case TokenKind::PLUS:            return "+";
            case TokenKind::MINUS:           return "-";
            case TokenKind::STAR:            return "*";
            case TokenKind::SLASH:           return "/";
            case TokenKind::PERCENT:         return "%";
            case TokenKind::ASSIGN:          return "=";
            case TokenKind::PLUS_ASSIGN:     return "+=";
            case TokenKind::MINUS_ASSIGN:    return "-=";
            case TokenKind::STAR_ASSIGN:     return "*=";
            case TokenKind::SLASH_ASSIGN:    return "/=";
            case TokenKind::PERCENT_ASSIGN:  return "%=";
            case TokenKind::AMP_ASSIGN:      return "&=";
            case TokenKind::PIPE_ASSIGN:     return "|=";
            case TokenKind::CARET_ASSIGN:    return "^=";
            case TokenKind::SHL_ASSIGN:      return "<<=";
            case TokenKind::SHR_ASSIGN:      return ">>=";
            case TokenKind::EQ:              return "==";
            case TokenKind::NEQ:             return "!=";
            case TokenKind::LT:              return "<";
            case TokenKind::LE:              return "<=";
            case TokenKind::GT:              return ">";
            case TokenKind::GE:              return ">=";
            case TokenKind::AND_AND:         return "&&";
            case TokenKind::OR_OR:           return "||";
            case TokenKind::BANG:            return "!";
            case TokenKind::BANG_BANG:       return "!!";
            case TokenKind::AMP:             return "&";
            case TokenKind::PIPE:            return "|";
            case TokenKind::CARET:           return "^";
            case TokenKind::TILDE:           return "~";
            case TokenKind::SHL:             return "<<";
            case TokenKind::SHR:             return ">>";
            case TokenKind::PLUS_PLUS:       return "++";
            case TokenKind::MINUS_MINUS:     return "--";
            case TokenKind::LPAREN:          return "(";
            case TokenKind::RPAREN:          return ")";
            case TokenKind::LBRACE:          return "{";
            case TokenKind::RBRACE:          return "}";
            case TokenKind::LBRACKET:        return "[";
            case TokenKind::RBRACKET:        return "]";
            case TokenKind::COMMA:           return ",";
            case TokenKind::SEMICOLON:       return ";";
            case TokenKind::COLON:           return ":";
            case TokenKind::DOT:             return ".";
            case TokenKind::ARROW:           return "->";
            case TokenKind::FAT_ARROW:       return "=>";
            case TokenKind::QUESTION:        return "?";
            case TokenKind::AT:              return "@";
            case TokenKind::TOKEN_KIND_COUNT:return "<count>";
        }
        // Defensa: si llega un valor fuera de rango, devolver algo no-null.
        return "<unknown>";
    }

    // -----------------------------------------------------------------------
    // classify_identifier
    //
    // Despacha por primera letra del lexema (jump table en el compilador) y
    // luego por longitud exacta + memcmp del resto.  Esto es ~3-5x mas rapido
    // que std::unordered_map para palabras reservadas y ademas evita el
    // overhead de construccion de la tabla en cada Lexer creado.
    //
    // Macros para legibilidad:
    //   KW_AT(N, str, kind)  -> compara los siguientes N bytes en lex+1
    //                            con la cadena literal y devuelve kind si
    //                            la longitud y los bytes coinciden.
    // -----------------------------------------------------------------------

    // Comprobar coincidencia exacta del lexema con una cadena literal estatica.
    // sizeof(LIT) - 1 elimina el nul terminador del literal (constexpr).
    #define VEX_KW_EXACT(LIT, KIND)                                         \
        do {                                                                \
            if (n == sizeof(LIT) - 1 && std::memcmp(p, LIT, n) == 0)        \
                return (KIND);                                              \
        } while (0)

    TokenKind classify_identifier(const std::string &lexeme) noexcept {
        // Datos del lexema en una sola lectura local; el compilador los
        // mantiene en registros durante todo el dispatch.
        const char  *p = lexeme.data();
        const size_t n = lexeme.size();
        if (n == 0) return TokenKind::IDENTIFIER;

        // Switch sobre primera letra: jump table en GCC/Clang/MSVC.
        switch (p[0]) {
            // tipos primitivos de coleccion (CamelCase para coincidir
            // con la convencion de "tipo importado" comun en otros lenguajes).
            case 'A':
                VEX_KW_EXACT("ArrayList", TokenKind::KW_ARRAYLIST);
                break;
            case 'D':
                VEX_KW_EXACT("Deque", TokenKind::KW_DEQUE);
                break;
            case 'H':
                VEX_KW_EXACT("HashMap", TokenKind::KW_HASHMAP);
                VEX_KW_EXACT("HashSet", TokenKind::KW_HASHSET);
                break;
            case 'Q':
                VEX_KW_EXACT("Queue", TokenKind::KW_QUEUE);
                break;
            case 'S':
                VEX_KW_EXACT("Stack", TokenKind::KW_STACK);
                break;
            case 'T':
                VEX_KW_EXACT("TreeMap", TokenKind::KW_TREEMAP);
                VEX_KW_EXACT("TreeSet", TokenKind::KW_TREESET);
                break;
            case 'a':
                VEX_KW_EXACT("await", TokenKind::KW_AWAIT);
                VEX_KW_EXACT("asm",   TokenKind::KW_ASM);
                break;
            case 'b':
                VEX_KW_EXACT("bool",  TokenKind::KW_BOOL);
                VEX_KW_EXACT("break", TokenKind::KW_BREAK);
                break;
            case 'c':
                VEX_KW_EXACT("char",     TokenKind::KW_CHAR);
                VEX_KW_EXACT("class",    TokenKind::KW_CLASS);
                VEX_KW_EXACT("const",    TokenKind::KW_CONST);
                VEX_KW_EXACT("continue", TokenKind::KW_CONTINUE);
                VEX_KW_EXACT("catch",    TokenKind::KW_CATCH);
                VEX_KW_EXACT("case",     TokenKind::KW_CASE);
                break;
            case 'd':
                VEX_KW_EXACT("do",     TokenKind::KW_DO);
                VEX_KW_EXACT("double", TokenKind::KW_DOUBLE);
                VEX_KW_EXACT("delete", TokenKind::KW_DELETE);
                break;
            case 'e':
                VEX_KW_EXACT("else",   TokenKind::KW_ELSE);
                VEX_KW_EXACT("enum",   TokenKind::KW_ENUM);
                VEX_KW_EXACT("extern", TokenKind::KW_EXTERN);
                break;
            case 'f':
                VEX_KW_EXACT("f32",     TokenKind::KW_F32);
                VEX_KW_EXACT("f64",     TokenKind::KW_F64);
                VEX_KW_EXACT("for",     TokenKind::KW_FOR);
                VEX_KW_EXACT("fn",      TokenKind::KW_FN);
                VEX_KW_EXACT("false",   TokenKind::FALSE_KW);
                VEX_KW_EXACT("final",   TokenKind::KW_FINAL);
                VEX_KW_EXACT("finally", TokenKind::KW_FINALLY);
                VEX_KW_EXACT("float",   TokenKind::KW_FLOAT);
                break;
            case 'g':
                VEX_KW_EXACT("get",  TokenKind::KW_GET);
                VEX_KW_EXACT("goto", TokenKind::KW_GOTO);
                break;
            case 'i':
                VEX_KW_EXACT("if",        TokenKind::KW_IF);
                VEX_KW_EXACT("in",        TokenKind::KW_IN);
                VEX_KW_EXACT("i8",        TokenKind::KW_INT8);
                VEX_KW_EXACT("i16",       TokenKind::KW_INT16);
                VEX_KW_EXACT("i32",       TokenKind::KW_INT32);
                VEX_KW_EXACT("i64",       TokenKind::KW_INT64);
                VEX_KW_EXACT("int8_t",    TokenKind::KW_INT8_T);
                VEX_KW_EXACT("int16_t",   TokenKind::KW_INT16_T);
                VEX_KW_EXACT("int32_t",   TokenKind::KW_INT32_T);
                VEX_KW_EXACT("int64_t",   TokenKind::KW_INT64_T);
                VEX_KW_EXACT("import",    TokenKind::KW_IMPORT);
                VEX_KW_EXACT("interface", TokenKind::KW_INTERFACE);
                break;
            case 'm':
                VEX_KW_EXACT("monitor", TokenKind::KW_MONITOR);
                VEX_KW_EXACT("match",   TokenKind::KW_MATCH);
                break;
            case 'n':
                VEX_KW_EXACT("new",     TokenKind::KW_NEW);
                VEX_KW_EXACT("null",    TokenKind::NULL_KW);
                VEX_KW_EXACT("nonnull", TokenKind::KW_NONNULL);
                break;
            case 'o':
                VEX_KW_EXACT("override", TokenKind::KW_OVERRIDE);
                break;
            case 'p':
                VEX_KW_EXACT("public",    TokenKind::KW_PUBLIC);
                VEX_KW_EXACT("private",   TokenKind::KW_PRIVATE);
                VEX_KW_EXACT("protected", TokenKind::KW_PROTECTED);
                break;
            case 'r':
                VEX_KW_EXACT("return", TokenKind::KW_RETURN);
                VEX_KW_EXACT("rspawn", TokenKind::KW_RSPAWN);
                break;
            case 's':
                VEX_KW_EXACT("set",          TokenKind::KW_SET);
                VEX_KW_EXACT("string",       TokenKind::KW_STRING);
                VEX_KW_EXACT("struct",       TokenKind::KW_STRUCT);
                VEX_KW_EXACT("static",       TokenKind::KW_STATIC);
                VEX_KW_EXACT("super",        TokenKind::KW_SUPER);
                VEX_KW_EXACT("spawn",        TokenKind::KW_SPAWN);
                VEX_KW_EXACT("synchronized", TokenKind::KW_SYNCHRONIZED);
                break;
            case 't':
                VEX_KW_EXACT("this",    TokenKind::KW_THIS);
                VEX_KW_EXACT("throw",   TokenKind::KW_THROW);
                VEX_KW_EXACT("true",    TokenKind::TRUE_KW);
                VEX_KW_EXACT("try",     TokenKind::KW_TRY);
                VEX_KW_EXACT("typedef", TokenKind::KW_TYPEDEF);
                break;
            case 'u':
                VEX_KW_EXACT("u8",       TokenKind::KW_UINT8);
                VEX_KW_EXACT("u16",      TokenKind::KW_UINT16);
                VEX_KW_EXACT("u32",      TokenKind::KW_UINT32);
                VEX_KW_EXACT("u64",      TokenKind::KW_UINT64);
                VEX_KW_EXACT("uint8_t",  TokenKind::KW_UINT8_T);
                VEX_KW_EXACT("uint16_t", TokenKind::KW_UINT16_T);
                VEX_KW_EXACT("uint32_t", TokenKind::KW_UINT32_T);
                VEX_KW_EXACT("uint64_t", TokenKind::KW_UINT64_T);
                VEX_KW_EXACT("using",    TokenKind::KW_USING);
                break;
            case 'v':
                VEX_KW_EXACT("void", TokenKind::KW_VOID);
                break;
            case 'w':
                VEX_KW_EXACT("while", TokenKind::KW_WHILE);
                break;
            default:
                break;
        }
        // No coincidio con ninguna palabra reservada: identificador normal.
        return TokenKind::IDENTIFIER;
    }

    #undef VEX_KW_EXACT

    // -----------------------------------------------------------------------
    // Lexer: implementacion.
    // -----------------------------------------------------------------------

    Lexer::Lexer(std::string source, std::string filename, Diagnostics &diags)
        : source_(std::move(source)),
          filename_(std::move(filename)),
          diags_(diags) {
        // Reservar capacidad del lexema cacheado para evitar relocaciones
        // tipicas (la mayoria de los identificadores caben en SSO).
        peeked_.lexeme.reserve(16);
    }

    char Lexer::peek_char(size_t offset) const noexcept {
        // Acceso seguro al buffer; '\0' fuera de rango simplifica los
        // chequeos en los call sites (no requiere if explicito).
        const size_t idx = pos_ + offset;
        return idx < source_.size() ? source_[idx] : '\0';
    }

    char Lexer::advance() {
        // Lectura del caracter actual y avance del cursor.  Actualiza
        // contadores de linea y columna en el orden correcto: si el
        // caracter es '\n', incrementamos la linea ANTES de pisar la
        // columna a 1 (de modo que la posicion del '\n' en si misma
        // pertenezca a la linea anterior, no a la nueva).
        const char c = source_[pos_++];
        if (c == '\n') {
            ++line_;
            column_ = 1;
        } else {
            ++column_;
        }
        return c;
    }

    void Lexer::skip_trivia() {
        // Loop estrecho: avanza mientras haya whitespace o comentarios.
        // El switch interno se convierte en jump table; el caso comun
        // (espacio, salto de linea) cae en la rama mas barata.
        while (pos_ < source_.size()) {
            const char c = source_[pos_];
            switch (c) {
                case ' ':
                case '\t':
                case '\r':
                case '\n':
                    advance();
                    continue;

                case '/': {
                    // Distinguir comentario de linea, comentario de bloque
                    // y operador de division mirando un caracter mas alla.
                    const char n = peek_char(1);
                    if (n == '/') {
                        // Comentario de linea: avanzar hasta '\n' o EOF.
                        // No consumimos el '\n' aqui para que el bucle
                        // exterior actualice line_ correctamente.
                        pos_ += 2; column_ += 2;
                        while (pos_ < source_.size() && source_[pos_] != '\n') {
                            ++pos_; ++column_;
                        }
                        continue;
                    }
                    if (n == '*') {
                        // Comentario de bloque: avanzar hasta "*/" o EOF.
                        // Mantenemos el track de linea para diagnosticar
                        // comentarios sin cerrar.
                        const SourceLoc start{filename_, line_, column_, (uint32_t)pos_, 2};
                        pos_ += 2; column_ += 2;
                        while (pos_ < source_.size()) {
                            if (source_[pos_] == '*' && peek_char(1) == '/') {
                                pos_ += 2; column_ += 2;
                                goto block_done;
                            }
                            advance();
                        }
                        // Si llegamos aqui es que el comentario nunca cerro.
                        error_at(start, "comentario de bloque sin cerrar (falta '*/')");
                    block_done:
                        continue;
                    }
                    // No era comentario: dejar que lex_symbol lo trate.
                    return;
                }

                default:
                    // Cualquier otra cosa: terminar skip_trivia.
                    return;
            }
        }
    }

    Token Lexer::make_token(TokenKind kind, std::string lexeme, SourceLoc loc) const {
        // Pequena utilidad de construccion; se inlinea por completo.
        Token t;
        t.kind   = kind;
        t.lexeme = std::move(lexeme);
        t.loc    = std::move(loc);
        return t;
    }

    void Lexer::error_at(SourceLoc loc, std::string msg) {
        // Centraliza la emision: ayuda a poner breakpoints durante depuracion.
        diags_.error(std::move(loc), std::move(msg));
    }

    // -----------------------------------------------------------------------
    // Lectura de literales numericos.
    //
    // Reconoce:
    //   - 0x[0-9A-Fa-f_]+      (hex, con _ como separador opcional)
    //   - 0b[01_]+             (binario)
    //   - 0o[0-7_]+            (octal)
    //   - [0-9_]+              (decimal)
    //   - [0-9_]+ . [0-9_]+    (flotante)
    //   - exponente cientifico [eE][+-]?[0-9]+
    //
    // Los separadores '_' se ignoran a efectos de valor numerico.
    // -----------------------------------------------------------------------
    Token Lexer::lex_number() {
        const SourceLoc start{filename_, line_, column_, (uint32_t)pos_, 0};
        const size_t    begin = pos_;

        bool     is_float = false;
        uint64_t int_val  = 0;

        // Detectar prefijo de base: 0x / 0b / 0o.
        if (source_[pos_] == '0' && pos_ + 1 < source_.size()) {
            const char b = source_[pos_ + 1];
            if (b == 'x' || b == 'X') {
                pos_ += 2; column_ += 2;
                while (pos_ < source_.size()) {
                    const char c = source_[pos_];
                    if (c == '_') { ++pos_; ++column_; continue; }
                    int d;
                    if (c >= '0' && c <= '9')      d = c - '0';
                    else if (c >= 'a' && c <= 'f') d = 10 + (c - 'a');
                    else if (c >= 'A' && c <= 'F') d = 10 + (c - 'A');
                    else break;
                    int_val = (int_val << 4) | (uint64_t)d;
                    ++pos_; ++column_;
                }
                std::string lex = source_.substr(begin, pos_ - begin);
                SourceLoc loc = start; loc.length = (uint32_t)(pos_ - begin);
                Token t = make_token(TokenKind::INT_LIT, std::move(lex), loc);
                t.int_val = int_val;
                return t;
            }
            if (b == 'b' || b == 'B') {
                pos_ += 2; column_ += 2;
                while (pos_ < source_.size()) {
                    const char c = source_[pos_];
                    if (c == '_') { ++pos_; ++column_; continue; }
                    if (c != '0' && c != '1') break;
                    int_val = (int_val << 1) | (uint64_t)(c - '0');
                    ++pos_; ++column_;
                }
                std::string lex = source_.substr(begin, pos_ - begin);
                SourceLoc loc = start; loc.length = (uint32_t)(pos_ - begin);
                Token t = make_token(TokenKind::INT_LIT, std::move(lex), loc);
                t.int_val = int_val;
                return t;
            }
            if (b == 'o' || b == 'O') {
                pos_ += 2; column_ += 2;
                while (pos_ < source_.size()) {
                    const char c = source_[pos_];
                    if (c == '_') { ++pos_; ++column_; continue; }
                    if (c < '0' || c > '7') break;
                    int_val = (int_val << 3) | (uint64_t)(c - '0');
                    ++pos_; ++column_;
                }
                std::string lex = source_.substr(begin, pos_ - begin);
                SourceLoc loc = start; loc.length = (uint32_t)(pos_ - begin);
                Token t = make_token(TokenKind::INT_LIT, std::move(lex), loc);
                t.int_val = int_val;
                return t;
            }
        }

        // Decimal: parte entera.
        while (pos_ < source_.size()) {
            const char c = source_[pos_];
            if (c == '_') { ++pos_; ++column_; continue; }
            if (c < '0' || c > '9') break;
            int_val = int_val * 10 + (uint64_t)(c - '0');
            ++pos_; ++column_;
        }

        // Parte fraccional?  (Solo si lo siguiente al '.' es digito; asi
        // "1.method()" sigue siendo entero seguido de DOT.)
        if (pos_ < source_.size() && source_[pos_] == '.'
            && pos_ + 1 < source_.size()
            && std::isdigit((unsigned char)source_[pos_ + 1]))
        {
            is_float = true;
            ++pos_; ++column_;
            while (pos_ < source_.size()) {
                const char c = source_[pos_];
                if (c == '_') { ++pos_; ++column_; continue; }
                if (c < '0' || c > '9') break;
                ++pos_; ++column_;
            }
        }

        // Exponente cientifico opcional.
        if (pos_ < source_.size() && (source_[pos_] == 'e' || source_[pos_] == 'E')) {
            is_float = true;
            ++pos_; ++column_;
            if (pos_ < source_.size() && (source_[pos_] == '+' || source_[pos_] == '-')) {
                ++pos_; ++column_;
            }
            while (pos_ < source_.size() && std::isdigit((unsigned char)source_[pos_])) {
                ++pos_; ++column_;
            }
        }

        std::string lex = source_.substr(begin, pos_ - begin);
        SourceLoc   loc = start; loc.length = (uint32_t)(pos_ - begin);

        if (is_float) {
            // strtod ignora '_' incrustados?  No.  Limpiamos antes.
            std::string clean; clean.reserve(lex.size());
            for (char c : lex) if (c != '_') clean.push_back(c);
            Token t = make_token(TokenKind::FLOAT_LIT, std::move(lex), loc);
            t.flt_val = std::strtod(clean.c_str(), nullptr);
            return t;
        }
        Token t = make_token(TokenKind::INT_LIT, std::move(lex), loc);
        t.int_val = int_val;
        return t;
    }

    // -----------------------------------------------------------------------
    // Lectura de identificadores y palabras reservadas.
    //
    // [A-Za-z_][A-Za-z0-9_]* -- ASCII puro, sin Unicode (decision A.1).
    // -----------------------------------------------------------------------
    Token Lexer::lex_identifier() {
        const SourceLoc start{filename_, line_, column_, (uint32_t)pos_, 0};
        const size_t    begin = pos_;

        while (pos_ < source_.size()) {
            const char c = source_[pos_];
            const bool ok = (c >= 'a' && c <= 'z')
                         || (c >= 'A' && c <= 'Z')
                         || (c >= '0' && c <= '9')
                         || c == '_';
            if (!ok) break;
            ++pos_; ++column_;
        }
        std::string lex = source_.substr(begin, pos_ - begin);
        SourceLoc   loc = start; loc.length = (uint32_t)(pos_ - begin);
        // Clasificacion en ruta caliente: dispatch por primera letra.
        const TokenKind kind = classify_identifier(lex);
        return make_token(kind, std::move(lex), loc);
    }

    // -----------------------------------------------------------------------
    // Helper: decodifica un escape \X tras consumir el backslash.
    // Devuelve el codepoint resuelto (en uint64_t) y avanza el cursor.
    // En caso de error reporta y devuelve 0.
    // -----------------------------------------------------------------------
    static uint64_t decode_escape(Lexer * /*self*/,
                                  const std::string &source,
                                  size_t &pos,
                                  uint32_t &line,
                                  uint32_t &column,
                                  Diagnostics &diags,
                                  const std::string &filename)
    {
        // Asume que pos apunta al caracter SIGUIENTE al backslash.
        if (pos >= source.size()) {
            diags.error({filename, line, column, (uint32_t)pos, 1},
                        "secuencia de escape incompleta");
            return 0;
        }
        const char e = source[pos++];
        ++column;
        switch (e) {
            case 'n':  return '\n';
            case 't':  return '\t';
            case 'r':  return '\r';
            case '0':  return 0;
            case '\\': return '\\';
            case '\'': return '\'';
            case '"':  return '"';
            case 'a':  return 7;
            case 'b':  return 8;
            case 'f':  return 12;
            case 'v':  return 11;
            case 'x': {
                // \xHH -> 1 a 2 digitos hex.
                uint64_t v = 0; int count = 0;
                while (count < 2 && pos < source.size()) {
                    const char c = source[pos];
                    int d;
                    if (c >= '0' && c <= '9')      d = c - '0';
                    else if (c >= 'a' && c <= 'f') d = 10 + (c - 'a');
                    else if (c >= 'A' && c <= 'F') d = 10 + (c - 'A');
                    else break;
                    v = (v << 4) | (uint64_t)d;
                    ++pos; ++column; ++count;
                }
                if (count == 0) {
                    diags.error({filename, line, column, (uint32_t)pos, 1},
                                "esperado digito hex tras \\x");
                }
                return v;
            }
            case 'u': {
                // \uHHHH -> exactamente 4 digitos hex.
                uint64_t v = 0;
                for (int i = 0; i < 4; ++i) {
                    if (pos >= source.size()) break;
                    const char c = source[pos];
                    int d;
                    if (c >= '0' && c <= '9')      d = c - '0';
                    else if (c >= 'a' && c <= 'f') d = 10 + (c - 'a');
                    else if (c >= 'A' && c <= 'F') d = 10 + (c - 'A');
                    else { d = -1; break; }
                    v = (v << 4) | (uint64_t)d;
                    ++pos; ++column;
                }
                return v;
            }
            default:
                diags.error({filename, line, column, (uint32_t)pos, 1},
                            std::string("secuencia de escape desconocida: \\") + e);
                return (uint64_t)e;
        }
    }

    // -----------------------------------------------------------------------
    // Lectura de literales de caracter '...'.
    // -----------------------------------------------------------------------
    Token Lexer::lex_char() {
        const SourceLoc start{filename_, line_, column_, (uint32_t)pos_, 0};
        const size_t    begin = pos_;
        // Consumir la comilla simple inicial.
        advance();

        uint64_t value = 0;
        if (pos_ >= source_.size()) {
            error_at(start, "literal de caracter sin cerrar");
        } else if (source_[pos_] == '\\') {
            ++pos_; ++column_;
            value = decode_escape(this, source_, pos_, line_, column_, diags_, filename_);
        } else {
            value = (uint64_t)(unsigned char)source_[pos_++];
            ++column_;
        }
        // Cierre obligatorio.
        if (pos_ >= source_.size() || source_[pos_] != '\'') {
            error_at({filename_, line_, column_, (uint32_t)pos_, 1},
                     "esperado ''' para cerrar literal de caracter");
        } else {
            ++pos_; ++column_;
        }

        std::string lex = source_.substr(begin, pos_ - begin);
        SourceLoc   loc = start; loc.length = (uint32_t)(pos_ - begin);
        Token t = make_token(TokenKind::CHAR_LIT, std::move(lex), loc);
        t.int_val = value;
        return t;
    }

    // -----------------------------------------------------------------------
    // Lectura de strings "..." o r"...".
    //
    // En A.1 NO soportamos interpolacion ${...} ni triple-quoted """...""";
    // si las detectamos, reportamos un diagnostico explicito y emitimos
    // un STRING_LIT vacio para que el parser pueda seguir reportando errores.
    // -----------------------------------------------------------------------
    Token Lexer::lex_string(bool raw) {
        const SourceLoc start{filename_, line_, column_, (uint32_t)pos_, 0};
        const size_t    begin = pos_;

        if (raw) {
            // 'r' ya esta consumido fuera; aqui apuntamos a la comilla.
        }
        if (pos_ >= source_.size() || source_[pos_] != '"') {
            error_at(start, "esperado '\"' al inicio de string literal");
            return make_token(TokenKind::STRING_LIT, "", start);
        }
        // Detectar triple-quoted: si vemos """, es multilinea.  Termina en
        // el primer """ que aparezca.  Newlines dentro son literales.  Los
        // escapes (\n, \xHH, \uHHHH...) y la interpolacion ${expr} siguen
        // funcionando salvo en raw triple-quoted (r"""...""").  Como las
        // comillas dentro del bloque pueden ser unicas o dobles sin
        // problema, no hay que escaparlas mientras no formen una secuencia
        // de tres consecutivas.
        if (peek_char(1) == '"' && peek_char(2) == '"') {
            advance(); advance(); advance(); // consumir """
            std::string value; value.reserve(64);
            while (pos_ < source_.size()) {
                // Cierre con """.
                if (source_[pos_] == '"' && peek_char(1) == '"'
                 && peek_char(2) == '"') {
                    advance(); advance(); advance();
                    std::string lex = source_.substr(begin, pos_ - begin);
                    SourceLoc   loc = start; loc.length = (uint32_t)(pos_ - begin);
                    const TokenKind kind = raw ? TokenKind::RAW_STRING_LIT
                                               : TokenKind::STRING_LIT;
                    Token t = make_token(kind, std::move(lex), loc);
                    t.str_val = std::move(value);
                    return t;
                }
                const char c = source_[pos_];
                if (!raw && c == '\\') {
                    advance();
                    const uint64_t cp = decode_escape(this, source_, pos_,
                                                       line_, column_,
                                                       diags_, filename_);
                    value.push_back(static_cast<char>(cp & 0xFF));
                    continue;
                }
                if (c == '\n') {
                    ++line_; column_ = 1; ++pos_;
                    value.push_back('\n');
                    continue;
                }
                value.push_back(c);
                advance();
            }
            error_at(start, "string triple-quoted sin cerrar (\"\"\")");
            return make_token(TokenKind::STRING_LIT, "", start);
        }
        advance(); // consumir la comilla apertura

        // Acumulamos el contenido resuelto en 'value': para strings normales
        // los escapes ya se traducen aqui (\n -> 0x0A, \xFF -> 0xFF...),
        // de modo que el parser y el lowering consumen bytes finales sin
        // procesar mas; para strings raw, value es identico al texto entre
        // comillas (sin transformaciones).  Esto evita duplicar el procesado
        // de escapes en el lowering.
        std::string value; value.reserve(16);
        while (pos_ < source_.size()) {
            const char c = source_[pos_];
            if (c == '"') {
                advance();
                std::string lex = source_.substr(begin, pos_ - begin);
                SourceLoc   loc = start; loc.length = (uint32_t)(pos_ - begin);
                const TokenKind kind = raw ? TokenKind::RAW_STRING_LIT
                                           : TokenKind::STRING_LIT;
                Token t = make_token(kind, std::move(lex), loc);
                t.str_val = std::move(value);
                return t;
            }
            if (!raw && c == '\\') {
                advance();
                const uint64_t cp = decode_escape(this, source_, pos_, line_, column_,
                                                  diags_, filename_);
                // Caracter ASCII directo; codepoints >127 se truncan al byte
                // bajo (suficiente para A.1.5; el soporte UTF-8 completo
                // llegara con el sistema de strings managed en A.2).
                value.push_back(static_cast<char>(cp & 0xFF));
                continue;
            }
            if (!raw && c == '$' && peek_char(1) == '{') {
                // A.16: detectada interpolacion.  Cambiamos a modo
                // multi-token y procesamos TODO el string entero en una
                // pasada, depositando los tokens en string_emit_queue_.
                // Devolvemos el primer token (ISTR_BEGIN); las siguientes
                // llamadas a lex_one_raw drenan la cola.
                //
                // Layout: ISTR_BEGIN, [ISTR_TEXT(value_pre)?,]
                //         (ISTR_EXPR_BEGIN, <tokens internos>, ISTR_EXPR_END,
                //          ISTR_TEXT(value_chunk)?)+, ISTR_END.
                SourceLoc loc_begin = start;
                loc_begin.length = 0;
                Token tok_begin = make_token(TokenKind::ISTR_BEGIN, "", loc_begin);
                string_emit_queue_.push_back(std::move(tok_begin));

                // Descargar el texto acumulado hasta este momento como
                // ISTR_TEXT (si lo hay).  Usamos la SourceLoc del inicio
                // del string para diagnostico, length = 0 marca "sintetico".
                if (!value.empty()) {
                    Token tt = make_token(TokenKind::ISTR_TEXT, "", loc_begin);
                    tt.str_val = std::move(value);
                    string_emit_queue_.push_back(std::move(tt));
                    value.clear();
                }

                // Loop principal: alternar entre tramos ${expr} y texto
                // literal hasta cerrar el string con ".
                while (pos_ < source_.size()) {
                    SourceLoc loc_dollar{filename_, line_, column_,
                                         (uint32_t)pos_, 2};
                    advance(); // consumir '$'
                    advance(); // consumir '{'
                    string_emit_queue_.push_back(
                        make_token(TokenKind::ISTR_EXPR_BEGIN, "", loc_dollar));

                    // Lex tokens internos hasta cerrar con '}' matching.
                    // depth empieza en 1 porque ya consumimos el '{'.  Cada
                    // SYM_LBRACE adicional incrementa, cada SYM_RBRACE
                    // decrementa.  Al llegar a 0 emitimos ISTR_EXPR_END y
                    // salimos del sub-loop.
                    int depth = 1;
                    while (pos_ < source_.size() && depth > 0) {
                        // skip_trivia + lex_one_raw del flujo normal, sin
                        // tocar string_emit_queue_ (ya estamos drenando).
                        // OJO: hay que evitar que lex_one_raw vea la cola
                        // y se devore los ISTR_EXPR_BEGIN que acabamos de
                        // empujar.  Solucion: leer tokens del source_
                        // directamente saltando la cola.  Para eso
                        // empleamos un pequeno helper inline.
                        skip_trivia();
                        if (pos_ >= source_.size()) break;
                        const char ic = source_[pos_];
                        Token inner;
                        if (ic == 'r' && peek_char(1) == '"') {
                            advance();
                            inner = lex_string(true);
                        } else if ((ic >= 'a' && ic <= 'z')
                                || (ic >= 'A' && ic <= 'Z') || ic == '_') {
                            inner = lex_identifier();
                        } else if (ic >= '0' && ic <= '9') {
                            inner = lex_number();
                        } else if (ic == '"') {
                            // String anidado dentro de la expresion
                            // interpolada: lex normal.  Si tambien tiene
                            // interpolacion, recursivamente se acumula en
                            // la misma string_emit_queue_.  Para evitar
                            // colision empuja al final y nosotros lo
                            // consumimos despues.  Solucion mas simple:
                            // lex_string emite ahora a una cola TEMPORAL.
                            //
                            // Para A.16 MVP: NO permitir strings anidados
                            // dentro de ${...}.  El usuario puede assignar
                            // a una variable y interpolar la variable.
                            inner = lex_string(false);
                        } else if (ic == '\'') {
                            inner = lex_char();
                        } else {
                            inner = lex_symbol();
                        }
                        // Detectar { y } para tracking de profundidad.
                        if (inner.kind == TokenKind::LBRACE) {
                            ++depth;
                        } else if (inner.kind == TokenKind::RBRACE) {
                            --depth;
                            if (depth == 0) {
                                // No empujamos el RBRACE; en su lugar
                                // emitimos ISTR_EXPR_END.
                                Token tend = make_token(TokenKind::ISTR_EXPR_END,
                                                        "", inner.loc);
                                string_emit_queue_.push_back(std::move(tend));
                                break;
                            }
                        }
                        string_emit_queue_.push_back(std::move(inner));
                    }
                    if (depth > 0) {
                        error_at(loc_dollar, "interpolacion ${...} sin '}' de cierre");
                        break;
                    }

                    // Tras cerrar la expresion, seguir leyendo texto literal.
                    std::string chunk;
                    bool string_done = false;
                    bool nested_interp = false;
                    while (pos_ < source_.size()) {
                        const char tc = source_[pos_];
                        if (tc == '"') {
                            advance();
                            string_done = true;
                            break;
                        }
                        if (!raw && tc == '$' && peek_char(1) == '{') {
                            nested_interp = true;
                            break;  // saltar a la siguiente vuelta del loop principal
                        }
                        if (!raw && tc == '\\') {
                            advance();
                            const uint64_t cp = decode_escape(this, source_,
                                pos_, line_, column_, diags_, filename_);
                            chunk.push_back(static_cast<char>(cp & 0xFF));
                            continue;
                        }
                        if (tc == '\n' && !raw) {
                            error_at({filename_, line_, column_, (uint32_t)pos_, 1},
                                     "salto de linea dentro de string literal no raw");
                        }
                        chunk.push_back(tc);
                        advance();
                    }
                    if (!chunk.empty()) {
                        Token tt = make_token(TokenKind::ISTR_TEXT, "", start);
                        tt.str_val = std::move(chunk);
                        string_emit_queue_.push_back(std::move(tt));
                    }
                    if (string_done) {
                        SourceLoc loc_end{filename_, line_, column_,
                                          (uint32_t)pos_, 0};
                        string_emit_queue_.push_back(
                            make_token(TokenKind::ISTR_END, "", loc_end));
                        // Devolvemos el primer token de la cola (ISTR_BEGIN
                        // empujado al inicio) y dejamos el resto para
                        // proximas llamadas a lex_one_raw.
                        Token first = std::move(string_emit_queue_.front());
                        string_emit_queue_.pop_front();
                        return first;
                    }
                    if (!nested_interp) {
                        error_at(start, "string literal interpolado sin cerrar");
                        // Cierre defensivo: ISTR_END para mantener parser sano.
                        SourceLoc loc_end{filename_, line_, column_,
                                          (uint32_t)pos_, 0};
                        string_emit_queue_.push_back(
                            make_token(TokenKind::ISTR_END, "", loc_end));
                        Token first = std::move(string_emit_queue_.front());
                        string_emit_queue_.pop_front();
                        return first;
                    }
                    // nested_interp: continuar con la siguiente vuelta del
                    // loop principal (procesara el ${...} adicional).
                }
                // Si llegamos aqui es por EOF mid-interpolacion.
                error_at(start, "string interpolado sin cerrar (EOF)");
                SourceLoc loc_end{filename_, line_, column_, (uint32_t)pos_, 0};
                string_emit_queue_.push_back(
                    make_token(TokenKind::ISTR_END, "", loc_end));
                Token first = std::move(string_emit_queue_.front());
                string_emit_queue_.pop_front();
                return first;
            }
            if (c == '\n') {
                if (!raw) {
                    error_at({filename_, line_, column_, (uint32_t)pos_, 1},
                             "salto de linea dentro de string literal no raw");
                }
                value.push_back(c);
                advance();
                continue;
            }
            value.push_back(c);
            advance();
        }

        error_at(start, "string literal sin cerrar");
        std::string lex = source_.substr(begin, pos_ - begin);
        SourceLoc   loc = start; loc.length = (uint32_t)(pos_ - begin);
        const TokenKind kind = raw ? TokenKind::RAW_STRING_LIT : TokenKind::STRING_LIT;
        Token t = make_token(kind, std::move(lex), loc);
        t.str_val = std::move(value);
        return t;
    }

    // -----------------------------------------------------------------------
    // Lectura de simbolos / operadores.
    //
    // El switch de primer caracter es la ruta caliente del lexer despues
    // del whitespace.  Las ramas que desambiguan miran un caracter mas alla
    // y consumen 1 o 2 (3 para <<= y >>=) bytes.
    // -----------------------------------------------------------------------
    Token Lexer::lex_symbol() {
        const SourceLoc start{filename_, line_, column_, (uint32_t)pos_, 0};
        const size_t    begin = pos_;
        const char      c     = source_[pos_];

        auto emit = [&](TokenKind k, size_t len) {
            // Helper local para construir un Token consumiendo len bytes.
            for (size_t i = 0; i < len; ++i) advance();
            std::string lex = source_.substr(begin, pos_ - begin);
            SourceLoc   loc = start; loc.length = (uint32_t)len;
            return make_token(k, std::move(lex), loc);
        };

        switch (c) {
            case '+':
                if (peek_char(1) == '+') return emit(TokenKind::PLUS_PLUS,   2);
                if (peek_char(1) == '=') return emit(TokenKind::PLUS_ASSIGN, 2);
                return emit(TokenKind::PLUS, 1);
            case '-':
                if (peek_char(1) == '-') return emit(TokenKind::MINUS_MINUS,  2);
                if (peek_char(1) == '=') return emit(TokenKind::MINUS_ASSIGN, 2);
                if (peek_char(1) == '>') return emit(TokenKind::ARROW,        2);
                return emit(TokenKind::MINUS, 1);
            case '*':
                if (peek_char(1) == '=') return emit(TokenKind::STAR_ASSIGN, 2);
                return emit(TokenKind::STAR, 1);
            case '/':
                // Los comentarios ya se trataron en skip_trivia().
                if (peek_char(1) == '=') return emit(TokenKind::SLASH_ASSIGN, 2);
                return emit(TokenKind::SLASH, 1);
            case '%':
                if (peek_char(1) == '=') return emit(TokenKind::PERCENT_ASSIGN, 2);
                return emit(TokenKind::PERCENT, 1);
            case '=':
                if (peek_char(1) == '=') return emit(TokenKind::EQ,        2);
                if (peek_char(1) == '>') return emit(TokenKind::FAT_ARROW, 2);
                return emit(TokenKind::ASSIGN, 1);
            case '!':
                if (peek_char(1) == '=') return emit(TokenKind::NEQ, 2);
                if (peek_char(1) == '!') return emit(TokenKind::BANG_BANG, 2);
                return emit(TokenKind::BANG, 1);
            case '<':
                if (peek_char(1) == '<') {
                    if (peek_char(2) == '=') return emit(TokenKind::SHL_ASSIGN, 3);
                    return emit(TokenKind::SHL, 2);
                }
                if (peek_char(1) == '=') return emit(TokenKind::LE, 2);
                return emit(TokenKind::LT, 1);
            case '>':
                if (peek_char(1) == '>') {
                    if (peek_char(2) == '=') return emit(TokenKind::SHR_ASSIGN, 3);
                    return emit(TokenKind::SHR, 2);
                }
                if (peek_char(1) == '=') return emit(TokenKind::GE, 2);
                return emit(TokenKind::GT, 1);
            case '&':
                if (peek_char(1) == '&') return emit(TokenKind::AND_AND,    2);
                if (peek_char(1) == '=') return emit(TokenKind::AMP_ASSIGN, 2);
                return emit(TokenKind::AMP, 1);
            case '|':
                if (peek_char(1) == '|') return emit(TokenKind::OR_OR,       2);
                if (peek_char(1) == '=') return emit(TokenKind::PIPE_ASSIGN, 2);
                return emit(TokenKind::PIPE, 1);
            case '^':
                if (peek_char(1) == '=') return emit(TokenKind::CARET_ASSIGN, 2);
                return emit(TokenKind::CARET, 1);
            case '~':  return emit(TokenKind::TILDE,     1);
            case '(':  return emit(TokenKind::LPAREN,    1);
            case ')':  return emit(TokenKind::RPAREN,    1);
            case '{':  return emit(TokenKind::LBRACE,    1);
            case '}':  return emit(TokenKind::RBRACE,    1);
            case '[':  return emit(TokenKind::LBRACKET,  1);
            case ']':  return emit(TokenKind::RBRACKET,  1);
            case ',':  return emit(TokenKind::COMMA,     1);
            case ';':  return emit(TokenKind::SEMICOLON, 1);
            case ':':  return emit(TokenKind::COLON,     1);
            case '.':  return emit(TokenKind::DOT,       1);
            case '?':  return emit(TokenKind::QUESTION,  1);
            case '@':  return emit(TokenKind::AT,        1);
            default:
                break;
        }

        // Caracter desconocido: reportar y consumir un byte para no
        // bucar infinitamente sobre el mismo error.
        error_at(start, std::string("caracter inesperado: '") + c + "'");
        advance();
        std::string lex(1, c);
        SourceLoc   loc = start; loc.length = 1;
        return make_token(TokenKind::UNKNOWN, std::move(lex), loc);
    }

    // -----------------------------------------------------------------------
    // next() y peek().
    // -----------------------------------------------------------------------
    /**
     * @brief Lee directamente del fuente un token nuevo, sin consultar caches.
     *
     * Implementa el camino real del lexer (skip_trivia + dispatch sobre el
     * primer caracter).  @c next() y @c peek_at() construyen su politica de
     * cache encima de esta primitiva.  De este modo evitamos cualquier
     * recursividad accidental entre los caches y la lectura cruda.
     */
    Token Lexer::lex_one_raw() {
        // A.2/A.16 interpolacion: si lex_string emitio una cadena de tokens
        // ISTR_* en una pasada anterior, drenar el buffer antes de tocar
        // @c source_.  Cero overhead cuando esta vacio (caso comun).
        if (!string_emit_queue_.empty()) {
            Token t = std::move(string_emit_queue_.front());
            string_emit_queue_.pop_front();
            return t;
        }
        skip_trivia();
        if (pos_ >= source_.size()) {
            SourceLoc loc{filename_, line_, column_, (uint32_t)pos_, 0};
            return make_token(TokenKind::END_OF_FILE, "", loc);
        }
        const char c = source_[pos_];

        // Identificadores y keywords (incluye prefijo r" para raw strings).
        if (c == 'r' && peek_char(1) == '"') {
            advance(); // consumir 'r'
            return lex_string(true);
        }
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
            return lex_identifier();
        }
        if (c >= '0' && c <= '9') {
            return lex_number();
        }
        if (c == '"')  return lex_string(false);
        if (c == '\'') return lex_char();
        return lex_symbol();
    }

    Token Lexer::next() {
        if (has_peeked_) {
            // Devolver peeked_ y promover el primer extra al hueco para que
            // peek() siga devolviendo el "siguiente" token de forma consistente.
            Token out = std::move(peeked_);
            if (!extra_lookahead_.empty()) {
                peeked_ = std::move(extra_lookahead_.front());
                extra_lookahead_.pop_front();
                // has_peeked_ permanece true (peeked_ aun esta cargado).
            } else {
                has_peeked_ = false;
            }
            return out;
        }
        return lex_one_raw();
    }

    const Token &Lexer::peek() {
        if (!has_peeked_) {
            peeked_     = lex_one_raw();
            has_peeked_ = true;
        }
        return peeked_;
    }

    const Token &Lexer::peek_at(size_t offset) {
        if (offset == 0) return peek();
        // Asegurar peek() lleno; despues precargar tokens crudos hasta
        // tener `offset` adicionales en extra_lookahead_.
        (void)peek();
        while (extra_lookahead_.size() < offset) {
            extra_lookahead_.push_back(lex_one_raw());
        }
        return extra_lookahead_[offset - 1];
    }

} // namespace vex
