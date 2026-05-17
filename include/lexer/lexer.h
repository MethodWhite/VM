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
 * @file lexer.h
 * @brief Analizador lexico (lexer/tokenizer) para el lenguaje ensamblador de VestaVM.
 *
 * Convierte el codigo fuente .vel en una secuencia de tokens (@c Token) que el parser
 * consume para construir el AST.
 *
 * Tokens reconocidos (@c TokenType):
 *  - Literales numericos: hexadecimales (0xFF), binarios (0b101), octales (0o7),
 *    flotantes (3.14), decimales (42).
 *  - Identificadores y registros (r0..r15, rip, rbp, rsp, rflags, cur0..cur3).
 *  - Directivas de datos: db, dw, dd, dq, ptr.
 *  - Literales de caracter y cadena.
 *  - Simbolos de agrupacion: (), [], {}.
 *  - Operadores aritmeticos: + - * / %.
 *  - Simbolos especiales: @ $ . : , ; & | ? \ _
 *  - Comentarios, saltos de linea y fin de archivo.
 *
 * Funciones auxiliares en el namespace @c vm:
 *  - @c is_register()     : comprueba si un lexema es un registro valido.
 *  - @c is_data_directive(): comprueba si un lexema es una directiva de datos.
 *  - @c token_type_to_string(): convierte un @c TokenType a cadena para depuracion.
 */

#ifndef LEXER_H
#define LEXER_H

#include <cctype>
#include <iostream>
#include <string>
#include <fstream>
#include <unordered_set>
#include <utility>
#include <vector>

namespace vm {

    /**
     * @brief Comprueba si un lexema corresponde a un nombre de registro de la VM.
     *
     * Registros especiales reconocidos: @c rip, @c rbp, @c rsp, @c rflags,
     * @c cur0, @c cur1, @c cur2, @c cur3.
     *
     * Registros generales: @c r0..r15 con sufijo opcional @c b (byte),
     * @c w (word) o @c d (dword).  Por ejemplo: @c r0, @c r3b, @c r15d.
     *
     * @param lexeme Lexema a verificar.
     * @return @c true si el lexema es un registro valido; @c false en caso contrario.
     */
    inline bool is_register(const std::string &lexeme) {
        if (lexeme.empty()) return false;

        /* tabla de registros especiales de la VM */
        static const std::unordered_set<std::string> special_regs = {
            "rip", "rbp", "rsp", "rflags",
            "cur0", "cur1", "cur2", "cur3"
        };

        if (special_regs.count(lexeme))
            return true;

        /* registros generales: r[0-15][b|w|d]? */
        if (lexeme[0] == 'r') {
            size_t i = 1;

            /* leer la parte numerica */
            while (i < lexeme.size() && isdigit((unsigned char)lexeme[i]))
                i++;

            if (i == 1) return false; /* no habia numero despues de 'r' */

            int reg_num = std::stoi(lexeme.substr(1, i - 1));
            if (reg_num < 0 || reg_num > 15)
                return false;

            /* sufijo de tamano opcional: b, w, d */
            if (i == lexeme.size())
                return true;

            if (i + 1 == lexeme.size()) {
                char suf = lexeme[i];
                return (suf == 'b' || suf == 'w' || suf == 'd');
            }

            return false; /* mas de un caracter tras el numero: no es registro */
        }

        /* registros vectoriales escalares: f[0-15] */
        if (lexeme[0] == 'f' && lexeme.size() >= 2) {
            size_t i = 1;
            while (i < lexeme.size() && isdigit((unsigned char)lexeme[i]))
                i++;
            if (i == lexeme.size() && i > 1) {
                int n = std::stoi(lexeme.substr(1));
                return (n >= 0 && n <= 15);
            }
        }

        /* registros XMM (128 bits): xmm[0-15] */
        if (lexeme.size() >= 4 && lexeme.substr(0, 3) == "xmm") {
            try {
                int n = std::stoi(lexeme.substr(3));
                return (n >= 0 && n <= 15 && lexeme == "xmm" + std::to_string(n));
            } catch (...) { return false; }
        }

        /* registros YMM (256 bits): ymm[0-15] */
        if (lexeme.size() >= 4 && lexeme.substr(0, 3) == "ymm") {
            try {
                int n = std::stoi(lexeme.substr(3));
                return (n >= 0 && n <= 15 && lexeme == "ymm" + std::to_string(n));
            } catch (...) { return false; }
        }

        /* registros ZMM (512 bits): zmm[0-15] */
        if (lexeme.size() >= 4 && lexeme.substr(0, 3) == "zmm") {
            try {
                int n = std::stoi(lexeme.substr(3));
                return (n >= 0 && n <= 15 && lexeme == "zmm" + std::to_string(n));
            } catch (...) { return false; }
        }

        return false;
    }

    /**
     * @brief Comprueba si un lexema es una directiva de declaracion de datos.
     *
     * Directivas reconocidas:
     *  - @c db : Define Byte    (1 byte por elemento).
     *  - @c dw : Define Word    (2 bytes por elemento).
     *  - @c dd : Define Dword   (4 bytes por elemento).
     *  - @c dq : Define Qword   (8 bytes por elemento).
     *  - @c ptr: Puntero        (8 bytes en x64).
     *
     * @param lexeme Lexema a verificar.
     * @return @c true si el lexema es una directiva de datos valida.
     */
    inline bool is_data_directive(const std::string &lexeme) {
        return lexeme == "db" ||
               lexeme == "dw" ||
               lexeme == "dd" ||
               lexeme == "dq" ||
               lexeme == "ptr";
    }

    /**
     * @enum TokenType
     * @brief Tipos de tokens que el lexer puede reconocer.
     *
     * Agrupados en categorias:
     *  - Literales numericos (NUMBER_*).
     *  - Identificadores, registros y directivas (IDENTIFIER, REGISTER, DATA_DIRECTIVE).
     *  - Literales de texto (CHAR, STRING).
     *  - Simbolos de agrupacion (LPAREN..RBRACE).
     *  - Operadores aritmeticos (PLUS..PERCENT).
     *  - Signos de puntuacion y especiales (EQUAL..UNDERSCORE).
     *  - Control (COMMENT, NEWLINE, SYMBOL, END_LABEL, EndOfFile).
     */
    enum class TokenType {
        /* --- Literales numericos --- */
        NUMBER_HEX,   ///< Hexadecimal: 0xFF, 0xDEAD
        NUMBER_BIN,   ///< Binario: 0b101010
        NUMBER_OCT,   ///< Octal: 0o755
        NUMBER_FLOAT, ///< Flotante: 3.14, 1.0e-3
        NUMBER_DEC,   ///< Decimal: 12345

        /* --- Identificadores --- */
        IDENTIFIER,     ///< Identificador generico: variable, nombre de label, mnemotecnico
        DATA_DIRECTIVE, ///< Directiva de datos: db, dq, dw, dd, ptr
        REGISTER,       ///< Registro de la VM: r0..r15, rip, rbp, rsp, rflags, cur0..cur3

        /* --- Literales de texto --- */
        CHAR,   ///< Literal de caracter: 'a', '\n'
        STRING, ///< Literal de cadena: "hola mundo"

        /* --- Simbolos de agrupacion --- */
        LPAREN,   ///< (
        RPAREN,   ///< )
        LBRACKET, ///< [
        RBRACKET, ///< ]
        LBRACE,   ///< {
        RBRACE,   ///< }

        /* --- Operadores aritmeticos --- */
        PLUS,    ///< +
        MINUS,   ///< -
        STAR,    ///< *
        SLASH,   ///< /
        PERCENT, ///< %

        /* --- Puntuacion y asignacion --- */
        EQUAL,     ///< =
        COLON,     ///< :
        SEMICOLON, ///< ;
        COMMA,     ///< ,

        /* --- Simbolos especiales --- */
        DOT,        ///< .
        AT,         ///< @  (prefijo de anotaciones)
        DOLLAR,     ///< $  (referencia a la posicion actual)
        BACKSLASH,  ///< backslash
        AMPERSAND,  ///< &
        QUESTION,   ///< ?
        INVERTED_Q, ///< signo de interrogacion invertido (no utilizado en el nucleo)
        PIPE,       ///< |
        UNDERSCORE, ///< _

        /* --- Control --- */
        COMMENT,   ///< Comentario de linea (;...) o bloque
        NEWLINE,   ///< Salto de linea '\n'
        SYMBOL,    ///< Simbolo generico no clasificado en otra categoria
        END_LABEL, ///< Fin de declaracion de label (marcador interno del parser)
        EndOfFile  ///< Fin de archivo
    };

    /**
     * @struct Token
     * @brief Unidad minima de informacion producida por el lexer.
     *
     * Cada token almacena su tipo, el texto original del codigo fuente y la
     * posicion (linea y columna) donde fue reconocido, lo que permite mensajes
     * de error precisos durante el parseo.
     */
    struct Token {
        TokenType   type   = TokenType::EndOfFile; ///< Tipo semantico del token.
        std::string lexeme;                        ///< Texto original tal como aparece en la fuente.
        int         line   = 0;                    ///< Linea del codigo fuente (empieza en 1).
        int         column = 0;                    ///< Columna del codigo fuente (empieza en 1).

        /**
         * @brief Construye un token con todos sus campos.
         * @param t   Tipo de token.
         * @param l   Lexema (texto original).
         * @param ln  Linea en el fuente.
         * @param col Columna en el fuente.
         */
        Token(TokenType t, std::string l, int ln, int col)
            : type(t), lexeme(std::move(l)), line(ln), column(col) {}
    };

    /**
     * @class Lexer
     * @brief Convierte codigo fuente .vel en una secuencia de tokens.
     *
     * Mantiene internamente un cursor sobre el texto fuente y produce tokens
     * uno a uno mediante @c next_token().  Tambien permite inspeccionar el
     * siguiente token sin consumirlo (@c peek_token()).
     *
     * Caracteres de comentario: el lexer reconoce comentarios de linea que
     * comienzan con @c ; y los emite como tokens @c COMMENT (no los descarta).
     *
     * @note El lexer no lanza excepciones directamente; notifica errores mediante
     *       el metodo privado @c error() que imprime el diagnostico en @c stderr.
     */
    class Lexer {
    public:
        /**
         * @brief Numero de linea fuente Vex marcada por el ultimo
         *        comentario `// @line N` visto por el lexer.
         *
         * El IR emitter puede emitir comentarios `// @line N` antes de
         * cada instruccion para indicar la linea Vex que la genero
         * (ver opt CompileOptions::emit_debug).  El lexer NORMALMENTE
         * los descarta como comentarios, pero `skip_whitespace` actualiza
         * este campo cuando los detecta.  El parser puede leerlo tras
         * cada `next_token()` para asociar la instruccion entrante a la
         * linea Vex correcta y poblar la tabla de debug del .velb.
         *
         * 0 = no hay info de linea para el siguiente token.
         */
        int last_src_line = 0;

        /**
         * @brief Path del archivo fuente Vex marcado por el comentario
         *        `// @file <path>` (tipicamente al inicio del .vel).
         *
         * El compilador Vex inserta esa linea en el .vel cuando
         * --vex-debug esta activo, para que el linker sepa que archivo
         * es la fuente original.  El lexer captura el primer match en
         * skip_whitespace.
         */
        std::string last_src_file;

    private:
        std::string source;     ///< Codigo fuente completo almacenado en memoria.
        size_t      pos    = 0; ///< Posicion actual del cursor en @c source.
        int         line   = 1; ///< Linea actual (comienza en 1).
        int         column = 1; ///< Columna actual (comienza en 1).

        /**
         * @brief Devuelve el caracter actual sin avanzar el cursor.
         * @return Caracter en la posicion actual, o @c '\0' al llegar al final.
         */
        char peek() const;

        /**
         * @brief Devuelve el caracter actual y avanza el cursor una posicion.
         *
         * Actualiza @c line y @c column correctamente al encontrar '\n'.
         *
         * @return Caracter consumido.
         */
        char advance();

        /**
         * @brief Salta espacios en blanco y tabulaciones (pero no saltos de linea).
         */
        void skip_whitespace();

        /**
         * @brief Reporta un error lexico con informacion de posicion.
         * @param tok Token donde se detecto el error (proporciona linea/columna).
         * @param msg Descripcion del error.
         */
        void error(const Token &tok, const std::string &msg);

        /* Constructor privado para peek_token: duplica el estado del lexer sin consumir. */
        Lexer(std::string src, size_t p, int ln, int col)
            : source(std::move(src)), pos(p), line(ln), column(col) {}

    public:
        /**
         * @brief Construye el lexer con el codigo fuente a analizar.
         * @param src Cadena con el contenido completo del archivo .vel.
         */
        explicit Lexer(std::string src);

        /**
         * @brief Produce y consume el siguiente token del codigo fuente.
         *
         * Avanza el cursor tantas posiciones como ocupe el token reconocido.
         * Devuelve @c Token{EndOfFile} al llegar al final del codigo.
         *
         * @return Token siguiente en la secuencia.
         */
        Token next_token();

        /**
         * @brief Devuelve el caracter situado @p offset posiciones por delante del cursor.
         *
         * No modifica el estado del lexer.
         *
         * @param offset Numero de posiciones a adelantar para el vistazo.
         * @return Caracter en esa posicion, o @c '\0' si esta fuera del rango.
         */
        char peek_token(int offset) const;

        /**
         * @brief Devuelve el siguiente token sin consumirlo (lookahead de 1 token).
         *
         * Internamente crea una copia temporal del estado del lexer, produce un
         * token con ella y descarta la copia.  El cursor del lexer original no avanza.
         *
         * @return Token que seria devuelto por la proxima llamada a @c next_token().
         */
        [[nodiscard]] Token peek_token() const;
    };

    /**
     * @brief Convierte un @c TokenType a su representacion textual para depuracion.
     * @param type Tipo de token.
     * @return Cadena con el nombre del tipo (p.ej. "NUMBER_HEX", "REGISTER", "COMMA").
     */
    std::string token_type_to_string(vm::TokenType type);

} // namespace vm

#endif // LEXER_H
