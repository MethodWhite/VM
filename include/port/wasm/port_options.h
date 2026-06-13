/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

#ifndef PORT_WASM_PORT_OPTIONS_H
#define PORT_WASM_PORT_OPTIONS_H

#include <cstdint>
#include "port/port_options.h"

namespace port {

    struct WasmPortOptions : public PortOptions {
        uint32_t initial_memory_pages = 1;
        uint32_t max_memory_pages = 256;
        std::string wasm_module_name;
        bool emit_start_function = true;
        uint32_t wasm_version = 1;
    };

} // namespace port

#endif // PORT_WASM_PORT_OPTIONS_H
