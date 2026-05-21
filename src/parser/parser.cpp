/*
 * VestaVM - Maquina Virtual Distribuida
 * 
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 * 
 * USO LIBRE NO COMERCIAL con atribucion obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 * 
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file parser.cpp
 * @brief Implementacion principal del parser recursivo descendente de VestaVM.
 *
 * Implementa @c vm::Parser: el metodo @c parse() que genera el AST completo,
 * @c parse_instruction(), @c parse_operand(), @c parse_annotation() y demas
 * metodos de produccion.  Tambien implementa @c is_valid_number() y @c parse_number().
 */
#include "parser/parser.h"

#include <iomanip>

#include "Levenshtein.hpp"

namespace vm {
    static const std::unordered_map<std::string, InstructionPattern> InstructionSet = {
        // DOS operandos
        {"mov", {"mov", OpArity::TWO}},
        {"movh", {"movh", OpArity::TWO}},
        {"xchg", {"xchg", OpArity::TWO}},
        {"readcur",  {"readcur",  OpArity::TWO}},
        {"writecur", {"writecur", OpArity::TWO}},
        {"gcderef",  {"gcderef",  OpArity::TWO}},
        {"addcur",   {"addcur",   OpArity::TWO}},
        {"vmcopy",   {"vmcopy",   OpArity::THREE}},
        {"vcopyh",   {"vcopyh",   OpArity::THREE}},
        {"getproc",  {"getproc",  OpArity::ONE}},
        {"getvm",    {"getvm",    OpArity::ONE}},
        {"getmgr",   {"getmgr",   OpArity::ONE}},
        {"realloc", {"realloc", OpArity::TWO}},

        // UN operando
        {"newobj", {"newobj", OpArity::ONE}},
        {"gcconfig", {"gcconfig", OpArity::ONE}},
        {"drop", {"drop", OpArity::ONE}},
        {"gcwb", {"gcwb", OpArity::ONE}},
        {"alloc", {"alloc", OpArity::ONE}},
        {"free", {"free", OpArity::ONE}},

        // Meta-programacion OOP: definir clases / fields / methods en runtime
        // y consultarlas por nombre.  Cada una toma 2 registros (destino +
        // direccion VM de la struct DefXxxParams).
        {"defclass",  {"defclass",  OpArity::TWO}},
        {"deffield",  {"deffield",  OpArity::TWO}},
        {"defmethod", {"defmethod", OpArity::TWO}},
        {"findclass", {"findclass", OpArity::TWO}},
        {"findmethod",{"findmethod",OpArity::TWO}},
        {"findfield", {"findfield", OpArity::TWO}},
        {"callm",     {"callm",     OpArity::TWO}},
        {"proceed",   {"proceed",   OpArity::ZERO}},
        // addadvice toma 3 operandos textualmente: r_target, r_advice, kind.
        // El emisor empaqueta r_target en reg1 (lo de byte2), r_advice en
        // reg2 (hi de byte2) y kind en byte3.
        {"addadvice", {"addadvice", OpArity::THREE}},
        // getstatic / setstatic toman 3 operandos: 2 registros + offset_imm.
        //   getstatic r_dst, r_class, offset_u32
        //   setstatic r_class, r_value, offset_u32
        // El emisor empaqueta los 2 regs en byte2 = (r0<<4)|r1 y emite el
        // offset uint32 en bytes 4-7 (byte3 es padding reservado).  FIXED_8.
        {"getstatic", {"getstatic", OpArity::THREE}},
        {"setstatic", {"setstatic", OpArity::THREE}},
        // FFI runtime dinamico (decision en runtime de que DLL y simbolo):
        //   dlopen r_dst, r_path_addr, r_path_len
        //   dlsym  r_dst, r_handle, r_name_addr, r_name_len
        //   callni r_fn        (argc en R15, args en R01..R12)
        // El parser permite hasta THREE operandos por entrada del InstrSet;
        // dlsym tiene 4 pero los 4 se empacan en 2 bytes (b2/b3) por nibble,
        // asi que lo registramos con un parser separado: arity=THREE registra
        // (r_dst, r_handle, r_name_addr) y el ultimo arg (r_name_len) se
        // pasa via convencion al emit (helper acepta hasta 4).  Ver el
        // emit_instr_dlsym dedicado mas abajo.
        {"dlopen", {"dlopen", OpArity::THREE}},
        {"dlsym",  {"dlsym",  OpArity::FOUR}},
        {"callni", {"callni", OpArity::ONE}},

        // Mejora I optimizada (extended 0x65, FIXED_4, REG, 2 regs):
        //   gcallocp r_dst, r_size
        //
        // Aloca @c r_size bytes en el GcHeap y deja el host_ptr al payload
        // directo en @c r_dst (1 instruccion VM en lugar de 3).  Equivale
        // a la secuencia:
        //   gcalloc r_size       ; R0 = handle
        //   gcderef cur0, r0     ; cur0 = host_ptr
        //   xchg cur0, r_dst     ; r_dst = host_ptr
        // pero fusionada en 1 instruccion: ahorra 2 instr VM + reduce el
        // bytecode emitido por env block heap de closures (3x speedup en
        // creacion de closures que escapan).
        {"gcallocp", {"gcallocp", OpArity::TWO}},

        // Mejora II optimizada (extended 0x66, FIXED_4, REG, 1 reg):
        //   spawnargs r_pc       ; R15 = argc, R1..R[argc] = args
        //
        // Variante de @c spawn que copia R1..R[R15] del padre al child antes
        // de make_ready.  Calling convention identica a CALLVM: argc en R15,
        // args en R1..R12.  El child encuentra los params ya en sus regs
        // (sin necesidad de msgrecv + deserializar buffer).  Usado por
        // lower_async_function para eliminar la serializacion via msgsend.
        // Devuelve PID encoded del child en R0.
        {"spawnargs", {"spawnargs", OpArity::ONE}},

        // Optimizacion @Async helper (extended 0x67, FIXED_4, REG, 2 regs):
        //   fulfillhlt r_fut, r_value
        //
        // Combina @c fulfill r_fut, r_value (resolver future con valor) y
        // @c hlt (terminar proceso) en 1 instruccion atomic.  Usado en el
        // path critico del helper sintetico de @Async: cada `return X` del
        // body se traduce a un solo `fulfillhlt` en lugar de dos instruccions.
        {"fulfillhlt", {"fulfillhlt", OpArity::TWO}},

        // Optimizacion hot loops: cmpjmp / cmpjmpu (extended 0x68 / 0x69)
        //   cmpjmp.cc r_a, r_b, label    (cmps + jmp.cc fusionados)
        //   cmpjmpu.cc r_a, r_b, label   (cmpu + jmp.cc fusionados)
        //
        // Cada variante codifica la condicion en byte3 (igual set que jmp.j*).
        // Encoding FIXED_8: [0x00][0x68|0x69][b2][cond][target_u32_LE].
        // Reduce 2 instr (cmp + jcc) a 1 por comparacion condicional.
        {"cmpjmp.je",   {"cmpjmp.je",   OpArity::THREE}},
        {"cmpjmp.jz",   {"cmpjmp.jz",   OpArity::THREE}},
        {"cmpjmp.jne",  {"cmpjmp.jne",  OpArity::THREE}},
        {"cmpjmp.jnz",  {"cmpjmp.jnz",  OpArity::THREE}},
        {"cmpjmp.jcs",  {"cmpjmp.jcs",  OpArity::THREE}},
        {"cmpjmp.jb",   {"cmpjmp.jb",   OpArity::THREE}},
        {"cmpjmp.jcc",  {"cmpjmp.jcc",  OpArity::THREE}},
        {"cmpjmp.jae",  {"cmpjmp.jae",  OpArity::THREE}},
        {"cmpjmp.jmi",  {"cmpjmp.jmi",  OpArity::THREE}},
        {"cmpjmp.jpl",  {"cmpjmp.jpl",  OpArity::THREE}},
        {"cmpjmp.jvs",  {"cmpjmp.jvs",  OpArity::THREE}},
        {"cmpjmp.jvc",  {"cmpjmp.jvc",  OpArity::THREE}},
        {"cmpjmp.jhi",  {"cmpjmp.jhi",  OpArity::THREE}},
        {"cmpjmp.jls",  {"cmpjmp.jls",  OpArity::THREE}},
        {"cmpjmp.jge",  {"cmpjmp.jge",  OpArity::THREE}},
        {"cmpjmp.jlt",  {"cmpjmp.jlt",  OpArity::THREE}},
        {"cmpjmp.jgt",  {"cmpjmp.jgt",  OpArity::THREE}},
        {"cmpjmp.jle",  {"cmpjmp.jle",  OpArity::THREE}},
        {"cmpjmpu.je",  {"cmpjmpu.je",  OpArity::THREE}},
        {"cmpjmpu.jz",  {"cmpjmpu.jz",  OpArity::THREE}},
        {"cmpjmpu.jne", {"cmpjmpu.jne", OpArity::THREE}},
        {"cmpjmpu.jnz", {"cmpjmpu.jnz", OpArity::THREE}},
        {"cmpjmpu.jcs", {"cmpjmpu.jcs", OpArity::THREE}},
        {"cmpjmpu.jb",  {"cmpjmpu.jb",  OpArity::THREE}},
        {"cmpjmpu.jcc", {"cmpjmpu.jcc", OpArity::THREE}},
        {"cmpjmpu.jae", {"cmpjmpu.jae", OpArity::THREE}},
        {"cmpjmpu.jmi", {"cmpjmpu.jmi", OpArity::THREE}},
        {"cmpjmpu.jpl", {"cmpjmpu.jpl", OpArity::THREE}},
        {"cmpjmpu.jvs", {"cmpjmpu.jvs", OpArity::THREE}},
        {"cmpjmpu.jvc", {"cmpjmpu.jvc", OpArity::THREE}},
        {"cmpjmpu.jhi", {"cmpjmpu.jhi", OpArity::THREE}},
        {"cmpjmpu.jls", {"cmpjmpu.jls", OpArity::THREE}},
        {"cmpjmpu.jge", {"cmpjmpu.jge", OpArity::THREE}},
        {"cmpjmpu.jlt", {"cmpjmpu.jlt", OpArity::THREE}},
        {"cmpjmpu.jgt", {"cmpjmpu.jgt", OpArity::THREE}},
        {"cmpjmpu.jle", {"cmpjmpu.jle", OpArity::THREE}},

        // decjnz r_counter, label  (extended 0x6A, FIXED_8):
        //   r_counter -= 1; if (r_counter != 0) jmp target
        // Combina decremento + branch en 1 instr (ahorra 2 instr por iter
        // en loops contadores estilo `for (i = N; i > 0; i--)`).
        {"decjnz", {"decjnz", OpArity::TWO}},

        // fastpush <mask16>, fastpop <mask16>  (extended 0x6B / 0x6C, FIXED_4):
        //   Empuja/desempila N registros en una sola instruccion segun el
        //   bitmask (bit r = r0..r15).  Reemplaza N x push/pop por 1 instr
        //   con 1 syscall de aritmetica sobre RSP + N memcpy lineales.
        //   Ascendente: r0 primero pushed, r0 ultimo popped.
        {"fastpush", {"fastpush", OpArity::ONE}},
        {"fastpop",  {"fastpop",  OpArity::ONE}},

        // CERO operandos
        {"gcrun", {"gcrun", OpArity::ZERO}},

        {"addu", {"addu", OpArity::TWO}},
        {"subu", {"subu", OpArity::TWO}},
        {"mulu", {"mulu", OpArity::TWO}},
        {"divu", {"divu", OpArity::TWO}},
        {"cmpu", {"cmpu", OpArity::TWO}},
        {"modu", {"modu", OpArity::TWO}},

        {"adds", {"adds", OpArity::TWO}},
        {"subs", {"subs", OpArity::TWO}},
        {"muls", {"muls", OpArity::TWO}},
        {"divs", {"divs", OpArity::TWO}},
        {"cmps", {"cmps", OpArity::TWO}},
        {"mods", {"mods", OpArity::TWO}},

        // Super-instrucciones ALU 3-operandos.  Combinan el patron
        // `mov rd, rs1; OP rd, rs2` (2-address codegen cuando regalloc no
        // pudo coalescer) en una sola instruccion VM.  Reduce dispatch +
        // tiempo de decode.  Encoding FIXED_4 con un opcode extendido por
        // variante (signed/unsigned x 6 ops).
        {"adds3", {"adds3", OpArity::THREE}},
        {"subs3", {"subs3", OpArity::THREE}},
        {"muls3", {"muls3", OpArity::THREE}},
        {"addu3", {"addu3", OpArity::THREE}},
        {"subu3", {"subu3", OpArity::THREE}},
        {"mulu3", {"mulu3", OpArity::THREE}},
        {"and3",  {"and3",  OpArity::THREE}},
        {"or3",   {"or3",   OpArity::THREE}},
        {"xor3",  {"xor3",  OpArity::THREE}},

        // Super-instruccion LOAD con zero-extend (1 instr en vez de 2).
        // Sintaxis: loadz r_dst, r_src (arity TWO).  El size (8/16/32/64-bit)
        // se selecciona por sufijo de tamano del registro destino igual que mov.
        // loadz = memoria VM, loadzh = memoria HOST (host_ptr).
        {"loadz",  {"loadz",  OpArity::TWO}},
        {"loadzh", {"loadzh", OpArity::TWO}},

        {"setcc",    {"setcc",    OpArity::TWO}},
        {"tryenter", {"tryenter", OpArity::TWO}},
        {"tryleave", {"tryleave", OpArity::ZERO}},
        {"strmake",  {"strmake",  OpArity::THREE}},
        {"strmake_h",{"strmake_h",OpArity::THREE}},
        {"strlen",   {"strlen",   OpArity::TWO}},
        {"strcat",   {"strcat",   OpArity::THREE}},
        {"strcmp",   {"strcmp",   OpArity::THREE}},
        {"strconv",    {"strconv",    OpArity::THREE}},
        {"strraw",     {"strraw",     OpArity::TWO}},
        {"strslice",   {"strslice",   OpArity::THREE}},
        {"strflat",    {"strflat",    OpArity::TWO}},
        {"strhash",    {"strhash",    OpArity::TWO}},
        {"strintern",  {"strintern",  OpArity::TWO}},
        {"strgetenc",  {"strgetenc",  OpArity::TWO}},
        {"strgetbytes",{"strgetbytes",OpArity::TWO}},
        {"strgetkind", {"strgetkind", OpArity::TWO}},
        {"strreserve", {"strreserve", OpArity::TWO}},
        {"strfinalize",{"strfinalize",OpArity::TWO}},

        {"xor", {"xor", OpArity::TWO}},
        {"and", {"and", OpArity::TWO}},
        {"or",  {"or",  OpArity::TWO}},

        // TRES operandos: movc/movch destino, fuente, flag
        {"movc",  {"movc",  OpArity::THREE}},
        {"movch", {"movch", OpArity::THREE}},
        {"not", {"not", OpArity::ONE}},

        {"shl", {"shl", OpArity::TWO}},
        {"shr", {"shr", OpArity::TWO}},
        {"sar", {"sar", OpArity::TWO}},

        // DOS operandos OOP
        {"newobjraw",   {"newobjraw",   OpArity::TWO}},
        {"callvirt",    {"callvirt",    OpArity::TWO}},
        {"callsuper",   {"callsuper",   OpArity::TWO}},
        {"instanceof",  {"instanceof",  OpArity::TWO}},
        {"checkcast",   {"checkcast",   OpArity::TWO}},
        {"getfield",    {"getfield",    OpArity::TWO}},
        {"getmethod",   {"getmethod",   OpArity::TWO}},

        // UN operando OOP
        {"getclass",        {"getclass",        OpArity::ONE}},
        {"fieldcount",      {"fieldcount",       OpArity::ONE}},
        {"methodcount",     {"methodcount",      OpArity::ONE}},
        {"classname",       {"classname",        OpArity::ONE}},
        {"gcalloc",         {"gcalloc",          OpArity::ONE}},
        {"throw",           {"throw",            OpArity::ONE}},
        // doc/atributos - UN operando
        {"classdoc",        {"classdoc",         OpArity::ONE}},
        {"classattrcount",  {"classattrcount",   OpArity::ONE}},
        {"methodname",      {"methodname",       OpArity::ONE}},
        {"methoddoc",       {"methoddoc",        OpArity::ONE}},
        {"methoddesc",      {"methoddesc",       OpArity::ONE}},
        {"methodattrcount", {"methodattrcount",  OpArity::ONE}},
        {"fieldname",       {"fieldname",        OpArity::ONE}},
        {"fielddoc",        {"fielddoc",         OpArity::ONE}},
        {"fieldattrcount",  {"fieldattrcount",   OpArity::ONE}},
        // doc/atributos - DOS operandos (reg, idx)
        {"classattrkey",    {"classattrkey",     OpArity::TWO}},
        {"classattrval",    {"classattrval",     OpArity::TWO}},
        {"methodattrkey",   {"methodattrkey",    OpArity::TWO}},
        {"methodattrval",   {"methodattrval",    OpArity::TWO}},
        {"fieldattrkey",    {"fieldattrkey",     OpArity::TWO}},
        {"fieldattrval",    {"fieldattrval",     OpArity::TWO}},

        // --- Corutinas y fibras ---
        {"yield",   {"yield",   OpArity::ZERO}},
        {"resume",  {"resume",  OpArity::ONE}},
        {"spawn",   {"spawn",   OpArity::ONE}},
        // spawnon r_fn, r_hint -- spawn con scheduler hint.  r_hint: -1
        // (signed) = Here (mismo scheduler que el padre, ruta cooperativa
        // sin cruzar threads OS); 0..N-1 = Pinned al scheduler indicado
        // (modulo num_schedulers, paralelismo real OS-thread).
        {"spawnon", {"spawnon", OpArity::TWO}},
        // loadmod r_path_addr, r_path_len -- carga dinamica de un .velb
        // adicional desde el filesystem.  r0 = init_pc del modulo cargado
        // (>0) o 0 si fallo la carga.  Usado para hot-reload y plugins.
        {"loadmod", {"loadmod", OpArity::TWO}},
        // panic r_msg_addr, r_msg_len -- lanza FatalError con
        // kind=FATAL_USER_ABORT.  Capturable con try/catch FatalError.
        {"panic",   {"panic",   OpArity::TWO}},
        // setmethdbg r_method, r_params -- registra debug info
        // (file + start_line) para un MethodInfo en la tabla global.
        {"setmethdbg", {"setmethdbg", OpArity::TWO}},
        {"swapctx", {"swapctx", OpArity::TWO}},

        // --- Closures GC y raw ---
        {"mkclosure",      {"mkclosure",      OpArity::TWO}},
        {"callclosure",    {"callclosure",    OpArity::ONE}},
        {"mkrawclosure",   {"mkrawclosure",   OpArity::TWO}},
        {"callrawclosure", {"callrawclosure", OpArity::ONE}},

        // --- TCO (tail call optimization) ---
        {"tailcall", {"tailcall", OpArity::ONE}},

        // --- Nullable ---
        {"isnull", {"isnull", OpArity::TWO}},
        {"unwrap",  {"unwrap",  OpArity::TWO}},

        // --- Punto flotante escalar y vectorial ---
        {"fmov",     {"fmov",     OpArity::TWO}},
        {"fadd",     {"fadd",     OpArity::TWO}},
        {"fadd.pd",  {"fadd.pd",  OpArity::TWO}},
        {"fadd.ps",  {"fadd.ps",  OpArity::TWO}},
        {"fsub",     {"fsub",     OpArity::TWO}},
        {"fsub.pd",  {"fsub.pd",  OpArity::TWO}},
        {"fsub.ps",  {"fsub.ps",  OpArity::TWO}},
        {"fmul",     {"fmul",     OpArity::TWO}},
        {"fmul.pd",  {"fmul.pd",  OpArity::TWO}},
        {"fmul.ps",  {"fmul.ps",  OpArity::TWO}},
        {"fdiv",     {"fdiv",     OpArity::TWO}},
        {"fdiv.pd",  {"fdiv.pd",  OpArity::TWO}},
        {"fdiv.ps",  {"fdiv.ps",  OpArity::TWO}},
        {"fcmp",     {"fcmp",     OpArity::TWO}},
        {"fcmp.pd",  {"fcmp.pd",  OpArity::TWO}},
        {"fcmp.ps",  {"fcmp.ps",  OpArity::TWO}},
        {"fsqrt",    {"fsqrt",    OpArity::TWO}},
        {"fsqrt.pd", {"fsqrt.pd", OpArity::TWO}},
        {"fsqrt.ps", {"fsqrt.ps", OpArity::TWO}},
        {"fabs",     {"fabs",     OpArity::TWO}},
        {"fabs.pd",  {"fabs.pd",  OpArity::TWO}},
        {"fabs.ps",  {"fabs.ps",  OpArity::TWO}},
        {"fneg",     {"fneg",     OpArity::TWO}},
        {"fneg.pd",  {"fneg.pd",  OpArity::TWO}},
        {"fneg.ps",  {"fneg.ps",  OpArity::TWO}},
        {"fcvt",     {"fcvt",     OpArity::TWO}},
        {"fcvt.ps",  {"fcvt.ps",  OpArity::TWO}},
        {"fextend",  {"fextend",  OpArity::TWO}},
        {"fnarrow",  {"fnarrow",  OpArity::TWO}},
        {"fmowi",    {"fmowi",    OpArity::TWO}},
        {"fload",    {"fload",    OpArity::TWO}},
        {"fstore",   {"fstore",   OpArity::TWO}},

        // CERO operandos OOP
        {"rethrow",     {"rethrow",     OpArity::ZERO}},

        // CERO operandos
        {"nop1", {"nop1", OpArity::ZERO}},
        {"nop2", {"nop2", OpArity::ZERO}},
        {"hlt", {"hlt", OpArity::ZERO}},
        {"ret", {"ret", OpArity::ZERO}},
        {"resbp", {"resbp", OpArity::ZERO}},
        {"leave", {"leave", OpArity::ZERO}},

        // ----------------------------------------------------------------------
        // UN operando

        // aunque no es una instruccion, se detectara como una
        {"org", {"org", OpArity::ONE}},
        {"align", {"align", OpArity::ONE}},
        {"import", {"import", OpArity::ONE}},


        {"jmp",     {"jmp",     OpArity::ONE}},
        {"jmp.je",  {"jmp.je",  OpArity::ONE}},
        {"jmp.jz",  {"jmp.jz",  OpArity::ONE}},
        {"jmp.jne", {"jmp.jne", OpArity::ONE}},
        {"jmp.jnz", {"jmp.jnz", OpArity::ONE}},
        {"jmp.jcs", {"jmp.jcs", OpArity::ONE}},
        {"jmp.jae", {"jmp.jae", OpArity::ONE}},
        {"jmp.jcc", {"jmp.jcc", OpArity::ONE}},
        {"jmp.jb",  {"jmp.jb",  OpArity::ONE}},
        {"jmp.jmi", {"jmp.jmi", OpArity::ONE}},
        {"jmp.jpl", {"jmp.jpl", OpArity::ONE}},
        {"jmp.jvs", {"jmp.jvs", OpArity::ONE}},
        {"jmp.jvc", {"jmp.jvc", OpArity::ONE}},
        {"jmp.jhi", {"jmp.jhi", OpArity::ONE}},
        {"jmp.jls", {"jmp.jls", OpArity::ONE}},
        {"jmp.jge", {"jmp.jge", OpArity::ONE}},
        {"jmp.jlt", {"jmp.jlt", OpArity::ONE}},
        {"jmp.jgt", {"jmp.jgt", OpArity::ONE}},
        {"jmp.jle", {"jmp.jle", OpArity::ONE}},

        {"jrel",     {"jrel",     OpArity::ONE}},
        {"jrel.je",  {"jrel.je",  OpArity::ONE}},
        {"jrel.jz",  {"jrel.jz",  OpArity::ONE}},
        {"jrel.jne", {"jrel.jne", OpArity::ONE}},
        {"jrel.jnz", {"jrel.jnz", OpArity::ONE}},
        {"jrel.jcs", {"jrel.jcs", OpArity::ONE}},
        {"jrel.jae", {"jrel.jae", OpArity::ONE}},
        {"jrel.jcc", {"jrel.jcc", OpArity::ONE}},
        {"jrel.jb",  {"jrel.jb",  OpArity::ONE}},
        {"jrel.jmi", {"jrel.jmi", OpArity::ONE}},
        {"jrel.jpl", {"jrel.jpl", OpArity::ONE}},
        {"jrel.jvs", {"jrel.jvs", OpArity::ONE}},
        {"jrel.jvc", {"jrel.jvc", OpArity::ONE}},
        {"jrel.jhi", {"jrel.jhi", OpArity::ONE}},
        {"jrel.jls", {"jrel.jls", OpArity::ONE}},
        {"jrel.jge", {"jrel.jge", OpArity::ONE}},
        {"jrel.jlt", {"jrel.jlt", OpArity::ONE}},
        {"jrel.jgt", {"jrel.jgt", OpArity::ONE}},
        {"jrel.jle", {"jrel.jle", OpArity::ONE}},

        {"calln", {"calln", OpArity::ONE}},
        {"callvm", {"callvm", OpArity::ONE}},
        // closures: callvmr = "callvm con direccion en registro" (1 op).
        // Mismo opcode bytecode que callvm pero AddressingMode REG en
        // lugar de INMED.  Necesario para CALLIND y CALLCLOSURE del IR
        // donde el target lo tiene un registro (resultado de LOAD).
        {"callvmr", {"callvmr", OpArity::ONE}},

        {"enter", {"enter", OpArity::ONE}},

        {"push", {"push", OpArity::ONE}},
        {"pop", {"pop", OpArity::ONE}},
        {"inc", {"inc", OpArity::ONE}},
        {"dec", {"dec", OpArity::ONE}},
        {"vminfo", {"vminfo", OpArity::ONE}},
        {"vminfomanager", {"vminfomanager", OpArity::ONE}},

        /* --- Pattern matching nativo --- */
        {"jumptable",  {"jumptable",  OpArity::THREE}},
        {"typeswitch", {"typeswitch", OpArity::THREE}},

        /* --- Async/await --- */
        {"future",  {"future",  OpArity::ZERO}},
        {"await",   {"await",   OpArity::ONE}},
        {"fulfill", {"fulfill", OpArity::TWO}},
        {"reject",  {"reject",  OpArity::TWO}},

        /* --- Aritmetica sobre registros especiales (rsp/rbp) con inmediato --- */
        {"subsp", {"subsp", OpArity::TWO}},
        {"addsp", {"addsp", OpArity::TWO}},

        /* --- Referencias debiles --- */
        {"weakref",   {"weakref",   OpArity::ONE}},
        {"deref_weak",{"deref_weak",OpArity::ONE}},
        {"free_weak", {"free_weak", OpArity::ONE}},

        /* --- Lookup inverso ptr -> handle --- */
        {"gchandle",  {"gchandle",  OpArity::TWO}},

        /* --- PID del proceso actual --- */
        {"getpid",    {"getpid",    OpArity::ONE}},

        /* --- argv del script (builtins args_count / args_get) --- */
        {"getargc",   {"getargc",   OpArity::ONE}},
        {"getarg",    {"getarg",    OpArity::TWO}},

        /* --- Move-and-take (primitivo de smart pointers unique<T> / shared<T>) --- */
        {"mvtake",    {"mvtake",    OpArity::TWO}},

        /* --- descarga dinamica de modulos (builtin unloadmodule) --- */
        {"unloadmod", {"unloadmod", OpArity::TWO}},

        /* --- introspeccion runtime: getMethodAt / getFieldAt --- */
        {"getmethat", {"getmethat", OpArity::TWO}},
        {"getfldat",  {"getfldat",  OpArity::TWO}},

        /* --- Monitor / sincronizacion --- */
        {"monenter",  {"monenter",  OpArity::ONE}},
        {"monexit",   {"monexit",   OpArity::ONE}},
        {"monwait",   {"monwait",   OpArity::ONE}},
        {"monnoti",   {"monnoti",   OpArity::ONE}},
        {"monnota",   {"monnota",   OpArity::ONE}},

        /* --- Genericos en tiempo de ejecucion --- */
        {"specialize",{"specialize",OpArity::THREE}},

        /* --- Instrucciones distribuidas VDP --- */
        {"rspawn",  {"rspawn",  OpArity::TWO}},
        {"msgsend", {"msgsend", OpArity::THREE}},
        {"msgrecv", {"msgrecv", OpArity::TWO}},
        {"memsync", {"memsync", OpArity::ONE}},

    };

    bool Parser::match(TokenType type) {
        if (current.type == type) {
            advance();
            return true;
        }
        return false;
    }

    bool Parser::expect(TokenType type, const std::string &msg) {
        if (current.type == type) {
            advance();
            return true;
        }
        error(current, msg);
        return false;
    }


    void Parser::error(const Token &tok, const std::string &msg) {
        throw ParseError(tok.line, tok.column,
                         "Parser ERROR: " + msg +
                         " [" + tok.lexeme + "]");
    }

    void Parser::warning(int line, int col, const std::string &msg, const std::string &sugg = "") {
        warnings.emplace_back(line, col, msg, sugg);
        std::cout << "Linea " << line << ":" << col
                << " - " << msg << (sugg.empty() ? "" : " (" + sugg + "?)") << std::endl;
    }


    void Parser::print_warnings() const {
        if (!warnings.empty()) {
            std::cout << "\n" << warnings.size() << " warnings encontrados:\n";
            for (const auto &w: warnings) {
                std::cout << "    Linea " << w.line << ":" << w.column
                        << " - " << w.what()
                        << (w.suggestion.empty() ? "" : " (sugerencia: " + w.suggestion + ")")
                        << std::endl;
            }
        }
    }

    std::vector<std::unique_ptr<ASTNode> > Parser::parse() {
        std::vector<std::unique_ptr<ASTNode> > program;

        while (current.type != TokenType::EndOfFile) {
            std::unique_ptr<ASTNode> node = nullptr;
            Token                    next = peek();
            // IDENTIFICADOR + COLON? -> SECCION
            if (current.type == TokenType::IDENTIFIER && peek().type == TokenType::COLON) {
                node = parse_section();
            }
            // IDENTIFICADOR solo? -> INSTRUCCION
            // IDENTIFICADOR IDENTIFICADOR? -> posible instruccion de dos identificadores
            else if (
                (current.type == TokenType::IDENTIFIER) ||
                (current.type == TokenType::IDENTIFIER) && peek().type == TokenType::IDENTIFIER
            ) {
                node = parse_statement();
            }

            // se encontro una o varias anotaciones
            else if (current.type == TokenType::AT) {
                node = parse_annotation();
            }

            // Omitir tokens invalidos
            else {
                std::cerr << "linea: " << current.line << ":" << current.column <<
                        " Parser ERROR: " << "Skipping invalid token: " + token_type_to_string(current.type) +
                        " [" + current.lexeme + "]" << std::endl;
                advance();
                continue;
            }

            if (node) program.push_back(std::move(node));
        }

        return program;
    }

    std::unique_ptr<ASTNode> Parser::parse_mem_term() {
        auto node = parse_mem_factor();

        while (current.type == TokenType::STAR) {
            Token op = current;
            advance();
            auto right = parse_mem_factor();
            node       = std::make_unique<BinaryExpr>(op.lexeme[0],
                                                std::unique_ptr<
                                                    ExprNode>(static_cast<ExprNode *>(node.release())),
                                                std::unique_ptr<ExprNode>(
                                                    static_cast<ExprNode *>(right.release())));
        }

        return node;
    }

    std::unique_ptr<ASTNode> Parser::parse_mem_expression() {
        auto node = parse_mem_term();

        while (current.type == TokenType::PLUS || current.type == TokenType::MINUS) {
            Token op = current;
            advance();
            auto right = parse_mem_term();
            node       = std::make_unique<BinaryExpr>(op.lexeme[0],
                                                std::unique_ptr<
                                                    ExprNode>(static_cast<ExprNode *>(node.release())),
                                                std::unique_ptr<ExprNode>(
                                                    static_cast<ExprNode *>(right.release())));
        }

        return node;
    }

    std::unique_ptr<ASTNode> Parser::parse_mem_factor() {
        // ( expr )
        if (match(TokenType::LPAREN)) {
            auto expr = parse_mem_expression();
            expect(TokenType::RPAREN, "Falta ')'");
            return expr;
        }

        // tipo REGISTRO
        if (current.type == TokenType::REGISTER) {
            return parse_operand(); // ya devuelve RegisterOperand
        }

        // tipo NUMERO
        if (is_number_token(current.type)) {
            return parse_operand(); // devuelve NumberOperand
        }

        // tipo ETIQUETA
        if (current.type == TokenType::IDENTIFIER) {
            return parse_operand(); // devuelve LabelOperand
        }

        error(current, "Expresion de memoria invalida");
        return nullptr;
    }

    /**
     * No pude usar el metodo principal parse en parse_section, ya que hacerlo hacia que todas
     * las secciones se unieran en un unico arbol como si fuera una unica seccion, cosa que no se quiere.
     * Para evitar esto usamos el mismo codigo, pero cuando se encuentre una seccion dentro de otra, ya no
     * se sigue analizando en la seccion anterior, sino que vuelve al parse principal y lo analiza desde ahi
     * @return seccion parseada
     */
    std::vector<std::unique_ptr<ASTNode> > Parser::parse_in_label() {
        std::vector<std::unique_ptr<ASTNode> > program;

        while (current.type != TokenType::EndOfFile) {
            std::unique_ptr<ASTNode> node = nullptr;
            Token                    next = peek();
            // IDENTIFICADOR + COLON? -> SECCION
            if (current.type == TokenType::IDENTIFIER && peek().type == TokenType::COLON) {
                //node = parse_section();
                break;
            }
            // IDENTIFICADOR solo? -> INSTRUCCION
            // IDENTIFICADOR IDENTIFICADOR? -> posible instruccion de dos identificadores
            if (
                (current.type == TokenType::IDENTIFIER) ||
                (current.type == TokenType::IDENTIFIER) && peek().type == TokenType::IDENTIFIER
            ) {
                node = parse_statement();
            }

            // se encontro una o varias anotaciones
            else if (current.type == TokenType::AT) {
                node = parse_annotation();
            } else if (current.type == TokenType::END_LABEL) {
                program.push_back(std::move(parser_end_label()));
                break;
            }

            // Omitir tokens invalidos
            else {
                std::cerr << "linea: " << current.line << ":" << current.column <<
                        " Parser ERROR: " << "Skipping invalid token: " + token_type_to_string(current.type) +
                        " [" + current.lexeme + "]" << std::endl;
                advance();
                continue;
            }

            if (node) program.push_back(std::move(node));
        }

        return program;
    }

    std::unique_ptr<ASTNode> Parser::parser_end_label() {
        advance(); // consumimos el token end
        return std::make_unique<EndLabelNode>("");
    }

    std::unique_ptr<ASTNode> Parser::parse_section() {
        if (current.type != TokenType::IDENTIFIER) {
            return nullptr;
        }

        // guardar nombre de la seccion:
        std::string section_name = current.lexeme;
        advance(); // consumimos el token

        // esperamos q haya dos puntos despues del ID de una section.
        expect(TokenType::COLON, "Expected COLON");

        std::vector<std::unique_ptr<ASTNode> > body = parse_in_label();

        if (!body.empty()) {
            ASTNode *last = body.back().get();

            if (auto end = dynamic_cast<EndLabelNode *>(last)) {
                // si el ultimo nodo del label, es un end label, le indicamos
                // el nombre al que pertenece
                end->label = section_name;
            } //else error(current, "Falta un 'end'");
        }

        return std::make_unique<LabelNode>(section_name, std::move(body));
    }

    std::unique_ptr<ASTNode> Parser::parse_operand() {
        switch (current.type) {
            case TokenType::REGISTER: {
                std::string reg = current.lexeme;
                advance();

                // Decodificar tamano (r0b=8bit, r0w=16bit, etc.)
                int size_bits = 64; // Default 64-bit
                if (reg.size() > 2) {
                    char suffix = reg.back();

                    // Solo quitar el sufijo si realmente es un sufijo valido
                    if (suffix == 'b' || suffix == 'w' || suffix == 'd') {
                        reg.pop_back();
                        switch (suffix) {
                            case 'b': size_bits = 8;
                                break;
                            case 'w': size_bits = 16;
                                break;
                            case 'd': size_bits = 32;
                                break;
                        }
                    }
                }

                return std::make_unique<RegisterOperand>(reg, size_bits);
            }

            case TokenType::NUMBER_DEC:
            case TokenType::NUMBER_HEX:
            case TokenType::NUMBER_BIN:
            case TokenType::NUMBER_OCT: {
                auto num = std::make_unique<NumberOperand>(
                    current.lexeme, current.type);
                advance();
                return std::move(num);
            }

            case TokenType::STRING: {
                auto str = std::make_unique<StringOperand>(current.lexeme);
                advance();
                return std::move(str);
            }

            case TokenType::IDENTIFIER: {
                // Label o variable
                auto label = std::make_unique<LabelOperand>(current.lexeme);
                advance();
                return std::move(label);
            }

            case TokenType::LBRACKET: {
                // '['
                advance(); // consumir '['

                // Parsear la expresion dentro de los corchetes
                auto expr = parse_mem_expression();

                if (current.type != TokenType::RBRACKET)
                    error(current, "Falta ']' en operando de memoria");

                advance(); // consumir ']'

                return std::make_unique<MemoryOperand>(std::move(expr));
            }
            case TokenType::AT: {
                return parse_annotation();
            }


            default:
                error(current, "Operando invalido");
                advance(); // omitir token invalido
                return nullptr;
        }
    }

    std::unique_ptr<ASTNode> Parser::parse_data_directive() {
        // PATRONES:
        // msg db "Hola mundo"
        // bytes db 0xFF, 0x00, 0x11
        // count dw 42, 100

        std::string label = current.lexeme; // "msg", "bytes"
        advance();                          // Consumir label

        // Esperar directiva: db, dw, dd, ptr, etc
        Token directiveTok = expectToken(TokenType::DATA_DIRECTIVE, "Expected data directive (dq, db, dw, dd, ptr)");
        if (directiveTok.type != TokenType::DATA_DIRECTIVE) {
            return nullptr;
        }
        advance(); // consumir la directiva

        std::string                             directive = directiveTok.lexeme; // "db", "dw", "dd"
        std::vector<std::unique_ptr<ExprNode> > values;

        // Parsear valores: "Hola", 42, 0xFF, label, etc.
        while (current.type != TokenType::EndOfFile) {
            // STRING, NUMBER, IDENTIFIER (labels)
            switch (current.type) {
                case TokenType::STRING:
                    values.push_back(std::make_unique<StringExpr>(current.lexeme));
                    advance();
                    break;
                case TokenType::NUMBER_DEC:
                case TokenType::NUMBER_HEX:
                case TokenType::NUMBER_BIN:
                case TokenType::NUMBER_OCT:
                case TokenType::IDENTIFIER:
                case TokenType::MINUS: // si es un -, puede ser valor negativo?
                    values.push_back(parse_expression());
                    break;

                default:
                    error(current, "Invalid data value");
                    advance();
                    continue;
            }

            // Coma opcional
            if (match(TokenType::COMMA)) {
                continue; // Mas datos
            }
            break; // Fin de datos
        }

        return std::make_unique<DataDecl>(std::move(label),
                                          std::move(directive),
                                          std::move(values));
    }


    std::unique_ptr<ASTNode> Parser::parse_instruction() {
        std::string opcode = current.lexeme;
        std::string ext_opcode;
        Token       tok = peek();
        if (tok.type == TokenType::DOT) {
            // si el siguiente token es un "." es una instruccion compuesta.
            advance(); // consumismos el token opcode

            opcode += current.lexeme;
            advance(); // consumismos el token opcode, el token "."

            // anadimos la siguiente parte de la instruccion compleja.
            ext_opcode = current.lexeme;
            opcode += ext_opcode;
        }
        auto it       = InstructionSet.find(opcode);
        auto valid_it = it;
        if (it == InstructionSet.end()) {
            float affinity = 0.0;
            int   dist     = 0;


            // intentamos recuperarnos del error ?de sintaxis?
            for (auto &option: InstructionSet) {
                dist     = utils::Levenshtein::distance(opcode, option.first);
                affinity = utils::Levenshtein::affinity(opcode, option.first);

                // Top 3 resultados
                if (dist <= 3 || affinity >= 80) {
                    // si la distancia es <= 3 o la afinidad es mayor o igual al 80%
                    warning(current.line, current.column,
                            "Instruccion '" + opcode + "' desconocida",
                            option.first);
                    opcode   = option.first;
                    valid_it = InstructionSet.find(option.first); // Actualizar
                    goto exit_error;                              // nos pudimos recuperar tal vez
                }
            }

            std::stringstream ss;
            for (auto &option: InstructionSet) {
                dist     = utils::Levenshtein::distance(opcode, option.first);
                affinity = utils::Levenshtein::affinity(opcode, option.first);

                if (affinity > 30) {
                    ss << "Instr: " << option.first << "\n"
                            << "Dist: " << dist << "\n"
                            << "Aff: " << std::fixed << std::setprecision(1)
                            << affinity * 100 << "%\n\n";
                }
            }

            error(current,
                  "Instruccion no existe esperado: " + opcode + "\n" + ss.str()
            );
            return nullptr;
        }
    exit_error:
        const auto &pattern = valid_it->second; // SIEMPRE valido

        advance(); // consumir el opcode

        std::vector<std::unique_ptr<ASTNode> > operands;

        switch (pattern.arity) {
            case OpArity::ZERO: break;
            case OpArity::ONE: operands.emplace_back(parse_operand());
                break;
            case OpArity::TWO:
                operands.emplace_back(parse_operand());
                expect(TokenType::COMMA, "Se esperaba ',' despues de destino");
                operands.emplace_back(parse_operand());
                break;
            case OpArity::THREE:
                operands.emplace_back(parse_operand());
                expect(TokenType::COMMA, "Se esperaba ',' despues del primer operando");
                operands.emplace_back(parse_operand());
                expect(TokenType::COMMA, "Se esperaba ',' despues del segundo operando");
                operands.emplace_back(parse_operand());
                break;
            case OpArity::FOUR:
                operands.emplace_back(parse_operand());
                expect(TokenType::COMMA, "Se esperaba ',' despues del primer operando");
                operands.emplace_back(parse_operand());
                expect(TokenType::COMMA, "Se esperaba ',' despues del segundo operando");
                operands.emplace_back(parse_operand());
                expect(TokenType::COMMA, "Se esperaba ',' despues del tercer operando");
                operands.emplace_back(parse_operand());
                break;
        }

        // Usa pattern.opcode para codegen
        auto instr = std::make_unique<Instruction>(pattern.opcode, std::move(operands));
        // Captura la linea fuente Vex del marcador `// @line N` mas
        // reciente (rellenado por el lexer en skip_whitespace).  Esto
        // se usa luego por el bytecode emitter para registrar el par
        // (byte_offset, source_line) en la tabla debug del linker.
        instr->source_line = lexer.last_src_line;
        return instr;
    }

    std::unique_ptr<ASTNode> Parser::parse_import() {
        // current == "import"
        advance(); // consumir 'import'

        if (current.type != TokenType::STRING) {
            error(current, "Se esperaba un nombre de archivo entre comillas");
        }

        std::string filename = current.lexeme;
        advance(); // consumir STRING

        return std::make_unique<ImportNode>(filename);
    }

    std::unique_ptr<ASTNode> Parser::parse_statement() {
        // Si es un registro y un identificador, se trata de una instruccion.

        Token next = peek();

        // directiva datos: msg db "Hola mundo"
        if (current.type == TokenType::IDENTIFIER && next.type == TokenType::DATA_DIRECTIVE) {
            return parse_data_directive();
        }

        bool cur_is_id       = current.type == TokenType::IDENTIFIER;
        bool next_is_operand =
                next.type == TokenType::REGISTER
                || is_number_token(next.type)
                || next.type == TokenType::IDENTIFIER
                || next.type == TokenType::LBRACKET
                || next.type == TokenType::DOT;

        /*
        if (
            (current.type == TokenType::IDENTIFIER && next.type == TokenType::REGISTER) ||
            (current.type == TokenType::IDENTIFIER && is_number_token(next.type)) ||
            (current.type == TokenType::IDENTIFIER && next.type == TokenType::IDENTIFIER) ||
            (current.type == TokenType::IDENTIFIER && next.type == TokenType::LBRACKET) ||
            current.lexeme == "call" && next.type == TokenType::AT
        ) */
        if ((cur_is_id && next_is_operand) ||
            (current.type == TokenType::IDENTIFIER && next.type == TokenType::AT)) {
            /**
             * Si es identificador + registro       ||
             * Si es identificador + numero         ||
             * Si es identificador + identificador  ||
             * Si es identificador + [ + identificador
             */
            return parse_instruction();
        }

        // instrucciones de identificador unico sin operandos:
        static const char *no_operand_instr[] = {"nop1", "nop2", "ret", "hlt", "leave"};

        for (auto &name: no_operand_instr)
            if (current.lexeme == name)
                return parse_instruction();


        if (current.lexeme == "import") {
            return parse_import();
        }

        error(current, "Token no esperado: " + current.lexeme);
        advance(); // omitir token no reconocido
        return nullptr;
    }
}
