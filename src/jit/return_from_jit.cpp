/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak)
 * Licencia VMProject
 */

#include "jit/interp_jit_bridge.h"
#include "runtime/proceso_runtime.h"
#include "runtime/runtime.h"

namespace jit {

void return_from_jit(vrt_proc *proc, uint64_t bytecode_pc) {
    if (!proc) return;
    auto *p = reinterpret_cast<runtime::ProcessVM *>(proc);
    p->registers.rip.qword(bytecode_pc);
    p->jit_active = 0;
}

} // namespace jit
