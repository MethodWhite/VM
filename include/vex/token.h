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
 * @file token.h
 * @brief Tipos de token y estructura Token para el frontend Vex.
 *
 * El conjunto de TokenKind declarado aqui cubre TODA la sintaxis cerrada
 * de Vex.  Esto evita renombres y reordenamientos del enum (que
 * invalidarian tablas planas indexadas por ordinal en el parser) cuando
 * se anyaden caracteristicas nuevas: cualquier futuro feature reusa
 * tokens existentes o anyade nuevos al final.
 *
 * Reglas de diseno:
 *  - El enum se mantiene compacto (uint16_t suficiente).
 *  - Las palabras reservadas se distinguen del IDENTIFIER en el lexer
 *    via tabla hash perfecta (string -> TokenKind) construida al
 *    iniciar el lexer.
 *  - El Token guarda lexema + posicion + valores pre-parseados para
 *    literales numericos (entero y flotante) y para el "modo" del
 *    string literal (estandar / raw / multilinea), evitando reparsear
 *    en el parser.
 *  - Los strings con interpolacion ${...} se modelan como una secuencia
 *    de tokens: ISTR_BEGIN, ISTR_TEXT, ISTR_EXPR_BEGIN, [tokens internos],
 *    ISTR_EXPR_END, [...], ISTR_END.  Mismo enfoque que VSH.
 */

#ifndef VEX_TOKEN_H
#define VEX_TOKEN_H

#include <cstdint>
#include <string>
#include <utility>

#include "vex/diagnostic.h"

namespace vex {

    /**
     * @enum TokenKind
     * @brief Categorias semanticas de los tokens del lexer Vex.
     *
     * Los valores ordinales son estables: el parser y el reportador de
     * errores los usan como indice en tablas planas (operator precedence,
     * "expected after X" messages, etc.).  No reordenar entradas; al
     * anyadir nuevas hacerlo SIEMPRE al final del bloque correspondiente.
     */
    enum class TokenKind : uint16_t {
        // ---------------------------------------------------------------
        // Categoria 0: control de flujo del lexer/parser.
        // ---------------------------------------------------------------
        END_OF_FILE = 0,    ///< Fin de fichero.
        UNKNOWN,            ///< Caracter no reconocido (acompanya a un diagnostico).

        // ---------------------------------------------------------------
        // Categoria 1: literales.
        // ---------------------------------------------------------------
        INT_LIT,            ///< Literal entero ya parseado en Token::int_val.
        FLOAT_LIT,          ///< Literal flotante ya parseado en Token::flt_val.
        CHAR_LIT,           ///< Literal de caracter.  El valor codepoint va en int_val.
        STRING_LIT,         ///< Cadena estandar simple sin interpolacion (resuelta).
        RAW_STRING_LIT,     ///< Cadena raw r"..." (sin escapes ni interpolacion).
        TRUE_KW,            ///< Literal booleano true.
        FALSE_KW,           ///< Literal booleano false.
        NULL_KW,            ///< Literal null.

        // ---------------------------------------------------------------
        // Categoria 2: tokens de string interpolado (multi-token).
        //
        // La secuencia para "hola ${nombre}!"  es:
        //   ISTR_BEGIN (sentinel), ISTR_TEXT("hola "), ISTR_EXPR_BEGIN,
        //   IDENTIFIER("nombre"), ISTR_EXPR_END, ISTR_TEXT("!"), ISTR_END.
        // ---------------------------------------------------------------
        ISTR_BEGIN,         ///< Inicio de string interpolado.
        ISTR_TEXT,          ///< Fragmento literal entre interpolaciones.
        ISTR_EXPR_BEGIN,    ///< Apertura de la expresion ${.
        ISTR_EXPR_FMT,      ///< Especificador de formato tras `:` dentro
                            ///< de `${expr:fmt}`.  El texto del formato
                            ///< va en @c str_val sin tokenizar (raw).
        ISTR_EXPR_END,      ///< Cierre de la expresion }.
        ISTR_END,           ///< Fin de string interpolado.

        // ---------------------------------------------------------------
        // Categoria 3: identificadores.
        // ---------------------------------------------------------------
        IDENTIFIER,         ///< Nombre de variable, funcion, tipo, etc.

        // ---------------------------------------------------------------
        // Categoria 4: palabras reservadas (tipos primitivos).
        // Aliases: ambos estilos (uint8_t y u8) son tokens distintos
        // pero se mapean al mismo IrType en el type checker.
        // ---------------------------------------------------------------
        KW_VOID,
        KW_BOOL,
        KW_CHAR,
        KW_INT8,            ///< i8
        KW_INT16,           ///< i16
        KW_INT32,           ///< i32
        KW_INT64,           ///< i64
        KW_UINT8,           ///< u8
        KW_UINT16,          ///< u16
        KW_UINT32,          ///< u32
        KW_UINT64,          ///< u64
        KW_INT8_T,          ///< int8_t
        KW_INT16_T,         ///< int16_t
        KW_INT32_T,         ///< int32_t
        KW_INT64_T,         ///< int64_t
        KW_UINT8_T,         ///< uint8_t
        KW_UINT16_T,        ///< uint16_t
        KW_UINT32_T,        ///< uint32_t
        KW_UINT64_T,        ///< uint64_t
        KW_F32,             ///< f32
        KW_F64,             ///< f64
        KW_FLOAT,           ///< float
        KW_DOUBLE,          ///< double
        KW_STRING,          ///< string  (clase managed)
        // tipos primitivos de coleccion (PrimitiveKind::ARRAYLIST etc.).
        // Cada uno es un GcHandle opaco al descriptor del plugin
        // vesta_collections.dll; el frontend lo trata como primitivo (sin
        // vtable, sin CALLVIRT) y libera automaticamente al exit del scope.
        KW_ARRAYLIST,
        KW_HASHMAP,
        KW_HASHSET,
        KW_QUEUE,
        KW_DEQUE,
        KW_TREEMAP,
        KW_TREESET,
        KW_STACK,
        // Smart pointers builtins (move-only + refcount).
        KW_UNIQUE,          ///< unique<T> (smart pointer move-only)
        KW_SHARED,          ///< shared<T> (smart pointer con refcount)
        KW_BORROW,          ///< borrow<T> (shared borrow, immutable)
        KW_BORROW_MUT,      ///< borrow_mut<T> (exclusive borrow, mutable)

        // ---------------------------------------------------------------
        // Categoria 5: palabras reservadas de declaracion.
        // ---------------------------------------------------------------
        KW_CONST,
        KW_STATIC,
        KW_FINAL,
        KW_NONNULL,
        KW_TYPEDEF,
        KW_USING,
        KW_STRUCT,
        KW_CLASS,
        KW_INTERFACE,
        KW_ENUM,
        KW_FN,              ///< Tipo de funcion: fn(T) -> R
        KW_PUBLIC,
        KW_PRIVATE,
        KW_PROTECTED,
        KW_IMPORT,
        KW_EXTERN,
        KW_GET,
        KW_SET,

        // ---------------------------------------------------------------
        // Categoria 6: control de flujo.
        // ---------------------------------------------------------------
        KW_IF,
        KW_ELSE,
        KW_WHILE,
        KW_DO,
        KW_FOR,
        KW_IN,              ///< for (x in col)
        KW_BREAK,
        KW_CONTINUE,
        KW_GOTO,
        KW_RETURN,
        KW_TRY,
        KW_CATCH,
        KW_FINALLY,
        KW_THROW,
        KW_NEW,
        KW_DELETE,
        KW_THIS,
        KW_SUPER,

        // ---------------------------------------------------------------
        // Categoria 7: concurrencia y meta.
        // ---------------------------------------------------------------
        KW_SYNCHRONIZED,
        KW_MONITOR,
        KW_AWAIT,
        KW_SPAWN,
        KW_RSPAWN,          ///< rspawn(node) { body } -> Future remoto
        KW_ASM,
        KW_OVERRIDE,        ///< Tambien aceptado como anotacion @Override.
        KW_MATCH,           ///< match expr { case Variant(payload) => ... } (destructuring de ADT).
        KW_CASE,            ///< Rama de un @c match.  Tambien reservado para futuras extensiones tipo switch.

        // ---------------------------------------------------------------
        // Categoria 8: operadores y simbolos.
        // ---------------------------------------------------------------
        // Aritmeticos
        PLUS,               ///< +
        MINUS,              ///< -
        STAR,               ///< *
        SLASH,              ///< /
        PERCENT,            ///< %

        // Asignacion
        ASSIGN,             ///< =
        PLUS_ASSIGN,        ///< +=
        MINUS_ASSIGN,       ///< -=
        STAR_ASSIGN,        ///< *=
        SLASH_ASSIGN,       ///< /=
        PERCENT_ASSIGN,     ///< %=
        AMP_ASSIGN,         ///< &=
        PIPE_ASSIGN,        ///< |=
        CARET_ASSIGN,       ///< ^=
        SHL_ASSIGN,         ///< <<=
        SHR_ASSIGN,         ///< >>=

        // Comparacion
        EQ,                 ///< ==
        NEQ,                ///< !=
        LT,                 ///< <
        LE,                 ///< <=
        GT,                 ///< >
        GE,                 ///< >=

        // Logicos
        AND_AND,            ///< &&
        OR_OR,              ///< ||
        BANG,               ///< !
        BANG_BANG,          ///< !! operador prefijo de unwrap sobre @c Optional<T>.

        // Bitwise
        AMP,                ///< &
        PIPE,               ///< |
        CARET,              ///< ^
        TILDE,              ///< ~
        SHL,                ///< <<
        SHR,                ///< >>

        // Increment/decrement
        PLUS_PLUS,          ///< ++
        MINUS_MINUS,        ///< --

        // Puntuacion
        LPAREN,             ///< (
        RPAREN,             ///< )
        LBRACE,             ///< {
        RBRACE,             ///< }
        LBRACKET,           ///< [
        RBRACKET,           ///< ]
        COMMA,              ///< ,
        SEMICOLON,          ///< ;
        COLON,              ///< :
        DOT,                ///< .
        DOTDOT,             ///< ..  (rango exclusivo en comptime for)
        DOTDOTEQ,           ///< ..= (rango inclusivo en comptime for)
        ARROW,              ///< ->
        FAT_ARROW,          ///< =>
        QUESTION,           ///< ?
        AT,                 ///< @ (prefijo de anotaciones)

        // ---------------------------------------------------------------
        // Sentinela (tamano del enum).  No usar como token real.
        // ---------------------------------------------------------------
        TOKEN_KIND_COUNT
    };

    /**
     * @brief Devuelve la representacion textual de un TokenKind para diagnosticos.
     *
     * Usa un switch interno que el compilador convierte en jump table; no
     * se asigna memoria.  Apto para hot path.
     *
     * @param k Tipo de token.
     * @return Cadena estatica (no liberar).
     */
    const char *token_kind_name(TokenKind k) noexcept;

    /**
     * @struct Token
     * @brief Unidad lexica producida por el lexer Vex.
     *
     * Layout disenyado para 64 bytes (cache line) en la mayoria de tokens
     * comunes, gracias a SSO en std::string para lexemas cortos.  Los
     * literales numericos llevan ademas su valor pre-parseado para evitar
     * que el parser tenga que reparsear cadenas como "0x1A2B".
     */
    struct Token {
        TokenKind   kind = TokenKind::END_OF_FILE; ///< Categoria del token.
        SourceLoc   loc;                            ///< Posicion en el fuente.
        std::string lexeme;                         ///< Texto original tal como aparece.

        // Valores pre-parseados para literales (zero-cost si no se usan).
        uint64_t    int_val = 0;   ///< Para INT_LIT y CHAR_LIT (sin signo).
        double      flt_val = 0.0; ///< Para FLOAT_LIT.
        std::string str_val;       ///< Para STRING_LIT y RAW_STRING_LIT (escapes ya resueltos).

        Token() = default;

        /**
         * @brief Constructor completo en una sola llamada.
         * @param k Categoria.
         * @param l Lexema (se mueve).
         * @param p Localizacion.
         */
        Token(TokenKind k, std::string l, SourceLoc p)
            : kind(k), loc(std::move(p)), lexeme(std::move(l)) {}
    };

} // namespace vex

#endif // VEX_TOKEN_H
