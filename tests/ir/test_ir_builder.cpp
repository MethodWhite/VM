/*
 * Test: IrBuilder API.
 *
 * Demuestra como un frontend ARBITRARIO (Vex, C-like, Java-like, Python-like,
 * cualquier lenguaje) puede emitir SSA IR de VestaVM usando solo el header
 * publico @c ir_builder.h, sin tocar la representacion interna.
 *
 * Caso de uso: implementar @c factorial(n) en una sola pasada.
 */

#include "ir/ir_builder.h"
#include "ir/ssa_ir.h"
#include "ir/ssa_ir_serialize.h"
#include <cassert>
#include <cstdio>
#include <sstream>

using namespace ir;

static IrFunction make_factorial() {
    /*
     * Equivalente C/Java:
     *   i32 factorial(i32 n) {
     *       if (n <= 1) return 1;
     *       return n * factorial(n - 1);
     *   }
     */
    IrFunction fn;
    fn.name     = "factorial";
    fn.ret_type = IrType::I32;

    IrBuilder b(fn);

    /* Parametro: n: i32 */
    const auto n = b.param(IrType::I32, "n");

    /* Bloques: entry, then (return 1), else (recursion), exit. */
    const auto entry_bb = b.new_block("entry");
    const auto then_bb  = b.new_block("then");
    const auto else_bb  = b.new_block("else");

    /* entry: if (n <= 1) goto then else goto else */
    b.set_insert_point(entry_bb);
    const auto one      = b.const_i32(1);
    const auto cond     = b.cmp_le(n, one);
    b.br_cond(cond, then_bb, else_bb);

    /* then: return 1 */
    b.set_insert_point(then_bb);
    const auto one2 = b.const_i32(1);
    b.ret(one2);

    /* else: tmp = n - 1; sub = factorial(tmp); return n * sub */
    b.set_insert_point(else_bb);
    const auto one3   = b.const_i32(1);
    const auto tmp    = b.sub(n, one3, IrType::I32);
    const auto sub_v  = b.call("factorial", {tmp}, IrType::I32);
    const auto result = b.mul(n, sub_v, IrType::I32);
    b.ret(result);

    return fn;
}

static IrFunction make_sum_loop() {
    /*
     * Demuestra un loop con PHI nodes:
     *   i32 sum_to_n(i32 n) {
     *       i32 sum = 0;
     *       i32 i   = 0;
     *       while (i < n) {
     *           sum = sum + i;
     *           i   = i + 1;
     *       }
     *       return sum;
     *   }
     */
    IrFunction fn;
    fn.name     = "sum_to_n";
    fn.ret_type = IrType::I32;

    IrBuilder b(fn);
    const auto n = b.param(IrType::I32, "n");

    const auto entry  = b.new_block("entry");
    const auto header = b.new_block("loop_header");
    const auto body   = b.new_block("loop_body");
    const auto exit_b = b.new_block("loop_exit");

    /* entry: sum=0, i=0, jump header */
    b.set_insert_point(entry);
    const auto zero = b.const_i32(0);
    b.br(header);

    /* header: PHI for sum, PHI for i; cmp i < n; cond br */
    b.set_insert_point(header);
    /* phi nodes referencian @c body como predecesor pero body aun no tiene
     * sus values asignados.  Para simplificar: emitimos los phi con
     * IR_NO_VALUE inicialmente y los actualizamos despues. */
    const auto sum_phi = b.phi(IrType::I32, {{entry, zero}});
    const auto i_phi   = b.phi(IrType::I32, {{entry, zero}});
    const auto cmp_lt  = b.cmp_lt(i_phi, n);
    b.br_cond(cmp_lt, body, exit_b);

    /* body: sum2 = sum + i; i2 = i + 1; back to header */
    b.set_insert_point(body);
    const auto new_sum = b.add(sum_phi, i_phi, IrType::I32);
    const auto one     = b.const_i32(1);
    const auto new_i   = b.add(i_phi, one, IrType::I32);
    b.br(header);

    /* Cerrar los PHIs anyadiendo el back-edge. */
    /* Buscar el PHI por su dst id y agregar phi_args manualmente. */
    for (auto &ins : fn.blocks[header].instrs) {
        if (ins.op == IrOp::PHI && ins.dst == sum_phi) {
            IrPhiArg pa;
            pa.block = body;
            pa.value = new_sum;
            ins.phi_args.push_back(pa);
        }
        if (ins.op == IrOp::PHI && ins.dst == i_phi) {
            IrPhiArg pa;
            pa.block = body;
            pa.value = new_i;
            ins.phi_args.push_back(pa);
        }
    }

    /* exit: return sum */
    b.set_insert_point(exit_b);
    b.ret(sum_phi);

    return fn;
}

int main() {
    int ok = 0, fail = 0;
    auto check = [&](bool cond, const char *desc) {
        if (cond) { ++ok; std::printf("OK: %s\n", desc); }
        else      { ++fail; std::printf("FAIL: %s\n", desc); }
    };

    /* Construir factorial via builder. */
    IrFunction factorial = make_factorial();
    check(factorial.name == "factorial", "factorial.name");
    check(factorial.params.size() == 1, "factorial.params");
    check(factorial.blocks.size() == 3, "factorial has 3 blocks");
    /* entry: 4 instrs (const, cmp, br, ...) -- chequear minimo. */
    check(!factorial.blocks[0].instrs.empty(), "factorial.entry not empty");

    /* Verificar que el bloque "then" tiene RET. */
    bool found_ret_then = false;
    for (const auto &ins : factorial.blocks[1].instrs) {
        if (ins.op == IrOp::RET) found_ret_then = true;
    }
    check(found_ret_then, "factorial.then has RET");

    /* Verificar que el bloque "else" tiene CALL recursivo. */
    bool found_recursive_call = false;
    for (const auto &ins : factorial.blocks[2].instrs) {
        if (ins.op == IrOp::CALL && ins.func_name == "factorial") {
            found_recursive_call = true;
        }
    }
    check(found_recursive_call, "factorial.else has recursive CALL");

    /* Serializar y deserializar para round-trip. */
    {
        std::vector<IrFunction> fns;
        fns.push_back(make_factorial());
        const auto bytes = emit_ir_section(fns);
        check(!bytes.empty(), "factorial serializes to bytes");

        std::vector<IrFunction> loaded;
        const bool ok_parse =
            parse_ir_section(bytes, 0, bytes.size(), loaded);
        check(ok_parse, "factorial parses back");
        check(loaded.size() == 1, "factorial round-trip count");
        if (!loaded.empty()) {
            check(loaded[0].name == "factorial", "factorial name preserved");
            check(loaded[0].blocks.size() == 3, "factorial blocks preserved");
        }
    }

    /* Construir sum_to_n via builder. */
    IrFunction sum_to_n = make_sum_loop();
    check(sum_to_n.name == "sum_to_n", "sum_to_n.name");
    check(sum_to_n.blocks.size() == 4, "sum_to_n has 4 blocks");

    /* Verificar PHI nodes en el header. */
    int phi_count = 0;
    for (const auto &ins : sum_to_n.blocks[1].instrs) {
        if (ins.op == IrOp::PHI) ++phi_count;
    }
    check(phi_count == 2, "sum_to_n header has 2 PHI nodes");

    /* Verificar que cada PHI tiene 2 args (entry + back-edge). */
    for (const auto &ins : sum_to_n.blocks[1].instrs) {
        if (ins.op == IrOp::PHI) {
            check(ins.phi_args.size() == 2, "PHI has 2 args");
        }
    }

    /* Imprimir IR para inspeccion humana. */
    std::ostringstream oss;
    IrModule mod;
    mod.name = "test_ir_builder";
    mod.functions.push_back(std::move(factorial));
    mod.functions.push_back(std::move(sum_to_n));
    ir_print(mod, oss);
    std::printf("---- IR generated ----\n%s\n----\n", oss.str().c_str());

    std::printf("\n=== RESULT: %d ok, %d fail ===\n", ok, fail);
    return fail == 0 ? 0 : 1;
}
