/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribucion obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 */

/**
 * @file test_ssa_ir.cpp
 * @brief Test de la SSA IR: construccion programatica, serializacion,
 *        parseo y verificacion de correctitud (round-trip).
 *
 * Cubre:
 *   - ir_type_name / ir_type_parse (todos los tipos)
 *   - ir_op_name   / ir_op_parse   (todos los grupos de opcodes)
 *   - IrFunction::new_value, new_block, append
 *   - ir_print (serializacion a texto)
 *   - ir_parse (parseo desde texto)
 *   - ir_verify (verificador SSA)
 *   - Instrucciones clave: CALLN, PHI, BR_COND, FUTURE/AWAIT, MONENTER/MONEXIT
 */

#include "ir/ssa_ir.h"

#include <cassert>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
//  Utilidades de test
// ---------------------------------------------------------------------------

static int g_pass = 0;
static int g_fail = 0;

static void check(bool cond, const char *msg) {
    if (cond) {
        ++g_pass;
        std::cout << "  [PASS] " << msg << "\n";
    } else {
        ++g_fail;
        std::cout << "  [FAIL] " << msg << "\n";
    }
}

// ---------------------------------------------------------------------------
//  Test 1: ir_type_name / ir_type_parse round-trip
// ---------------------------------------------------------------------------

static void test_type_roundtrip() {
    std::cout << "\n[Test 1] ir_type_name / ir_type_parse round-trip\n";

    struct { ir::IrType t; const char *expected; } cases[] = {
        { ir::IrType::VOID,   "void"   },
        { ir::IrType::I8,     "i8"     },
        { ir::IrType::I16,    "i16"    },
        { ir::IrType::I32,    "i32"    },
        { ir::IrType::I64,    "i64"    },
        { ir::IrType::U8,     "u8"     },
        { ir::IrType::U16,    "u16"    },
        { ir::IrType::U32,    "u32"    },
        { ir::IrType::U64,    "u64"    },
        { ir::IrType::F32,    "f32"    },
        { ir::IrType::F64,    "f64"    },
        { ir::IrType::PTR,    "ptr"    },
        { ir::IrType::HANDLE, "handle" },
        { ir::IrType::BOOL,   "bool"   },
    };

    for (auto &c : cases) {
        const char *name = ir::ir_type_name(c.t);
        char msg[64];
        snprintf(msg, sizeof(msg), "ir_type_name(%s) == \"%s\"", c.expected, c.expected);
        check(std::string(name) == c.expected, msg);

        ir::IrType parsed;
        bool ok = ir::ir_type_parse(c.expected, parsed);
        snprintf(msg, sizeof(msg), "ir_type_parse(\"%s\") round-trip", c.expected);
        check(ok && parsed == c.t, msg);
    }
}

// ---------------------------------------------------------------------------
//  Test 2: ir_op_name / ir_op_parse round-trip (muestra representativa)
// ---------------------------------------------------------------------------

static void test_op_roundtrip() {
    std::cout << "\n[Test 2] ir_op_name / ir_op_parse round-trip\n";

    struct { ir::IrOp op; const char *expected; } cases[] = {
        { ir::IrOp::CONST,    "const"    },
        { ir::IrOp::MOV,      "mov"      },
        { ir::IrOp::ADD,      "add"      },
        { ir::IrOp::SUB,      "sub"      },
        { ir::IrOp::MUL,      "mul"      },
        { ir::IrOp::DIV,      "div"      },
        { ir::IrOp::MOD,      "mod"      },
        { ir::IrOp::NEG,      "neg"      },
        { ir::IrOp::FADD,     "fadd"     },
        { ir::IrOp::FSUB,     "fsub"     },
        { ir::IrOp::FMUL,     "fmul"     },
        { ir::IrOp::FDIV,     "fdiv"     },
        { ir::IrOp::FNEG,     "fneg"     },
        { ir::IrOp::FABS,     "fabs"     },
        { ir::IrOp::FSQRT,    "fsqrt"    },
        { ir::IrOp::FMIN,     "fmin"     },
        { ir::IrOp::FMAX,     "fmax"     },
        { ir::IrOp::AND,      "and"      },
        { ir::IrOp::OR,       "or"       },
        { ir::IrOp::XOR,      "xor"      },
        { ir::IrOp::NOT,      "not"      },
        { ir::IrOp::SHL,      "shl"      },
        { ir::IrOp::SHR,      "shr"      },
        { ir::IrOp::SAR,      "sar"      },
        { ir::IrOp::CMP_EQ,   "cmp.eq"   },
        { ir::IrOp::CMP_NE,   "cmp.ne"   },
        { ir::IrOp::CMP_LT,   "cmp.lt"   },
        { ir::IrOp::CMP_GT,   "cmp.gt"   },
        { ir::IrOp::CMP_LE,   "cmp.le"   },
        { ir::IrOp::CMP_GE,   "cmp.ge"   },
        { ir::IrOp::CMP_ULT,  "cmp.ult"  },
        { ir::IrOp::CMP_UGT,  "cmp.ugt"  },
        { ir::IrOp::CMP_ULE,  "cmp.ule"  },
        { ir::IrOp::CMP_UGE,  "cmp.uge"  },
        { ir::IrOp::FCMP_EQ,  "fcmp.eq"  },
        { ir::IrOp::FCMP_NE,  "fcmp.ne"  },
        { ir::IrOp::FCMP_LT,  "fcmp.lt"  },
        { ir::IrOp::FCMP_GT,  "fcmp.gt"  },
        { ir::IrOp::FCMP_LE,  "fcmp.le"  },
        { ir::IrOp::FCMP_GE,  "fcmp.ge"  },
        { ir::IrOp::CAST,     "cast"     },
        { ir::IrOp::ZEXT,     "zext"     },
        { ir::IrOp::SEXT,     "sext"     },
        { ir::IrOp::TRUNC,    "trunc"    },
        { ir::IrOp::ITOF,     "itof"     },
        { ir::IrOp::UITOF,    "uitof"    },
        { ir::IrOp::FTOI,     "ftoi"     },
        { ir::IrOp::FTOUI,    "ftoui"    },
        { ir::IrOp::F32TOF64, "f32tof64" },
        { ir::IrOp::F64TOF32, "f64tof32" },
        { ir::IrOp::BITCAST,  "bitcast"  },
        { ir::IrOp::BR,       "br"       },
        { ir::IrOp::BR_COND,  "br.cond"  },
        { ir::IrOp::RET,      "ret"      },
        { ir::IrOp::UNREACHABLE, "unreachable" },
        { ir::IrOp::PHI,      "phi"      },
        { ir::IrOp::CALL,     "call"     },
        { ir::IrOp::CALLIND,  "callind"  },
        { ir::IrOp::TAILCALL, "tailcall" },
        { ir::IrOp::CALLVIRT, "callvirt" },
        { ir::IrOp::CALLN,    "calln"    },
        { ir::IrOp::ALLOCA,   "alloca"   },
        { ir::IrOp::LOAD,     "load"     },
        { ir::IrOp::STORE,    "store"    },
        { ir::IrOp::MEMCPY,   "memcpy"   },
        { ir::IrOp::NEWOBJ,   "newobj"   },
        { ir::IrOp::GETFIELD, "getfield" },
        { ir::IrOp::SETFIELD, "setfield" },
        { ir::IrOp::INSTANCEOF, "instanceof" },
        { ir::IrOp::CHECKCAST, "checkcast" },
        { ir::IrOp::ISNULL,   "isnull"   },
        { ir::IrOp::UNWRAP,   "unwrap"   },
        { ir::IrOp::SPECIALIZE, "specialize" },
        { ir::IrOp::THROW,    "throw"    },
        { ir::IrOp::TRYENTER, "tryenter" },
        { ir::IrOp::TRYLEAVE, "tryleave" },
        { ir::IrOp::LANDINGPAD, "landingpad" },
        { ir::IrOp::FUTURE,   "future"   },
        { ir::IrOp::AWAIT,    "await"    },
        { ir::IrOp::FULFILL,  "fulfill"  },
        { ir::IrOp::REJECT,   "reject"   },
        { ir::IrOp::MSGSEND,  "msgsend"  },
        { ir::IrOp::MSGRECV,  "msgrecv"  },
        { ir::IrOp::RSPAWN,   "rspawn"   },
        { ir::IrOp::MONENTER, "monenter" },
        { ir::IrOp::MONEXIT,  "monexit"  },
        { ir::IrOp::MONWAIT,  "monwait"  },
        { ir::IrOp::MONNOTI,  "monnoti"  },
        { ir::IrOp::MONNOTA,  "monnota"  },
        { ir::IrOp::GETPROC,  "getproc"  },
        { ir::IrOp::GETVM,    "getvm"    },
        { ir::IrOp::GETMGR,   "getmgr"   },
        { ir::IrOp::SPAWN,    "spawn"    },
        { ir::IrOp::RESUME,   "resume"   },
        { ir::IrOp::YIELD,    "yield"    },
        { ir::IrOp::SWAPCTX,  "swapctx"  },
    };

    for (auto &c : cases) {
        const char *name = ir::ir_op_name(c.op);
        char msg[80];
        snprintf(msg, sizeof(msg), "ir_op_name(%s)", c.expected);
        check(std::string(name) == c.expected, msg);

        ir::IrOp parsed;
        bool ok = ir::ir_op_parse(c.expected, parsed);
        snprintf(msg, sizeof(msg), "ir_op_parse(\"%s\") round-trip", c.expected);
        check(ok && parsed == c.op, msg);
    }
}

// ---------------------------------------------------------------------------
//  Test 3: construccion programatica + ir_verify (funcion add simple)
// ---------------------------------------------------------------------------

static ir::IrModule build_add_module() {
    ir::IrModule mod;
    mod.name = "test.add";

    ir::IrFunction fn;
    fn.name     = "add";
    fn.ret_type = ir::IrType::I64;

    // parametros: a: i64, b: i64
    ir::IrValueId va = fn.new_value(ir::IrType::I64, "a");
    ir::IrValueId vb = fn.new_value(ir::IrType::I64, "b");
    fn.params.push_back(va);
    fn.params.push_back(vb);
    fn.values[va].is_param = true;
    fn.values[vb].is_param = true;

    ir::IrBlockId entry = fn.new_block("entry");

    // %0 = add.i64 %a, %b
    ir::IrValueId vr = fn.new_value(ir::IrType::I64, "0");
    {
        ir::IrInstr ins;
        ins.op   = ir::IrOp::ADD;
        ins.type = ir::IrType::I64;
        ins.dst  = vr;
        ins.operands = { va, vb };
        fn.append(entry, ins);
    }

    // ret.i64 %0
    {
        ir::IrInstr ins;
        ins.op   = ir::IrOp::RET;
        ins.type = ir::IrType::I64;
        ins.operands = { vr };
        fn.append(entry, ins);
    }

    mod.add_function(std::move(fn));
    return mod;
}

static void test_build_and_verify_add() {
    std::cout << "\n[Test 3] Construccion programatica + ir_verify (add)\n";

    ir::IrModule mod = build_add_module();

    std::vector<std::string> errors;
    bool ok = ir::ir_verify(mod, errors);

    if (!ok) {
        for (auto &e : errors) std::cout << "    ERROR: " << e << "\n";
    }
    check(ok, "ir_verify(add) sin errores");
    check(mod.functions.size() == 1, "modulo tiene exactamente 1 funcion");
    check(mod.functions[0].blocks.size() == 1, "funcion tiene exactamente 1 bloque");
    check(mod.functions[0].blocks[0].instrs.size() == 2, "entry tiene 2 instrucciones");
}

// ---------------------------------------------------------------------------
//  Test 4: ir_print round-trip (add)
// ---------------------------------------------------------------------------

static void test_print_parse_add() {
    std::cout << "\n[Test 4] ir_print -> ir_parse round-trip (add)\n";

    ir::IrModule orig = build_add_module();

    std::ostringstream oss;
    ir::ir_print(orig, oss);
    std::string text = oss.str();

    check(!text.empty(), "ir_print produce salida no vacia");
    std::cout << "    --- texto generado ---\n";
    std::cout << text;
    std::cout << "    ----------------------\n";

    ir::IrModule parsed;
    std::string err;
    bool ok = ir::ir_parse(text, parsed, err);
    if (!ok) std::cout << "    PARSE ERROR: " << err << "\n";
    check(ok, "ir_parse del texto generado");
    check(parsed.functions.size() == 1, "modulo parseado tiene 1 funcion");

    std::vector<std::string> verr;
    bool vok = ir::ir_verify(parsed, verr);
    if (!vok) for (auto &e : verr) std::cout << "    VERIFY: " << e << "\n";
    check(vok, "ir_verify del modulo parseado");
}

// ---------------------------------------------------------------------------
//  Test 5: funcion con PHI y BR_COND (abs value)
// ---------------------------------------------------------------------------

static ir::IrModule build_abs_module() {
    ir::IrModule mod;
    mod.name = "test.abs";

    ir::IrFunction fn;
    fn.name     = "abs_val";
    fn.ret_type = ir::IrType::I64;

    ir::IrValueId vx = fn.new_value(ir::IrType::I64, "x");
    fn.params.push_back(vx);
    fn.values[vx].is_param = true;

    ir::IrBlockId entry  = fn.new_block("entry");
    ir::IrBlockId bb_neg = fn.new_block("negate");
    ir::IrBlockId bb_end = fn.new_block("end");

    // entry: cond = cmp.lt.i64 %x, 0
    ir::IrValueId v_zero = fn.new_value(ir::IrType::I64, "zero");
    {
        ir::IrInstr c;
        c.op  = ir::IrOp::CONST;
        c.type= ir::IrType::I64;
        c.dst = v_zero;
        c.imm = 0;
        fn.append(entry, c);
    }

    ir::IrValueId v_cond = fn.new_value(ir::IrType::BOOL, "cond");
    {
        ir::IrInstr c;
        c.op      = ir::IrOp::CMP_LT;
        c.type    = ir::IrType::I64;
        c.dst     = v_cond;
        c.operands= { vx, v_zero };
        fn.append(entry, c);
    }

    // br.cond %cond, negate, end
    {
        ir::IrInstr c;
        c.op           = ir::IrOp::BR_COND;
        c.type         = ir::IrType::VOID;
        c.operands     = { v_cond };
        c.target_block = bb_neg;
        c.false_block  = bb_end;
        fn.append(entry, c);
    }

    fn.blocks[bb_neg].preds = { entry };
    fn.blocks[bb_end].preds = { entry, bb_neg };

    // negate: %neg = neg.i64 %x
    ir::IrValueId v_neg = fn.new_value(ir::IrType::I64, "negx");
    {
        ir::IrInstr c;
        c.op      = ir::IrOp::NEG;
        c.type    = ir::IrType::I64;
        c.dst     = v_neg;
        c.operands= { vx };
        fn.append(bb_neg, c);
    }

    // br end
    {
        ir::IrInstr c;
        c.op           = ir::IrOp::BR;
        c.type         = ir::IrType::VOID;
        c.target_block = bb_end;
        fn.append(bb_neg, c);
    }

    // end: %result = phi.i64 [%x, entry], [%negx, negate]
    ir::IrValueId v_result = fn.new_value(ir::IrType::I64, "result");
    {
        ir::IrInstr c;
        c.op      = ir::IrOp::PHI;
        c.type    = ir::IrType::I64;
        c.dst     = v_result;
        c.phi_args= { {vx, entry}, {v_neg, bb_neg} };
        fn.append(bb_end, c);
    }

    // ret.i64 %result
    {
        ir::IrInstr c;
        c.op      = ir::IrOp::RET;
        c.type    = ir::IrType::I64;
        c.operands= { v_result };
        fn.append(bb_end, c);
    }

    mod.add_function(std::move(fn));
    return mod;
}

static void test_phi_brcond() {
    std::cout << "\n[Test 5] PHI + BR_COND round-trip (abs_val)\n";

    ir::IrModule orig = build_abs_module();

    std::vector<std::string> errs;
    bool vok = ir::ir_verify(orig, errs);
    if (!vok) for (auto &e : errs) std::cout << "    VERIFY: " << e << "\n";
    check(vok, "ir_verify(abs_val) sin errores");

    std::ostringstream oss;
    ir::ir_print(orig, oss);
    std::string text = oss.str();
    std::cout << "    --- texto ---\n" << text << "    ------------\n";

    ir::IrModule parsed;
    std::string perr;
    bool pok = ir::ir_parse(text, parsed, perr);
    if (!pok) std::cout << "    PARSE: " << perr << "\n";
    check(pok, "ir_parse(abs_val)");

    std::vector<std::string> verrs2;
    bool vok2 = ir::ir_verify(parsed, verrs2);
    if (!vok2) for (auto &e : verrs2) std::cout << "    VERIFY2: " << e << "\n";
    check(vok2, "ir_verify(abs_val parsed)");
}

// ---------------------------------------------------------------------------
//  Test 6: CALLN (llamada nativa FFI) + GETPROC
// ---------------------------------------------------------------------------

static void test_calln() {
    std::cout << "\n[Test 6] CALLN + GETPROC round-trip\n";

    ir::IrModule mod;
    mod.name = "test.calln";
    mod.native_libs.push_back("stdlib/native/io/vesta_io");

    ir::IrFunction fn;
    fn.name     = "print_hello";
    fn.ret_type = ir::IrType::VOID;

    ir::IrBlockId entry = fn.new_block("entry");

    // %proc = getproc
    ir::IrValueId v_proc = fn.new_value(ir::IrType::PTR, "proc");
    {
        ir::IrInstr c;
        c.op  = ir::IrOp::GETPROC;
        c.type= ir::IrType::PTR;
        c.dst = v_proc;
        fn.append(entry, c);
    }

    // %msg_addr = const.i64 0x1000
    ir::IrValueId v_addr = fn.new_value(ir::IrType::I64, "msg_addr");
    {
        ir::IrInstr c;
        c.op  = ir::IrOp::CONST;
        c.type= ir::IrType::I64;
        c.dst = v_addr;
        c.imm = 0x1000;
        fn.append(entry, c);
    }

    // %len = const.i64 13
    ir::IrValueId v_len = fn.new_value(ir::IrType::I64, "len");
    {
        ir::IrInstr c;
        c.op  = ir::IrOp::CONST;
        c.type= ir::IrType::I64;
        c.dst = v_len;
        c.imm = 13;
        fn.append(entry, c);
    }

    // calln.void @stdlib/native/io/vesta_io:vio_println(%proc, %msg_addr, %len)
    {
        ir::IrInstr c;
        c.op       = ir::IrOp::CALLN;
        c.type     = ir::IrType::VOID;
        c.dst      = ir::IR_NO_VALUE;
        c.func_name= "stdlib/native/io/vesta_io:vio_println";
        c.operands = { v_proc, v_addr, v_len };
        fn.append(entry, c);
    }

    // ret.void
    {
        ir::IrInstr c;
        c.op  = ir::IrOp::RET;
        c.type= ir::IrType::VOID;
        fn.append(entry, c);
    }

    mod.add_function(std::move(fn));

    std::vector<std::string> errs;
    bool vok = ir::ir_verify(mod, errs);
    if (!vok) for (auto &e : errs) std::cout << "    VERIFY: " << e << "\n";
    check(vok, "ir_verify(calln) sin errores");

    std::ostringstream oss;
    ir::ir_print(mod, oss);
    std::string text = oss.str();
    std::cout << "    --- texto ---\n" << text << "    ------------\n";

    ir::IrModule parsed;
    std::string perr;
    bool pok = ir::ir_parse(text, parsed, perr);
    if (!pok) std::cout << "    PARSE: " << perr << "\n";
    check(pok, "ir_parse(calln)");

    if (pok && !parsed.functions.empty()) {
        bool found_calln = false;
        bool found_getproc = false;
        for (auto &blk : parsed.functions[0].blocks) {
            for (auto &ins : blk.instrs) {
                if (ins.op == ir::IrOp::CALLN)   found_calln   = true;
                if (ins.op == ir::IrOp::GETPROC)  found_getproc = true;
            }
        }
        check(found_calln,   "instruccion CALLN presente tras parseo");
        check(found_getproc, "instruccion GETPROC presente tras parseo");
    }
}

// ---------------------------------------------------------------------------
//  Test 7: async — FUTURE / FULFILL / AWAIT
// ---------------------------------------------------------------------------

static void test_async() {
    std::cout << "\n[Test 7] FUTURE / FULFILL / AWAIT round-trip\n";

    ir::IrModule mod;
    mod.name = "test.async";

    ir::IrFunction fn;
    fn.name     = "async_example";
    fn.ret_type = ir::IrType::VOID;

    ir::IrBlockId entry = fn.new_block("entry");

    // %fut = future
    ir::IrValueId v_fut = fn.new_value(ir::IrType::HANDLE, "fut");
    {
        ir::IrInstr c;
        c.op  = ir::IrOp::FUTURE;
        c.type= ir::IrType::HANDLE;
        c.dst = v_fut;
        fn.append(entry, c);
    }

    // %val = const.i64 42
    ir::IrValueId v_val = fn.new_value(ir::IrType::I64, "val");
    {
        ir::IrInstr c;
        c.op  = ir::IrOp::CONST;
        c.type= ir::IrType::I64;
        c.dst = v_val;
        c.imm = 42;
        fn.append(entry, c);
    }

    // fulfill %fut, %val
    {
        ir::IrInstr c;
        c.op      = ir::IrOp::FULFILL;
        c.type    = ir::IrType::VOID;
        c.operands= { v_fut, v_val };
        fn.append(entry, c);
    }

    // %result = await.i64 %fut
    ir::IrValueId v_result = fn.new_value(ir::IrType::I64, "result");
    {
        ir::IrInstr c;
        c.op      = ir::IrOp::AWAIT;
        c.type    = ir::IrType::I64;
        c.dst     = v_result;
        c.operands= { v_fut };
        fn.append(entry, c);
    }

    // ret.void
    {
        ir::IrInstr c;
        c.op  = ir::IrOp::RET;
        c.type= ir::IrType::VOID;
        fn.append(entry, c);
    }

    mod.add_function(std::move(fn));

    std::ostringstream oss;
    ir::ir_print(mod, oss);
    std::string text = oss.str();

    ir::IrModule parsed;
    std::string perr;
    bool pok = ir::ir_parse(text, parsed, perr);
    if (!pok) std::cout << "    PARSE: " << perr << "\n";
    check(pok, "ir_parse(async: future/fulfill/await)");

    std::vector<std::string> verrs;
    bool vok = ir::ir_verify(parsed, verrs);
    if (!vok) for (auto &e : verrs) std::cout << "    VERIFY: " << e << "\n";
    check(vok, "ir_verify(async) sin errores");
}

// ---------------------------------------------------------------------------
//  Test 8: monitores — MONENTER / MONEXIT
// ---------------------------------------------------------------------------

static void test_monitors() {
    std::cout << "\n[Test 8] MONENTER / MONEXIT round-trip\n";

    ir::IrModule mod;
    mod.name = "test.monitor";

    ir::IrFunction fn;
    fn.name     = "sync_example";
    fn.ret_type = ir::IrType::VOID;

    ir::IrBlockId entry = fn.new_block("entry");

    // %obj = const.ptr 0x2000
    ir::IrValueId v_obj = fn.new_value(ir::IrType::PTR, "obj");
    {
        ir::IrInstr c;
        c.op  = ir::IrOp::CONST;
        c.type= ir::IrType::PTR;
        c.dst = v_obj;
        c.imm = 0x2000;
        fn.append(entry, c);
    }

    // monenter %obj
    {
        ir::IrInstr c;
        c.op      = ir::IrOp::MONENTER;
        c.type    = ir::IrType::VOID;
        c.operands= { v_obj };
        fn.append(entry, c);
    }

    // monexit %obj
    {
        ir::IrInstr c;
        c.op      = ir::IrOp::MONEXIT;
        c.type    = ir::IrType::VOID;
        c.operands= { v_obj };
        fn.append(entry, c);
    }

    // ret.void
    {
        ir::IrInstr c;
        c.op  = ir::IrOp::RET;
        c.type= ir::IrType::VOID;
        fn.append(entry, c);
    }

    mod.add_function(std::move(fn));

    std::ostringstream oss;
    ir::ir_print(mod, oss);
    std::string text = oss.str();

    ir::IrModule parsed;
    std::string perr;
    bool pok = ir::ir_parse(text, parsed, perr);
    if (!pok) std::cout << "    PARSE: " << perr << "\n";
    check(pok, "ir_parse(monitor)");

    std::vector<std::string> verrs;
    bool vok = ir::ir_verify(parsed, verrs);
    if (!vok) for (auto &e : verrs) std::cout << "    VERIFY: " << e << "\n";
    check(vok, "ir_verify(monitor) sin errores");
}

// ---------------------------------------------------------------------------
//  Test 9: excepciones — THROW / TRYENTER / TRYLEAVE
// ---------------------------------------------------------------------------

static void test_exceptions() {
    std::cout << "\n[Test 9] THROW / TRYENTER / TRYLEAVE round-trip\n";

    ir::IrModule mod;
    mod.name = "test.exc";

    ir::IrFunction fn;
    fn.name     = "exc_example";
    fn.ret_type = ir::IrType::VOID;

    ir::IrBlockId entry   = fn.new_block("entry");
    ir::IrBlockId handler = fn.new_block("handler");
    ir::IrBlockId bb_end  = fn.new_block("end");

    fn.blocks[handler].preds = { entry };
    fn.blocks[bb_end].preds  = { entry, handler };

    // %handler_addr = const.i64  (id de handler block — valor simbolico)
    ir::IrValueId v_hpc = fn.new_value(ir::IrType::I64, "hpc");
    {
        ir::IrInstr c;
        c.op  = ir::IrOp::CONST;
        c.type= ir::IrType::I64;
        c.dst = v_hpc;
        c.imm = 0xDEAD;
        fn.append(entry, c);
    }

    // %cls = const.ptr 0 (catch-all: nullptr)
    ir::IrValueId v_cls = fn.new_value(ir::IrType::PTR, "cls");
    {
        ir::IrInstr c;
        c.op  = ir::IrOp::CONST;
        c.type= ir::IrType::PTR;
        c.dst = v_cls;
        c.imm = 0;
        fn.append(entry, c);
    }

    // tryenter %hpc, %cls
    {
        ir::IrInstr c;
        c.op      = ir::IrOp::TRYENTER;
        c.type    = ir::IrType::VOID;
        c.operands= { v_hpc, v_cls };
        fn.append(entry, c);
    }

    // %exc = const.ptr 0x3000
    ir::IrValueId v_exc = fn.new_value(ir::IrType::PTR, "exc");
    {
        ir::IrInstr c;
        c.op  = ir::IrOp::CONST;
        c.type= ir::IrType::PTR;
        c.dst = v_exc;
        c.imm = 0x3000;
        fn.append(entry, c);
    }

    // throw %exc  (terminador del bloque entry)
    {
        ir::IrInstr c;
        c.op      = ir::IrOp::THROW;
        c.type    = ir::IrType::VOID;
        c.operands= { v_exc };
        fn.append(entry, c);
    }

    // handler: tryleave
    {
        ir::IrInstr c;
        c.op  = ir::IrOp::TRYLEAVE;
        c.type= ir::IrType::VOID;
        fn.append(handler, c);
    }

    // br end
    {
        ir::IrInstr c;
        c.op           = ir::IrOp::BR;
        c.type         = ir::IrType::VOID;
        c.target_block = bb_end;
        fn.append(handler, c);
    }

    // end: ret.void
    {
        ir::IrInstr c;
        c.op  = ir::IrOp::RET;
        c.type= ir::IrType::VOID;
        fn.append(bb_end, c);
    }

    mod.add_function(std::move(fn));

    std::ostringstream oss;
    ir::ir_print(mod, oss);
    std::string text = oss.str();
    std::cout << "    --- texto ---\n" << text << "    ------------\n";

    ir::IrModule parsed;
    std::string perr;
    bool pok = ir::ir_parse(text, parsed, perr);
    if (!pok) std::cout << "    PARSE: " << perr << "\n";
    check(pok, "ir_parse(exceptions)");

    std::vector<std::string> verrs;
    bool vok = ir::ir_verify(parsed, verrs);
    if (!vok) for (auto &e : verrs) std::cout << "    VERIFY: " << e << "\n";
    check(vok, "ir_verify(exceptions) sin errores");
}

// ---------------------------------------------------------------------------
//  Test 10: tipos flotantes — FADD, FSQRT, ITOF, FCMP_LT
// ---------------------------------------------------------------------------

static void test_float_ops() {
    std::cout << "\n[Test 10] Operaciones flotantes round-trip\n";

    ir::IrModule mod;
    mod.name = "test.float";

    ir::IrFunction fn;
    fn.name     = "float_ops";
    fn.ret_type = ir::IrType::F64;

    ir::IrValueId vn = fn.new_value(ir::IrType::I64, "n");
    fn.params.push_back(vn);
    fn.values[vn].is_param = true;

    ir::IrBlockId entry = fn.new_block("entry");

    // %f = itof.f64 %n
    ir::IrValueId vf = fn.new_value(ir::IrType::F64, "f");
    {
        ir::IrInstr c;
        c.op      = ir::IrOp::ITOF;
        c.type    = ir::IrType::F64;
        c.dst     = vf;
        c.operands= { vn };
        fn.append(entry, c);
    }

    // %sq = fsqrt.f64 %f
    ir::IrValueId vsq = fn.new_value(ir::IrType::F64, "sq");
    {
        ir::IrInstr c;
        c.op      = ir::IrOp::FSQRT;
        c.type    = ir::IrType::F64;
        c.dst     = vsq;
        c.operands= { vf };
        fn.append(entry, c);
    }

    // %sum = fadd.f64 %f, %sq
    ir::IrValueId vsum = fn.new_value(ir::IrType::F64, "sum");
    {
        ir::IrInstr c;
        c.op      = ir::IrOp::FADD;
        c.type    = ir::IrType::F64;
        c.dst     = vsum;
        c.operands= { vf, vsq };
        fn.append(entry, c);
    }

    // ret.f64 %sum
    {
        ir::IrInstr c;
        c.op      = ir::IrOp::RET;
        c.type    = ir::IrType::F64;
        c.operands= { vsum };
        fn.append(entry, c);
    }

    mod.add_function(std::move(fn));

    std::ostringstream oss;
    ir::ir_print(mod, oss);
    std::string text = oss.str();

    ir::IrModule parsed;
    std::string perr;
    bool pok = ir::ir_parse(text, parsed, perr);
    if (!pok) std::cout << "    PARSE: " << perr << "\n";
    check(pok, "ir_parse(float_ops)");

    std::vector<std::string> verrs;
    bool vok = ir::ir_verify(parsed, verrs);
    if (!vok) for (auto &e : verrs) std::cout << "    VERIFY: " << e << "\n";
    check(vok, "ir_verify(float_ops) sin errores");
}

// ---------------------------------------------------------------------------
//  main
// ---------------------------------------------------------------------------

int main() {
    std::cout << "=== SSA IR Test Suite ===\n";

    test_type_roundtrip();
    test_op_roundtrip();
    test_build_and_verify_add();
    test_print_parse_add();
    test_phi_brcond();
    test_calln();
    test_async();
    test_monitors();
    test_exceptions();
    test_float_ops();

    std::cout << "\n=== Resultado: " << g_pass << " PASS, " << g_fail << " FAIL ===\n";
    return g_fail == 0 ? 0 : 1;
}
