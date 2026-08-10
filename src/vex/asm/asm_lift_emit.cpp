/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 *
 * Software libre bajo GPLv2.  La salida del compilador (programas
 * escritos en Vesta) NO queda sujeta a la GPL (excepcion de runtime).
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file asm_lift_emit.cpp
 * @brief Implementacion de la emision del IR tipado del lift de asm.
 */

#include "vex/asm/asm_lift_emit.h"

namespace vex {

namespace {

/// Emite un LOAD i64 del slot @p slot y devuelve el valor.  Si @p host, marca el
/// resultado como host_ptr (una direccion @c [reg] del asm es una direccion del
/// proceso host, no del espacio VM).
ir::IrValueId load_slot(ir::IrFunction &fn, uint32_t block,
                        ir::IrValueId slot, bool host, uint32_t line) {
    const ir::IrValueId v = fn.new_value(ir::IrType::I64);
    ir::IrInstr ld{};
    ld.op = ir::IrOp::LOAD;
    ld.type = ir::IrType::I64;
    ld.dst = v;
    ld.operands = {slot};
    ld.source_line = line;
    fn.append(block, std::move(ld));
    if (host)
        fn.values[v].is_host_ptr = true;
    return v;
}

/// Emite un STORE i64 de @p val al slot @p slot.
void store_slot(ir::IrFunction &fn, uint32_t block, ir::IrValueId slot,
                ir::IrValueId val, uint32_t line) {
    ir::IrInstr st{};
    st.op = ir::IrOp::STORE;
    st.type = ir::IrType::I64;
    st.operands = {val, slot};
    st.source_line = line;
    fn.append(block, std::move(st));
}

/// Emite un op atomico tipado (@p op con @p operands) y devuelve el valor viejo.
ir::IrValueId emit_atomic(ir::IrFunction &fn, uint32_t block, ir::IrOp op,
                          std::vector<ir::IrValueId> operands, uint32_t line) {
    const ir::IrValueId v = fn.new_value(ir::IrType::I64);
    ir::IrInstr ins{};
    ins.op = op;
    ins.type = ir::IrType::I64;
    ins.dst = v;
    ins.operands = std::move(operands);
    ins.source_line = line;
    fn.append(block, std::move(ins));
    return v;
}

} // namespace

bool asm_lift_emit(
    ir::IrFunction &fn, uint32_t block, instr_db::Isa isa,
    const std::string &body,
    const std::unordered_map<std::string, ir::IrValueId> &slot_of,
    uint32_t line) {
    const AsmLift lift = asm_lift_detect(isa, body);
    if (lift.op == AsmLiftOp::None)
        return false;

    // Todos los registros del patron deben estar ligados por register().
    auto slot = [&](const std::string &r) -> const ir::IrValueId * {
        auto it = slot_of.find(r);
        return (it == slot_of.end()) ? nullptr : &it->second;
    };
    const ir::IrValueId *addr = slot(lift.addr_reg);
    const ir::IrValueId *des = slot(lift.des_reg);
    const ir::IrValueId *result = slot(lift.result_reg);
    if (!addr || !des || !result)
        return false;

    // La direccion [reg] es una direccion del host.
    const ir::IrValueId addr_v = load_slot(fn, block, *addr, true, line);
    ir::IrValueId old;
    if (lift.op == AsmLiftOp::AtomicCas) {
        const ir::IrValueId *exp = slot(lift.exp_reg);
        if (!exp)
            return false;
        const ir::IrValueId exp_v = load_slot(fn, block, *exp, false, line);
        const ir::IrValueId des_v = load_slot(fn, block, *des, false, line);
        old = emit_atomic(fn, block, ir::IrOp::ATOMIC_CAS_I64,
                          {addr_v, exp_v, des_v}, line);
    } else { // AtomicAdd
        const ir::IrValueId delta_v = load_slot(fn, block, *des, false, line);
        old = emit_atomic(fn, block, ir::IrOp::ATOMIC_ADD_I64,
                          {addr_v, delta_v}, line);
    }
    store_slot(fn, block, *result, old, line);
    return true;
}

} // namespace vex
