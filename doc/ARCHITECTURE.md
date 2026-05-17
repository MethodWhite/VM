# Arquitectura interna de VestaVM

Vision detallada de los subsistemas internos de la VM, sus interacciones, y
las decisiones arquitecturales clave.

---

## Indice

1. [Vision general](#1-vision-general)
2. [Pipeline de compilacion](#2-pipeline-de-compilacion)
3. [El formato bytecode .velb](#3-el-formato-bytecode-velb)
4. [Modelo de ejecucion](#4-modelo-de-ejecucion)
5. [Sistema de memoria](#5-sistema-de-memoria)
6. [Recolector de basura](#6-recolector-de-basura)
7. [Scheduler y procesos](#7-scheduler-y-procesos)
8. [Runtime distribuido (VDP)](#8-runtime-distribuido-vdp)
9. [JIT C1 baseline](#9-jit-c1-baseline)
10. [Optimizador SSA](#10-optimizador-ssa)
11. [Set de instrucciones VM](#11-set-de-instrucciones-vm)
12. [FFI nativo](#12-ffi-nativo)
13. [Sistema de plugins](#13-sistema-de-plugins)
14. [Layout del codigo fuente](#14-layout-del-codigo-fuente)

---

## 1. Vision general

VestaVM es una maquina virtual basada en registros (no stack-based) con un
modelo actor de concurrencia. Cada instancia del proceso `vm` puede:

- Ejecutar bytecode `.velb` en modo intérprete o JIT.
- Hospedar multiples VMs (cada uno con sus procesos, GC, scheduler).
- Conectarse a otros nodos VestaVM via VDP para clusters distribuidos.

Capas, de arriba a abajo:

```text
+-------------------------------------------------------------+
|  Vex (lenguaje fuente)                                      |
+-------------------------------------------------------------+
|  SSA IR (representacion intermedia compartida)              |
+-------------------------------------------------------------+
|  Bytecode .velb (formato distribuible)                      |
+-------------------------------------------------------------+
|  Interprete  |  JIT C1  |  Transpiler C (proyectado AOT)    |
+-------------------------------------------------------------+
|  Runtime: scheduler + GC + memoria + dist + FFI             |
+-------------------------------------------------------------+
|  OS (Windows, Linux, macOS via syscalls + libc + OpenSSL)  |
+-------------------------------------------------------------+
```

Los **3 modos de ejecucion** (interp, JIT, AOT planeado) **comparten el mismo
IR y el mismo backend de optimizaciones**. Esto evita duplicar pases y mantiene
la semantica consistente entre los modos.

---

## 2. Pipeline de compilacion

Desde `.vex` hasta bytecode ejecutable:

```text
.vex source
    |
    +-----> VPP preprocesador (~5000 LOC C)
    |       #define / #include / #if / #foreach / #repeat / #array / #exec
    |
    +-----> Lexer Vex (~1000 LOC C++)
    |       Token stream: keywords, identifiers, literals, string parts,
    |       interpolation markers (ISTR_*), triple-quoted detection.
    |
    +-----> Parser Vex (~3000 LOC)
    |       AST: decls (function, class, struct, enum, typedef),
    |       stmts (if, while, for, foreach, return, throw, try, sync, ...),
    |       exprs (binary, unary, call, field, index, new, lambda, match, ...),
    |       tipos (PrimitiveKind, NamedType, PointerType, FunctionType, ...).
    |
    +-----> Type Checker (~5000 LOC)
    |       - Resolucion de aliases (typedef/using).
    |       - Inferencia local de tipos.
    |       - Monomorphizacion de generics (compile-time).
    |       - Nullability + Optional/Result builtins.
    |       - Borrow checker (4 reglas + F1-F4).
    |       - Escape analysis para smart pointers.
    |       - AOP: pointcut matching + advice chain.
    |
    +-----> Lowering AST -> SSA IR (~4000 LOC)
    |       - Construye SSA on-the-fly (Braun-style).
    |       - Address-taken promotion.
    |       - PHI insertion en merges/loops.
    |       - Cleanup stack (RAII) para synchronized, smart ptrs, cleanups
    |         de exception/return.
    |       - Lowering especifico de cada feature (Optional SRET,
    |         smart ptr move via mvtake, etc.).
    |
    +-----> IR Optimizer (~3000 LOC, ~15 pasadas)
    |       (ver seccion 10)
    |
    +-----> ir_emitter SSA IR -> .vel (~3500 LOC)
    |       - Linear scan regalloc con coalesce hint.
    |       - Liveness analysis.
    |       - Phi destruction (parallel-move).
    |       - Spill code (save/restore alrededor de calls).
    |       - Emit super-instrucciones (alu3, loadz, cmpjmp, etc.).
    |       - GC-aware push/pop (gchandle conversion).
    |
    +-----> Assembler (~2000 LOC)
    |       .vel ensamblador VM -> bytecode crudo + reloc table.
    |
    +-----> Linker (~1500 LOC)
    |       Mezcla bytecode + datos + tabla de simbolos + reloc table +
    |       seccion @ir embebida + seccion @sym + (opcional) debug info.
    |       Emite .velb final.
    |
    .velb (ejecutable distribuible)
```

Total: ~30,000 LOC C++ para el frontend completo + optimizador.

---

## 3. El formato bytecode .velb

VestaLangBinary v3 (anyadido seccion `@sym` en 2026-05-14, seccion `@ir` en
2026-05-13).

**Header** (144 bytes):

```cpp
struct HeaderVELB {
    uint64_t magic;                  // 'VELB' = 0x424C4556 (LE)
    uint32_t version;                // 0x3 actual
    uint32_t format_v;               // formato binario interno
    uint64_t entry_point;            // VA del main
    uint64_t init_pc;                // VA del __module_init (POO)
    uint64_t offset_code;
    uint64_t size_code;
    uint64_t offset_data;
    uint64_t size_data;
    uint64_t offset_symbol_table;
    uint64_t size_symbol_table;
    uint64_t offset_import_table;
    uint64_t size_import_table;
    uint64_t offset_reloc_table;     // NEW v2: relocations para rebase dinamico
    uint32_t size_reloc_table;
    uint64_t offset_ir_section;      // NEW v3: IR embebido para JIT
    uint32_t size_ir_section;
    uint64_t offset_debug_section;   // debug info opcional (--vex-debug)
    uint32_t size_debug_section;
    uint8_t  debug_level;
    uint8_t  _pad[N];
};
```

**Secciones principales**:

- **Code**: bytecode VM (instrucciones extendidas con prefix 0x00 o primarias).
- **Data**: literales, static_data, params structs para `defclass`/`deffield`/etc.
- **Symbol table**: nombres calificados -> VA (para FFI y debug).
- **Import table**: plugins nativos requeridos (`stdlib/native/io/vesta_io`, etc.).
- **Reloc table** (v2+): direcciones a re-patcher si el modulo se carga en VA
  distinta a la compilada (`loadmodule` runtime).
- **IR section** (v3+, magic `VEIR`): SSA IR de todas las funciones serializado.
  Usado por el JIT C1 para compilar metodos hot a x86-64 nativo.
- **Symbol section** (v3+, magic `VSYM`): mapa nombre QUALIFIED -> VA del
  code section. Usado por el JIT para resolver referencias `@Absolute("code.label")`.
- **Debug section** (opcional, magic `DVBG`): tabla `bytecode_offset -> (file, line)`
  para el debugger.

Documentado en detalle: [doc/VMdoc/VELB (VEL Binary Format)/](./VMdoc/VELB%20(VEL%20Binary%20Format)/).

---

## 4. Modelo de ejecucion

### Maquina virtual basada en registros

Cada **ProcessVM** tiene:

- **16 registros generales** R0..R15 (cada uno 64 bits).
- **Registros especiales**: RSP, RBP, RIP, RFLAGS.
- **16 registros ZMM** (f0..f15 / xmm0..xmm15 / ymm/zmm) para float/SIMD.
- **Memoria virtual aislada** (`vm_mem`): cada proceso tiene su propio espacio
  de direcciones virtual, gestionado por arenas + TLB propio.
- **Stack independiente** (`stack_pointer` / `base_pointer`).
- **Mailbox**: cola de mensajes para IPC tipo actor.
- **Icache**: 1024 entradas de `DecodedInstr` para amortizar el coste de
  decode.
- **Exception frame stack**: cadena de `tryenter` activos.

### Convenciones de uso de registros (ABI Vesta)

- **R0**: valor de retorno (CALLVM).
- **R1-R12**: parametros (CALLVM, CALLVIRT, etc.).
- **R13**: scratch caller-saved.
- **R14**: scratch del emisor IR.
- **R15**: argc en CALLs / index en loops.
- **RSP/RBP**: stack frame.

El asignador de registros respeta esta convencion en la pre-asignacion de
parametros.

### Dispatch loop (threaded computed-goto)

El `Scheduler::run_loop` usa **threaded computed-goto**:

```cpp
static void *dispatch_table[512];   // [primary_opcode | 0x100 | extended_opcode]
goto *dispatch_table[idx];

L_MOV_RR: { ...do mov...; NEXT_DISPATCH(); }
L_ADD_RR: { ...do add...; NEXT_DISPATCH(); }
... 11 fast-path handlers ...
L_ALU3:   { switch(op) {...}; NEXT_DISPATCH(); }
L_SLOW:   { d->exec_cached(...); NEXT_DISPATCH(); }
```

Cada handler termina con su propio `goto *table[next.opcode]` (NEXT_DISPATCH),
dando al BTB del CPU una entrada predictiva por handler (vs un dispatch
centralizado con un solo BTB).

**NEXT_DISPATCH inlinea el icache hit path** (sin LTO en MinGW, la llamada a
`decode_instruction()` no se podia inlinear cross-TU; manualmente inline ahorra
~1ns/dispatch).

Resultado: ~340 MIPS promedio en el intérprete (vs ~150 baseline anterior),
medido en hot loops aritmeticos.

---

## 5. Sistema de memoria

### Arenas

`ArenaManager` por VM provee pools de memoria con permisos READ/WRITE/EXEC:

- **Arena per-proceso**: stack + datos del ProcessVM.
- **Arena compartida del VM**: code section, data section, GC heap.
- **Code cache arena** (futuro JIT): RX para codigo nativo emitido.

Implementacion en `include/arena/` usando `mmap` (POSIX) / `VirtualAlloc`
(Windows). TLB propio (`TLB.h`) cachea traducciones VA -> host.

### Memoria del proceso (VirtualMemory)

Cada ProcessVM tiene `vm_mem`: espacio de direcciones virtual 64-bit aislado.
Las VA empiezan en 0 (code section) y crecen hasta 0x10000000+ (stack base).

API:

```cpp
vm_mem.read_u8 / u16 / u32 / u64
vm_mem.write_u8 / u16 / u32 / u64
vm_mem.read_bytes / write_bytes
vm_mem.jit_cached_page_vaddr / host  // page cache para JIT (D.3-H+)
```

---

## 6. Recolector de basura

GC generacional Young/Old con design hibrido:

### Generaciones

- **Nursery (Young)**: aloc rapida via bump pointer. Minor GC mueve sobrevivientes
  a Old.
- **Old**: non-moving (mark-and-sweep). Strings (`StringObject`) se alocan
  directamente en Old (pinned) para que el `host_ptr` retornado por STRRAW sea
  estable.

### Root scanning

Hibrido:

1. **Conservativo de stack**: walks el stack del ProcessVM + GP regs buscando
   patrones que puedan ser GcHandles o host_ptrs. Falsos positivos posibles
   pero raros (~1/2^64).
2. **Interior scan en OldGen**: para cada bloque, busca el `ObjectHeader`
   contenedor de un host_ptr (cubre `STRRAW` que retorna `data[]` offset 40
   del StringObject).
3. **External refs** (A.30): plugins nativos que retienen GcHandles llaman
   `gc_addref(h)`. El GC los trata como roots permanentes.
4. **Stackmaps precisos** (D.2 Fase 1, additive con conservativo): para frames
   JIT-eados, el `JitRegistry` provee el stackmap del PC actual. Cero falsos
   positivos.

### Write barriers

Para colecciones que retienen GcHandles (ArrayList<string>, etc.), el plugin
nativo invoca `vrt_gc_write_barrier` antes de escribir un slot. El GC mantiene
un remembered set para minor GCs eficientes.

### Stats expuestos

`GcStats { alloc_count, freed_count, promoted_count, minor_gc_count,
major_gc_count, peak_old, total_allocated_bytes, live_handles,
precise_roots_marked, conservative_roots_marked }`.

---

## 7. Scheduler y procesos

### ProcessVM (proceso ligero)

Estilo Erlang: miles de procesos posibles, cada uno con stack pequeno (~1 MiB
por defecto), aislamiento de memoria, mailbox propio.

Estados: `NEW`, `READY`, `RUNNING`, `EXECUTE`, `DECODE`, `WAIT_IO`,
`BLOCKED`, `PAUSED`, `HALT`, `DEAD`.

### Scheduler

Implementacion en `src/runtime/scheduler.cpp`. Cada Scheduler corre en un OS
thread propio (con `--schedulers N` se crean N threads).

Cuota por proceso (`reductions_remaining`): cuantas instrucciones VM puede
ejecutar antes de ceder. Default ~10000. Procesos pueden ceder voluntariamente
con `yield` o por bloqueo (await, msgrecv, monwait, etc.).

### Mailbox y IPC

Cada ProcessVM tiene una cola FIFO de mensajes. `msgsend(pid, value)` pone
en la cola del destino; `msgrecv(buf, max)` extrae el siguiente (bloquea si
vacio).

PID encoding: `(scheduler_id << 32) | local_pid`. PID remoto: bit 63 = 1.

### Multi-threading real

Con `--schedulers N` (N >= 2), los procesos se distribuyen automaticamente
(round-robin por defecto) o con placement explicito via `spawn here` /
`spawn on(N)`.

Sincronizacion cross-thread: `wake_pending atomic<bool>` con double-check
para evitar lost wakeups. `make_ready` es idempotente y usa CAS-loop sobre
`state atomic<vm_state>`.

---

## 8. Runtime distribuido (VDP)

**Vesta Distribution Protocol** sobre TCP, opcionalmente cifrado con TLS.

### Mensajes principales

- `VDP_HELLO` / `VDP_HELLO_ACK`: handshake con node_id + capabilities.
- `VDP_AUTH_CHALLENGE` / `VDP_AUTH_RESPONSE`: CRAM SHA-256 con token compartido.
- `VDP_NODE_DISCOVER` / `VDP_NODE_ANNOUNCE`: discovery UDP en LAN.
- `VDP_RSPAWN`: lanza un proceso en nodo remoto con bytecode + state inicial.
- `VDP_RSPAWN_ACK`: notifica resolucion del Future asociado.
- `VDP_MSGSEND`: enviar mensaje a una mailbox remota.
- `VDP_MEMSYNC`: sincronizar region de memoria.
- `VDP_FUTURE_FULFILL`: resolver un Future cross-node.

### NodeRegistry

Cada VM mantiene un registro de nodos conocidos: `{idx, ip:port, name,
node_id, state, tls_config, ...}`. Discovery UDP lo poblea automaticamente
o se anyaden con `--dist-add-node ip:port`.

### Connection management

`TLSConnection` envuelve `TCPConnection` cuando TLS esta activo. Cada par tiene
una sesion persistente con reuso de conexion. Reconnect automatico ante
desconexiones detectadas.

Detalles: [doc/CLI_DIST.md](./CLI_DIST.md).

---

## 9. JIT C1 baseline

**Phase D.3 implementado** (cobertura ~52% de metodos reales en programas Vex).

### Cuando se dispara

`maybe_compile_method` se invoca tras cada `CALLVIRT` (hook `g_callvirt_post_hook`
desde el runtime). Cuando `method->invocation_count >= jit_threshold`, intenta
compilar.

Threshold configurable via `--jit-threshold N` o `-m jit` (= 1, compila a la
primera invocacion).

### Pipeline

```text
IR (de la seccion @ir del .velb)
    |
    v  Selector (incluyendo mini-parser de raw_asm)
    v  MachineIR (registros virtuales, ops x86-64-friendly)
    v  Regalloc (linear scan, target-aware en C2 futuro)
    v  Encoder x86-64 hand-rolled
    v  Code cache (mmap RWX + flush icache)
    v  JitRegistry (lookup pc -> stackmap, JitFn)
    |
    +-> method->jit_code = JitFn
```

### Cobertura del Selector

Soporta: ADD/SUB/MUL/AND/OR/XOR/NEG/NOT, CMP_* + SETcc, MOV/CONST,
LOAD/STORE, BR/BR_COND/PHI/RET, CALL a runtime entries con stackmap,
CALLVIRT (con inline cache slot mutable), CALLM, CALLCLOSURE, NEWOBJ via
`vrt_gc_alloc`, GC_ALLOC, GC_DEREF, GC_HANDLE_FOR_PTR, ALLOCA, DIV/MOD via
IDIV+CQO, SHL/SHR/SAR con imm const, SEXT/ZEXT/TRUNC/CAST/BITCAST,
patrones raw_asm comunes (`gchandle`, `gcderef`, etc.), @Absolute resolution
via symbol section.

Cuando un IR op no es soportada, el selector marca `unsupported=true` y el
metodo se queda en interp. Warning detallado con `--jit-warn`.

### Stackmaps precisos

Cada CALL y SAFEPOINT genera un `Stackmap { pc_offset, slots[] }` con la
naturaleza de cada slot vivo (HANDLE/HOSTPTR/STRING). El `JitRegistry`
permite al GC hacer `lookup_stackmap(rip)` durante el mark phase.

### Performance

bench_jit_method: 1700 ms interp -> **85 ms JIT** = **20× speedup**.

bench_callvirt_hot con JIT: similar speedup en metodos compilables.


---

## 10. Optimizador SSA

15 pasadas O2 organizadas en fix-point loop:

| Pasada                          | Que hace                                       |
| :------------------------------ | :--------------------------------------------- |
| `ir_pass_copy_prop`             | Propaga MOV %a = %b -> reemplaza usos de %a    |
| `ir_pass_simplify`              | Algebraic identities (x+0=x, x*1=x, etc.)      |
| `ir_pass_strength_reduction`    | mul/div power-of-2 -> shifts                    |
| `ir_pass_reassoc`               | (x op c1) op c2 -> x op (c1 op c2)              |
| `ir_pass_licm`                  | Hoist invariants fuera de loops                |
| `ir_pass_dead_alloc_elim`       | Elimina ALLOCAs cuyos slots nunca se leen      |
| `ir_pass_dce`                   | Dead Code Elimination                          |
| `ir_pass_const_fold`            | Plegado de constantes en compile time          |
| `ir_pass_unreachable`           | Elimina basic blocks inalcanzables             |
| `ir_pass_tailcall`              | CALL+RET -> TAILCALL                            |
| `ir_pass_inline_loop_header`    | Inline headers triviales para fusion cmpjmp    |
| `ir_pass_dse`                   | Dead Store Elim + Store-to-Load Forwarding     |
| `ir_pass_const_cse_entry`       | Dedupe CONST globalmente al entry block        |
| `ir_pass_cse`                   | Common Subexpression Elimination               |
| `ir_pass_load_narrow`           | Elide SEXT redundante tras LOAD i8/i16/i32     |
| `ir_pass_devirt_monomorphic`    | CALLVIRT -> CALL directo cuando 1 sola impl    |
| `ir_pass_inline`                | Inline de callees pequenos (threshold 12)     |
| `ir_pass_schedule`              | List scheduling (CPL) para ILP                 |

Documentadas: [doc/VMdoc/IR/SSA.md](./VMdoc/IR/SSA.md) seccion 9.

---

## 11. Set de instrucciones VM

~120 opcodes organizados en:

- **Primary opcodes** (0x01-0xFF): 1 byte, ALU + MOV + JMP + ...
- **Extended opcodes** (`0x00 <opcode2>`): 2 bytes para opcodes especializados
  (POO, distrib, GC, async, FFI, super-instrucciones).

Familias documentadas en [doc/VMdoc/SetInstruccionesVM/](./VMdoc/SetInstruccionesVM/):

| Familia | Opcodes |
|---|---|
| ALU + logica | 0x05-0x1D (add/sub/mul/div/cmp/and/or/xor/not/shl/shr/sar) |
| MOV variants | 0x14-0x1F (mov, movh, movc, movch) |
| Closures | 0x20-0x26 (mkclosure, callclosure, raw closure, tailcall, isnull, unwrap) |
| Pattern dispatch | 0x27-0x28 (jumptable, typeswitch) |
| Async | 0x29-0x2C (future, await, fulfill, reject) |
| Stack ops | 0x2E-0x2F (subsp, addsp) |
| Weak refs | 0x30-0x34 (weakref, deref_weak, free_weak) |
| Sincronizacion | 0x35-0x39 (monenter, monexit, monwait, monnoti, monnota) |
| Generics | 0x3A (specialize) |
| Distribucion | 0x3B-0x3E (rspawn, msgsend, msgrecv, memsync) |
| Mod / setcc / try | 0x40, 0x43, 0x44-0x45 |
| Strings | 0x46-0x54 (strmake, strlen, strcat, strcmp, strraw, etc.) |
| GC / runtime info | 0x56-0x5D (gchandle, getpid, spawnon, loadmod, panic, ...) |
| Static fields | 0x60-0x61 (getstatic, setstatic) |
| FFI runtime | 0x62-0x64 (dlopen, dlsym, callni) |
| Optimizados | 0x65-0x6A (gcallocp, spawnargs, fulfillhlt, cmpjmp, cmpjmpu, decjnz) |
| Smart pointers | 0x72 (mvtake) |
| Super-instr ALU | 0x73-0x7B (adds3, subs3, ..., and3/or3/xor3) |
| Super-instr LOAD | 0x7C-0x7D (loadz, loadzh) |
| Meta-OOP | 0xC9-0xCF (defclass, deffield, defmethod, findclass, findmethod, addadvice, findfield) |
| Float | 0xF0-0xFC (fadd, fsub, fmul, fdiv, fcmp, fsqrt, fabs, fneg, fcvt, fmowi, fload, fstore) |
| OOP dispatch | 0xFD-0xFE (callm, proceed) |

Documentos en [doc/VMdoc/SetInstruccionesVM/](./VMdoc/SetInstruccionesVM/) por
familia.

---

## 12. FFI nativo

Dos sabores:

### Declarativo (zero-overhead)

```vex
extern "kernel32.dll" {
    fn GetCurrentProcessId() -> u32;
}
```

Resolucion **compile-time**: el linker registra la dependencia. En runtime,
el Loader hace `LoadLibraryA` + `GetProcAddress` al cargar el `.velb`. Las
llamadas usan `calln @Method("kernel32.dll:GetCurrentProcessId")` (opcode
estatico).

### Runtime dinamico (cuando la lib/fn se decide en ejecucion)

```vex
i64 lib = ffi_open("user32.dll");
i64 fn = ffi_sym(lib, "MessageBoxA");
ffi_call(fn, 0, str_cstr("Hello"), str_cstr("Title"), 0);
```

Opcodes `dlopen` (0x62), `dlsym` (0x63), `callni` (0x64). Mismo
`invoke_native_unchecked` que CALLN estatico (cero overhead vs declarativo).

Ambos modos calling convention: args en R1..R12, argc en R15, return en R0.

---

## 13. Sistema de plugins

VestaVM puede cargar plugins nativos compilados como `.dll`/`.so` con API C
pura. Modelo "A" (sin overhead de C++):

```c
// En el plugin:
#include "vesta_plugin.h"

VESTA_PLUGIN_EXPORT
void vesta_init(const VestaPluginAPI *api) {
    // api->vm_read_bytes(proc, vm_addr, host_buf, len);
    // api->log("plugin loaded");
    // api->gc_addref / gc_release (write barrier para GC roots)
    g_api = api;
}

// Funciones exportadas para CALLN:
VESTA_PLUGIN_EXPORT
uint64_t my_function(uint64_t a, uint64_t b) {
    return a + b;
}
```

Plugins built-in en `stdlib/native/`:

- **vesta_io**: print/println, file I/O, formato (vio_print_*, vio_*_to_vmbuf).
- **vesta_math**: 18 funciones IEEE 754 (sqrt, pow, sin, cos, log, ...).
- **vesta_collections**: HashMap (swisstable + SIMD), TreeMap (Red-Black),
  ArrayList, Queue, Deque, HashSet, string ops, array ops.

Detalles: [doc/VMdoc/runtime/StdlibNativa/](./VMdoc/runtime/StdlibNativa/) y

---

## 14. Layout del codigo fuente

```text
VM/
├── README.md              # entrada principal del proyecto
├── CMakeLists.txt         # build system
├── main.cpp               # entry del binario vm
│
├── include/               # headers publicos (~140 archivos)
│   ├── arena/             # ArenaManager, VirtualMemory, TLB
│   ├── bytecode/          # decode, instr formats
│   ├── cli/               # REPL + VSH
│   ├── controller/        # TLS, request routing
│   ├── debug/             # debugger TCP, debug info
│   ├── emmit/             # parser_to_bytecode, emit functions
│   ├── ffi/               # native_ffi, vesta_plugin.h
│   ├── gc/                # GcHeap, RawAllocator
│   ├── ir/                # ssa_ir, ir_emitter, optimizer, regalloc, liveness
│   ├── jit/               # code_cache, runtime_entries, selector, encoder
│   ├── lexer/             # tokens
│   ├── linker/            # velb_linker_bytecode
│   ├── loader/            # loader, class_registry, string_object
│   ├── net/               # TCP, connection
│   ├── parser/            # AST nodes
│   ├── runtime/           # ProcessVM, scheduler, exec, decode_table
│   ├── util/              # ThreadPool, sqlite_singleton
│   ├── vesta_rt/          # public.h API (Phase C)
│   └── vex/               # frontend Vex (lexer, parser, type checker, lowering)
│
├── src/                   # implementaciones (mirror de include/)
│
├── stdlib/                # plugins built-in (.c)
│   └── native/
│       ├── io/vesta_io.c
│       ├── math/vesta_math.c
│       └── collections/vesta_collections.c
│
├── preprocessor/          # VPP (~5000 LOC C)
│
├── libs/                  # submodulos vendored
│   └── SourceCode/
│       ├── keystone/      # assembler nativo (BSD)
│       ├── capstone/      # disassembler (BSD)
│       └── LibPEparse/    # PE/COFF/ELF emit + parse (vendored)
│
├── doc/                   # documentacion (.md)
│   ├── VMdoc/             # docs en formato Obsidian-friendly
│   │   ├── Vex/           # docs del lenguaje (17 archivos)
│   │   ├── IR/            # SSA.md
│   │   ├── SetInstruccionesVM/  # 50+ docs por familia de opcodes
│   │   ├── runtime/       # ProcessVM, scheduler, plugins
│   │   └── ...
│   └── *.md               # QUICKSTART, ARCHITECTURE, BENCHMARKS, ROADMAP
│
├── examples_codes_vex/    # ~140 ejemplos .vex
│   ├── benchmark/         # benches sinteticos
│   └── ...
│
├── tests/                 # test suites
│   └── vex/
│       └── test_vex_e2e.sh  # suite oficial (200/200)
│
└── tools/                 # herramientas
    └── dbg_client.vsh     # cliente del debugger en VSH
```

---

Documentos relacionados:

- [doc/ROADMAP.md](./ROADMAP.md) - plan de fases A-H, estado.
- [doc/BENCHMARKS.md](./BENCHMARKS.md) - performance numbers + metodologia.
- [doc/VMdoc/IR/SSA.md](./VMdoc/IR/SSA.md) - documentacion completa del IR.
- [doc/VMdoc/SetInstruccionesVM/](./VMdoc/SetInstruccionesVM/) - referencia del
  set de instrucciones.
