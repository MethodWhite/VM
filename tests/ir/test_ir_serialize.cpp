/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file tests/ir/test_ir_serialize.cpp
 * @brief Tests round-trip del serializer/deserializer binario del IR
 *        (Opcion W del roadmap JIT, Fase 1).
 *
 * Verifica:
 *   1. Funcion trivial round-trip identico bit-a-bit (todos los campos).
 *   2. Funcion con multiples bloques + branches.
 *   3. Funcion con PHI nodes (phi_args).
 *   4. Funcion con call + func_name + operands.
 *   5. Funcion con generic_template_name (B.3 metadata).
 *   6. Tolerancia a corrupcion (deserialize bytes truncados -> false).
 *   7. Multiples funciones secuenciales en el mismo buffer.
 */

#include "ir/ssa_ir.h"
#include "ir/ssa_ir_serialize.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

    int pass_count = 0;
    int fail_count = 0;

    #define CHECK(cond, msg) do {                                            \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL: %s (linea %d)\n", msg, __LINE__);    \
            ++fail_count;                                                    \
        } else {                                                             \
            ++pass_count;                                                    \
        }                                                                    \
    } while (0)

    /* Helpers de comparacion para verificar round-trip. */
    bool same_value(const ir::IrValue &a, const ir::IrValue &b) {
        return a.type == b.type
            && a.is_param == b.is_param
            && a.is_const == b.is_const
            && a.is_host_ptr == b.is_host_ptr
            && a.pointee_is_host_ptr == b.pointee_is_host_ptr
            && a.is_gc_object == b.is_gc_object
            && (a.is_const ? (a.const_val == b.const_val) : true);
    }

    bool same_instr(const ir::IrInstr &a, const ir::IrInstr &b) {
        if (a.op != b.op) return false;
        if (a.type != b.type) return false;
        if (a.dst != b.dst) return false;
        if (a.preserve != b.preserve) return false;
        if (a.is_call_site != b.is_call_site) return false;
        if (a.source_line != b.source_line) return false;
        if (a.imm != b.imm) return false;
        if (a.operands != b.operands) return false;
        if (a.func_name != b.func_name) return false;
        if (a.func_ptr != b.func_ptr) return false;
        if (a.target_block != b.target_block) return false;
        if (a.false_block != b.false_block) return false;
        if (a.phi_args.size() != b.phi_args.size()) return false;
        for (size_t i = 0; i < a.phi_args.size(); ++i) {
            if (a.phi_args[i].value != b.phi_args[i].value) return false;
            if (a.phi_args[i].block != b.phi_args[i].block) return false;
        }
        return true;
    }

    bool same_block(const ir::IrBlock &a, const ir::IrBlock &b) {
        if (a.name != b.name) return false;
        if (a.instrs.size() != b.instrs.size()) return false;
        for (size_t i = 0; i < a.instrs.size(); ++i) {
            if (!same_instr(a.instrs[i], b.instrs[i])) return false;
        }
        if (a.preds != b.preds) return false;
        if (a.succs != b.succs) return false;
        return true;
    }

    bool same_function(const ir::IrFunction &a, const ir::IrFunction &b) {
        if (a.name != b.name) return false;
        if (a.ret_type != b.ret_type) return false;
        if (a.is_native != b.is_native) return false;
        if (a.is_variadic != b.is_variadic) return false;
        if (a.params != b.params) return false;
        if (a.values.size() != b.values.size()) return false;
        for (size_t i = 0; i < a.values.size(); ++i) {
            if (!same_value(a.values[i], b.values[i])) return false;
        }
        if (a.blocks.size() != b.blocks.size()) return false;
        for (size_t i = 0; i < a.blocks.size(); ++i) {
            if (!same_block(a.blocks[i], b.blocks[i])) return false;
        }
        if (a.generic_template_name != b.generic_template_name) return false;
        if (a.generic_type_args != b.generic_type_args) return false;
        return true;
    }

    /* Constructores helpers. */
    ir::IrValueId mk_value(ir::IrFunction &fn, ir::IrType t) {
        ir::IrValue v;
        v.type = t;
        v.id = static_cast<ir::IrValueId>(fn.values.size());
        fn.values.push_back(v);
        return v.id;
    }

    ir::IrValueId mk_const(ir::IrFunction &fn, ir::IrType t, uint64_t val) {
        ir::IrValue v;
        v.type = t;
        v.is_const = true;
        v.const_val = val;
        v.id = static_cast<ir::IrValueId>(fn.values.size());
        fn.values.push_back(v);
        return v.id;
    }

    /* ===================================================================== */
    /* Test 1: Round-trip de funcion trivial                                  */
    /* ===================================================================== */

    void test_trivial_function() {
        ir::IrFunction fn;
        fn.name = "ret42";
        fn.ret_type = ir::IrType::I64;

        const auto v = mk_value(fn, ir::IrType::I64);
        ir::IrBlock entry;
        entry.id = 0;
        entry.name = "entry";

        ir::IrInstr c;
        c.op = ir::IrOp::CONST;
        c.type = ir::IrType::I64;
        c.dst = v;
        c.imm = 42;
        c.source_line = 1;
        entry.instrs.push_back(c);

        ir::IrInstr r;
        r.op = ir::IrOp::RET;
        r.type = ir::IrType::I64;
        r.dst = ir::IR_NO_VALUE;
        r.operands.push_back(v);
        r.source_line = 2;
        entry.instrs.push_back(r);

        fn.blocks.push_back(entry);

        /* Serializar. */
        std::vector<uint8_t> buf;
        const size_t written = ir::serialize_function(fn, buf);
        CHECK(written > 0, "serialize > 0 bytes");
        CHECK(written == buf.size(), "written == buf.size()");

        /* Deserializar. */
        size_t off = 0;
        ir::IrFunction fn2;
        const bool ok = ir::deserialize_function(buf, off, fn2);
        CHECK(ok, "deserialize ok");
        CHECK(off == buf.size(), "consumido todo el buffer");
        CHECK(same_function(fn, fn2), "funciones identicas tras round-trip");
    }

    /* ===================================================================== */
    /* Test 2: Funcion con flags de IrValue                                   */
    /* ===================================================================== */

    void test_value_flags() {
        ir::IrFunction fn;
        fn.name = "with_flags";
        fn.ret_type = ir::IrType::HANDLE;

        ir::IrValue v0;
        v0.id = 0;
        v0.type = ir::IrType::HANDLE;
        v0.is_param = true;
        v0.is_gc_object = true;
        fn.values.push_back(v0);
        fn.params.push_back(0);

        ir::IrValue v1;
        v1.id = 1;
        v1.type = ir::IrType::PTR;
        v1.is_host_ptr = true;
        v1.pointee_is_host_ptr = true;
        fn.values.push_back(v1);

        ir::IrValue v2;
        v2.id = 2;
        v2.type = ir::IrType::I64;
        v2.is_const = true;
        v2.const_val = 0xDEADBEEFCAFE1234ULL;
        fn.values.push_back(v2);

        ir::IrBlock entry; entry.id = 0; entry.name = "entry";
        ir::IrInstr ret;
        ret.op = ir::IrOp::RET;
        ret.type = ir::IrType::HANDLE;
        ret.operands.push_back(0);
        entry.instrs.push_back(ret);
        fn.blocks.push_back(entry);

        std::vector<uint8_t> buf;
        ir::serialize_function(fn, buf);
        size_t off = 0;
        ir::IrFunction fn2;
        CHECK(ir::deserialize_function(buf, off, fn2), "deserialize ok");
        CHECK(same_function(fn, fn2), "round-trip con flags");
        CHECK(fn2.values[2].const_val == 0xDEADBEEFCAFE1234ULL,
              "const_val preservado");
    }

    /* ===================================================================== */
    /* Test 3: Funcion con multiples bloques + branches                        */
    /* ===================================================================== */

    void test_multi_block_branches() {
        ir::IrFunction fn;
        fn.name = "min";
        fn.ret_type = ir::IrType::I64;

        const auto a = mk_value(fn, ir::IrType::I64);
        const auto b = mk_value(fn, ir::IrType::I64);
        const auto cond = mk_value(fn, ir::IrType::BOOL);
        fn.params = {a, b};

        ir::IrBlock entry; entry.id = 0; entry.name = "entry";
        ir::IrInstr cmp;
        cmp.op = ir::IrOp::CMP_LT;
        cmp.type = ir::IrType::BOOL;
        cmp.dst = cond;
        cmp.operands = {a, b};
        entry.instrs.push_back(cmp);

        ir::IrInstr br;
        br.op = ir::IrOp::BR_COND;
        br.type = ir::IrType::VOID;
        br.dst = ir::IR_NO_VALUE;
        br.operands.push_back(cond);
        br.target_block = 1;
        br.false_block = 2;
        entry.instrs.push_back(br);
        entry.succs = {1, 2};

        ir::IrBlock then_bb; then_bb.id = 1; then_bb.name = "then";
        ir::IrInstr ret_a;
        ret_a.op = ir::IrOp::RET;
        ret_a.type = ir::IrType::I64;
        ret_a.operands.push_back(a);
        then_bb.instrs.push_back(ret_a);
        then_bb.preds = {0};

        ir::IrBlock else_bb; else_bb.id = 2; else_bb.name = "else";
        ir::IrInstr ret_b;
        ret_b.op = ir::IrOp::RET;
        ret_b.type = ir::IrType::I64;
        ret_b.operands.push_back(b);
        else_bb.instrs.push_back(ret_b);
        else_bb.preds = {0};

        fn.blocks = {entry, then_bb, else_bb};

        std::vector<uint8_t> buf;
        ir::serialize_function(fn, buf);
        size_t off = 0;
        ir::IrFunction fn2;
        CHECK(ir::deserialize_function(buf, off, fn2), "deserialize");
        CHECK(same_function(fn, fn2), "round-trip multi-block");
        CHECK(fn2.blocks[0].succs.size() == 2, "preds/succs preservados");
        CHECK(fn2.blocks[0].instrs[1].target_block == 1, "target_block preservado");
        CHECK(fn2.blocks[0].instrs[1].false_block == 2, "false_block preservado");
    }

    /* ===================================================================== */
    /* Test 4: PHI nodes                                                       */
    /* ===================================================================== */

    void test_phi_nodes() {
        ir::IrFunction fn;
        fn.name = "loop_with_phi";
        fn.ret_type = ir::IrType::I64;

        const auto p0 = mk_value(fn, ir::IrType::I64);
        const auto p1 = mk_value(fn, ir::IrType::I64);
        const auto phi = mk_value(fn, ir::IrType::I64);

        ir::IrBlock merge; merge.id = 0; merge.name = "merge";
        ir::IrInstr phi_instr;
        phi_instr.op = ir::IrOp::PHI;
        phi_instr.type = ir::IrType::I64;
        phi_instr.dst = phi;
        phi_instr.phi_args = {{p0, 1}, {p1, 2}};
        merge.instrs.push_back(phi_instr);

        ir::IrInstr ret;
        ret.op = ir::IrOp::RET;
        ret.type = ir::IrType::I64;
        ret.operands.push_back(phi);
        merge.instrs.push_back(ret);

        fn.blocks.push_back(merge);

        std::vector<uint8_t> buf;
        ir::serialize_function(fn, buf);
        size_t off = 0;
        ir::IrFunction fn2;
        CHECK(ir::deserialize_function(buf, off, fn2), "deserialize phi");
        CHECK(same_function(fn, fn2), "round-trip phi");
        CHECK(fn2.blocks[0].instrs[0].phi_args.size() == 2, "phi_args preservados");
        CHECK(fn2.blocks[0].instrs[0].phi_args[0].value == p0, "phi_args[0].value");
        CHECK(fn2.blocks[0].instrs[0].phi_args[1].block == 2, "phi_args[1].block");
    }

    /* ===================================================================== */
    /* Test 5: CALL con func_name                                              */
    /* ===================================================================== */

    void test_call_with_func_name() {
        ir::IrFunction fn;
        fn.name = "callee";
        fn.ret_type = ir::IrType::I64;

        const auto a = mk_value(fn, ir::IrType::I64);
        const auto b = mk_value(fn, ir::IrType::I64);
        const auto r = mk_value(fn, ir::IrType::I64);
        fn.params = {a, b};

        ir::IrBlock entry; entry.id = 0; entry.name = "entry";
        ir::IrInstr call;
        call.op = ir::IrOp::CALL;
        call.type = ir::IrType::I64;
        call.dst = r;
        call.func_name = "vrt_gc_alloc";
        call.operands = {a, b};
        call.source_line = 42;
        entry.instrs.push_back(call);

        ir::IrInstr ret;
        ret.op = ir::IrOp::RET;
        ret.type = ir::IrType::I64;
        ret.operands.push_back(r);
        entry.instrs.push_back(ret);

        fn.blocks.push_back(entry);

        std::vector<uint8_t> buf;
        ir::serialize_function(fn, buf);
        size_t off = 0;
        ir::IrFunction fn2;
        CHECK(ir::deserialize_function(buf, off, fn2), "deserialize call");
        CHECK(same_function(fn, fn2), "round-trip call");
        CHECK(fn2.blocks[0].instrs[0].func_name == "vrt_gc_alloc",
              "func_name preservado");
        CHECK(fn2.blocks[0].instrs[0].operands.size() == 2, "operands preservados");
        CHECK(fn2.blocks[0].instrs[0].source_line == 42, "source_line preservado");
    }

    /* ===================================================================== */
    /* Test 6: Generic template metadata (B.3)                                 */
    /* ===================================================================== */

    void test_generic_metadata() {
        ir::IrFunction fn;
        fn.name = "Box_i32__get";
        fn.ret_type = ir::IrType::I32;
        fn.generic_template_name = "Box";
        fn.generic_type_args = {"i32"};

        ir::IrBlock entry; entry.id = 0; entry.name = "entry";
        ir::IrInstr ret;
        ret.op = ir::IrOp::RET;
        ret.type = ir::IrType::I32;
        entry.instrs.push_back(ret);
        fn.blocks.push_back(entry);

        std::vector<uint8_t> buf;
        ir::serialize_function(fn, buf);
        size_t off = 0;
        ir::IrFunction fn2;
        CHECK(ir::deserialize_function(buf, off, fn2), "deserialize generic");
        CHECK(fn2.generic_template_name == "Box", "template_name preservado");
        CHECK(fn2.generic_type_args.size() == 1, "type_args size");
        CHECK(fn2.generic_type_args[0] == "i32", "type_args[0]");
    }

    /* ===================================================================== */
    /* Test 7: Multiples funciones secuenciales                                */
    /* ===================================================================== */

    void test_multiple_functions() {
        ir::IrFunction fn1;
        fn1.name = "f1";
        fn1.ret_type = ir::IrType::I64;
        ir::IrBlock e1; e1.id = 0; e1.name = "entry";
        ir::IrInstr i1;
        i1.op = ir::IrOp::RET; i1.type = ir::IrType::I64;
        e1.instrs.push_back(i1);
        fn1.blocks.push_back(e1);

        ir::IrFunction fn2;
        fn2.name = "f2";
        fn2.ret_type = ir::IrType::I32;
        ir::IrBlock e2; e2.id = 0; e2.name = "entry";
        ir::IrInstr i2;
        i2.op = ir::IrOp::RET; i2.type = ir::IrType::I32;
        e2.instrs.push_back(i2);
        fn2.blocks.push_back(e2);

        ir::IrFunction fn3;
        fn3.name = "f3_with_loop";
        fn3.ret_type = ir::IrType::I64;
        ir::IrBlock e3; e3.id = 0; e3.name = "loop";
        ir::IrInstr br;
        br.op = ir::IrOp::BR;
        br.target_block = 0;
        e3.instrs.push_back(br);
        e3.succs = {0};
        fn3.blocks.push_back(e3);

        /* Serializar las 3 al mismo buffer. */
        std::vector<uint8_t> buf;
        ir::serialize_function(fn1, buf);
        ir::serialize_function(fn2, buf);
        ir::serialize_function(fn3, buf);

        /* Deserializar las 3 secuencialmente. */
        size_t off = 0;
        ir::IrFunction got1, got2, got3;
        CHECK(ir::deserialize_function(buf, off, got1), "deserialize f1");
        CHECK(ir::deserialize_function(buf, off, got2), "deserialize f2");
        CHECK(ir::deserialize_function(buf, off, got3), "deserialize f3");
        CHECK(off == buf.size(), "consumido todo el buffer multi-fn");

        CHECK(same_function(fn1, got1), "f1 round-trip");
        CHECK(same_function(fn2, got2), "f2 round-trip");
        CHECK(same_function(fn3, got3), "f3 round-trip");
        CHECK(got1.name == "f1", "f1 nombre");
        CHECK(got2.name == "f2", "f2 nombre");
        CHECK(got3.name == "f3_with_loop", "f3 nombre");
    }

    /* ===================================================================== */
    /* Test 8: Buffer truncado retorna false                                   */
    /* ===================================================================== */

    /* ===================================================================== */
    /* Test 9: Seccion @ir completa (magic + multi-fn)                         */
    /* ===================================================================== */

    void test_ir_section_round_trip() {
        std::vector<ir::IrFunction> fns;

        /* Construir 3 funciones distintas. */
        for (int i = 0; i < 3; ++i) {
            ir::IrFunction fn;
            fn.name = "fn_" + std::to_string(i);
            fn.ret_type = ir::IrType::I64;

            const auto v = mk_const(fn, ir::IrType::I64,
                                     static_cast<uint64_t>(i * 100));
            ir::IrBlock e; e.id = 0; e.name = "entry";
            ir::IrInstr c;
            c.op = ir::IrOp::CONST;
            c.type = ir::IrType::I64;
            c.dst = v;
            c.imm = static_cast<uint64_t>(i * 100);
            e.instrs.push_back(c);
            ir::IrInstr r;
            r.op = ir::IrOp::RET;
            r.type = ir::IrType::I64;
            r.operands.push_back(v);
            e.instrs.push_back(r);
            fn.blocks.push_back(e);
            fns.push_back(std::move(fn));
        }

        /* Emit section. */
        std::vector<uint8_t> section = ir::emit_ir_section(fns);
        CHECK(section.size() > 12, "section tiene contenido");
        /* Magic en los primeros 4 bytes. */
        CHECK(section[0] == 'V' && section[1] == 'E'
           && section[2] == 'I' && section[3] == 'R',
              "magic VEIR al inicio");
        /* function_count en offset 8. */
        const uint32_t fc = static_cast<uint32_t>(section[8])
                           | (static_cast<uint32_t>(section[9])  << 8)
                           | (static_cast<uint32_t>(section[10]) << 16)
                           | (static_cast<uint32_t>(section[11]) << 24);
        CHECK(fc == 3, "function_count == 3");

        /* Parse de vuelta. */
        std::vector<ir::IrFunction> out_fns;
        bool ok = ir::parse_ir_section(section, 0, section.size(), out_fns);
        CHECK(ok, "parse_ir_section ok");
        CHECK(out_fns.size() == 3, "3 funciones parseadas");
        for (size_t i = 0; i < 3; ++i) {
            CHECK(same_function(fns[i], out_fns[i]),
                  ("fn[" + std::to_string(i) + "] round-trip").c_str());
        }
    }

    /* ===================================================================== */
    /* Test 10: parse_ir_section detecta magic invalido                        */
    /* ===================================================================== */

    void test_ir_section_invalid_magic() {
        /* Construir bytes con magic incorrecto. */
        std::vector<uint8_t> bad(16, 0);
        bad[0] = 'X'; bad[1] = 'X'; bad[2] = 'X'; bad[3] = 'X';  /* magic invalido */
        std::vector<ir::IrFunction> out;
        CHECK(!ir::parse_ir_section(bad, 0, bad.size(), out),
              "magic invalido -> false");
        CHECK(out.empty(), "out vacio tras failure");
    }

    /* ===================================================================== */
    /* Test 11: parse_ir_section detecta seccion truncada                      */
    /* ===================================================================== */

    void test_ir_section_truncated() {
        ir::IrFunction fn;
        fn.name = "single";
        fn.ret_type = ir::IrType::I64;
        ir::IrBlock e; e.id = 0; e.name = "entry";
        ir::IrInstr r; r.op = ir::IrOp::RET; r.type = ir::IrType::I64;
        e.instrs.push_back(r); fn.blocks.push_back(e);

        std::vector<ir::IrFunction> in_fns = {fn};
        std::vector<uint8_t> section = ir::emit_ir_section(in_fns);

        /* Truncar a la mitad. */
        std::vector<uint8_t> trunc(section.begin(),
                                    section.begin() + section.size() / 2);
        std::vector<ir::IrFunction> out;
        CHECK(!ir::parse_ir_section(trunc, 0, trunc.size(), out),
              "section truncada -> false");
    }

    /* ===================================================================== */
    /* Test 12: parse offset != 0 (seccion embedded en archivo mas grande)    */
    /* ===================================================================== */

    void test_ir_section_offset() {
        ir::IrFunction fn;
        fn.name = "embedded";
        fn.ret_type = ir::IrType::I32;
        ir::IrBlock e; e.id = 0; e.name = "entry";
        ir::IrInstr r; r.op = ir::IrOp::RET; r.type = ir::IrType::I32;
        e.instrs.push_back(r); fn.blocks.push_back(e);

        std::vector<ir::IrFunction> in_fns = {fn};
        std::vector<uint8_t> section = ir::emit_ir_section(in_fns);

        /* Simular un .velb: header dummy + section + footer dummy. */
        std::vector<uint8_t> velb;
        velb.resize(128, 0xFF);  /* header de 128 bytes basura */
        const size_t section_offset = velb.size();
        velb.insert(velb.end(), section.begin(), section.end());
        velb.resize(velb.size() + 64, 0xAA);  /* footer basura */

        /* Parse desde el offset correcto. */
        std::vector<ir::IrFunction> out;
        bool ok = ir::parse_ir_section(velb, section_offset,
                                        section.size(), out);
        CHECK(ok, "parse desde offset != 0");
        CHECK(out.size() == 1, "1 funcion encontrada");
        if (!out.empty()) {
            CHECK(out[0].name == "embedded", "nombre correcto");
            CHECK(out[0].ret_type == ir::IrType::I32, "ret_type correcto");
        }
    }

    void test_truncated_buffer() {
        ir::IrFunction fn;
        fn.name = "trunc_test";
        fn.ret_type = ir::IrType::I64;
        ir::IrBlock e; e.id = 0; e.name = "entry";
        ir::IrInstr ret;
        ret.op = ir::IrOp::RET; ret.type = ir::IrType::I64;
        e.instrs.push_back(ret);
        fn.blocks.push_back(e);

        std::vector<uint8_t> buf;
        ir::serialize_function(fn, buf);

        /* Truncar a la mitad y verificar que deserialize falla. */
        std::vector<uint8_t> trunc(buf.begin(),
                                    buf.begin() + buf.size() / 2);
        size_t off = 0;
        ir::IrFunction fn2;
        CHECK(!ir::deserialize_function(trunc, off, fn2),
              "buffer truncado -> false (sin crash)");
    }

} // namespace anonymous

int main() {
    test_trivial_function();
    test_value_flags();
    test_multi_block_branches();
    test_phi_nodes();
    test_call_with_func_name();
    test_generic_metadata();
    test_multiple_functions();
    test_ir_section_round_trip();
    test_ir_section_invalid_magic();
    test_ir_section_truncated();
    test_ir_section_offset();
    test_truncated_buffer();

    std::printf("test_ir_serialize: %d pass, %d fail\n", pass_count, fail_count);
    return fail_count == 0 ? 0 : 1;
}
