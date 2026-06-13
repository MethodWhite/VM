# Driver Model

> *"Un driver es el puente entre el mundo abstracto del software y la física del hardware."*

---

## 1. Filosofía

En VexOS, los drivers son **procesos de espacio de usuario** con acceso controlado al hardware a través de:

- **MMIO**: regiones de memoria física mapeadas al espacio del driver.
- **PIO**: instrucciones `in`/`out` encapsuladas en `@Extern`.
- **IRQ**: líneas de interrupción enrutadas por el kernel al driver.
- **DMA**: buffers de memoria física compartidos entre driver y dispositivo.

Cada driver se aísla usando el borrow checker de Vex: el acceso a hardware se encapsula en módulos `unsafe` mínimos, y toda la lógica del driver usa Vex seguro.

---

## 2. Hardware Abstraction Layer (HAL)

La HAL de VexOS se construye con metaprogramación:

```rust
// hal/port.vex — Abstracción de puertos E/S
pub trait Port {
    type Width;
    fn read(port: u16) -> Self;
    fn write(port: u16, value: Self);
}

impl Port for u8 {
    type Width = u8;
    fn read(port: u16) -> u8 {
        let val: u8;
        unsafe { raw_asm!("in al, dx", "dx" => port, lateout "al" => val); }
        val
    }
    fn write(port: u16, value: u8) {
        unsafe { raw_asm!("out dx, al", "dx" => port, "al" => value); }
    }
}

impl Port for u16 {
    type Width = u16;
    fn read(port: u16) -> u16 {
        let val: u16;
        unsafe { raw_asm!("in ax, dx", "dx" => port, lateout "ax" => val); }
        val
    }
    fn write(port: u16, value: u16) {
        unsafe { raw_asm!("out dx, ax", "dx" => port, "ax" => value); }
    }
}

impl Port for u32 {
    type Width = u32;
    fn read(port: u16) -> u32 {
        let val: u32;
        unsafe { raw_asm!("in eax, dx", "dx" => port, lateout "eax" => val); }
        val
    }
    fn write(port: u16, value: u32) {
        unsafe { raw_asm!("out dx, eax", "dx" => port, "eax" => value); }
    }
}
```

### 2.1 MMIO con acceso volátil

```rust
#[repr(C)]
pub struct MmioReg<T: Copy> {
    value: T,
}

impl<T: Copy> MmioReg<T> {
    pub fn read(&self) -> T {
        unsafe { raw_asm!("" : : : "memory" : "volatile"); }
        self.value
    }

    pub fn write(&mut self, val: T) {
        self.value = val;
        unsafe { raw_asm!("" : : : "memory" : "volatile"); }
    }
}
```

### 2.2 Ejemplo: PCI configuration space

```rust
pub struct PciDevice {
    bus: u8,
    device: u8,
    function: u8,
}

impl PciDevice {
    pub fn read_config(&self, offset: u8) -> u32 {
        let addr = 0x8000_0000u32
            | (self.bus as u32) << 16
            | (self.device as u32) << 11
            | (self.function as u32) << 8
            | (offset as u32) & 0xFC
            | 0x01; // Enable bit
        Port::<u32>::write(0xCF8, addr);
        Port::<u32>::read(0xCFC)
    }
}
```

---

## 3. DMA, MMIO e IRQ

### 3.1 DMA (Direct Memory Access)

El kernel expone un allocador de memoria física para buffers DMA:

```rust
#[syscall]
pub fn vex_dma_alloc(size: usize) -> Result<DmaBuffer, DmaError> {
    // Alloca frames físicos contiguos
    // Los mapea al espacio de dirección del driver
    // Retorna dirección física + virtual
}

pub struct DmaBuffer {
    phys: PhysAddr,
    virt: VirtAddr,
    size: usize,
}

impl DmaBuffer {
    pub fn phys_addr(&self) -> PhysAddr { self.phys }
    pub fn as_slice(&self) -> &[u8] {
        unsafe { slice::from_raw_parts(self.virt.as_ptr(), self.size) }
    }
    pub fn as_slice_mut(&mut self) -> &mut [u8] {
        unsafe { slice::from_raw_parts_mut(self.virt.as_mut_ptr(), self.size) }
    }
}
```

### 3.2 MMIO mapping

```rust
// El kernel mapea la BAR del dispositivo PCI al espacio del driver
pub fn map_mmio(phys_base: PhysAddr, size: usize) -> Result<MmioRegion, MmapError> {
    let virt = vex_mmap(
        ptr::null_mut(),
        size,
        MmapProt::READ | MmapProt::WRITE | MmapProt::MMIO,
    );
    // Mapea páginas con caché deshabilitada (PAT: WC o UC)
    Ok(MmioRegion { base: virt, size })
}
```

### 3.3 IRQ handling

```rust
pub struct IrqHandler {
    irq: u8,
    callback: Box<dyn Fn() -> IrqResult>,
}

impl IrqHandler {
    pub fn register(irq: u8, callback: impl Fn() -> IrqResult + 'static) -> Self {
        let handler = IrqHandler { irq, callback: Box::new(callback) };
        vex_irq_register(irq, irq_handler_trampoline);
        handler
    }
}

// El kernel llama a esta función cuando llega la IRQ
#[no_mangle]
pub extern "C" fn irq_dispatcher(irq: u8) {
    // Busca el handler registrado y lo ejecuta
}
```

---

## 4. Integración con sistema fotónico

En el universo de la novela, D++ controla sistemas de láser y energía. En VexOS, esto se traduce en controladores para hardware fotónico real (VCSEL, fotodiodos, moduladores ópticos).

### 4.1 Control de láser via PIO

```rust
pub struct LaserController {
    // Puertos PIO para control de láser
    power_port: u16,     // Puerto de control de potencia
    modulation_port: u16, // Puerto de modulación
    status_port: u16,    // Puerto de estado
}

impl LaserController {
    pub fn new(power_port: u16, modulation_port: u16, status_port: u16) -> Self {
        LaserController { power_port, modulation_port, status_port }
    }

    pub fn set_power(&self, mw: u32) {
        // Escribe potencia en milivatios
        Port::<u32>::write(self.power_port, mw);
    }

    pub fn modulate(&self, frequency_mhz: u32, amplitude: u16) {
        ModulatorConfig {
            frequency: frequency_mhz,
            amplitude,
            waveform: Waveform::Sine,
        }.apply(self.modulation_port);
    }

    pub fn read_status(&self) -> LaserStatus {
        let status = Port::<u8>::read(self.status_port);
        LaserStatus::from_bits(status)
    }
}

pub struct ModulatorConfig {
    frequency: u32,
    amplitude: u16,
    waveform: Waveform,
}

impl ModulatorConfig {
    fn apply(&self, port: u16) {
        // Protocolo: [freq(32) | amp(16) | wave(8)] → 64 bits en 2 escrituras
        let high = self.frequency;
        let low = (self.amplitude as u32) | ((self.waveform as u32) << 16);
        Port::<u32>::write(port, low);
        Port::<u32>::write(port + 4, high);
    }
}
```

### 4.2 Sistema de sensores fotónicos

```rust
pub struct PhotonicSensorArray {
    mmio: MmioRegion,
}

impl PhotonicSensorArray {
    pub fn new(mmio: MmioRegion) -> Self {
        PhotonicSensorArray { mmio }
    }

    pub fn read_all(&self) -> Vec<SensorReading> {
        // Barre todos los sensores en el array MMIO
        (0..self.sensor_count())
            .map(|i| self.read_sensor(i))
            .collect()
    }

    fn read_sensor(&self, index: usize) -> SensorReading {
        let reg = self.mmio.read::<PhotonicRegister>(index);
        SensorReading {
            wavelength_nm: reg.wavelength,
            intensity_mw:  reg.intensity,
            temperature_c: reg.temp,
        }
    }
}
```

---

## 5. Ejemplo: VGA Framebuffer Driver

```rust
pub struct VgaFramebuffer {
    fb: &'static mut [u32],  // Framebuffer mapeado en memoria
    width: usize,
    height: usize,
    pitch: usize,
}

impl VgaFramebuffer {
    pub fn new(boot_info: &BootInfo) -> Self {
        let fb_info = &boot_info.framebuffer;
        let fb = unsafe {
            slice::from_raw_parts_mut(
                fb_info.address as *mut u32,
                fb_info.pitch * fb_info.height / 4,
            )
        };
        VgaFramebuffer {
            fb,
            width: fb_info.width as usize,
            height: fb_info.height as usize,
            pitch: fb_info.pitch as usize,
        }
    }

    pub fn clear(&mut self, color: RgbColor) {
        let pixel = color.to_u32();
        for p in self.fb.iter_mut() {
            *p = pixel;
        }
    }

    pub fn draw_rect(&mut self, x: usize, y: usize, w: usize, h: usize, color: RgbColor) {
        let pixel = color.to_u32();
        for row in y..(y + h).min(self.height) {
            for col in x..(x + w).min(self.width) {
                self.fb[row * (self.pitch / 4) + col] = pixel;
            }
        }
    }

    pub fn scroll(&mut self, lines: usize) {
        let line_pixels = self.pitch / 4;
        let copy_bytes = (self.height - lines) * self.pitch;
        let dst = self.fb.as_mut_ptr();
        let src = unsafe { self.fb.as_ptr().add(lines * line_pixels) };
        unsafe { ptr::copy(src, dst, copy_bytes / 4) };
        // Limpiar las nuevas líneas
        let clear_start = (self.height - lines) * line_pixels;
        for p in self.fb[clear_start..].iter_mut() {
            *p = 0;
        }
    }
}
```

---

## 6. Ejemplo: AHCI/SATA Storage Driver

```rust
pub struct AhciController {
    mmio: &'static mut AhciRegisters,
    ports: Vec<AhciPort>,
}

#[repr(C)]
struct AhciRegisters {
    cap: MmioReg<u32>,        // Capacidades
    ghc: MmioReg<u32>,        // Control global
    is: MmioReg<u32>,         // Interrupt status
    pi: MmioReg<u32>,         // Ports implemented
    vs: MmioReg<u32>,         // Version
    ccc_ctl: MmioReg<u32>,    // Command completion coalescing
    ccc_pts: MmioReg<u32>,    // CCC ports
    _reserved: [u8; 0xA0],
    ports: [AhciPortRegs; 32],
}

#[repr(C)]
struct AhciPortRegs {
    clb:  MmioReg<u64>,  // Command list base
    fb:   MmioReg<u64>,  // FIS base
    is:   MmioReg<u32>,  // Interrupt status
    ie:   MmioReg<u32>,  // Interrupt enable
    cmd:  MmioReg<u32>,  // Command and status
    _res: [u8; 0x4],
    tfd:  MmioReg<u32>,  // Task file data
    sig:  MmioReg<u32>,  // Signature
    ssts: MmioReg<u32>,  // SATA status
    sctl: MmioReg<u32>,  // SATA control
    serr: MmioReg<u32>,  // SATA error
    sact: MmioReg<u32>,  // SATA active
    ci:   MmioReg<u32>,  // Command issue
    sntf: MmioReg<u32>,  // SATA notification
    fbw:  MmioReg<u32>,  // FIS-based switching
}

impl AhciController {
    pub fn new(pci_device: &PciDevice) -> Result<Self, AhciError> {
        let bar5 = pci_device.read_config(0x24) as u64;
        let abar = bar5 & 0xFFFF_FFFF_FFFF_FFF0;
        let mmio = map_mmio(PhysAddr::new(abar), 0x1000)?;
        let regs = unsafe { &mut *(mmio.as_mut_ptr() as *mut AhciRegisters) };

        Ok(AhciController {
            mmio: regs,
            ports: Vec::new(),
        })
    }

    pub fn detect_ports(&mut self) {
        let pi = self.mmio.pi.read();
        for i in 0..32 {
            if pi & (1 << i) != 0 {
                let port_regs = &self.mmio.ports[i];
                let ssts = port_regs.ssts.read();
                let det = ssts & 0x0F;
                let ipm = (ssts >> 8) & 0x0F;
                if det == 0x03 && ipm == 0x01 {
                    // Dispositivo presente y activo
                    let sig = port_regs.sig.read();
                    match sig {
                        0x0000_0000 => self.init_port(i, DeviceType::Sata),
                        0xEB14_0000 => self.init_port(i, DeviceType::Atapi),
                        0xC33C_0000 => self.init_port(i, DeviceType::PortMultiplier),
                        _ => {},
                    }
                }
            }
        }
    }

    fn init_port(&mut self, port_no: usize, dev_type: DeviceType) {
        let port = AhciPort::new(port_no, dev_type);
        port.allocate_dma_buffers();
        port.configure_interrupts();
        self.ports.push(port);
    }

    pub fn read_block(&self, port_no: usize, lba: u64, buffer: &mut [u8]) -> Result<(), AhciError> {
        let port = &self.ports[port_no];
        let cmd = port.build_command(lba, buffer.len(), CommandType::ReadDma);
        port.issue_command(&cmd);
        port.wait_for_completion();
        port.copy_data(buffer);
        Ok(())
    }

    pub fn write_block(&self, port_no: usize, lba: u64, buffer: &[u8]) -> Result<(), AhciError> {
        let port = &self.ports[port_no];
        port.copy_data_in(buffer);
        let cmd = port.build_command(lba, buffer.len(), CommandType::WriteDma);
        port.issue_command(&cmd);
        port.wait_for_completion();
        Ok(())
    }
}
```

---

## 7. Estructura de un driver

Cada driver en VexOS sigue esta convención:

```
drivers/
├── vga/
│   ├── src/
│   │   ├── lib.vex        # Punto de entrada del driver
│   │   ├── framebuffer.vex # Lógica del framebuffer
│   │   └── modeset.vex     # Configuración de modos
│   └── build.vex
├── ahci/
│   ├── src/
│   │   ├── lib.vex
│   │   ├── controller.vex  # Controlador AHCI
│   │   ├── port.vex        # Gestión de puertos
│   │   └── dma.vex         # Buffers DMA
│   └── build.vex
└── photonic/
    ├── src/
    │   ├── lib.vex
    │   ├── laser.vex       # Control de láser
    │   ├── sensor.vex      # Matriz de sensores
    │   └── protocol.vex    # Protocolo de comunicación
    └── build.vex
```

---

## 8. Ciclo de vida de un driver

```
       ┌──────────────┐
       │  PCI/Virtio  │
       │  Detection   │
       └──────┬───────┘
              │
       ┌──────▼───────┐
       │  MMIO/PIO    │
       │  Mapping     │
       └──────┬───────┘
              │
       ┌──────▼───────┐
       │  DMA Buffer  │
       │  Allocation  │
       └──────┬───────┘
              │
       ┌──────▼───────┐
       │  IRQ         │
       │  Register    │
       └──────┬───────┘
              │
       ┌──────▼───────┐
       │  Active      │
       │  Operation   │
       └──────┬───────┘
              │
       ┌──────▼───────┐
       │  Teardown    │
       │  & Free      │
       └──────────────┘
```
