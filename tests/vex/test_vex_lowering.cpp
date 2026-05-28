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
 * @file test_vex_lowering.cpp
 * @brief Test del pase AST -> ir::IrModule de Vex.
 *
 * Verifica:
 *   - Programa minimo (main retornando entero) genera 1 funcion con
 *     bloque entry, instrucciones CONST + RET.
 *   - Funcion con expresion (return a + b) genera ADD + RET.
 *   - if/else genera CFG con 4 bloques (entry, then, else, merge) y
 *     terminadores correctos.
 *   - Llamada recursiva genera instruccion CALL con func_name correcto.
 *   - Caracteristicas no soportadas en A.1 (loops, asignacion) generan
 *     error explicito en el lowering.
 */

#include "ir/ssa_ir.h"
#include "vex/lexer.h"
#include "vex/lowering.h"
#include "vex/parser.h"
#include "vex/type_checker.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

using vex::Diagnostics;
using vex::Lexer;
using vex::Lowering;
using vex::Parser;
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

#define VEX_ASSERT_EQ(a, b, msg) do {                                                       \
    auto av = (a); auto bv = (b);                                                           \
    if (!(av == bv)) {                                                                      \
        std::fprintf(stderr, "FAIL [%s:%d] %s (got %lld, expected %lld)\n",                 \
                     __FILE__, __LINE__, msg,                                               \
                     (long long)av, (long long)bv);                                         \
        ++g_failed;                                                                         \
    } else { ++g_passed; }                                                                  \
} while (0)

struct LowerOut {
    Diagnostics diags;
    std::unique_ptr<ast::ModuleNode> mod;
    ir::IrModule irmod;
    bool         ok = false;
};

static LowerOut compile_to_ir(const std::string &src) {
    LowerOut out;
    Lexer lx(src, "<test>", out.diags);
    Parser p(lx, out.diags);
    out.mod = p.parse_program();
    if (out.diags.has_errors() || !out.mod) return out;
    TypeChecker tc(*out.mod, out.diags);
    if (!tc.run()) return out;
    Lowering lo(*out.mod, tc, out.diags);
    out.ok = lo.run(out.irmod, "test");
    return out;
}

// =====================================================================
// Tests.
// =====================================================================

static void test_minimal_main_lowers() {
    auto out = compile_to_ir("i32 main() { return 0; }");
    VEX_ASSERT(out.ok, "minimal main lower ok");
    VEX_ASSERT_EQ(out.irmod.functions.size(), (size_t)1, "una funcion");

    const auto &fn = out.irmod.functions[0];
    VEX_ASSERT(fn.name == "main", "funcion main");
    VEX_ASSERT(fn.ret_type == ir::IrType::I32, "retorno I32");
    VEX_ASSERT_EQ(fn.params.size(), (size_t)0, "sin params");
    VEX_ASSERT_EQ(fn.blocks.size(), (size_t)1, "un bloque");
    const auto &b = fn.blocks[0];
    // El nombre del bloque incluye un sufijo "_<id>" garantizado unico
    // (necesario por el bug de etiquetas duplicadas en loops anidados).
    VEX_ASSERT(b.name.compare(0, 5, "entry") == 0, "bloque entry (con sufijo unico)");
    VEX_ASSERT(b.instrs.size() >= (size_t)2, "al menos CONST y RET");
    // El ultimo instr debe ser RET.
    VEX_ASSERT(b.instrs.back().op == ir::IrOp::RET, "termina en RET");
}

static void test_add_function_lowers() {
    auto out = compile_to_ir(
        "i32 add(i32 a, i32 b) { return a + b; }\n"
        "i32 main() { return add(40, 2); }\n");
    VEX_ASSERT(out.ok, "add+main lower ok");
    VEX_ASSERT_EQ(out.irmod.functions.size(), (size_t)2, "dos funciones");

    // Buscar la funcion 'add'.
    const ir::IrFunction *add = nullptr;
    for (const auto &f : out.irmod.functions) {
        if (f.name == "add") { add = &f; break; }
    }
    VEX_ASSERT(add != nullptr, "encontrada add");
    VEX_ASSERT_EQ(add->params.size(), (size_t)2, "add tiene 2 params");
    // Esperamos al menos un ADD y un RET en el body.
    bool found_add = false, found_ret = false;
    for (const auto &b : add->blocks) {
        for (const auto &ins : b.instrs) {
            if (ins.op == ir::IrOp::ADD) found_add = true;
            if (ins.op == ir::IrOp::RET) found_ret = true;
        }
    }
    VEX_ASSERT(found_add, "ADD presente en add");
    VEX_ASSERT(found_ret, "RET presente en add");

    // main debe tener una instruccion CALL a 'add'.
    const ir::IrFunction *m = nullptr;
    for (const auto &f : out.irmod.functions) {
        if (f.name == "main") { m = &f; break; }
    }
    VEX_ASSERT(m != nullptr, "encontrada main");
    bool found_call = false;
    for (const auto &b : m->blocks) {
        for (const auto &ins : b.instrs) {
            if (ins.op == ir::IrOp::CALL && ins.func_name == "add") {
                found_call = true;
                VEX_ASSERT_EQ(ins.operands.size(), (size_t)2, "call con 2 args");
            }
        }
    }
    VEX_ASSERT(found_call, "CALL a add presente");
}

static void test_if_else_cfg() {
    auto out = compile_to_ir(
        "i32 absval(i32 x) {\n"
        "    if (x < 0) return 0 - x;\n"
        "    else       return x;\n"
        "}\n");
    VEX_ASSERT(out.ok, "absval lower ok");
    const auto &fn = out.irmod.functions[0];

    // Esperamos al menos: entry, if_then, if_else, if_merge.
    // Los nombres llevan sufijo "_<id>" unico; comparamos por prefijo.
    VEX_ASSERT(fn.blocks.size() >= 4, "al menos 4 bloques con if/else");
    auto has_prefix = [](const std::string &s, const char *p) {
        return s.compare(0, std::strlen(p), p) == 0;
    };
    bool has_then = false, has_else = false, has_merge = false;
    for (const auto &b : fn.blocks) {
        if (has_prefix(b.name, "if_then"))  has_then  = true;
        if (has_prefix(b.name, "if_else"))  has_else  = true;
        if (has_prefix(b.name, "if_merge")) has_merge = true;
    }
    VEX_ASSERT(has_then,  "bloque if_then presente");
    VEX_ASSERT(has_else,  "bloque if_else presente");
    VEX_ASSERT(has_merge, "bloque if_merge presente");

    // El bloque entry debe terminar con BR_COND.
    const auto &entry = fn.blocks[0];
    VEX_ASSERT(has_prefix(entry.name, "entry"), "entry block primero");
    VEX_ASSERT(!entry.instrs.empty() && entry.instrs.back().op == ir::IrOp::BR_COND,
               "entry termina con BR_COND");
    // Debe haber un CMP_LT antes del BR_COND.
    bool found_cmp = false;
    for (const auto &ins : entry.instrs) {
        if (ins.op == ir::IrOp::CMP_LT) { found_cmp = true; break; }
    }
    VEX_ASSERT(found_cmp, "CMP_LT presente en entry");
}

static void test_recursion() {
    auto out = compile_to_ir(R"(
        i64 fact(i64 n) {
            if (n <= 1) return 1;
            return n * fact(n - 1);
        }
        i64 main() { return fact(5); }
    )");
    VEX_ASSERT(out.ok, "factorial recursivo lower ok");
    // Buscar la llamada recursiva fact->fact.
    const ir::IrFunction *fact = nullptr;
    for (const auto &f : out.irmod.functions) {
        if (f.name == "fact") { fact = &f; break; }
    }
    VEX_ASSERT(fact != nullptr, "encontrada fact");

    bool found_recursive_call = false;
    for (const auto &b : fact->blocks) {
        for (const auto &ins : b.instrs) {
            if (ins.op == ir::IrOp::CALL && ins.func_name == "fact") {
                found_recursive_call = true;
            }
        }
    }
    VEX_ASSERT(found_recursive_call, "fact se llama a si misma (CALL fact)");
}

static void test_multiple_types_aliases() {
    // Dos aliases para u32: uint32_t y u32, deben generar el mismo IrType.
    auto out = compile_to_ir(
        "uint32_t f1() { return 1; }\n"
        "u32 f2() { return 2; }\n"
        "u32 main() { return f1() + f2(); }\n");
    VEX_ASSERT(out.ok, "aliases lower ok");
    for (const auto &f : out.irmod.functions) {
        VEX_ASSERT(f.ret_type == ir::IrType::U32, "ret_type U32 para todas");
    }
}

static void test_while_supported_a2() {
    // Desde A.2 el while se baja con phi-construction Braun-style.
    auto out = compile_to_ir(
        "i32 main() {\n"
        "    i32 i = 0;\n"
        "    while (i < 3) { i = i + 1; }\n"
        "    return i;\n"
        "}\n");
    VEX_ASSERT(out.ok, "while soportado en A.2");

    // Verificar que se genero al menos un PHI node.
    bool has_phi = false;
    for (const auto &fn : out.irmod.functions) {
        for (const auto &b : fn.blocks) {
            for (const auto &ins : b.instrs) {
                if (ins.op == ir::IrOp::PHI) { has_phi = true; break; }
            }
        }
    }
    VEX_ASSERT(has_phi, "while genera PHI nodes");
}

static void test_for_supported_a2() {
    // for(init; cond; step) body desazucara internamente a while.
    auto out = compile_to_ir(
        "i32 main() {\n"
        "    i32 sum = 0;\n"
        "    for (i32 i = 0; i < 5; i++) { sum = sum + i; }\n"
        "    return sum;\n"
        "}\n");
    VEX_ASSERT(out.ok, "for soportado en A.2");
}

static void test_assignment_supported_a2() {
    // A partir del hito A.2: la asignacion simple a variables locales se
    // baja correctamente; el lowering Braun-style anota el nuevo IrValueId
    // como "current value de x" en el scope.  Un return posterior leera ese
    // ultimo valor automaticamente.
    auto out = compile_to_ir(
        "i32 main() {\n"
        "    i32 x = 1;\n"
        "    x = 2;\n"
        "    return x;\n"
        "}\n");
    VEX_ASSERT(out.ok, "asignacion soportada en A.2");
}

static void test_compound_assign_supported_a2() {
    // Las asignaciones compuestas (+=, -=, etc.) se reescriben como
    // x = x op rhs y reusan el mismo emisor binop que lower_binary.
    auto out = compile_to_ir(
        "i32 main() {\n"
        "    i32 acc = 1;\n"
        "    acc += 2;\n"
        "    acc *= 3;\n"
        "    acc -= 1;\n"
        "    return acc;\n"
        "}\n");
    VEX_ASSERT(out.ok, "compound assigns soportados en A.2");
}

static void test_increment_supported_a2() {
    // ++ / -- requieren leer la variable, sumar/restar 1 y reescribir.
    // En A.2 solo se acepta cuando el operando es un IdentExpr; punteros
    // y campos llegan en hitos posteriores.
    auto out = compile_to_ir(
        "i32 main() {\n"
        "    i32 x = 5;\n"
        "    x++;\n"
        "    ++x;\n"
        "    x--;\n"
        "    return x;\n"
        "}\n");
    VEX_ASSERT(out.ok, "++/-- soportados en A.2");
}

// ---------------------------------------------------------------------
int main() {
    test_minimal_main_lowers();
    test_add_function_lowers();
    test_if_else_cfg();
    test_recursion();
    test_multiple_types_aliases();
    test_while_supported_a2();
    test_for_supported_a2();
    test_assignment_supported_a2();
    test_compound_assign_supported_a2();
    test_increment_supported_a2();

    std::printf("\n=== test_vex_lowering: %d pasos OK, %d fallidos ===\n",
                g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
