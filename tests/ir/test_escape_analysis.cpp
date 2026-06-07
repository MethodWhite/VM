/*
 * Test: Escape Analysis + Scalar Replacement de objetos GC (Phase C2.13).
 *
 * Verifica el comportamiento de @c ir_pass_scalar_replace_gc construyendo
 * modulos IR a mano (sin pasar por el frontend Vex):
 *
 *   1. Objeto read-only `f = new Foo(i); return f.x`  -> se transforma
 *      (el `call __new_Foo` desaparece, el load se reemplaza por el arg).
 *   2. Objeto mutable single-block (store-to-load forwarding) -> se transforma.
 *   3. Objeto usado en un PHI (`r = cond ? a : b`)            -> BAILA (el pase
 *      NO debe eliminar el alloc dejando el PHI colgando).  Regresion de un bug
 *      en el que la recoleccion de usos solo miraba @c operands y no
 *      @c phi_args (rompia bug69_phi_class).
 *   4. Objeto que escapa por return                          -> BAILA.
 */

#include "ir/ir_builder.h"
#include "ir/ssa_ir.h"
#include "ir/ir_optimizer.h"
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace ir;

static int g_checks = 0;
static int g_fails  = 0;

static void check(bool cond, const std::string &msg) {
    ++g_checks;
    if (!cond) { ++g_fails; std::printf("  FAIL: %s\n", msg.c_str()); }
    else        std::printf("  ok:   %s\n", msg.c_str());
}

/** @brief Cuenta las instrucciones `call __new_Foo` en @p fn. */
static int count_new_calls(const IrFunction &fn) {
    int n = 0;
    for (const auto &b : fn.blocks)
        for (const auto &in : b.instrs)
            if (in.op == IrOp::CALL && in.func_name == "__new_Foo") ++n;
    return n;
}

/** @brief Modulo con la clase Foo { i32 x } y su ctor trivial Foo__ctor. */
static IrModule make_module_with_foo() {
    IrModule mod;
    mod.name = "test";

    /* Clase Foo con un unico constructor propio. */
    IrClass cls;
    cls.name = "Foo";
    cls.has_destructor = false;
    cls.has_destructible_field = false;
    cls.is_aspect = false;
    IrMethod ctor;
    ctor.name = "ctor";
    ctor.ir_fn_name = "Foo__ctor";
    ctor.is_constructor = true;
    ctor.defining_class = "Foo";
    cls.methods.push_back(ctor);
    mod.classes.push_back(cls);

    /* Foo__ctor(this: ptr, v: i32) { store v, this+24; ret } */
    IrFunction ctor_fn;
    ctor_fn.name = "Foo__ctor";
    ctor_fn.ret_type = IrType::VOID;
    {
        IrBuilder b(ctor_fn);
        IrValueId thisv = b.param(IrType::PTR, "this");
        IrValueId v     = b.param(IrType::I32, "v");
        IrBlockId entry = b.new_block("entry");
        b.set_insert_point(entry);
        IrValueId off  = b.const_i64(24);
        IrValueId addr = b.add(thisv, off, IrType::PTR);
        b.store(v, addr, IrType::I32);
        b.ret_void();
    }
    mod.functions.push_back(std::move(ctor_fn));
    return mod;
}

/** @brief read_only(i): `f = new Foo(i); return f.x`. */
static IrFunction make_readonly_caller() {
    IrFunction fn; fn.name = "read_only"; fn.ret_type = IrType::I32;
    IrBuilder b(fn);
    IrValueId arg = b.param(IrType::I32, "i");
    IrBlockId entry = b.new_block("entry");
    b.set_insert_point(entry);
    IrValueId o    = b.call("__new_Foo", {arg}, IrType::PTR);
    IrValueId off  = b.const_i64(24);
    IrValueId addr = b.add(o, off, IrType::PTR);
    IrValueId val  = b.load(addr, IrType::I32);
    b.ret(val);
    return fn;
}

/** @brief mutable(v): `c = new Foo(v); c.x = c.x + 5; return c.x`. */
static IrFunction make_mutable_caller() {
    IrFunction fn; fn.name = "mutable"; fn.ret_type = IrType::I32;
    IrBuilder b(fn);
    IrValueId arg = b.param(IrType::I32, "v");
    IrBlockId entry = b.new_block("entry");
    b.set_insert_point(entry);
    IrValueId o    = b.call("__new_Foo", {arg}, IrType::PTR);
    IrValueId off  = b.const_i64(24);
    IrValueId addr = b.add(o, off, IrType::PTR);
    IrValueId v1   = b.load(addr, IrType::I32);     // ctor-init
    IrValueId five = b.const_i32(5);
    IrValueId w    = b.add(v1, five, IrType::I32);
    b.store(w, addr, IrType::I32);                   // field write
    IrValueId v2   = b.load(addr, IrType::I32);     // forward a w
    b.ret(v2);
    return fn;
}

/** @brief phi(cond): dos objetos seleccionados por un PHI -> debe BAILAR. */
static IrFunction make_phi_caller() {
    IrFunction fn; fn.name = "phi_pick"; fn.ret_type = IrType::I32;
    IrBuilder b(fn);
    IrValueId cond = b.param(IrType::I32, "cond");
    IrBlockId entry = b.new_block("entry");
    IrBlockId bt    = b.new_block("t");
    IrBlockId bf    = b.new_block("f");
    IrBlockId merge = b.new_block("merge");
    b.set_insert_point(entry);
    IrValueId c10  = b.const_i32(10);
    IrValueId a    = b.call("__new_Foo", {c10}, IrType::PTR);
    IrValueId c20  = b.const_i32(20);
    IrValueId bb   = b.call("__new_Foo", {c20}, IrType::PTR);
    IrValueId zero = b.const_i32(0);
    IrValueId cmp  = b.cmp_eq(cond, zero);
    b.br_cond(cmp, bt, bf);
    b.set_insert_point(bt); b.br(merge);
    b.set_insert_point(bf); b.br(merge);
    b.set_insert_point(merge);
    IrValueId r    = b.phi(IrType::PTR, {{bt, a}, {bf, bb}});
    IrValueId off  = b.const_i64(24);
    IrValueId addr = b.add(r, off, IrType::PTR);
    IrValueId val  = b.load(addr, IrType::I32);
    b.ret(val);
    return fn;
}

/** @brief escaping(i): `o = new Foo(i); return o`  (el objeto escapa). */
static IrFunction make_escaping_caller() {
    IrFunction fn; fn.name = "escaping"; fn.ret_type = IrType::PTR;
    IrBuilder b(fn);
    IrValueId arg = b.param(IrType::I32, "i");
    IrBlockId entry = b.new_block("entry");
    b.set_insert_point(entry);
    IrValueId o = b.call("__new_Foo", {arg}, IrType::PTR);
    b.ret(o);
    return fn;
}

/**
 * @brief crossb(cond, p): objeto con escritura del campo en AMBAS ramas y
 *        lectura tras el merge.  Cross-block -> requiere SROA/mem2reg con un
 *        PHI para el campo en el bloque merge.
 *
 *   f = new Foo(p);
 *   if (cond == 0) f.x = f.x + 10; else f.x = f.x * 2;
 *   return f.x;
 */
static IrFunction make_crossblock_caller() {
    IrFunction fn; fn.name = "crossb"; fn.ret_type = IrType::I32;
    IrBuilder b(fn);
    IrValueId cond = b.param(IrType::I32, "cond");
    IrValueId p    = b.param(IrType::I32, "p");
    IrBlockId entry = b.new_block("entry");
    IrBlockId bt    = b.new_block("then");
    IrBlockId be    = b.new_block("else");
    IrBlockId merge = b.new_block("merge");
    b.set_insert_point(entry);
    IrValueId o    = b.call("__new_Foo", {p}, IrType::PTR);
    IrValueId off  = b.const_i64(24);
    IrValueId addr = b.add(o, off, IrType::PTR);   /* field-addr cross-block */
    IrValueId zero = b.const_i32(0);
    IrValueId cmp  = b.cmp_eq(cond, zero);
    b.br_cond(cmp, bt, be);
    b.set_insert_point(bt);
    IrValueId la  = b.load(addr, IrType::I32);
    IrValueId ten = b.const_i32(10);
    IrValueId a1  = b.add(la, ten, IrType::I32);
    b.store(a1, addr, IrType::I32);
    b.br(merge);
    b.set_insert_point(be);
    IrValueId lb  = b.load(addr, IrType::I32);
    IrValueId two = b.const_i32(2);
    IrValueId a2  = b.mul(lb, two, IrType::I32);
    b.store(a2, addr, IrType::I32);
    b.br(merge);
    b.set_insert_point(merge);
    IrValueId v   = b.load(addr, IrType::I32);
    b.ret(v);
    return fn;
}

int main() {
    /* SROA/mem2reg esta activo por defecto; asegurar que el entorno no lo
     * tenga desactivado para el caso cross-block. */
#if defined(_WIN32)
    _putenv("VESTA_NO_ESCAPE_MEM2REG=0");
#else
    setenv("VESTA_NO_ESCAPE_MEM2REG", "0", 1);
#endif
    std::printf("=== test_escape_analysis (Phase C2.13) ===\n");

    /* (1) read-only -> se transforma. */
    {
        IrModule mod = make_module_with_foo();
        IrFunction caller = make_readonly_caller();
        check(count_new_calls(caller) == 1, "read_only: 1 call __new_Foo antes");
        bool changed = ir_pass_scalar_replace_gc(caller, mod);
        check(changed, "read_only: el pase reporta cambio");
        check(count_new_calls(caller) == 0,
              "read_only: el alloc se elimino (scalar replacement)");
    }

    /* (2) mutable single-block -> se transforma (field versioning). */
    {
        IrModule mod = make_module_with_foo();
        IrFunction caller = make_mutable_caller();
        check(count_new_calls(caller) == 1, "mutable: 1 call __new_Foo antes");
        bool changed = ir_pass_scalar_replace_gc(caller, mod);
        check(changed, "mutable: el pase reporta cambio");
        check(count_new_calls(caller) == 0,
              "mutable: el alloc se elimino (field versioning)");
    }

    /* (3) PHI de objetos -> BAILA (no elimina los allocs). */
    {
        IrModule mod = make_module_with_foo();
        IrFunction caller = make_phi_caller();
        check(count_new_calls(caller) == 2, "phi: 2 call __new_Foo antes");
        ir_pass_scalar_replace_gc(caller, mod);
        check(count_new_calls(caller) == 2,
              "phi: los allocs se preservan (bail por uso en PHI)");
    }

    /* (4) objeto que escapa por return -> BAILA. */
    {
        IrModule mod = make_module_with_foo();
        IrFunction caller = make_escaping_caller();
        check(count_new_calls(caller) == 1, "escaping: 1 call __new_Foo antes");
        ir_pass_scalar_replace_gc(caller, mod);
        check(count_new_calls(caller) == 1,
              "escaping: el alloc se preserva (objeto retornado)");
    }

    /* (5) cross-block con escrituras -> SROA/mem2reg lo transforma (inserta un
     * PHI para el campo en el merge). */
    {
        IrModule mod = make_module_with_foo();
        IrFunction caller = make_crossblock_caller();
        check(count_new_calls(caller) == 1, "crossb: 1 call __new_Foo antes");
        bool changed = ir_pass_scalar_replace_gc(caller, mod);
        check(changed, "crossb: el pase reporta cambio");
        check(count_new_calls(caller) == 0,
              "crossb: el alloc se elimino (mem2reg cross-block)");
        /* Debe existir un PHI para el campo en el bloque merge. */
        int n_phi = 0;
        for (const auto &b : caller.blocks)
            for (const auto &in : b.instrs)
                if (in.op == ir::IrOp::PHI) ++n_phi;
        check(n_phi >= 1, "crossb: se inserto un PHI para el campo");
    }

    std::printf("--- %d checks, %d fallos ---\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
