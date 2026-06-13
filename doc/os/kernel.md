# Kernel Architecture

> *"El kernel es el punto donde el código se encuentra con la realidad."*

---

## 1. Overview

VexOS sigue un diseño **microkernel** minimalista implementado completamente en Vex. El kernel se compila como un ELF independiente usando `vex build --bare --target=x86_64-vexos`.

### 1.1 Responsabilidades del kernel

- Gestión de memoria (paginación, memoria virtual, heap físico)
- Scheduling de procesos (modelo M:N)
- Manejo de interrupciones (IDT/IVT)
- Enrutamiento de IPC (VDP)
- Primitivas de sincronización (mutex, semáforos, canales)
- Exposición de syscalls vía FFI

Todo lo demás — controladores, sistemas de archivos, red, gráficos — vive en espacio de usuario.

---

## 2. Gestión de memoria

### 2.1 Page tables

El kernel configura page tables de 4 o 5 niveles (x86_64) usando estructuras Vex con layout explícito:

```rust
#[repr(C, align(4096))]
pub struct PageTable {
    entries: [PageTableEntry; 512],
}

#[repr(C)]
pub struct PageTableEntry {
    bits: u64,
}

impl PageTableEntry {
    pub fn set_frame(&mut self, frame: PhysFrame, flags: PageFlags) {
        self.bits = frame.start_address().as_u64() | flags.bits();
    }

    pub fn is_present(&self) -> bool {
        self.bits & 1 != 0
    }
}
```

El borrow checker de Vex garantiza que:

- Solo el kernel puede mapear páginas (`&mut PageTable` es exclusivo).
- Las regiones de usuario nunca se solapan con las del kernel.
- Los page tables no se liberan mientras están en uso (ownership).

### 2.2 Frame allocator

```rust
pub struct FrameAllocator {
    bitmap: &'static mut [u64],
    total_frames: usize,
    next_hint: AtomicUsize,
}

impl FrameAllocator {
    pub fn allocate(&mut self) -> Option<PhysFrame> {
        // Escanea el bitmap buscando frames libres
        // Usa el borrow checker para garantizar exclusividad
    }

    pub fn deallocate(&mut self, frame: PhysFrame) {
        // Marca el frame como libre
    }
}
```

### 2.3 Heap del kernel

El heap usa un allocador buddy system:
- Allocaciones pequeñas (< 4KB) via slab allocator
- Allocaciones grandes via buddy (potencias de 2)
- Los bloques liberados se coalescen inmediatamente

### 2.4 Memoria virtual de usuario

Cada proceso recibe un `AddressSpace`:

```rust
pub struct AddressSpace {
    page_table: Box<PageTable>,
    regions: Vec<MemoryRegion>,
    allocator: UserVmm,
}
```

- El kernel mapea páginas bajo demanda (demand paging)
- Las páginas se marcan como presentes solo cuando se acceden
- Copy-on-Write para forks
- `mprotect` vía syscall con verificación de límites

---

## 3. Scheduling (Modelo M:N)

VexOS usa el modelo **M:N** (many user threads, few kernel threads) heredado de VestaVM.

### 3.1 Estructura

```
                   ┌──────────────────┐
                   │  CPU 0   CPU 1   │
                   └────────┬─────────┘
                            │
              ┌─────────────┴─────────────┐
              │       Scheduler           │
              │  ┌─────────────────────┐  │
              │  │  Run Queue (per CPU)│  │
              │  │  [KThread 0]        │  │
              │  │  [KThread 1]        │  │
              │  └─────────────────────┘  │
              └───────────────────────────┘
                        │
        ┌───────────────┼───────────────┐
        │               │               │
  ┌─────▼─────┐  ┌──────▼──────┐  ┌─────▼─────┐
  │  UThread A │  │  UThread B  │  │  UThread C │
  │  (Proceso) │  │  (Proceso)  │  │  (Proceso) │
  └────────────┘  └─────────────┘  └────────────┘
```

### 3.2 Kernel threads (KThreads)

- 1 KThread por núcleo de CPU (o configurable).
- Ejecutan en modo kernel.
- Gestionan interrupciones y syscalls.
- Se aprovisionan de la run queue global.

### 3.3 User threads (UThreads)

- Múltiples UThreads por proceso.
- Planificados sobre los KThreads por el scheduler.
- Cambio de contexto sin syscall (usando `swapcontext`).
- El scheduler usa round-robin con prioridades dinámicas.

### 3.4 Implementación en Vex

```rust
pub struct Scheduler {
    kthreads: Vec<KThread>,
    run_queue: Arc<RwLock<VecDeque<UThreadId>>>,
}

impl Scheduler {
    pub fn schedule(&mut self, cpu_id: usize) {
        let kthread = &mut self.kthreads[cpu_id];
        // Selecciona el próximo UThread de la run queue
        // Guarda contexto, carga nuevo contexto, retorna
    }
}
```

El borrow checker asegura que:
- Cada KThread tiene acceso exclusivo a su stack y contexto.
- La run queue es `Arc<RwLock<>>` para acceso desde múltiples núcleos.
- Los UThreads no pueden escapar de sus límites de memoria.

---

## 4. Manejo de interrupciones

### 4.1 IDT (Interrupt Descriptor Table)

```rust
#[repr(C, packed)]
pub struct IdtEntry {
    base_low: u16,
    selector: u16,
    ist: u8,
    flags: u8,
    base_mid: u16,
    base_high: u32,
    _reserved: u32,
}
```

Se configura usando `raw_asm`:

```rust
pub fn load_idt(idt: &Idt) {
    unsafe {
        raw_asm!("lidt [rdi]", "rdi" => idt);
    }
}
```

### 4.2 ISRs e IRQs

```rust
// Definido en tiempo de compilación para cada interrupción
#[interrupt]
fn isr_page_fault(frame: &PageFaultFrame) {
    let address = read_cr2();
    match handle_page_fault(address, frame.error_code) {
        Ok(()) => {},
        Err(_) => panic!("Page fault no manejable en {:#x}", address),
    }
}
```

Las interrupciones de hardware (IRQ) se despachan a los controladores registrados:

```rust
pub struct IrqManager {
    handlers: [Option<IrqHandler>; 16], // PIC tiene 16 IRQs
}

impl IrqManager {
    pub fn register(&mut self, irq: u8, handler: IrqHandler) {
        self.handlers[irq as usize] = Some(handler);
    }
}
```

### 4.3 APIC (x2APIC)

Para sistemas SMP, se usa el x2APIC:

```rust
pub fn send_ipi(cpu_id: u8, vector: u8) {
    unsafe {
        raw_asm!(
            "wrmsr",
            "ecx" => 0x830, // ICR
            "eax" => vector as u32 | (1 << 14), // fixed + assert
            "edx" => cpu_id as u32,
        );
    }
}
```

---

## 5. System Calls

### 5.1 Convención FFI

Las syscalls se exponen como funciones Vex marcadas con `@Extern`:

```rust
// En userland:
@Extern
fn vex_read(fd: i32, buf: *mut u8, count: usize) -> isize;

@Extern
fn vex_write(fd: i32, buf: *const u8, count: usize) -> isize;

@Extern
fn vex_vdp_send(msg: &VdpMessage) -> Result<(), VdpError>;

@Extern
fn vex_mmap(addr: *mut u8, len: usize, prot: MmapProt) -> *mut u8;
```

### 5.2 Implementación en el kernel

```rust
// Handler de syscall
#[no_mangle]
pub extern "C" fn syscall_handler(syscall_no: u64, args: &[u64; 6]) -> u64 {
    match syscall_no {
        0 => sys_read(args[0] as i32, args[1] as *mut u8, args[2] as usize),
        1 => sys_write(args[0] as i32, args[1] as *const u8, args[2] as usize),
        2 => sys_vdp_send(args[0] as *const VdpMessage),
        9 => sys_mmap(args[0] as *mut u8, args[1] as usize, args[2] as MmapProt),
        _ => -1, // ENOSYS
    }
}
```

### 5.3 Llamada de syscall

```rust
pub fn syscall(number: u64, args: &[u64; 6]) -> u64 {
    let result: u64;
    unsafe {
        raw_asm!(
            "syscall",
            "rax" => number,
            "rdi" => args[0],
            "rsi" => args[1],
            "rdx" => args[2],
            "r10" => args[3],
            "r8"  => args[4],
            "r9"  => args[5],
            lateout "rax" => result,
        );
    }
    result
}
```

---

## 6. Boot process

```
┌──────────┐     ┌──────────┐     ┌──────────┐     ┌──────────┐
│  UEFI    │ ──► │  limine  │ ──► │  VexOS   │ ──► │  Kernel  │
│  Firmware│     │  boot    │     │  Loader  │     │  Init    │
│          │     │  (C)     │     │  (Vex    │     │  (Vex    │
│          │     │          │     │   Bare)  │     │   Bare)  │
└──────────┘     └──────────┘     └──────────┘     └──────────┘
```

### 6.1 Fase 1: UEFI

- La UEFI firmware carga el bootloader `limine` desde la partición ESP.
- Limine se configura con `limine.cfg` que apunta al cargador VexOS.

### 6.2 Fase 2: Limine boot

- Limine cambia a modo largo (long mode).
- Pasa información de hardware a VexOS Loader via el protocolo Limine.
- Incluye: mapa de memoria, framebuffer, RSDP (ACPI), SMP info.

### 6.3 Fase 3: VexOS Loader (Vex Bare)

```rust
// entry.vex — Punto de entrada en Vex Bare
#[no_mangle]
pub extern "C" fn _start(boot_info: &BootInfo) -> ! {
    // 1. Configurar page tables iniciales
    // 2. Configurar GDT/IDT
    // 3. Inicializar frame allocator con mapa de memoria
    // 4. Cargar kernel desde el disco (VexFS)
    // 5. Saltar a kernel::init()
    kernel::init(boot_info)
}
```

### 6.4 Fase 4: Kernel init

```rust
pub fn init(boot_info: &BootInfo) -> ! {
    // 1. Inicializar consola serial
    // 2. Inicializar heap del kernel
    // 3. Configurar IDT completa con ISRs
    // 4. Inicializar PIC/APIC
    // 5. Inicializar scheduler
    // 6. Montar VexFS raíz
    // 7. Cargar init.vex (primer proceso de usuario)
    // 8. Iniciar scheduler
    init::start_scheduler()
}
```

---

## 7. Vex Bare Tier

### 7.1 ¿Qué es Vex Bare?

Vex Bare es el modo de compilación que produce ejecutables independientes **sin libc**. Es el equivalente a `no_std` en Rust, pero nativo en Vex.

### 7.2 Características

- **Sin libc**: no hay dependencia de glibc, musl, etc.
- **ELF standalone**: produce un ELF ejecutable directamente cargable.
- **Linker script custom**: define la disposición de memoria.
- **Stack explícito**: el kernel define su propio stack.
- **Panic handler**: el usuario define `panic_handler`.

### 7.3 Uso

```bash
vex build --bare --target=x86_64-vexos --linker=src/linker.ld src/main.vex
```

### 7.4 Ejemplo mínimo

```rust
// kernel/src/main.vex
#![bare]

#[panic_handler]
fn panic(info: &PanicInfo) -> ! {
    serial_write(b"KERNEL PANIC: ");
    serial_write(info.message.as_bytes());
    loop {}
}

#[no_mangle]
pub extern "C" fn _start() -> ! {
    // Inicializar hardware...
    loop {}
}
```

---

## 8. Estructura de directorios del kernel

```
kernel/
├── src/
│   ├── main.vex          # Punto de entrada
│   ├── boot/
│   │   ├── uefi.vex      # Información de arranque UEFI
│   │   └── limine.vex    # Protocolo Limine
│   ├── arch/
│   │   ├── x86_64/
│   │   │   ├── gdt.vex
│   │   │   ├── idt.vex
│   │   │   ├── paging.vex
│   │   │   ├── apic.vex
│   │   │   └── syscall.vex
│   │   └── aarch64/      # Futuro soporte ARM
│   ├── mem/
│   │   ├── frame.vex     # Frame allocator
│   │   ├── heap.vex      # Kernel heap
│   │   └── vmm.vex       # Virtual memory manager
│   ├── proc/
│   │   ├── scheduler.vex
│   │   ├── thread.vex
│   │   └── process.vex
│   ├── ipc/
│   │   ├── vdp.vex       # VDP protocol core
│   │   └── channel.vex   # IPC channels
│   └── sync/
│       ├── mutex.vex
│       └── semaphore.vex
├── linker.ld
├── limine.cfg
└── build.vex
```

---

## 9. Syscall reference

| # | Nombre | Args | Descripción |
|---|---|---|---|
| 0 | `vex_read` | fd, buf, count | Leer de FD |
| 1 | `vex_write` | fd, buf, count | Escribir a FD |
| 2 | `vex_vdp_send` | msg | Enviar mensaje VDP |
| 3 | `vex_vdp_recv` | buf, timeout | Recibir mensaje VDP |
| 4 | `vex_mmap` | addr, len, prot | Mapear memoria |
| 5 | `vex_munmap` | addr, len | Desmapear memoria |
| 6 | `vex_spawn` | path, args | Crear proceso |
| 7 | `vex_exit` | code | Terminar proceso |
| 8 | `vex_clock_get` | clock_id | Obtener tiempo |
| 9 | `vex_irq_register` | irq, handler | Registrar IRQ handler |
