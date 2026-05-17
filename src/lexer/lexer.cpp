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
 * @brief Implementacion del analizador lexico para el lenguaje ensamblador de VestaVM.
 *
 * Implementa @c vm::Lexer: @c next_token() que clasifica el caracter actual
 * y produce el token correspondiente, @c peek_token() para lookahead sin consumir,
 * y las funciones auxiliares @c is_register(), @c is_data_directive() y
 * @c token_type_to_string().
 */
#include <utility>

#include "lexer/lexer.h"

namespace vm {
    Lexer::Lexer(std::string src) : source(std::move(src)), pos(0), line(1), column(1) {}


    char Lexer::peek() const {
        return pos < source.size() ? source[pos] : '\0';
    }

    char Lexer::advance() {
        char c = peek();
        if (c == '\n') {
            ++line;
            column = 1;
        } else {
            ++column;
        }
        ++pos;
        return c;
    }

    void Lexer::skip_whitespace() {
        while (true) {
            if (isspace(peek())) {
                advance();
                continue;
            }

            // ---------------- COMENTARIOS // ----------------
            // Caso especial: `// @line N` capturado en `last_src_line`
            // (no consumido como token; queda accesible al parser).
            // Esto permite asociar cada instruccion del .vel a su linea
            // Vex original sin anadir un opcode dedicado al bytecode.
            if (peek() == '/' && peek_token(1) == '/') {
                size_t save_pos = pos;
                int    save_ln  = line;
                int    save_col = column;
                advance(); advance(); // saltar `//`
                // skip leading spaces/tabs entre `//` y posible `@line`
                while (peek() == ' ' || peek() == '\t') advance();
                // Detectar `@line `, capturar el numero hasta fin de linea.
                if (peek() == '@'
                 && peek_token(1) == 'l'
                 && peek_token(2) == 'i'
                 && peek_token(3) == 'n'
                 && peek_token(4) == 'e'
                 && (peek_token(5) == ' ' || peek_token(5) == '\t')) {
                    advance(); advance(); advance(); advance(); advance(); // @line
                    while (peek() == ' ' || peek() == '\t') advance();
                    int ln_val = 0;
                    while (peek() >= '0' && peek() <= '9') {
                        ln_val = ln_val * 10 + (peek() - '0');
                        advance();
                    }
                    if (ln_val > 0) last_src_line = ln_val;
                    // consumir el resto del comentario hasta '\n'
                    while (peek() != '\n' && peek() != '\0') advance();
                    continue;
                }
                // Detectar `@file <path>` (path puede contener espacios y
                // ':' en Windows; consumimos hasta fin de linea).
                if (peek() == '@'
                 && peek_token(1) == 'f'
                 && peek_token(2) == 'i'
                 && peek_token(3) == 'l'
                 && peek_token(4) == 'e'
                 && (peek_token(5) == ' ' || peek_token(5) == '\t')) {
                    advance(); advance(); advance(); advance(); advance(); // @file
                    while (peek() == ' ' || peek() == '\t') advance();
                    std::string path;
                    while (peek() != '\n' && peek() != '\0') {
                        path.push_back(peek());
                        advance();
                    }
                    // Trim espacios al final
                    while (!path.empty()
                        && (path.back() == ' ' || path.back() == '\t' ||
                            path.back() == '\r')) {
                        path.pop_back();
                    }
                    if (!path.empty() && last_src_file.empty()) {
                        last_src_file = path;
                    }
                    continue;
                }
                // No es `@line`: rebobinar al `//` y consumir como
                // comentario normal (descartar hasta fin de linea).
                pos    = save_pos;
                line   = save_ln;
                column = save_col;
                while (peek() != '\n' && peek() != '\0') {
                    advance();
                }
                continue;
            }

            // ---------------- COMENTARIOS /* */ ---------------- (opcional)
            if (peek() == '/' && peek_token(1) == '*') {
                advance(); advance();  // Saltar /*
                while (!(peek() == '*' && peek_token(1) == '/') && peek() != '\0') {
                    advance();
                }
                if (peek() == '*' && peek_token(1) == '/') {
                    advance(); advance();  // Saltar */
                }
                continue;
            }

            break;  // No mas whitespace/comentarios
        }
    }

    void Lexer::error(const Token &tok, const std::string &msg) {
        std::cerr << "Lexer Error: " << msg
                << " en linea " << tok.line
                << ", columna " << tok.column
                << " (token: '" << tok.lexeme << "')\n";
    }

    Token Lexer::next_token() {
        skip_whitespace();

        if (pos >= source.size())
            return {TokenType::EndOfFile, "", line, column};

        int  start_column = column;
        char c            = advance();

        // ---------------- CHAR LITERALS ----------------
        if (c == '\'') {
            std::string lexeme;
            int         start_line = line;
            int         start_col  = start_column;

            if (peek() == '\0') {
                error(Token(TokenType::CHAR, lexeme, start_line, start_col),
                      "char literal vacio o incompleto");
                return {TokenType::CHAR, lexeme, start_line, start_col};
            }

            if (peek() == '\\') {
                // secuencia de escape
                lexeme += advance(); // '\'
                if (peek() != '\0')
                    lexeme += advance(); // 'n', 't', etc
                else {
                    error(Token(TokenType::CHAR, lexeme, start_line, start_col),
                          "escape sequence incompleta en char literal");
                    return {TokenType::CHAR, lexeme, start_line, start_col};
                }
            } else {
                lexeme += advance(); // caracter normal
            }

            if (peek() == '\'') {
                advance(); // cerrar comilla
            } else {
                error(Token(TokenType::CHAR, lexeme, start_line, start_col),
                      "char literal no cerrado");
            }

            return {TokenType::CHAR, lexeme, start_line, start_col};
        }

        // ---------------- STRING LITERALS ----------------
        if (c == '"') {
            std::string lexeme;
            int         start_line = line;
            int         start_col  = start_column;

            while (peek() != '"' && peek() != '\0') {
                lexeme += advance();
            }

            if (peek() == '"') {
                advance(); // cerrar comillas
            } else {
                error(Token(TokenType::STRING, lexeme, start_line, start_col),
                      "string literal no cerrado");
            }

            return {TokenType::STRING, lexeme, start_line, start_col};
        }

        if (isalpha(c) || c == '_') {
            std::string lexeme(1, c);

            while (isalnum(peek()) || peek() == '_')
                lexeme += advance();

            /*
            if (
                lexeme == "rax" || lexeme == "rbx" || lexeme == "rcx" || lexeme == "rdx" ||
                lexeme == "eax" || lexeme == "ebx" || lexeme == "ecx" || lexeme == "edx" ||
                lexeme == "sp" || lexeme == "bp" || lexeme == "ip"
            ) {
                return {TokenType::REGISTER, lexeme, line, start_column};
            }*/

            // registros tipo r0 r1 r2 r15, ...
            if (is_register(lexeme)) {
                return {TokenType::REGISTER, lexeme, line, start_column};
            }

            if (is_data_directive(lexeme)) {
                return {TokenType::DATA_DIRECTIVE, lexeme, line, start_column};
            }

            if (lexeme == "end") {
                return {TokenType::END_LABEL, lexeme, line, start_column};
            }

            return {TokenType::IDENTIFIER, lexeme, line, start_column};
        }

        if (isdigit(c)) {
            std::string lexeme(1, c);

            if (c == '0') {
                char p = peek();

                if (p == 'x' || p == 'X') {
                    lexeme += advance();
                    while (isxdigit(peek()))
                        lexeme += advance();
                    return {TokenType::NUMBER_HEX, lexeme, line, start_column};
                }

                if (p == 'b' || p == 'B') {
                    lexeme += advance();
                    while (peek() == '0' || peek() == '1')
                        lexeme += advance();
                    return {TokenType::NUMBER_BIN, lexeme, line, start_column};
                }

                if (p == 'o' || p == 'O') {
                    lexeme += advance();
                    while (peek() >= '0' && peek() <= '7')
                        lexeme += advance();
                    return {TokenType::NUMBER_OCT, lexeme, line, start_column};
                }
            }

            bool is_float = false;

            while (isdigit(peek()))
                lexeme += advance();

            if (peek() == '.') {
                is_float = true;
                lexeme += advance();

                while (isdigit(peek()))
                    lexeme += advance();
            }

            if (is_float)
                return {TokenType::NUMBER_FLOAT, lexeme, line, start_column};

            return {TokenType::NUMBER_DEC, lexeme, line, start_column};
        }

        if (c == ';') {
            std::string comment;

            while (peek() != '\n' && peek() != '\0')
                comment += advance();

            return {TokenType::COMMENT, comment, line, start_column};
        }

        switch (c) {
            case '=': return {TokenType::EQUAL, "=", line, start_column};
            case ':': return {TokenType::COLON, ":", line, start_column};
            case ',': return {TokenType::COMMA, ",", line, start_column};

            case '(': return {TokenType::LPAREN, "(", line, start_column};
            case ')': return {TokenType::RPAREN, ")", line, start_column};

            case '[': return {TokenType::LBRACKET, "[", line, start_column};
            case ']': return {TokenType::RBRACKET, "]", line, start_column};

            case '{': return {TokenType::LBRACE, "{", line, start_column};
            case '}': return {TokenType::RBRACE, "}", line, start_column};

            case '+': return {TokenType::PLUS, "+", line, start_column};
            case '-': return {TokenType::MINUS, "-", line, start_column};
            case '*': return {TokenType::STAR, "*", line, start_column};
            case '/': return {TokenType::SLASH, "/", line, start_column};
            case '%': return {TokenType::PERCENT, "%", line, start_column};

            case '.': return {TokenType::DOT, ".", line, start_column};
            case '@': return {TokenType::AT, "@", line, start_column};
            case '$': return {TokenType::DOLLAR, "$", line, start_column};
            case '\\': return {TokenType::BACKSLASH, "\\", line, start_column};
            case '&': return {TokenType::AMPERSAND, "&", line, start_column};
            case '?': return {TokenType::QUESTION, "?", line, start_column};
            case '|': return {TokenType::PIPE, "|", line, start_column};
            case '_': return {TokenType::UNDERSCORE, "_", line, start_column};

            default:
                return {TokenType::SYMBOL, std::string(1, c), line, start_column};
        }
    }

    char Lexer::peek_token(int offset) const {
        size_t idx = pos + offset;
        return (idx < source.size()) ? source[idx] : '\0';
    }

    Token Lexer::peek_token() const {
        // Performance fix CRITICO: la version anterior creaba un Lexer
        // entero (copiando el source completo, ~1 MB en el editor) por
        // cada peek.  Con ~500k peeks en compilaciones grandes eso
        // significaba ~500 GB de allocs y desencadenaba que el "parser"
        // tardara 30+ s donde el lexer es realmente el bottleneck.
        //
        // Solucion: re-ejecutar next_token() sobre ESTE objeto y luego
        // restaurar todo el estado mutable.  Usamos const_cast porque la
        // API publica es const pero la implementacion necesita modificar
        // pos/line/column transitoriamente.  Tambien restauramos los
        // marcadores de debug (last_src_line / last_src_file) que
        // skip_whitespace puede mutar, para que peek no tenga efectos
        // secundarios observables fuera del valor retornado.
        auto *self = const_cast<Lexer *>(this);
        const size_t      save_pos       = self->pos;
        const int         save_line      = self->line;
        const int         save_column    = self->column;
        const int         save_src_line  = self->last_src_line;
        const std::string save_src_file  = self->last_src_file;
        Token peeked = self->next_token();
        self->pos           = save_pos;
        self->line          = save_line;
        self->column        = save_column;
        self->last_src_line = save_src_line;
        self->last_src_file = save_src_file;
        return peeked;
    }

    static const char *token_names[] = {
        "NUMBER_HEX", "NUMBER_BIN", "NUMBER_OCT", "NUMBER_FLOAT", "NUMBER_DEC",

        "IDENTIFIER", "DATA_DIRECTIVE", "REGISTER",

        "CHAR", "STRING",

        "LPAREN", "RPAREN", "LBRACKET", "RBRACKET", "LBRACE", "RBRACE",

        "PLUS", "MINUS", "STAR", "SLASH", "PERCENT",

        "EQUAL", "COLON", "SEMICOLON", "COMMA",

        "DOT", "AT", "DOLLAR", "BACKSLASH", "AMPERSAND", "QUESTION", "INVERTED_Q", "PIPE", "UNDERSCORE",

        "COMMENT", "NEWLINE",

        "SYMBOL", "END_LABEL", "EndOfFile"
    };

    std::string token_type_to_string(vm::TokenType type) {
        auto idx = static_cast<int>(type);
        if (idx < 0 || idx >= std::size(token_names)) return "UNKNOWN";
        return token_names[idx];
    }
} // namespace vm
