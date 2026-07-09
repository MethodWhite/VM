#include "runtime/instruction_handlers.h"
#include "runtime/decode_table.h"

namespace runtime {

void init_instruction_handlers() {
    static bool initialized = false;
    if (initialized) return;
    initialized = true;

    auto &pri = decode_table_primary;
    auto &ext = decode_table_extended;

    // --- Familia Float: extendidos 0x5C, 0x5D, 0x80-0x87, 0xF0-0xFC ---
    ext[0x5C].handler = g_float_handler;
    ext[0x5D].handler = g_float_handler;
    for (int i = 0x80; i <= 0x87; ++i) ext[i].handler = g_float_handler;
    for (int i = 0xF0; i <= 0xFC; ++i) ext[i].handler = g_float_handler;

    // --- Familia ALU ---
    pri[0x04].handler = g_alu_handler;
    for (int i = 0x05; i <= 0x14; ++i) ext[i].handler = g_alu_handler;
    for (int i = 0x16; i <= 0x1F; ++i) ext[i].handler = g_alu_handler;
    for (int i = 0x40; i <= 0x43; ++i) ext[i].handler = g_alu_handler;
    ext[0x68].handler = g_alu_handler; ext[0x69].handler = g_alu_handler;
    ext[0x6A].handler = g_alu_handler;
    for (int i = 0x70; i <= 0x7D; ++i) ext[i].handler = g_alu_handler;

    // --- Familia GC ---
    ext[0x56].handler = g_gc_handler; ext[0x57].handler = g_gc_handler;
    ext[0x72].handler = g_gc_handler; ext[0x7E].handler = g_gc_handler;
    for (int i = 0xA0; i <= 0xAD; ++i) ext[i].handler = g_gc_handler;
    for (int i = 0xB0; i <= 0xB2; ++i) ext[i].handler = g_gc_handler;
    for (int i = 0xC0; i <= 0xC8; ++i) ext[i].handler = g_gc_handler;

    // --- Familia OOP ---
    ext[0xAE].handler = g_oop_handler;
    for (int i = 0xD0; i <= 0xEB; ++i) ext[i].handler = g_oop_handler;
    ext[0xFD].handler = g_oop_handler; ext[0xFE].handler = g_oop_handler;

    // --- Familia String ---
    for (int i = 0x46; i <= 0x54; ++i) ext[i].handler = g_string_handler;
    ext[0x5E].handler = g_string_handler;
    ext[0x6B].handler = g_string_handler; ext[0x6C].handler = g_string_handler;
    ext[0x6E].handler = g_string_handler; ext[0x6F].handler = g_string_handler;

    // --- Familia Sync ---
    for (int i = 0x35; i <= 0x39; ++i) ext[i].handler = g_sync_handler;

    // --- Familia Async ---
    for (int i = 0x29; i <= 0x2C; ++i) ext[i].handler = g_async_handler;
    ext[0x67].handler = g_async_handler;

    // --- Familia Coro ---
    ext[0x58].handler = g_coro_handler; ext[0x66].handler = g_coro_handler;
    for (int i = 0xEC; i <= 0xEF; ++i) ext[i].handler = g_coro_handler;

    // --- Familia Weak ---
    ext[0x30].handler = g_weak_handler; ext[0x32].handler = g_weak_handler;
    ext[0x34].handler = g_weak_handler;

    // --- Familia Closure ---
    for (int i = 0x20; i <= 0x26; ++i) ext[i].handler = g_closure_handler;

    // --- Familia Spimm ---
    ext[0x2E].handler = g_spimm_handler; ext[0x2F].handler = g_spimm_handler;

    // --- Familia Pattern ---
    ext[0x27].handler = g_pattern_handler; ext[0x28].handler = g_pattern_handler;

    // --- Familia Generic ---
    ext[0x3A].handler = g_generic_handler;

    // --- Familia Distrib ---
    for (int i = 0x3B; i <= 0x3E; ++i) ext[i].handler = g_distrib_handler;
    ext[0x59].handler = g_distrib_handler; ext[0x6D].handler = g_distrib_handler;

    // --- Familia Meta ---
    for (int i = 0x60; i <= 0x65; ++i) ext[i].handler = g_meta_handler;
    for (int i = 0xC9; i <= 0xCF; ++i) ext[i].handler = g_meta_handler;

    // --- Familia Exc ---
    ext[0x44].handler = g_exc_handler; ext[0x45].handler = g_exc_handler;

    // --- Familia Core (instrucciones base) ---
    for (int i = 0x10; i <= 0x16; ++i) pri[i].handler = g_core_handler;
    pri[0x28].handler = g_core_handler; pri[0x29].handler = g_core_handler;
    pri[0xC3].handler = g_core_handler;
    ext[0x03].handler = g_core_handler; ext[0x15].handler = g_core_handler;
    ext[0x2D].handler = g_core_handler; ext[0x55].handler = g_core_handler;
}

} // namespace runtime
