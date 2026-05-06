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
 * @file test_vex_parser.cpp
 * @brief Test automatico del parser Vex.
 *
 * Cobertura:
 *   - Programa minimo (i32 main() { return 0; }).
 *   - Variables globales con y sin init, const.
 *   - Funciones: cero parametros, varios parametros (mezcla de aliases).
 *   - Statements: if/else, while, for(;;), return, break, continue,
 *                 declaraciones de variable local con init.
 *   - Expresiones: precedencia C correcta para *, /, +, -, comparacion,
 *                  &&, ||, asignacion, llamada a funcion, paretesis.
 *   - Recuperacion de errores: tras un error, sigue parseando para
 *                              detectar mas errores en el mismo fichero.
 */

#include "vex/lexer.h"
#include "vex/parser.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>

using vex::Diagnostics;
using vex::Lexer;
using vex::Parser;
using vex::PrimitiveKind;
using vex::TokenKind;
namespace ast = vex::ast;

static int g_passed = 0;
static int g_failed = 0;

#define VEX_ASSERT(cond, msg) do {                                          \
    if (!(cond)) {                                                          \
        std::fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg); \
        ++g_failed;                                                         \
    } else { ++g_passed; }                                                  \
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

// Helper: parsea una cadena y devuelve el ModuleNode + diagnosticos.
struct ParseOut {
    std::unique_ptr<ast::ModuleNode> mod;
    Diagnostics                      diags;
};

static ParseOut parse_src(const std::string &src) {
    ParseOut out;
    Lexer lx(src, "<test>", out.diags);
    Parser p(lx, out.diags);
    out.mod = p.parse_program();
    return out;
}

// Cast helper: chequea kind y hace static_cast (no RTTI).
template <typename T>
static const T *as_node(const ast::Node *n, ast::NodeKind k) {
    if (!n || n->kind != k) return nullptr;
    return static_cast<const T*>(n);
}

// =====================================================================
// Tests.
// =====================================================================

static void test_minimal_main() {
    auto out = parse_src("i32 main() { return 0; }");
    VEX_ASSERT(!out.diags.has_errors(), "minimal main sin errores");
    VEX_ASSERT(out.mod != nullptr, "modulo no nulo");
    VEX_ASSERT_EQ(out.mod->decls.size(), (size_t)1, "una declaracion top-level");
    auto fn = as_node<ast::FunctionDecl>(out.mod->decls[0].get(), ast::NodeKind::FunctionDecl);
    VEX_ASSERT(fn != nullptr, "es FunctionDecl");
    VEX_ASSERT(fn->name == "main", "nombre main");
    auto pt = as_node<ast::PrimitiveTypeNode>(fn->return_type.get(), ast::NodeKind::PrimitiveTypeNode);
    VEX_ASSERT(pt != nullptr && pt->prim == PrimitiveKind::I32, "retorno i32");
    VEX_ASSERT(fn->params.empty(), "sin params");
    VEX_ASSERT(fn->body != nullptr, "tiene body");
    VEX_ASSERT_EQ(fn->body->body.size(), (size_t)1, "un statement en el body");
    auto ret = as_node<ast::ReturnStmt>(fn->body->body[0].get(), ast::NodeKind::ReturnStmt);
    VEX_ASSERT(ret != nullptr, "es ReturnStmt");
    auto lit = as_node<ast::IntLitExpr>(ret->value.get(), ast::NodeKind::IntLitExpr);
    VEX_ASSERT(lit != nullptr, "valor es IntLit");
    VEX_ASSERT_EQ(lit->value, (uint64_t)0, "valor 0");
}

static void test_global_var_decl() {
    auto out = parse_src(
        "u8 contador = 42;\n"
        "const f64 PI = 3.14;\n"
        "i32 sin_init;\n"
    );
    VEX_ASSERT(!out.diags.has_errors(), "globales sin errores");
    VEX_ASSERT_EQ(out.mod->decls.size(), (size_t)3, "tres globales");

    auto g0 = as_node<ast::GlobalVarDecl>(out.mod->decls[0].get(), ast::NodeKind::GlobalVarDecl);
    VEX_ASSERT(g0 && g0->name == "contador",  "global contador");
    VEX_ASSERT(!g0->is_const,                  "contador no const");
    VEX_ASSERT(g0->init != nullptr,            "contador con init");

    auto g1 = as_node<ast::GlobalVarDecl>(out.mod->decls[1].get(), ast::NodeKind::GlobalVarDecl);
    VEX_ASSERT(g1 && g1->name == "PI", "global PI");
    VEX_ASSERT(g1->is_const,           "PI const");

    auto g2 = as_node<ast::GlobalVarDecl>(out.mod->decls[2].get(), ast::NodeKind::GlobalVarDecl);
    VEX_ASSERT(g2 && g2->name == "sin_init", "global sin_init");
    VEX_ASSERT(g2->init == nullptr,          "sin init");
}

static void test_function_with_params() {
    // Mezcla de los dos estilos de alias para verificar que ambos se aceptan.
    auto out = parse_src(
        "uint32_t suma(int32_t a, i32 b, u8 c) {\n"
        "    return a + b;\n"
        "}\n"
    );
    VEX_ASSERT(!out.diags.has_errors(), "funcion con params sin errores");
    auto fn = as_node<ast::FunctionDecl>(out.mod->decls[0].get(), ast::NodeKind::FunctionDecl);
    VEX_ASSERT(fn && fn->name == "suma", "es funcion suma");
    VEX_ASSERT_EQ(fn->params.size(), (size_t)3, "tres params");
    auto rt = as_node<ast::PrimitiveTypeNode>(fn->return_type.get(), ast::NodeKind::PrimitiveTypeNode);
    VEX_ASSERT(rt && rt->prim == PrimitiveKind::U32, "retorno U32 (uint32_t)");

    auto p0 = fn->params[0].get();
    auto p1 = fn->params[1].get();
    auto p2 = fn->params[2].get();
    auto t0 = as_node<ast::PrimitiveTypeNode>(p0->type.get(), ast::NodeKind::PrimitiveTypeNode);
    auto t1 = as_node<ast::PrimitiveTypeNode>(p1->type.get(), ast::NodeKind::PrimitiveTypeNode);
    auto t2 = as_node<ast::PrimitiveTypeNode>(p2->type.get(), ast::NodeKind::PrimitiveTypeNode);
    VEX_ASSERT(t0 && t0->prim == PrimitiveKind::I32, "param 0 -> int32_t -> I32");
    VEX_ASSERT(t1 && t1->prim == PrimitiveKind::I32, "param 1 -> i32 -> I32");
    VEX_ASSERT(t2 && t2->prim == PrimitiveKind::U8,  "param 2 -> u8 -> U8");
    VEX_ASSERT(p0->name == "a" && p1->name == "b" && p2->name == "c",
               "nombres de params");
}

static void test_arithmetic_precedence() {
    // 1 + 2 * 3 debe parsear como 1 + (2 * 3).
    // El AST esperado: BinaryExpr(Add, IntLit(1), BinaryExpr(Mul, IntLit(2), IntLit(3))).
    auto out = parse_src("i32 f() { return 1 + 2 * 3; }");
    VEX_ASSERT(!out.diags.has_errors(), "precedencia sin errores");
    auto fn  = as_node<ast::FunctionDecl>(out.mod->decls[0].get(), ast::NodeKind::FunctionDecl);
    auto ret = as_node<ast::ReturnStmt>(fn->body->body[0].get(), ast::NodeKind::ReturnStmt);
    auto add = as_node<ast::BinaryExpr>(ret->value.get(), ast::NodeKind::BinaryExpr);
    VEX_ASSERT(add && add->op == ast::BinOp::Add, "raiz Add");
    auto lhs = as_node<ast::IntLitExpr>(add->lhs.get(), ast::NodeKind::IntLitExpr);
    VEX_ASSERT(lhs && lhs->value == 1, "lhs == 1");
    auto mul = as_node<ast::BinaryExpr>(add->rhs.get(), ast::NodeKind::BinaryExpr);
    VEX_ASSERT(mul && mul->op == ast::BinOp::Mul, "rhs es Mul");
    auto m1 = as_node<ast::IntLitExpr>(mul->lhs.get(), ast::NodeKind::IntLitExpr);
    auto m2 = as_node<ast::IntLitExpr>(mul->rhs.get(), ast::NodeKind::IntLitExpr);
    VEX_ASSERT(m1 && m1->value == 2, "mul lhs 2");
    VEX_ASSERT(m2 && m2->value == 3, "mul rhs 3");
}

static void test_left_associativity() {
    // 10 - 4 - 3 debe parsear como (10 - 4) - 3.
    auto out = parse_src("i32 f() { return 10 - 4 - 3; }");
    VEX_ASSERT(!out.diags.has_errors(), "left-assoc sin errores");
    auto fn  = as_node<ast::FunctionDecl>(out.mod->decls[0].get(), ast::NodeKind::FunctionDecl);
    auto ret = as_node<ast::ReturnStmt>(fn->body->body[0].get(), ast::NodeKind::ReturnStmt);
    auto sub = as_node<ast::BinaryExpr>(ret->value.get(), ast::NodeKind::BinaryExpr);
    VEX_ASSERT(sub && sub->op == ast::BinOp::Sub, "raiz Sub");
    // lhs del sub raiz debe ser otro Sub.
    auto inner = as_node<ast::BinaryExpr>(sub->lhs.get(), ast::NodeKind::BinaryExpr);
    VEX_ASSERT(inner && inner->op == ast::BinOp::Sub, "inner es Sub");
    auto i_lhs = as_node<ast::IntLitExpr>(inner->lhs.get(), ast::NodeKind::IntLitExpr);
    auto i_rhs = as_node<ast::IntLitExpr>(inner->rhs.get(), ast::NodeKind::IntLitExpr);
    auto r_rhs = as_node<ast::IntLitExpr>(sub->rhs.get(), ast::NodeKind::IntLitExpr);
    VEX_ASSERT(i_lhs && i_lhs->value == 10, "10");
    VEX_ASSERT(i_rhs && i_rhs->value == 4,  "4");
    VEX_ASSERT(r_rhs && r_rhs->value == 3,  "3");
}

static void test_comparison_and_logical() {
    // a < b && c == d || !e
    auto out = parse_src(
        "i32 f(i32 a, i32 b, i32 c, i32 d, i32 e) {\n"
        "    return a < b && c == d || !e;\n"
        "}\n");
    VEX_ASSERT(!out.diags.has_errors(), "comp+logical sin errores");
    auto fn  = as_node<ast::FunctionDecl>(out.mod->decls[0].get(), ast::NodeKind::FunctionDecl);
    auto ret = as_node<ast::ReturnStmt>(fn->body->body[0].get(), ast::NodeKind::ReturnStmt);
    auto orx = as_node<ast::BinaryExpr>(ret->value.get(), ast::NodeKind::BinaryExpr);
    VEX_ASSERT(orx && orx->op == ast::BinOp::LogicalOr, "raiz Or");
    auto andx = as_node<ast::BinaryExpr>(orx->lhs.get(), ast::NodeKind::BinaryExpr);
    VEX_ASSERT(andx && andx->op == ast::BinOp::LogicalAnd, "lhs And");
    auto notx = as_node<ast::UnaryExpr>(orx->rhs.get(), ast::NodeKind::UnaryExpr);
    VEX_ASSERT(notx && notx->op == ast::UnOp::LogicalNot, "rhs Not(e)");
    auto lt = as_node<ast::BinaryExpr>(andx->lhs.get(), ast::NodeKind::BinaryExpr);
    auto eq = as_node<ast::BinaryExpr>(andx->rhs.get(), ast::NodeKind::BinaryExpr);
    VEX_ASSERT(lt && lt->op == ast::BinOp::Lt, "and lhs Lt");
    VEX_ASSERT(eq && eq->op == ast::BinOp::Eq, "and rhs Eq");
}

static void test_assignment_right_assoc() {
    // a = b = 3  debe parsear como a = (b = 3).
    auto out = parse_src(
        "i32 f(i32 a, i32 b) {\n"
        "    a = b = 3;\n"
        "    return a;\n"
        "}\n");
    VEX_ASSERT(!out.diags.has_errors(), "assign right-assoc sin errores");
    auto fn  = as_node<ast::FunctionDecl>(out.mod->decls[0].get(), ast::NodeKind::FunctionDecl);
    auto es  = as_node<ast::ExprStmt>(fn->body->body[0].get(), ast::NodeKind::ExprStmt);
    auto a1  = as_node<ast::AssignExpr>(es->expr.get(), ast::NodeKind::AssignExpr);
    VEX_ASSERT(a1 && a1->op == ast::AssignOp::Assign, "primer assign");
    auto a2  = as_node<ast::AssignExpr>(a1->value.get(), ast::NodeKind::AssignExpr);
    VEX_ASSERT(a2 && a2->op == ast::AssignOp::Assign, "segundo assign");
    auto target_a = as_node<ast::IdentExpr>(a1->target.get(), ast::NodeKind::IdentExpr);
    auto target_b = as_node<ast::IdentExpr>(a2->target.get(), ast::NodeKind::IdentExpr);
    VEX_ASSERT(target_a && target_a->name == "a", "target a");
    VEX_ASSERT(target_b && target_b->name == "b", "target b");
}

static void test_if_while_for() {
    auto out = parse_src(R"(
        i32 acc(i32 n) {
            i32 i = 0;
            i32 s = 0;
            while (i < n) {
                if (i % 2 == 0) {
                    s = s + i;
                } else {
                    s = s - i;
                }
                i = i + 1;
            }
            for (i32 k = 0; k < 10; k = k + 1) {
                s = s + k;
            }
            return s;
        }
    )");
    VEX_ASSERT(!out.diags.has_errors(), "if/while/for sin errores");
    auto fn  = as_node<ast::FunctionDecl>(out.mod->decls[0].get(), ast::NodeKind::FunctionDecl);
    VEX_ASSERT(fn != nullptr, "es funcion");
    VEX_ASSERT_EQ(fn->body->body.size(), (size_t)5,
                  "decl i, decl s, while, for, return");
    auto wh = as_node<ast::WhileStmt>(fn->body->body[2].get(), ast::NodeKind::WhileStmt);
    VEX_ASSERT(wh != nullptr, "while presente");
    auto fr = as_node<ast::ForStmt>(fn->body->body[3].get(), ast::NodeKind::ForStmt);
    VEX_ASSERT(fr != nullptr, "for presente");
    VEX_ASSERT(fr->init != nullptr && fr->cond != nullptr && fr->step != nullptr,
               "for con tres partes");
    auto fbody = as_node<ast::BlockStmt>(fr->body.get(), ast::NodeKind::BlockStmt);
    VEX_ASSERT(fbody != nullptr, "for body es block");
}

static void test_function_call() {
    auto out = parse_src(
        "i32 g(i32 x) { return x + 1; }\n"
        "i32 main() { return g(g(40)); }\n"
    );
    VEX_ASSERT(!out.diags.has_errors(), "calls sin errores");
    VEX_ASSERT_EQ(out.mod->decls.size(), (size_t)2, "dos decls");
    auto main_fn = as_node<ast::FunctionDecl>(out.mod->decls[1].get(), ast::NodeKind::FunctionDecl);
    auto ret     = as_node<ast::ReturnStmt>(main_fn->body->body[0].get(), ast::NodeKind::ReturnStmt);
    auto outer   = as_node<ast::CallExpr>(ret->value.get(), ast::NodeKind::CallExpr);
    VEX_ASSERT(outer && outer->args.size() == 1, "outer call con 1 arg");
    auto inner = as_node<ast::CallExpr>(outer->args[0].get(), ast::NodeKind::CallExpr);
    VEX_ASSERT(inner && inner->args.size() == 1, "inner call con 1 arg");
    auto lit = as_node<ast::IntLitExpr>(inner->args[0].get(), ast::NodeKind::IntLitExpr);
    VEX_ASSERT(lit && lit->value == 40, "literal 40");
}

static void test_break_continue() {
    auto out = parse_src(R"(
        void f() {
            while (true) {
                break;
                continue;
            }
        }
    )");
    VEX_ASSERT(!out.diags.has_errors(), "break/continue sin errores");
    auto fn = as_node<ast::FunctionDecl>(out.mod->decls[0].get(), ast::NodeKind::FunctionDecl);
    auto wh = as_node<ast::WhileStmt>(fn->body->body[0].get(), ast::NodeKind::WhileStmt);
    auto bk = as_node<ast::BlockStmt>(wh->body.get(), ast::NodeKind::BlockStmt);
    VEX_ASSERT(as_node<ast::BreakStmt>(bk->body[0].get(), ast::NodeKind::BreakStmt) != nullptr,
               "break presente");
    VEX_ASSERT(as_node<ast::ContinueStmt>(bk->body[1].get(), ast::NodeKind::ContinueStmt) != nullptr,
               "continue presente");
}

static void test_compound_assignments() {
    auto out = parse_src(
        "i32 f(i32 a) {\n"
        "    a += 1;\n"
        "    a -= 1;\n"
        "    a *= 2;\n"
        "    a /= 2;\n"
        "    a %= 2;\n"
        "    a &= 0xFF;\n"
        "    a |= 0x1;\n"
        "    a ^= 0x2;\n"
        "    a <<= 1;\n"
        "    a >>= 1;\n"
        "    return a;\n"
        "}\n");
    VEX_ASSERT(!out.diags.has_errors(), "compound assigns sin errores");
    auto fn = as_node<ast::FunctionDecl>(out.mod->decls[0].get(), ast::NodeKind::FunctionDecl);
    VEX_ASSERT_EQ(fn->body->body.size(), (size_t)11, "10 ExprStmt + 1 return");
    static const ast::AssignOp expected[] = {
        ast::AssignOp::AddAssign,
        ast::AssignOp::SubAssign,
        ast::AssignOp::MulAssign,
        ast::AssignOp::DivAssign,
        ast::AssignOp::ModAssign,
        ast::AssignOp::BitAndAssign,
        ast::AssignOp::BitOrAssign,
        ast::AssignOp::BitXorAssign,
        ast::AssignOp::ShlAssign,
        ast::AssignOp::ShrAssign,
    };
    for (size_t i = 0; i < 10; ++i) {
        auto es = as_node<ast::ExprStmt>(fn->body->body[i].get(), ast::NodeKind::ExprStmt);
        auto a  = as_node<ast::AssignExpr>(es->expr.get(), ast::NodeKind::AssignExpr);
        VEX_ASSERT(a && a->op == expected[i], "compound assign correcto");
    }
}

static void test_error_recovery() {
    // Falta ; tras la primera variable global, pero el parser debe seguir y
    // detectar la siguiente declaracion.
    auto out = parse_src(
        "i32 a = 1\n"           // <-- falta ;
        "i32 b = 2;\n"
    );
    VEX_ASSERT(out.diags.has_errors(), "error de ; reportado");
    // Pese al error, el parser debe haber recuperado al menos b.
    bool found_b = false;
    for (auto &d : out.mod->decls) {
        if (auto g = as_node<ast::GlobalVarDecl>(d.get(), ast::NodeKind::GlobalVarDecl)) {
            if (g->name == "b") found_b = true;
        }
    }
    VEX_ASSERT(found_b, "recuperacion: b detectado tras error");
}

static void test_position_propagated() {
    auto out = parse_src("i32 main() {\n    return 0;\n}\n");
    auto fn = as_node<ast::FunctionDecl>(out.mod->decls[0].get(), ast::NodeKind::FunctionDecl);
    VEX_ASSERT_EQ(fn->loc.line, (uint32_t)1, "fn linea 1");
    auto ret = as_node<ast::ReturnStmt>(fn->body->body[0].get(), ast::NodeKind::ReturnStmt);
    VEX_ASSERT_EQ(ret->loc.line, (uint32_t)2, "return linea 2");
}

// ---------------------------------------------------------------------
// main del test.
// ---------------------------------------------------------------------
int main() {
    test_minimal_main();
    test_global_var_decl();
    test_function_with_params();
    test_arithmetic_precedence();
    test_left_associativity();
    test_comparison_and_logical();
    test_assignment_right_assoc();
    test_if_while_for();
    test_function_call();
    test_break_continue();
    test_compound_assignments();
    test_error_recovery();
    test_position_propagated();

    std::printf("\n=== test_vex_parser: %d pasos OK, %d fallidos ===\n",
                g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
