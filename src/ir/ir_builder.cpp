/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file ir_builder.cpp
 * @brief Implementacion del builder de SSA IR para multi-language frontends.
 */

#include "ir/ir_builder.h"
#include <cassert>

namespace ir {

// -- Bloques ----------------------------------------------------------------

IrBlockId IrBuilder::new_block(const std::string &name) {
    const IrBlockId id = static_cast<IrBlockId>(fn_.blocks.size());
    IrBlock         bb;
    bb.id   = id;
    bb.name = name.empty() ? ("bb_" + std::to_string(id)) : name;
    fn_.blocks.push_back(std::move(bb));
    return id;
}

// -- Parametros -------------------------------------------------------------

IrValueId IrBuilder::param(IrType type, const std::string &name) {
    const IrValueId id = new_value(type, name);
    fn_.values[id].is_param = true;
    fn_.params.push_back(id);
    return id;
}

// -- Constantes -------------------------------------------------------------

IrValueId IrBuilder::const_i32(int32_t v) {
    const IrValueId id = new_value(IrType::I32);
    fn_.values[id].is_const  = true;
    fn_.values[id].const_val = static_cast<uint64_t>(static_cast<int64_t>(v));
    IrInstr ins{};
    ins.op   = IrOp::CONST;
    ins.type = IrType::I32;
    ins.dst  = id;
    ins.imm  = static_cast<uint64_t>(static_cast<int64_t>(v));
    append(std::move(ins));
    return id;
}

IrValueId IrBuilder::const_i64(int64_t v) {
    const IrValueId id = new_value(IrType::I64);
    fn_.values[id].is_const  = true;
    fn_.values[id].const_val = static_cast<uint64_t>(v);
    IrInstr ins{};
    ins.op   = IrOp::CONST;
    ins.type = IrType::I64;
    ins.dst  = id;
    ins.imm  = static_cast<uint64_t>(v);
    append(std::move(ins));
    return id;
}

IrValueId IrBuilder::const_u32(uint32_t v) {
    const IrValueId id = new_value(IrType::U32);
    fn_.values[id].is_const  = true;
    fn_.values[id].const_val = v;
    IrInstr ins{};
    ins.op   = IrOp::CONST;
    ins.type = IrType::U32;
    ins.dst  = id;
    ins.imm  = v;
    append(std::move(ins));
    return id;
}

IrValueId IrBuilder::const_u64(uint64_t v) {
    const IrValueId id = new_value(IrType::U64);
    fn_.values[id].is_const  = true;
    fn_.values[id].const_val = v;
    IrInstr ins{};
    ins.op   = IrOp::CONST;
    ins.type = IrType::U64;
    ins.dst  = id;
    ins.imm  = v;
    append(std::move(ins));
    return id;
}

IrValueId IrBuilder::const_bool(bool v) {
    const IrValueId id = new_value(IrType::BOOL);
    fn_.values[id].is_const  = true;
    fn_.values[id].const_val = v ? 1u : 0u;
    IrInstr ins{};
    ins.op   = IrOp::CONST;
    ins.type = IrType::BOOL;
    ins.dst  = id;
    ins.imm  = v ? 1u : 0u;
    append(std::move(ins));
    return id;
}

IrValueId IrBuilder::const_ptr(uint64_t addr) {
    const IrValueId id = new_value(IrType::PTR);
    fn_.values[id].is_const  = true;
    fn_.values[id].const_val = addr;
    IrInstr ins{};
    ins.op   = IrOp::CONST;
    ins.type = IrType::PTR;
    ins.dst  = id;
    ins.imm  = addr;
    append(std::move(ins));
    return id;
}

// -- Aritmetica binaria -----------------------------------------------------

IrValueId IrBuilder::binop(IrOp op, IrValueId a, IrValueId b, IrType type) {
    const IrValueId id = new_value(type);
    IrInstr ins{};
    ins.op       = op;
    ins.type     = type;
    ins.dst      = id;
    ins.operands = {a, b};
    append(std::move(ins));
    return id;
}

IrValueId IrBuilder::add (IrValueId a, IrValueId b, IrType t) { return binop(IrOp::ADD, a, b, t); }
IrValueId IrBuilder::sub (IrValueId a, IrValueId b, IrType t) { return binop(IrOp::SUB, a, b, t); }
IrValueId IrBuilder::mul (IrValueId a, IrValueId b, IrType t) { return binop(IrOp::MUL, a, b, t); }
IrValueId IrBuilder::sdiv(IrValueId a, IrValueId b, IrType t) { return binop(IrOp::DIV, a, b, t); }
IrValueId IrBuilder::udiv(IrValueId a, IrValueId b, IrType t) { return binop(IrOp::DIV, a, b, t); }
IrValueId IrBuilder::smod(IrValueId a, IrValueId b, IrType t) { return binop(IrOp::MOD, a, b, t); }
IrValueId IrBuilder::umod(IrValueId a, IrValueId b, IrType t) { return binop(IrOp::MOD, a, b, t); }

IrValueId IrBuilder::and_(IrValueId a, IrValueId b, IrType t) { return binop(IrOp::AND, a, b, t); }
IrValueId IrBuilder::or_ (IrValueId a, IrValueId b, IrType t) { return binop(IrOp::OR,  a, b, t); }
IrValueId IrBuilder::xor_(IrValueId a, IrValueId b, IrType t) { return binop(IrOp::XOR, a, b, t); }
IrValueId IrBuilder::shl (IrValueId a, IrValueId b, IrType t) { return binop(IrOp::SHL, a, b, t); }
IrValueId IrBuilder::shr (IrValueId a, IrValueId b, IrType t) { return binop(IrOp::SHR, a, b, t); }
IrValueId IrBuilder::sar (IrValueId a, IrValueId b, IrType t) { return binop(IrOp::SAR, a, b, t); }

// -- Unarias ----------------------------------------------------------------

IrValueId IrBuilder::unop(IrOp op, IrValueId v, IrType type) {
    const IrValueId id = new_value(type);
    IrInstr ins{};
    ins.op       = op;
    ins.type     = type;
    ins.dst      = id;
    ins.operands = {v};
    append(std::move(ins));
    return id;
}

IrValueId IrBuilder::neg (IrValueId v, IrType t) { return unop(IrOp::NEG, v, t); }
IrValueId IrBuilder::not_(IrValueId v, IrType t) { return unop(IrOp::NOT, v, t); }

// -- Comparaciones ----------------------------------------------------------

IrValueId IrBuilder::cmpop(IrOp op, IrValueId a, IrValueId b) {
    const IrValueId id = new_value(IrType::BOOL);
    IrInstr ins{};
    ins.op       = op;
    ins.type     = IrType::BOOL;
    ins.dst      = id;
    ins.operands = {a, b};
    append(std::move(ins));
    return id;
}

IrValueId IrBuilder::cmp_eq (IrValueId a, IrValueId b) { return cmpop(IrOp::CMP_EQ,  a, b); }
IrValueId IrBuilder::cmp_ne (IrValueId a, IrValueId b) { return cmpop(IrOp::CMP_NE,  a, b); }
IrValueId IrBuilder::cmp_lt (IrValueId a, IrValueId b) { return cmpop(IrOp::CMP_LT,  a, b); }
IrValueId IrBuilder::cmp_le (IrValueId a, IrValueId b) { return cmpop(IrOp::CMP_LE,  a, b); }
IrValueId IrBuilder::cmp_gt (IrValueId a, IrValueId b) { return cmpop(IrOp::CMP_GT,  a, b); }
IrValueId IrBuilder::cmp_ge (IrValueId a, IrValueId b) { return cmpop(IrOp::CMP_GE,  a, b); }
IrValueId IrBuilder::cmp_ult(IrValueId a, IrValueId b) { return cmpop(IrOp::CMP_ULT, a, b); }
IrValueId IrBuilder::cmp_ule(IrValueId a, IrValueId b) { return cmpop(IrOp::CMP_ULE, a, b); }
IrValueId IrBuilder::cmp_ugt(IrValueId a, IrValueId b) { return cmpop(IrOp::CMP_UGT, a, b); }
IrValueId IrBuilder::cmp_uge(IrValueId a, IrValueId b) { return cmpop(IrOp::CMP_UGE, a, b); }

// -- Casts ------------------------------------------------------------------

IrValueId IrBuilder::castop(IrOp op, IrValueId v, IrType to) {
    const IrValueId id = new_value(to);
    IrInstr ins{};
    ins.op       = op;
    ins.type     = to;
    ins.dst      = id;
    ins.operands = {v};
    append(std::move(ins));
    return id;
}

IrValueId IrBuilder::sext   (IrValueId v, IrType t) { return castop(IrOp::SEXT,    v, t); }
IrValueId IrBuilder::zext   (IrValueId v, IrType t) { return castop(IrOp::ZEXT,    v, t); }
IrValueId IrBuilder::trunc  (IrValueId v, IrType t) { return castop(IrOp::TRUNC,   v, t); }
IrValueId IrBuilder::bitcast(IrValueId v, IrType t) { return castop(IrOp::BITCAST, v, t); }

// -- Memoria ----------------------------------------------------------------

IrValueId IrBuilder::alloca_bytes(uint32_t size_bytes) {
    const IrValueId id = new_value(IrType::PTR);
    IrInstr ins{};
    ins.op   = IrOp::ALLOCA;
    ins.type = IrType::I8;
    ins.dst  = id;
    ins.imm  = size_bytes;
    append(std::move(ins));
    return id;
}

IrValueId IrBuilder::raw_alloc(IrValueId size_bytes) {
    const IrValueId id = new_value(IrType::PTR);
    fn_.values[id].is_host_ptr = true;
    IrInstr ins{};
    ins.op       = IrOp::RAW_ALLOC;
    ins.type     = IrType::PTR;
    ins.dst      = id;
    ins.operands = {size_bytes};
    append(std::move(ins));
    return id;
}

void IrBuilder::raw_free(IrValueId ptr) {
    IrInstr ins{};
    ins.op       = IrOp::RAW_FREE;
    ins.type     = IrType::VOID;
    ins.operands = {ptr};
    append(std::move(ins));
}

IrValueId IrBuilder::load(IrValueId ptr, IrType type) {
    const IrValueId id = new_value(type);
    IrInstr ins{};
    ins.op       = IrOp::LOAD;
    ins.type     = type;
    ins.dst      = id;
    ins.operands = {ptr};
    append(std::move(ins));
    return id;
}

void IrBuilder::store(IrValueId value, IrValueId ptr, IrType type) {
    IrInstr ins{};
    ins.op       = IrOp::STORE;
    ins.type     = type;
    ins.operands = {value, ptr};
    append(std::move(ins));
}

// -- Control de flujo -------------------------------------------------------

void IrBuilder::br(IrBlockId target) {
    IrInstr ins{};
    ins.op           = IrOp::BR;
    ins.type         = IrType::VOID;
    ins.target_block = target;
    append(std::move(ins));
}

void IrBuilder::br_cond(IrValueId cond, IrBlockId t, IrBlockId f) {
    IrInstr ins{};
    ins.op           = IrOp::BR_COND;
    ins.type         = IrType::VOID;
    ins.operands     = {cond};
    ins.target_block = t;
    ins.false_block  = f;
    append(std::move(ins));
}

void IrBuilder::ret_void() {
    IrInstr ins{};
    ins.op   = IrOp::RET;
    ins.type = IrType::VOID;
    append(std::move(ins));
}

void IrBuilder::ret(IrValueId value) {
    IrInstr ins{};
    ins.op       = IrOp::RET;
    ins.type     = (value < fn_.values.size()) ? fn_.values[value].type : IrType::I64;
    ins.operands = {value};
    append(std::move(ins));
}

IrValueId IrBuilder::phi(IrType type,
                          const std::vector<std::pair<IrBlockId, IrValueId>> &pairs) {
    const IrValueId id = new_value(type);
    IrInstr ins{};
    ins.op   = IrOp::PHI;
    ins.type = type;
    ins.dst  = id;
    ins.phi_args.reserve(pairs.size());
    for (const auto &p : pairs) {
        IrPhiArg pa;
        pa.block = p.first;
        pa.value = p.second;
        ins.phi_args.push_back(pa);
    }
    append(std::move(ins));
    return id;
}

// -- Calls ------------------------------------------------------------------

IrValueId IrBuilder::call(const std::string &fn_name,
                           const std::vector<IrValueId> &args,
                           IrType ret_type) {
    const IrValueId id = (ret_type == IrType::VOID) ? IR_NO_VALUE
                                                    : new_value(ret_type);
    IrInstr ins{};
    ins.op        = IrOp::CALL;
    ins.type      = ret_type;
    ins.dst       = id;
    ins.func_name = fn_name;
    ins.operands  = args;
    append(std::move(ins));
    return id;
}

void IrBuilder::call_void(const std::string &fn_name,
                           const std::vector<IrValueId> &args) {
    IrInstr ins{};
    ins.op        = IrOp::CALL;
    ins.type      = IrType::VOID;
    ins.func_name = fn_name;
    ins.operands  = args;
    append(std::move(ins));
}

IrValueId IrBuilder::call_indirect(IrValueId fn_ptr,
                                    const std::vector<IrValueId> &args,
                                    IrType ret_type) {
    const IrValueId id = (ret_type == IrType::VOID) ? IR_NO_VALUE
                                                    : new_value(ret_type);
    IrInstr ins{};
    ins.op       = IrOp::CALLIND;
    ins.type     = ret_type;
    ins.dst      = id;
    ins.func_ptr = fn_ptr;
    ins.operands = args;
    append(std::move(ins));
    return id;
}

IrValueId IrBuilder::call_native(const std::string &lib_func,
                                  const std::vector<IrValueId> &args,
                                  IrType ret_type) {
    const IrValueId id = (ret_type == IrType::VOID) ? IR_NO_VALUE
                                                    : new_value(ret_type);
    IrInstr ins{};
    ins.op        = IrOp::CALLN;
    ins.type      = ret_type;
    ins.dst       = id;
    ins.func_name = lib_func;
    ins.operands  = args;
    append(std::move(ins));
    return id;
}

// -- Helpers ----------------------------------------------------------------

void IrBuilder::append(IrInstr ins) {
    if (current_block_ == IR_NO_BLOCK
     || current_block_ >= fn_.blocks.size()) {
        return;  // no-op: el frontend olvido set_insert_point
    }
    fn_.blocks[current_block_].instrs.push_back(std::move(ins));
}

IrValueId IrBuilder::alloca_init(IrValueId initial, IrType type) {
    /* Crear slot stack del tamano del tipo + STORE inicial. */
    uint32_t bytes = 8;
    switch (type) {
        case IrType::I8:  case IrType::U8:  case IrType::BOOL: bytes = 1; break;
        case IrType::I16: case IrType::U16:                    bytes = 2; break;
        case IrType::I32: case IrType::U32: case IrType::F32:  bytes = 4; break;
        case IrType::I64: case IrType::U64: case IrType::F64:
        case IrType::PTR:                                       bytes = 8; break;
        default: bytes = 8; break;
    }
    /* Alinear a 8 bytes para que el slot sea reusable */
    if (bytes < 8) bytes = 8;
    const IrValueId slot = alloca_bytes(bytes);
    store(initial, slot, type);
    return slot;
}

IrValueId IrBuilder::new_value(IrType type, const std::string &name) {
    const IrValueId id = static_cast<IrValueId>(fn_.values.size());
    IrValue v;
    v.id   = id;
    v.type = type;
    v.name = name.empty() ? ("%" + std::to_string(id)) : name;
    fn_.values.push_back(std::move(v));
    return id;
}

} // namespace ir
