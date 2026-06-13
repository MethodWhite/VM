# VexOS: Un sistema operativo construido con Vex

> *"D++ no era solo un lenguaje. Era un sistema operativo, una forma de vida, una declaración de independencia."*
> — Tengo una mansión en el apocalipsis

---

## 1. Introducción: La visión de D++

En la novela *Tengo una mansión en el apocalipsis*, el protagonista obtiene **D++**, un lenguaje de programación de otro mundo que le permite construir cualquier cosa: defensas, sensores, redes, automatización. D++ no distingue entre código y realidad — escribe lógica y el mundo responde.

**VexOS** es nuestro intento de llevar esa visión al mundo real usando **Vex**, el lenguaje de sistemas moderno. VexOS no es un kernel académico. Es una plataforma que aspira a la misma integración total que D++ prometía: hardware, red, distribución, seguridad — todo expresado en un solo lenguaje, con un solo compilador, bajo un solo modelo de memoria.

---

## 2. ¿Por qué Vex para desarrollo de SO?

| Capacidad D++ | Cómo la logra Vex |
|---|---|
| Control total del hardware | `raw_asm`, `@Extern`, FFI, punteros raw |
| Programas que nunca crashean | Borrow checker + `no_std` + kernel panic handlers |
| Compilación instantánea | JIT vía `vex build --jit` para hot paths |
| Abstracción del hardware | Metaprogramación con `comptime` y genéricos |
| Código autoconsciente | Traits, `anyop`, reflexión en tiempo de compilación |

### 2.1 Memory safety para el kernel

El borrow checker de Vex garantiza que el núcleo del sistema esté libre de:

- **Use-after-free**: el compilador rechaza accesos a memoria liberada.
- **Data races**: el sistema de ownership elimina carreras en tiempo de compilación.
- **Buffer overflows**: los bounds checks (opt-in con `--safe`) protegen arreglos.

Para código de bajo nivel (IDT, page tables, MMIO), `raw_asm` y `unsafe` permiten el control necesario, encapsulado en módulos verificados.

### 2.2 JIT para paths críticos

El compilador JIT de Vex permite compilar y ejecutar código en caliente:

```rust
// Hot path compilado con JIT
let fast_path = vex::jit::compile!(|| {
    for packet in network_rx_queue {
        process_packet(packet);
    }
});
```

Los controladores de red, el scheduling y el renderizado gráfico se benefician de la optimización JIT.

### 2.3 Metaprogramación para abstracción de hardware

Con `comptime` y genéricos, VexOS genera código específico para cada plataforma:

```rust
// Generado en tiempo de compilación para cada arquitectura
pub fn write_port<T: PortWidth>(port: u16, value: T) {
    comptime {
        match T {
            u8  => raw_asm!("out dx, al", "dx" => port, "al" => value),
            u16 => raw_asm!("out dx, ax", "dx" => port, "ax" => value),
            u32 => raw_asm!("out dx, eax", "dx" => port, "eax" => value),
        }
    }
}
```

### 2.4 FFI para acceso directo al hardware

```rust
@Extern
fn inb(port: u16) -> u8;
@Extern
fn outb(port: u16, value: u8);
```

### 2.5 Vex Bare tier

Vex soporta `vex build --bare`, produciendo ELFs independientes sin libc, ideales para:

- Cargador UEFI
- Kernel `no_std`
- Controladores en espacio real
- Módulos de boot

---

## 3. Arquitectura general

```
┌──────────────────────────────────────────────────────┐
│                   USERSPACE                          │
│  ┌──────────┐  ┌──────────┐  ┌───────────────────┐  │
│  │ vex_app  │  │ vex_app  │  │ SolidEngine (GUI) │  │
│  │ (JIT)    │  │ (AOT)    │  │ (JIT + GL)        │  │
│  └────┬─────┘  └────┬─────┘  └────────┬──────────┘  │
│       │              │                 │             │
│  ┌────▼──────────────▼─────────────────▼──────────┐  │
│  │            VDP Protocol (IPC)                  │  │
│  │  (Vex Distributed Protocol — msgpack + cap)    │  │
│  └────────────────────┬───────────────────────────┘  │
├───────────────────────┼──────────────────────────────┤
│                KERNEL │                              │
│  ┌────────────────────▼───────────────────────────┐  │
│  │         VexOS Microkernel                      │  │
│  │  ┌──────────┐ ┌──────────┐ ┌────────────────┐ │  │
│  │  │ Scheduler│ │ Memory   │ │ IPC Router     │ │  │
│  │  │ (M:N)   │ │ (Borrow) │ │ (VDP over shm) │ │  │
│  │  └──────────┘ └──────────┘ └────────────────┘ │  │
│  └────────────────────┬───────────────────────────┘  │
│                       │                               │
│  ┌────────────────────▼───────────────────────────┐  │
│  │              Driver Layer                      │  │
│  │  ┌──────────┐ ┌──────────┐ ┌────────────────┐ │  │
│  │  │ VGA/FB   │ │ AHCI/SATA│ │ Photonic/Laser │ │  │
│  │  │ (Borrow) │ │ (Borrow) │ │ (PIO/MMIO)     │ │  │
│  │  └──────────┘ └──────────┘ └────────────────┘ │  │
│  └────────────────────┬───────────────────────────┘  │
├───────────────────────┼──────────────────────────────┤
│                  HARDWARE                             │
│  ┌────────────────────▼───────────────────────────┐  │
│  │   CPU  │  RAM  │  GPU  │  Disk  │  NIC  │ ... │  │
│  └────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────┘
```

### 3.1 Microkernel

VexOS sigue un diseño **microkernel minimalista**:

- El kernel solo gestiona: scheduling, memoria, IPC.
- Los controladores y servicios viven en espacio de usuario.
- Cada driver es un proceso aislado con su propio espacio de memoria.
- La comunicación entre servicios usa VDP sobre memoria compartida.

### 3.2 Drivers en Vex con borrow checking

Cada driver es un crate Vex independiente que:

- Declara puertos de E/S con `@Extern` o `raw_asm`
- Gestiona MMIO con punteros `volatile` protegidos por borrow checker
- Maneja IRQs mediante callbacks registrados en el kernel
- Accede a DMA a través del allocador de memoria física del kernel

### 3.3 Servicios de usuario compilados con JIT

- `vex build --jit` genera código nativo optimizado en caliente
- Los servicios críticos (servidor de archivos, servidor de red) se benefician
- La compilación AOT (`vex build --release`) se usa para servicios estables

### 3.4 IPC distribuido via VDP

**VDP (Vex Distributed Protocol)** es el protocolo de comunicación universal:

```
┌────────────────────────────────────────────┐
│              VDP Message                    │
├────────────────────────────────────────────┤
│ Version: 1                                 │
│ Flags:   [RELIABLE, ORDERED, ENCRYPTED]    │
│ Src:     node://a1b2/service/fs            │
│ Dst:     node://c3d4/app/editor            │
│ Payload: msgpack{method, path, data, cap}  │
│ Caps:    [READ, WRITE]                     │
│ Nonce:   0xdeadbeef                        │
└────────────────────────────────────────────┘
```

- Funciona entre procesos en la misma máquina (memoria compartida + ring buffer)
- Funciona entre nodos en la red (TCP/TLS + msgpack)
- Capacidades (caps) se delegan entre procesos
- Cada mensaje es asíncrono y no bloqueante

---

## 4. Comparativa: VexOS vs Linux vs seL4 vs D++

| Característica | Linux | seL4 | D++ (novela) | **VexOS** |
|---|---|---|---|---|
| **Modelo de kernel** | Monolítico | Microkernel verificado | Mágico | **Microkernel** |
| **Seguridad de memoria** | Ninguna (C) | Verificación formal | Garantizada | **Borrow checker** |
| **Lenguaje único** | No (C + asm + scripts) | No (C + asm + Haskell) | Sí (D++) | **Sí (Vex)** |
| **JIT en caliente** | eBPF parcial | No | Instantáneo | **Vex JIT** |
| **IPC distribuido** | TCP/IP + D-Bus | SeL4 IPC local | Telepático | **VDP nativo** |
| **Metaprogramación** | Macro C | No | Comptime | **`comptime` + traits** |
| **Control hardware** | C + asm inline | C + asm | Absoluto | **raw_asm + @Extern** |
| **Tamaño del kernel** | ~30M SLoC | ~10K SLoC | Inexistente | **~50K SLoC** |
| **Aislamiento** | COW + cgroups | Capabilities | Mágico | **Borrow + VDP caps** |
| **¿Verificado?** | No | Sí (Isabelle/HOL) | Intrínseco | **En desarrollo** |
| **Filosofía** | Todo en kernel | Lo mínimo verificado | Poder absoluto | **Un lenguaje para todo** |

### 4.1 VexOS vs D++

D++ en la novela es un lenguaje mágico: no tiene bugs, compila instantáneamente, y su código se manifiesta directamente en la realidad. VexOS no es mágico, pero cierra la brecha:

- **Donde D++ usa magia**, VexOS usa borrow checking, JIT y `comptime`.
- **Donde D++ es instantáneo**, VexOS ofrece compilación JIT en milisegundos.
- **Donde D++ controla el mundo**, VexOS controla hardware real con `raw_asm`.
- **Donde D++ no tiene bugs**, VexOS garantiza seguridad de memoria.
- **Donde D++ es un lenguaje único**, VexOS es un ecosistema: compilador, gestor de paquetes, protocolo VDP.

> VexOS es D++ hecho realidad — con los pies en la tierra y la cabeza en las estrellas.

---

## 5. Stack tecnológico completo

| Capa | Tecnología | Rol |
|---|---|---|
| **Lenguaje** | Vex | Todo el sistema |
| **Compilador** | vexc (Vex compiler) | Compilación cruzada, JIT, AOT |
| **Bare metal** | Vex Bare (`--bare`) | Kernel ELF sin libc |
| **Boot** | UEFI → limine → VexOS loader | Arranque |
| **IPC** | VDP | Comunicación universal |
| **Gráficos** | SolidScript + SolidEngine | GUI 2D/3D |
| **Paquetes** | vexpm | Gestión de software |
| **Build** | vexbuild | Compilación de proyectos |
| **Sistema de archivos** | VexFS (Copy-on-Write + ZSTD) | Almacenamiento |

---

## 6. Estado del proyecto

- [x] Compilador Vex funcional (AST → IR → codegen)
- [x] Soporte `--bare` para ELF standalone
- [x] JIT funcional para x86_64
- [ ] Port de limine para Vex Bare
- [ ] Kernel base (IDT, GDT, paging)
- [ ] Controlador UART (serial)
- [ ] VDP protocolo (msgpack + caps)
- [ ] Servidor de archivos
- [ ] SolidEngine (ventanas + GPU)
- [ ] vexpm para paquetes

---

## 7. Referencias

- [Vex Language Repository](https://github.com/anomalyco/Vex)
- [seL4 Microkernel](https://sel4.systems)
- [D++ — Tengo una mansión en el apocalipsis (novela china)](https://www.novelupdates.com/series/i-have-a-mansion-in-the-apocalypse/)
- [Limine Boot Protocol](https://github.com/limine-bootloader/limine)
- [msgpack](https://msgpack.org)
