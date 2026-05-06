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
 * @file test_vex_typechecker.cpp
 * @brief Test del pase de comprobacion de tipos de Vex.
 *
 * Cobertura:
 *   - Programa correcto (compila limpio).
 *   - Forward refs (main llama a g declarada despues).
 *   - Aliases de tipos (uint32_t == u32).
 *   - Errores: nombre no declarado, redefinicion, return incompatible,
 *              llamada con aridad incorrecta, tipos incompatibles,
 *              asignacion a const, '%' sobre flotantes, etc.
 *   - Que el campo result_type queda relleno tras el check.
 */

#include "vex/lexer.h"
#include "vex/parser.h"
#include "vex/type_checker.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>

using vex::Diagnostics;
using vex::Lexer;
using vex::Parser;
using vex::PrimitiveKind;
using vex::TypeChecker;
namespace ast = vex::ast;

static int g_passed = 0;
static int g_failed = 0;

#define VEX_ASSERT(cond, msg) do {                                          \
    if (!(cond)) {                                                          \
        std::fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg); \
        ++g_failed;                                                         \
    } else { ++g_passed; }                                                  \
} while (0)

struct CheckOut {
    std::unique_ptr<ast::ModuleNode> mod;
    Diagnostics                       diags;
    bool                              ok = false;
};

static CheckOut check_src(const std::string &src) {
    CheckOut out;
    Lexer lx(src, "<test>", out.diags);
    Parser p(lx, out.diags);
    out.mod = p.parse_program();
    if (out.mod && !out.diags.has_errors()) {
        TypeChecker tc(*out.mod, out.diags);
        out.ok = tc.run();
    }
    return out;
}

template <typename T>
static const T *as_node(const ast::Node *n, ast::NodeKind k) {
    if (!n || n->kind != k) return nullptr;
    return static_cast<const T*>(n);
}

// =====================================================================
// Programas correctos.
// =====================================================================

static void test_minimal_ok() {
    auto out = check_src("i32 main() { return 0; }");
    VEX_ASSERT(out.ok, "minimal ok");
}

static void test_alias_styles_equivalent() {
    auto out = check_src(
        "u32 a = 1;\n"
        "uint32_t b = 2;\n"
        "i32 main() {\n"
        "    return a + b;\n"
        "}\n");
    VEX_ASSERT(out.ok, "u32 == uint32_t -> mismo tipo");
}

static void test_forward_reference() {
    // main llama a g antes de su declaracion.
    auto out = check_src(
        "i32 main() { return g(40); }\n"
        "i32 g(i32 x) { return x + 2; }\n"
    );
    VEX_ASSERT(out.ok, "forward ref ok");
}

static void test_result_type_filled() {
    auto out = check_src(
        "i32 main() {\n"
        "    return 1 + 2;\n"
        "}\n"
    );
    VEX_ASSERT(out.ok, "compila ok");
    auto fn  = as_node<ast::FunctionDecl>(out.mod->decls[0].get(), ast::NodeKind::FunctionDecl);
    auto ret = as_node<ast::ReturnStmt>(fn->body->body[0].get(), ast::NodeKind::ReturnStmt);
    auto add = as_node<ast::BinaryExpr>(ret->value.get(), ast::NodeKind::BinaryExpr);
    VEX_ASSERT(add != nullptr, "es Add");
    VEX_ASSERT(add->result_type.kind == PrimitiveKind::I64,
               "literales 1+2 promueven a i64");
}

static void test_typedef_alias_resolves() {
    // typedef hace que 'Edad' se resuelva como u32; pasarle un literal i64
    // es valido (promocion numerica).
    auto out = check_src(
        "typedef u32 Edad;\n"
        "i32 main() { Edad a = 18; return a; }\n"
    );
    VEX_ASSERT(out.ok, "typedef u32 Edad ok");
}

static void test_using_alias_resolves() {
    // using <nombre> = <tipo>; estilo C++.  Mismo efecto que typedef.
    auto out = check_src(
        "using MyInt = i64;\n"
        "i32 main() { MyInt n = 42; return n; }\n"
    );
    VEX_ASSERT(out.ok, "using MyInt = i64 ok");
}

static void test_alias_chain() {
    // Cadena de alias: typedef A B; typedef B C; usar C como i32.
    auto out = check_src(
        "typedef u32 A;\n"
        "typedef A   B;\n"
        "typedef B   C;\n"
        "i32 main() { C v = 7; return v; }\n"
    );
    VEX_ASSERT(out.ok, "cadena typedef A->B->C->u32 ok");
}

static void test_struct_decl_only() {
    // En A.3.1 solo se declara el struct; usarlo como tipo de variable
    // aun no esta cableado (eso llega en A.3.3).  La declaracion deberia
    // pasar el type checker sin error.
    auto out = check_src(
        "struct Punto { f64 x; f64 y; }\n"
        "i32 main() { return 0; }\n"
    );
    VEX_ASSERT(out.ok, "struct declarativo ok (sin uso)");
}

static void test_struct_layout_simple() {
    // Layout natural: f64 + f64 = 16 bytes, align 8.
    auto out = check_src(
        "struct Punto { f64 x; f64 y; }\n"
        "i32 main() { return 0; }\n"
    );
    VEX_ASSERT(out.ok, "struct Punto compila");
    // Dado que el TypeChecker se descarta tras run(), accedemos al
    // ModuleNode parsed para verificar que el StructDecl tiene 2 campos.
    bool found = false;
    for (auto &d : out.mod->decls) {
        if (d && d->kind == ast::NodeKind::StructDecl) {
            auto *s = static_cast<ast::StructDecl *>(d.get());
            if (s->name == "Punto" && s->fields.size() == 2) {
                found = true;
                VEX_ASSERT(s->fields[0].name == "x", "campo x");
                VEX_ASSERT(s->fields[1].name == "y", "campo y");
            }
        }
    }
    VEX_ASSERT(found, "Punto encontrado con sus campos");
}

static void test_struct_alias_combined() {
    // Combinar struct + alias: typedef sobre el nombre del struct.
    auto out = check_src(
        "struct Punto { f64 x; f64 y; }\n"
        "typedef Punto P;\n"
        "i32 main() { return 0; }\n"
    );
    VEX_ASSERT(out.ok, "alias sobre struct ok");
}

static void test_struct_field_dup_error() {
    auto out = check_src(
        "struct Bad { i32 x; i32 x; }\n"
        "i32 main() { return 0; }\n"
    );
    VEX_ASSERT(!out.ok, "campo duplicado detectado");
}

static void test_struct_redef_error() {
    auto out = check_src(
        "struct A { i32 x; }\n"
        "struct A { i32 y; }\n"
        "i32 main() { return 0; }\n"
    );
    VEX_ASSERT(!out.ok, "struct redefinido detectado");
}

static void test_complex_program_ok() {
    auto out = check_src(R"(
        u32 contador = 0;
        i32 fibonacci(i32 n) {
            if (n < 2) return n;
            return fibonacci(n - 1) + fibonacci(n - 2);
        }
        i32 main() {
            i32 i = 0;
            i32 total = 0;
            for (i = 0; i < 10; i = i + 1) {
                total = total + fibonacci(i);
            }
            return total;
        }
    )");
    VEX_ASSERT(out.ok, "programa fibonacci ok");
}

// =====================================================================
// Programas erroneos: cada test verifica que el checker DETECTA el error.
// =====================================================================

static void test_undeclared_name() {
    auto out = check_src("i32 main() { return foo; }");
    VEX_ASSERT(!out.ok, "nombre no declarado detectado");
}

static void test_redef_global() {
    auto out = check_src(
        "i32 a = 1;\n"
        "u8  a = 2;\n"
        "i32 main() { return 0; }\n"
    );
    VEX_ASSERT(!out.ok, "redefinicion de global detectada");
}

static void test_redef_local() {
    auto out = check_src(
        "i32 main() {\n"
        "    i32 x = 1;\n"
        "    i32 x = 2;\n"
        "    return x;\n"
        "}\n"
    );
    VEX_ASSERT(!out.ok, "redefinicion local detectada");
}

static void test_return_void_with_value() {
    auto out = check_src(
        "void f() { return 1; }\n"
        "i32 main() { return 0; }\n"
    );
    VEX_ASSERT(!out.ok, "return con valor en void detectado");
}

static void test_return_nonvoid_without_value() {
    auto out = check_src(
        "i32 f() { return; }\n"
        "i32 main() { return 0; }\n"
    );
    VEX_ASSERT(!out.ok, "return sin valor en no-void detectado");
}

static void test_call_arity() {
    auto out = check_src(
        "i32 g(i32 x, i32 y) { return x + y; }\n"
        "i32 main() { return g(1); }\n"
    );
    VEX_ASSERT(!out.ok, "aridad incorrecta detectada");
}

static void test_assign_to_const() {
    auto out = check_src(
        "i32 main() {\n"
        "    const i32 K = 10;\n"
        "    K = 20;\n"
        "    return K;\n"
        "}\n"
    );
    VEX_ASSERT(!out.ok, "asignacion a const detectada");
}

static void test_assign_to_function() {
    auto out = check_src(
        "i32 g() { return 0; }\n"
        "i32 main() { g = 1; return 0; }\n"
    );
    VEX_ASSERT(!out.ok, "asignacion a funcion detectada");
}

static void test_mod_on_floats() {
    auto out = check_src(
        "f64 main() { f64 a = 1.5; f64 b = 0.5; return a % b; }\n"
    );
    VEX_ASSERT(!out.ok, "% sobre flotantes detectado");
}

static void test_call_non_function() {
    auto out = check_src(
        "i32 main() {\n"
        "    i32 x = 1;\n"
        "    return x(1);\n"
        "}\n"
    );
    VEX_ASSERT(!out.ok, "llamada a no-funcion detectada");
}

static void test_bitwise_on_floats() {
    auto out = check_src(
        "f64 main() { f64 a = 1.0; return a & 1.0; }\n"
    );
    VEX_ASSERT(!out.ok, "& sobre flotantes detectado");
}

static void test_arithmetic_on_bool() {
    auto out = check_src(
        "i32 main() { bool b = true; return b + 1; }\n"
    );
    VEX_ASSERT(!out.ok, "aritmetica sobre bool detectada");
}

static void test_call_inside_call() {
    // Test que valida tanto el correcto encadenamiento como que las
    // promociones implicitas (i64 lit -> i32 param) son toleradas.
    auto out = check_src(
        "i32 dbl(i32 x) { return x + x; }\n"
        "i32 main() { return dbl(dbl(7)); }\n"
    );
    VEX_ASSERT(out.ok, "calls anidadas ok con promocion i64->i32");
}

// ---------------------------------------------------------------------
int main() {
    test_minimal_ok();
    test_alias_styles_equivalent();
    test_forward_reference();
    test_result_type_filled();
    test_typedef_alias_resolves();
    test_using_alias_resolves();
    test_alias_chain();
    test_struct_decl_only();
    test_struct_layout_simple();
    test_struct_alias_combined();
    test_struct_field_dup_error();
    test_struct_redef_error();
    test_complex_program_ok();

    test_undeclared_name();
    test_redef_global();
    test_redef_local();
    test_return_void_with_value();
    test_return_nonvoid_without_value();
    test_call_arity();
    test_assign_to_const();
    test_assign_to_function();
    test_mod_on_floats();
    test_call_non_function();
    test_bitwise_on_floats();
    test_arithmetic_on_bool();
    test_call_inside_call();

    std::printf("\n=== test_vex_typechecker: %d pasos OK, %d fallidos ===\n",
                g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
