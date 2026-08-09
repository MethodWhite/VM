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
 * @file aot/aot_compiler.cpp
 * @brief Implementacion del compilador AOT Vex Bare.
 *
 * Pipeline:
 *   1. Optimizar IR (ir_optimize)
 *   2. Asignar registros (allocate_regs + liveness)
 *   3. Seleccionar instrucciones (jit::Selector)
 *   4. Emitir codigo nativo (jit::X86Encoder)
 *   5. Emitir archivo objeto ELF64 (ElfEmitter)
 *   6. Linkar con runtime segun tier
 *
 = Tres tiers =
 *
 *   Full:  runtime completo (GC, scheduler, async).  Salida ~3-5 MB.
 *   Embed: mini-runtime, sin GC, sin distribucion.  Salida ~500 KB-1 MB.
 *   Bare:  freestanding, sin runtime.  Salida ~50-200 KB.
 */

#include "aot/aot_compiler.h"
#include "aot/elf_emitter.h"

#include "ir/ssa_ir.h"
#include "ir/ir_optimizer.h"
#include "ir/liveness.h"
#include "ir/regalloc.h"
#include "jit/selector.h"
#include "jit/x86_encoder.h"
#include "jit/runtime_entries.h"
#include "jit/machine_ir.h"
#include "jit/auto_jit.h" // g_jit_warn_unsupported (diagnostico AOT)

#include <cstring>
#include <cstdio>
#include <algorithm>
#include <unordered_set>

namespace aot {

    // =========================================================================
    //  Helpers internos
    // =========================================================================

    namespace {

        /**
         * @brief Convierte un Tier AOT en un modo de selector.
         */
        jit::SelectorMode selector_mode_for_tier(Tier t) {
            switch (t) {
                case Tier::BARE:  return jit::SelectorMode::NATIVE_ABI;
                case Tier::EMBED: return jit::SelectorMode::VM_ABI;
                case Tier::FULL:
                default:          return jit::SelectorMode::VM_ABI;
            }
        }

        /**
         * @brief Combina STT_* y STB_* en st_info.
         */
        uint8_t st_info(uint8_t bind, uint8_t type) {
            return static_cast<uint8_t>((bind << 4) | type);
        }

        /**
         * @brief Indice de seccion para un nombre dado (1-based, 0 = undef).
         */
        uint16_t section_index(const std::vector<SectionInfo> &sections,
                               const std::string &name) {
            for (size_t i = 0; i < sections.size(); ++i) {
                if (sections[i].name == name)
                    return static_cast<uint16_t>(i + 1);
            }
            return SHN_UNDEF;
        }

        /**
         * @brief Emite codigo de prologo estandar x86-64.
         *
         * push rbp
         * mov  rbp, rsp
         * sub  rsp, <frame_size>
         */
        void emit_prologue(std::vector<uint8_t> &code, uint32_t frame_size) {
            // push rbp
            code.push_back(0x55);
            // mov rbp, rsp
            code.push_back(0x48);
            code.push_back(0x89);
            code.push_back(0xE5);
            // sub rsp, frame_size
            if (frame_size > 0) {
                code.push_back(0x48);
                code.push_back(0x81);
                code.push_back(0xEC);
                code.push_back(static_cast<uint8_t>(frame_size));
                code.push_back(static_cast<uint8_t>(frame_size >> 8));
                code.push_back(static_cast<uint8_t>(frame_size >> 16));
                code.push_back(static_cast<uint8_t>(frame_size >> 24));
            }
        }

        /**
         * @brief Emite codigo de epilogo estandar x86-64.
         *
         * mov rsp, rbp
         * pop rbp
         * ret
         */
        void emit_epilogue(std::vector<uint8_t> &code) {
            // mov rsp, rbp
            code.push_back(0x48);
            code.push_back(0x89);
            code.push_back(0xEC);
            // pop rbp
            code.push_back(0x5D);
            // ret
            code.push_back(0xC3);
        }

        /**
         * @brief Emite un salto relativo a 32 bits.
         */
        void emit_jmp_rel32(std::vector<uint8_t> &code, int32_t offset) {
            code.push_back(0xE9);
            code.push_back(static_cast<uint8_t>(offset));
            code.push_back(static_cast<uint8_t>(offset >> 8));
            code.push_back(static_cast<uint8_t>(offset >> 16));
            code.push_back(static_cast<uint8_t>(offset >> 24));
        }

        /**
         * @brief Emite mov rax, imm64
         */
        void emit_mov_rax_imm64(std::vector<uint8_t> &code, uint64_t val) {
            code.push_back(0x48);
            code.push_back(0xB8);
            for (int i = 0; i < 8; ++i)
                code.push_back(static_cast<uint8_t>((val >> (i * 8)) & 0xFF));
        }

        /**
         * @brief Emite call rax
         */
        void emit_call_rax(std::vector<uint8_t> &code) {
            code.push_back(0xFF);
            code.push_back(0xD0);
        }

        /**
         * @brief Emite xor eax, eax (return 0)
         */
        void emit_xor_eax(std::vector<uint8_t> &code) {
            code.push_back(0x31);
            code.push_back(0xC0);
        }

        /**
         * @brief Emite un syscall de salida (exit_group).
         * rdi = exit code
         */
        void emit_exit_syscall(std::vector<uint8_t> &code) {
            // mov eax, 231 (exit_group en x86-64)
            code.push_back(0xB8);
            code.push_back(0xE7);
            code.push_back(0x00);
            code.push_back(0x00);
            code.push_back(0x00);
            // syscall
            code.push_back(0x0F);
            code.push_back(0x05);
        }

        /**
         * @brief Emite codigo _start para Bare tier (freestanding).
         *
         * _start:
         *     call main
         *     mov  edi, eax
         *     mov  eax, 231  ; exit_group
         *     syscall
         */
        void emit_bare_start_stub(std::vector<uint8_t> &code) {
            // _start:
            // call main (placeholder, relocacion R_X86_64_PLT32)
            code.push_back(0xE8);
            // offset placeholder (4 bytes, rellenado por relocacion)
            code.push_back(0x00);
            code.push_back(0x00);
            code.push_back(0x00);
            code.push_back(0x00);
            // mov edi, eax  (return value -> arg0 de exit)
            code.push_back(0x89);
            code.push_back(0xC7);
            // exit_group syscall
            emit_exit_syscall(code);
        }

    } // namespace anon

    // =========================================================================
    //  Resolucion de simbolos runtime
    // =========================================================================

    std::vector<std::pair<std::string, uint64_t>> AotCompiler::resolve_runtime_symbols() {
        std::vector<std::pair<std::string, uint64_t>> syms;

        switch (options_.tier) {
            case Tier::FULL:
                syms = {
                    {"vrt_gc_alloc",         0},
                    {"vrt_gc_alloc_pinned",  0},
                    {"vrt_gc_deref",         0},
                    {"vrt_gc_handle_for_ptr",0},
                    {"vrt_gc_drop",          0},
                    {"vrt_gc_write_barrier", 0},
                    {"vrt_throw_fatal",      0},
                    {"vrt_safepoint_poll",   0},
                    {"vrt_async_spawn",      0},
                    {"vrt_async_await",      0},
                    {"vrt_sched_yield",      0},
                    {"vrt_monitor_enter",    0},
                    {"vrt_monitor_exit",     0},
                    {"malloc",               0},
                    {"free",                 0},
                    {"memcpy",               0},
                    {"memset",               0},
                };
                break;

            case Tier::EMBED:
                syms = {
                    {"vrt_throw_fatal",      0},
                    {"vrt_async_spawn",      0},
                    {"vrt_async_await",      0},
                    {"malloc",               0},
                    {"free",                 0},
                    {"memcpy",               0},
                    {"memset",               0},
                };
                break;

            case Tier::BARE:
                // Sin runtime: solo funciones intrinsicas
                syms = {
                    {"memcpy", 0},
                    {"memset", 0},
                };
                break;
        }

        return syms;
    }

    // =========================================================================
    //  Startup stub segun tier
    // =========================================================================

    void AotCompiler::emit_startup_stub(std::vector<uint8_t> &code) {
        switch (options_.tier) {
            case Tier::FULL:
            case Tier::EMBED:
                // Para FULL/EMBED: _start que inicializa runtime y llama a main
                // En v1, usamos el _start del runtime enlazado externamente.
                // Aqui solo emitimos un stub que llama a _vesta_init y luego main.
                emit_prologue(code, 32);
                // call _vesta_init (placeholder)
                code.push_back(0xE8);
                code.push_back(0x00); code.push_back(0x00);
                code.push_back(0x00); code.push_back(0x00);
                // xor eax, eax (argc = 0)
                emit_xor_eax(code);
                // xor edi, edi (argv = NULL)
                code.push_back(0x31); code.push_back(0xFF);
                // call main
                code.push_back(0xE8);
                code.push_back(0x00); code.push_back(0x00);
                code.push_back(0x00); code.push_back(0x00);
                // exit_group
                emit_exit_syscall(code);
                break;

            case Tier::BARE:
                emit_bare_start_stub(code);
                break;
        }
    }

    // =========================================================================
    //  Compilacion de funcion individual
    // =========================================================================

    bool AotCompiler::compile_function(const ir::IrFunction &ir_fn,
                                       std::vector<uint8_t> &code,
                                       std::unordered_map<std::string, uint64_t> &sym_offsets,
                                       const std::function<uint64_t(const std::string &)> &resolve_user_fn) {
        // 1. Calcular liveness
        ir::LivenessResult liveness;
        try {
            liveness = ir::compute_liveness(ir_fn);
        } catch (...) {
            return false;
        }

        // 2. Asignar registros
        ir::AllocResult regs = ir::allocate_regs(ir_fn, liveness);
        if (!regs.ok) {
            return false;
        }

        // 3. Seleccionar instrucciones (IrFunction -> MFunction)
        jit::SelectorOptions sel_opts;
        sel_opts.mode = selector_mode_for_tier(options_.tier);
        sel_opts.resolve_user_fn = resolve_user_fn;

        /* Diagnostico: si VESTA_JIT_WARN esta activo, que el selector
         * emita el detalle de la op no soportada DURANTE la seleccion. */
        if (std::getenv("VESTA_JIT_WARN"))
            jit::g_jit_warn_unsupported = true;

        jit::Selector selector(sel_opts);
        bool unsupported = false;
        jit::MFunction mfn = selector.select(ir_fn, &unsupported);
        if (unsupported) {
            std::fprintf(stderr, "[aot] funcion '%s': op no soportada "
                                 "por el selector (NATIVE_ABI)\n",
                         ir_fn.name.c_str());
            return false;
        }
        jit::X86Encoder encoder;
        size_t before = code.size();
        size_t emitted = encoder.encode(mfn, code);
        if (emitted == 0) {
            return false;
        }

        // Registrar offset de la funcion
        sym_offsets[ir_fn.name] = before;

        return true;
    }

    // =========================================================================
    //  Compilacion principal
    // =========================================================================

    AotResult AotCompiler::compile(const ir::IrModule &mod) {
        AotResult result;

        try {
            // 1. Copiar modulo y optimizar
            ir::IrModule opt_mod = mod;
            ir::ir_optimize(opt_mod, options_.opt_level);

            // 2. Compilar cada funcion.
            //    main se compila AL FINAL: asi el resolver de CALLs ya
            //    conoce los offsets de las callees (compiladas antes).
            std::vector<uint8_t> text_code;
            std::unordered_map<std::string, uint64_t> fn_offsets;

            /* Resolver de CALLs a funciones user: devuelve el offset del
             * callee dentro del texto.  Solo es valido para callees ya
             * compiladas (por eso main va al final). */
            /* Resolver de CALLs a funciones user: devuelve la direccion
             * ABSOLUTA del callee (0x400000 + offset en el texto).  El
             * texto se mapea en 0x400000 en el ejecutable final; el
             * selector emite call rax con esta direccion.  0 = no
             * resuelto (sentinel del selector).
             *
             * Self-recursion: cuando el callee es la funcion en compilacion
             * (todavia no registrada en fn_offsets), se devuelve la direccion
             * basada en el offset ACTUAL del texto (donde empieza esta fn). */
            const uint64_t AOT_TEXT_BASE = 0x400000u;
            uint64_t cur_fn_offset = 0; // offset del texto de la fn en curso
            auto aot_resolver = [&](const std::string &name) -> uint64_t {
                auto it = fn_offsets.find(name);
                if (it != fn_offsets.end())
                    return AOT_TEXT_BASE + it->second;
                /* Self-ref: la fn se esta compilando; su offset es el actual. */
                if (name == current_fn_name_)
                    return AOT_TEXT_BASE + cur_fn_offset;
                if (std::getenv("VESTA_JIT_WARN"))
                    std::fprintf(stderr, "[aot-resolver] '%s' NO compilada (offsets=%zu)\n",
                                 name.c_str(), fn_offsets.size());
                return 0;
            };

            auto compile_one = [&](const ir::IrFunction &fn) -> bool {
                if (fn.is_native) return true; // saltar stubs nativos
                uint64_t offset_before = text_code.size();
                current_fn_name_ = fn.name;
                cur_fn_offset    = offset_before;
                if (!compile_function(fn, text_code, fn_offsets, aot_resolver)) {
                    current_fn_name_.clear();
                    result.error = "Fallo al compilar funcion: " + fn.name;
                    return false;
                }
                fn_offsets[fn.name] = offset_before;
                current_fn_name_.clear();
                return true;
            };

            for (const auto &fn : opt_mod.functions) {
                if (fn.name == "main") continue;
                if (!compile_one(fn)) return result;
            }
            for (const auto &fn : opt_mod.functions) {
                if (fn.name != "main") continue;
                if (!compile_one(fn)) return result;
            }

            // 3. Emitir startup stub
            uint64_t start_offset = text_code.size();
            emit_startup_stub(text_code);

            /* Resolver la relocacion interna del stub: en el ejecutable
             * directo (BARE) el _start hace `call main` con un placeholder
             * de 4 bytes (offset rel32).  Sin resolverlo, el call apunta a
             * si mismo y el exit code de main se pierde.  El offset relativo
             * es: dst - (PC_after_call) = main_off - (start_off + 5).
             * El call main es la primera instruccion del stub (offset 0). */
            if (options_.tier == Tier::BARE && !opt_mod.functions.empty()) {
                auto it_main = fn_offsets.find("main");
                if (it_main == fn_offsets.end() && !opt_mod.functions.empty()) {
                    /* Si no hay `main` literal, usar la primera funcion. */
                    it_main = fn_offsets.begin();
                }
                if (it_main != fn_offsets.end()) {
                    const uint64_t call_pc = start_offset + 5; // tras E8 + 4
                    const int64_t  disp =
                        static_cast<int64_t>(it_main->second) -
                        static_cast<int64_t>(call_pc);
                    const size_t disp_off = start_offset + 1;
                    if (disp_off + 4 <= text_code.size()) {
                        text_code[disp_off + 0] = static_cast<uint8_t>(disp & 0xFF);
                        text_code[disp_off + 1] = static_cast<uint8_t>((disp >> 8) & 0xFF);
                        text_code[disp_off + 2] = static_cast<uint8_t>((disp >> 16) & 0xFF);
                        text_code[disp_off + 3] = static_cast<uint8_t>((disp >> 24) & 0xFF);
                    }
                }
            }

            // 4. Construir secciones para el emisor ELF
            std::vector<SectionInfo> sections;

            // .text
            SectionInfo text_sec;
            text_sec.name   = ".text";
            text_sec.type   = SHT_PROGBITS;
            text_sec.flags  = SHF_ALLOC | SHF_EXECINSTR;
            text_sec.addr   = 0;
            text_sec.align  = 16;
            text_sec.entsize = 0;
            text_sec.link   = 0;
            text_sec.info   = 0;
            text_sec.data   = std::move(text_code);
            sections.push_back(text_sec);

            // .rodata (static data del modulo)
            SectionInfo rodata_sec;
            rodata_sec.name   = ".rodata";
            rodata_sec.type   = SHT_PROGBITS;
            rodata_sec.flags  = SHF_ALLOC;
            rodata_sec.addr   = 0;
            rodata_sec.align  = 8;
            rodata_sec.entsize = 0;
            rodata_sec.link   = 0;
            rodata_sec.info   = 0;
            rodata_sec.data   = mod.static_data.bytes;
            sections.push_back(rodata_sec);

            // .data (vacio en v1)
            SectionInfo data_sec;
            data_sec.name   = ".data";
            data_sec.type   = SHT_PROGBITS;
            data_sec.flags  = SHF_ALLOC | SHF_WRITE;
            data_sec.addr   = 0;
            data_sec.align  = 8;
            data_sec.entsize = 0;
            data_sec.link   = 0;
            data_sec.info   = 0;
            sections.push_back(data_sec);

            // .bss (vacio en v1)
            SectionInfo bss_sec;
            bss_sec.name   = ".bss";
            bss_sec.type   = SHT_NOBITS;
            bss_sec.flags  = SHF_ALLOC | SHF_WRITE;
            bss_sec.addr   = 0;
            bss_sec.align  = 8;
            bss_sec.entsize = 0;
            bss_sec.link   = 0;
            bss_sec.info   = 0;
            sections.push_back(bss_sec);

            // 5. Construir tabla de simbolos
            std::vector<SymbolInfo> symbols;
            std::vector<RelocInfo>  relocations;

            // Simbolo STT_FILE
            SymbolInfo file_sym;
            file_sym.name  = mod.name + ".vex";
            file_sym.info  = st_info(STB_LOCAL, STT_FILE);
            file_sym.other = STV_DEFAULT;
            file_sym.shndx = SHN_ABS;
            file_sym.value = 0;
            file_sym.size  = 0;
            symbols.push_back(file_sym);

            // Simbolos de funciones (STB_GLOBAL, STT_FUNC)
            uint16_t text_shndx = section_index(sections, ".text");
            for (const auto &fn : opt_mod.functions) {
                if (fn.is_native) continue;
                auto it = fn_offsets.find(fn.name);
                if (it == fn_offsets.end()) continue;

                SymbolInfo sym;
                sym.name  = fn.name;
                sym.info  = st_info(STB_GLOBAL, STT_FUNC);
                sym.other = STV_DEFAULT;
                sym.shndx = text_shndx;
                sym.value = it->second;
                sym.size  = 0; // se calcula si se desea
                symbols.push_back(sym);
            }

            // Simbolo _start
            SymbolInfo start_sym;
            start_sym.name  = "_start";
            start_sym.info  = st_info(STB_GLOBAL, STT_FUNC);
            start_sym.other = STV_DEFAULT;
            start_sym.shndx = text_shndx;
            start_sym.value = start_offset;
            start_sym.size  = 0;
            symbols.push_back(start_sym);

            // 6. Relocaciones para simbolos runtime
            auto rt_syms = resolve_runtime_symbols();
            uint32_t extern_sym_idx = static_cast<uint32_t>(symbols.size());

            for (const auto &rt : rt_syms) {
                SymbolInfo sym;
                sym.name  = rt.first;
                sym.info  = st_info(STB_GLOBAL, STT_NOTYPE);
                sym.other = STV_DEFAULT;
                sym.shndx = SHN_UNDEF;  // simbolo externo no definido
                sym.value = 0;
                sym.size  = 0;
                symbols.push_back(sym);
            }

            // Relocaciones: por ahora placeholder para _start -> main
            // En un sistema real, el linker (ld) resuelve estas.
            // Para el ejecutable directo, emitimos relocaciones internas.

            // 7. Crear emisor ELF y generar archivo objeto
            ElfEmitter emitter;
            for (const auto &sec : sections) {
                emitter.add_section(sec);
            }
            for (const auto &sym : symbols) {
                emitter.add_symbol(sym);
            }
            for (const auto &reloc : relocations) {
                emitter.add_relocation(reloc);
            }

            if (options_.output_format == OutputFormat::ELF) {
                result.object_data = emitter.emit();

                // Generar ejecutable directo si es Bare
                if (options_.tier == Tier::BARE) {
                    uint64_t entry_vaddr = 0x400000 + start_offset;
                    // Ajustar: entry es offset dentro de .text, sumar base vaddr
                    result.executable = emitter.emit_executable(entry_vaddr);
                }
            }

            // 8. Llenar metadatos del resultado
            result.ok = true;

            // Calcular sizes
            result.code_size = 0;
            result.data_size = 0;
            for (const auto &sec : sections) {
                if (sec.flags & SHF_EXECINSTR)
                    result.code_size += sec.data.size();
                else
                    result.data_size += sec.data.size();
            }

            // Estimar tamano total segun tier
            switch (options_.tier) {
                case Tier::FULL:
                    result.total_size = result.code_size + result.data_size + 3 * 1024 * 1024;
                    break;
                case Tier::EMBED:
                    result.total_size = result.code_size + result.data_size + 512 * 1024;
                    break;
                case Tier::BARE:
                    result.total_size = result.code_size + result.data_size + 50 * 1024;
                    break;
            }

            result.symbol_offsets = std::move(fn_offsets);

        } catch (const std::exception &e) {
            result.ok    = false;
            result.error = std::string("Excepcion: ") + e.what();
        } catch (...) {
            result.ok    = false;
            result.error = "Error desconocido durante compilacion AOT";
        }

        return result;
    }

} // namespace aot
