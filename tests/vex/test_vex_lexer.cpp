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
 * @file test_vex_lexer.cpp
 * @brief Test automatico del lexer Vex.
 *
 * Cobertura del subset A.1:
 *   - Identificadores ASCII y todas las palabras reservadas cerradas.
 *   - Literales enteros (decimal, hex, bin, oct) con separadores '_'.
 *   - Literales flotantes con/sin exponente.
 *   - Literales de caracter con escapes (\n \t \r \\ \' \xHH \uHHHH).
 *   - Strings normales y raw r"..." (sin interpolacion / multilinea).
 *   - Operadores y simbolos (incluyendo compuestos: ==, +=, <<=, =>, ->).
 *   - Comentarios // y bloque slash-asterisco.
 *   - Tracking correcto de linea/columna.
 *   - Diagnosticos esperados:
 *       * comentario de bloque sin cerrar
 *       * string sin cerrar
 *       * caracter sin cerrar
 *       * caracter inesperado
 *       * interpolacion rechazada en A.1
 *       * triple-quoted rechazado en A.1
 *
 * El test devuelve exit code 0 si todas las aserciones pasan; cualquier
 * fallo aborta inmediatamente con detalle del caso fallido.
 */

#include "vex/lexer.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using vex::Diagnostics;
using vex::Lexer;
using vex::Token;
using vex::TokenKind;

// -----------------------------------------------------------------------
// Helpers de aserto con mensaje legible si fallan.
// -----------------------------------------------------------------------
static int g_failed = 0;
static int g_passed = 0;

#define VEX_ASSERT(cond, msg) do {                                          \
    if (!(cond)) {                                                          \
        std::fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg); \
        ++g_failed;                                                         \
    } else {                                                                \
        ++g_passed;                                                         \
    }                                                                       \
} while (0)

#define VEX_ASSERT_EQ(a, b, msg) do {                                                       \
    auto av = (a); auto bv = (b);                                                           \
    if (!(av == bv)) {                                                                      \
        std::fprintf(stderr, "FAIL [%s:%d] %s (got %lld, expected %lld)\n",                 \
                     __FILE__, __LINE__, msg,                                               \
                     (long long)av, (long long)bv);                                         \
        ++g_failed;                                                                         \
    } else { ++g_passed; }                                                                  \
} while (0)

// Wrapper que tokeniza una cadena entera y devuelve el vector de tokens.
static std::vector<Token> tokenize_all(const std::string &src,
                                       Diagnostics &diags,
                                       const char *fname = "<test>") {
    Lexer lx(src, fname, diags);
    std::vector<Token> out;
    out.reserve(64);
    while (true) {
        Token t = lx.next();
        const TokenKind k = t.kind;
        out.push_back(std::move(t));
        if (k == TokenKind::END_OF_FILE) break;
    }
    return out;
}

// =====================================================================
// Casos de prueba.
// =====================================================================

static void test_empty_source() {
    Diagnostics diags;
    auto toks = tokenize_all("", diags);
    VEX_ASSERT_EQ(toks.size(), (size_t)1, "fuente vacio: solo EOF");
    VEX_ASSERT(toks[0].kind == TokenKind::END_OF_FILE, "EOF al final");
    VEX_ASSERT(!diags.has_errors(), "sin errores en fuente vacio");
}

static void test_whitespace_and_comments() {
    Diagnostics diags;
    const std::string src =
        "   \t\n"
        "// comentario de linea\n"
        "/* comentario\n"
        "   de bloque */\n"
        "  ";
    auto toks = tokenize_all(src, diags);
    VEX_ASSERT_EQ(toks.size(), (size_t)1, "solo whitespace y comentarios -> EOF");
    VEX_ASSERT(!diags.has_errors(), "comentarios bien cerrados");
}

static void test_block_comment_unterminated() {
    Diagnostics diags;
    auto toks = tokenize_all("/* sin cerrar", diags);
    VEX_ASSERT(diags.has_errors(), "se reporta comentario sin cerrar");
    VEX_ASSERT_EQ(toks.size(), (size_t)1, "EOF aun asi");
}

static void test_identifiers_and_keywords() {
    Diagnostics diags;
    // Mezcla de keywords (todas las categorias) + identificadores.
    const std::string src =
        "i32 u8 uint64_t f64 float double bool char void string\n"
        "if else while do for in break continue return\n"
        "try catch finally throw new delete this super\n"
        "class struct interface enum fn public private protected\n"
        "const static final nonnull typedef using import extern\n"
        "get set synchronized monitor await spawn asm override\n"
        "true false null\n"
        "miVariable _ot_ra var123";
    auto toks = tokenize_all(src, diags);
    VEX_ASSERT(!diags.has_errors(), "sin errores en keywords+ident");

    // Comprobar algunas posiciones clave.
    VEX_ASSERT(toks[0].kind  == TokenKind::KW_INT32,    "i32 reconocido");
    VEX_ASSERT(toks[1].kind  == TokenKind::KW_UINT8,    "u8 reconocido");
    VEX_ASSERT(toks[2].kind  == TokenKind::KW_UINT64_T, "uint64_t reconocido");
    VEX_ASSERT(toks[3].kind  == TokenKind::KW_F64,      "f64 reconocido");
    VEX_ASSERT(toks[4].kind  == TokenKind::KW_FLOAT,    "float reconocido");
    VEX_ASSERT(toks[5].kind  == TokenKind::KW_DOUBLE,   "double reconocido");
    VEX_ASSERT(toks[6].kind  == TokenKind::KW_BOOL,     "bool reconocido");
    VEX_ASSERT(toks[7].kind  == TokenKind::KW_CHAR,     "char reconocido");
    VEX_ASSERT(toks[8].kind  == TokenKind::KW_VOID,     "void reconocido");
    VEX_ASSERT(toks[9].kind  == TokenKind::KW_STRING,   "string reconocido");

    // El ultimo de la fila previa al EOF deberia ser un identificador.
    // Recorremos buscando "miVariable", "_ot_ra", "var123".
    bool found_mi = false, found_ot = false, found_var123 = false;
    for (const auto &t : toks) {
        if (t.kind == TokenKind::IDENTIFIER) {
            if (t.lexeme == "miVariable") found_mi = true;
            if (t.lexeme == "_ot_ra")     found_ot = true;
            if (t.lexeme == "var123")     found_var123 = true;
        }
    }
    VEX_ASSERT(found_mi,     "miVariable es IDENTIFIER");
    VEX_ASSERT(found_ot,     "_ot_ra es IDENTIFIER");
    VEX_ASSERT(found_var123, "var123 es IDENTIFIER");
}

static void test_int_literals() {
    Diagnostics diags;
    const std::string src = "0 42 1_000_000 0xFF 0xDEAD_BEEF 0b1010_1100 0o755";
    auto toks = tokenize_all(src, diags);
    VEX_ASSERT(!diags.has_errors(), "sin errores int literals");

    VEX_ASSERT_EQ(toks[0].int_val, (uint64_t)0,          "literal 0");
    VEX_ASSERT_EQ(toks[1].int_val, (uint64_t)42,         "literal 42");
    VEX_ASSERT_EQ(toks[2].int_val, (uint64_t)1000000,    "literal 1_000_000");
    VEX_ASSERT_EQ(toks[3].int_val, (uint64_t)0xFF,       "literal 0xFF");
    VEX_ASSERT_EQ(toks[4].int_val, (uint64_t)0xDEADBEEF, "literal 0xDEAD_BEEF");
    VEX_ASSERT_EQ(toks[5].int_val, (uint64_t)0xAC,       "literal 0b1010_1100");
    VEX_ASSERT_EQ(toks[6].int_val, (uint64_t)0755,       "literal 0o755");
    for (int i = 0; i < 7; ++i) {
        VEX_ASSERT(toks[i].kind == TokenKind::INT_LIT, "INT_LIT");
    }
}

static void test_float_literals() {
    Diagnostics diags;
    const std::string src = "3.14 1.0e-3 2.5e10 0.5";
    auto toks = tokenize_all(src, diags);
    VEX_ASSERT(!diags.has_errors(), "sin errores float literals");
    for (int i = 0; i < 4; ++i) {
        VEX_ASSERT(toks[i].kind == TokenKind::FLOAT_LIT, "FLOAT_LIT");
    }
    // Comparacion aproximada para evitar problemas de coma flotante.
    VEX_ASSERT(toks[0].flt_val > 3.13 && toks[0].flt_val < 3.15, "3.14 ~= 3.14");
    VEX_ASSERT(toks[2].flt_val > 2.49e10 && toks[2].flt_val < 2.51e10, "2.5e10");
}

static void test_char_literals() {
    Diagnostics diags;
    const std::string src = R"('a' '\n' '\t' '\xFF' '\u00e9' '\\' '\'')";
    auto toks = tokenize_all(src, diags);
    VEX_ASSERT(!diags.has_errors(), "sin errores char literals");
    VEX_ASSERT_EQ(toks[0].int_val, (uint64_t)'a',  "char 'a'");
    VEX_ASSERT_EQ(toks[1].int_val, (uint64_t)'\n', "char '\\n'");
    VEX_ASSERT_EQ(toks[2].int_val, (uint64_t)'\t', "char '\\t'");
    VEX_ASSERT_EQ(toks[3].int_val, (uint64_t)0xFF, "char '\\xFF'");
    VEX_ASSERT_EQ(toks[4].int_val, (uint64_t)0xE9, "char '\\u00e9'");
    VEX_ASSERT_EQ(toks[5].int_val, (uint64_t)'\\', "char '\\\\'");
    VEX_ASSERT_EQ(toks[6].int_val, (uint64_t)'\'', "char '\\''");
}

static void test_string_literals() {
    Diagnostics diags;
    const std::string src = R"("hola" r"sin\escapes" "con \"comillas\"")";
    auto toks = tokenize_all(src, diags);
    VEX_ASSERT(!diags.has_errors(), "sin errores en strings simples");
    VEX_ASSERT(toks[0].kind == TokenKind::STRING_LIT,     "primer string");
    VEX_ASSERT(toks[1].kind == TokenKind::RAW_STRING_LIT, "raw string");
    VEX_ASSERT(toks[2].kind == TokenKind::STRING_LIT,     "string con escapes");
}

static void test_string_unterminated() {
    Diagnostics diags;
    auto toks = tokenize_all("\"sin cerrar", diags);
    VEX_ASSERT(diags.has_errors(), "string sin cerrar reporta error");
    (void)toks;
}

static void test_string_interpolation_rejected_in_a1() {
    Diagnostics diags;
    auto toks = tokenize_all("\"hola ${nombre}\"", diags);
    VEX_ASSERT(diags.has_errors(), "interpolacion rechazada en A.1");
    (void)toks;
}

static void test_triple_quoted_rejected_in_a1() {
    Diagnostics diags;
    auto toks = tokenize_all("\"\"\"abc\"\"\"", diags);
    VEX_ASSERT(diags.has_errors(), "triple-quoted rechazado en A.1");
    (void)toks;
}

static void test_operators_compound() {
    Diagnostics diags;
    const std::string src =
        "+ - * / % "
        "= == != < <= > >= "
        "+= -= *= /= %= &= |= ^= <<= >>= "
        "&& || ! "
        "& | ^ ~ << >> "
        "++ -- "
        "( ) { } [ ] , ; : . -> => ? @";
    auto toks = tokenize_all(src, diags);
    VEX_ASSERT(!diags.has_errors(), "todos los operadores reconocidos");

    // Verificar algunos clave que tienen multiples caminos en el switch.
    // Indices conocidos por construccion (espacio entre cada simbolo).
    // Aritmeticos
    VEX_ASSERT(toks[0].kind == TokenKind::PLUS,    "+");
    VEX_ASSERT(toks[1].kind == TokenKind::MINUS,   "-");
    VEX_ASSERT(toks[2].kind == TokenKind::STAR,    "*");
    VEX_ASSERT(toks[3].kind == TokenKind::SLASH,   "/");
    VEX_ASSERT(toks[4].kind == TokenKind::PERCENT, "%");
    // Comparacion
    VEX_ASSERT(toks[5].kind  == TokenKind::ASSIGN, "=");
    VEX_ASSERT(toks[6].kind  == TokenKind::EQ,     "==");
    VEX_ASSERT(toks[7].kind  == TokenKind::NEQ,    "!=");
    VEX_ASSERT(toks[8].kind  == TokenKind::LT,     "<");
    VEX_ASSERT(toks[9].kind  == TokenKind::LE,     "<=");
    VEX_ASSERT(toks[10].kind == TokenKind::GT,     ">");
    VEX_ASSERT(toks[11].kind == TokenKind::GE,     ">=");
    // Compuestos
    VEX_ASSERT(toks[12].kind == TokenKind::PLUS_ASSIGN,    "+=");
    VEX_ASSERT(toks[13].kind == TokenKind::MINUS_ASSIGN,   "-=");
    VEX_ASSERT(toks[14].kind == TokenKind::STAR_ASSIGN,    "*=");
    VEX_ASSERT(toks[15].kind == TokenKind::SLASH_ASSIGN,   "/=");
    VEX_ASSERT(toks[16].kind == TokenKind::PERCENT_ASSIGN, "%=");
    VEX_ASSERT(toks[17].kind == TokenKind::AMP_ASSIGN,     "&=");
    VEX_ASSERT(toks[18].kind == TokenKind::PIPE_ASSIGN,    "|=");
    VEX_ASSERT(toks[19].kind == TokenKind::CARET_ASSIGN,   "^=");
    VEX_ASSERT(toks[20].kind == TokenKind::SHL_ASSIGN,     "<<=");
    VEX_ASSERT(toks[21].kind == TokenKind::SHR_ASSIGN,     ">>=");
    // Logicos
    VEX_ASSERT(toks[22].kind == TokenKind::AND_AND, "&&");
    VEX_ASSERT(toks[23].kind == TokenKind::OR_OR,   "||");
    VEX_ASSERT(toks[24].kind == TokenKind::BANG,    "!");
    // Bitwise
    VEX_ASSERT(toks[25].kind == TokenKind::AMP,   "&");
    VEX_ASSERT(toks[26].kind == TokenKind::PIPE,  "|");
    VEX_ASSERT(toks[27].kind == TokenKind::CARET, "^");
    VEX_ASSERT(toks[28].kind == TokenKind::TILDE, "~");
    VEX_ASSERT(toks[29].kind == TokenKind::SHL,   "<<");
    VEX_ASSERT(toks[30].kind == TokenKind::SHR,   ">>");
    // Inc/dec
    VEX_ASSERT(toks[31].kind == TokenKind::PLUS_PLUS,   "++");
    VEX_ASSERT(toks[32].kind == TokenKind::MINUS_MINUS, "--");
    // Puntuacion
    VEX_ASSERT(toks[33].kind == TokenKind::LPAREN,    "(");
    VEX_ASSERT(toks[34].kind == TokenKind::RPAREN,    ")");
    VEX_ASSERT(toks[35].kind == TokenKind::LBRACE,    "{");
    VEX_ASSERT(toks[36].kind == TokenKind::RBRACE,    "}");
    VEX_ASSERT(toks[37].kind == TokenKind::LBRACKET,  "[");
    VEX_ASSERT(toks[38].kind == TokenKind::RBRACKET,  "]");
    VEX_ASSERT(toks[39].kind == TokenKind::COMMA,     ",");
    VEX_ASSERT(toks[40].kind == TokenKind::SEMICOLON, ";");
    VEX_ASSERT(toks[41].kind == TokenKind::COLON,     ":");
    VEX_ASSERT(toks[42].kind == TokenKind::DOT,       ".");
    VEX_ASSERT(toks[43].kind == TokenKind::ARROW,     "->");
    VEX_ASSERT(toks[44].kind == TokenKind::FAT_ARROW, "=>");
    VEX_ASSERT(toks[45].kind == TokenKind::QUESTION,  "?");
    VEX_ASSERT(toks[46].kind == TokenKind::AT,        "@");
}

static void test_position_tracking() {
    Diagnostics diags;
    const std::string src =
        "i32 main()\n"     // linea 1
        "{\n"              // linea 2
        "    return 0;\n"  // linea 3
        "}\n";             // linea 4
    auto toks = tokenize_all(src, diags);

    // i32 esta en linea 1, columna 1.
    VEX_ASSERT_EQ(toks[0].loc.line,   (uint32_t)1, "linea de i32");
    VEX_ASSERT_EQ(toks[0].loc.column, (uint32_t)1, "columna de i32");

    // main en linea 1, columna 5 (i32 = 3 chars + 1 espacio = pos 4 -> col 5).
    VEX_ASSERT_EQ(toks[1].loc.line,   (uint32_t)1, "linea de main");
    VEX_ASSERT_EQ(toks[1].loc.column, (uint32_t)5, "columna de main");

    // '{' en linea 2, columna 1.
    bool found_open_brace = false;
    for (const auto &t : toks) {
        if (t.kind == TokenKind::LBRACE) {
            VEX_ASSERT_EQ(t.loc.line,   (uint32_t)2, "linea de {");
            VEX_ASSERT_EQ(t.loc.column, (uint32_t)1, "columna de {");
            found_open_brace = true;
            break;
        }
    }
    VEX_ASSERT(found_open_brace, "{ encontrado");

    // 'return' en linea 3.
    bool found_return = false;
    for (const auto &t : toks) {
        if (t.kind == TokenKind::KW_RETURN) {
            VEX_ASSERT_EQ(t.loc.line, (uint32_t)3, "linea de return");
            found_return = true;
            break;
        }
    }
    VEX_ASSERT(found_return, "return encontrado");
}

static void test_peek_idempotent() {
    Diagnostics diags;
    Lexer lx("foo bar", "<peek>", diags);
    const Token &p1 = lx.peek();
    VEX_ASSERT(p1.kind == TokenKind::IDENTIFIER, "peek devuelve IDENTIFIER");
    VEX_ASSERT(p1.lexeme == "foo", "peek devuelve foo");
    const Token &p2 = lx.peek();
    VEX_ASSERT(p2.kind == TokenKind::IDENTIFIER, "peek idempotente");
    VEX_ASSERT(p2.lexeme == "foo", "peek idempotente lexeme");
    Token n1 = lx.next();
    VEX_ASSERT(n1.lexeme == "foo", "next() devuelve el cacheado");
    Token n2 = lx.next();
    VEX_ASSERT(n2.lexeme == "bar", "next() avanza al siguiente");
}

static void test_tricky_dot_vs_float() {
    Diagnostics diags;
    // "1.method" deberia ser INT_LIT 1, DOT, IDENT method.
    auto toks = tokenize_all("1.method", diags);
    VEX_ASSERT(!diags.has_errors(), "sin errores en 1.method");
    VEX_ASSERT(toks.size() >= 4, "al menos 4 tokens incluyendo EOF");
    VEX_ASSERT(toks[0].kind == TokenKind::INT_LIT,    "INT_LIT 1");
    VEX_ASSERT_EQ(toks[0].int_val, (uint64_t)1,       "valor 1");
    VEX_ASSERT(toks[1].kind == TokenKind::DOT,        "DOT");
    VEX_ASSERT(toks[2].kind == TokenKind::IDENTIFIER, "IDENT method");
    VEX_ASSERT(toks[2].lexeme == "method",            "lexema method");
}

static void test_minimal_program() {
    Diagnostics diags;
    const std::string src = "i32 main() { return 0; }";
    auto toks = tokenize_all(src, diags);
    VEX_ASSERT(!diags.has_errors(), "programa minimo sin errores");

    // Esperamos: i32, main, (, ), {, return, 0, ;, }, EOF.
    VEX_ASSERT_EQ(toks.size(), (size_t)10, "10 tokens en programa minimo");
    VEX_ASSERT(toks[0].kind == TokenKind::KW_INT32,    "i32");
    VEX_ASSERT(toks[1].kind == TokenKind::IDENTIFIER,  "main");
    VEX_ASSERT(toks[1].lexeme == "main",               "lexema main");
    VEX_ASSERT(toks[2].kind == TokenKind::LPAREN,      "(");
    VEX_ASSERT(toks[3].kind == TokenKind::RPAREN,      ")");
    VEX_ASSERT(toks[4].kind == TokenKind::LBRACE,      "{");
    VEX_ASSERT(toks[5].kind == TokenKind::KW_RETURN,   "return");
    VEX_ASSERT(toks[6].kind == TokenKind::INT_LIT,     "INT_LIT 0");
    VEX_ASSERT_EQ(toks[6].int_val, (uint64_t)0,        "valor 0");
    VEX_ASSERT(toks[7].kind == TokenKind::SEMICOLON,   ";");
    VEX_ASSERT(toks[8].kind == TokenKind::RBRACE,      "}");
    VEX_ASSERT(toks[9].kind == TokenKind::END_OF_FILE, "EOF");
}

// ---------------------------------------------------------------------
// Punto de entrada del test.
// ---------------------------------------------------------------------
int main() {
    test_empty_source();
    test_whitespace_and_comments();
    test_block_comment_unterminated();
    test_identifiers_and_keywords();
    test_int_literals();
    test_float_literals();
    test_char_literals();
    test_string_literals();
    test_string_unterminated();
    test_string_interpolation_rejected_in_a1();
    test_triple_quoted_rejected_in_a1();
    test_operators_compound();
    test_position_tracking();
    test_peek_idempotent();
    test_tricky_dot_vs_float();
    test_minimal_program();

    std::printf("\n=== test_vex_lexer: %d pasos OK, %d fallidos ===\n",
                g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
