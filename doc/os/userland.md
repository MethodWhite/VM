# User-space

> *"El espacio de usuario es donde Vex demuestra que es más que un lenguaje de sistemas."*

---

## 1. Filosofía

En VexOS, el espacio de usuario no es un "segundo ciudadano". Todo el software de usuario se escribe en Vex y se compila con el mismo compilador que el kernel, lo que garantiza:

- **Seguridad de tipos** en toda la pila.
- **IPC eficiente** mediante VDP con aprovechamiento del borrow checker.
- **Compilación JIT** para hot paths en tiempo real.
- **Paquetes gestionados** con vexpm desde el primer momento.

---

## 2. Compilación: vexbuild

`vexbuild` es el sistema de construcción de Vex. Para VexOS soporta tres modos:

### 2.1 AOT (Ahead-of-Time)

```bash
vex build --release --target=x86_64-vexos src/main.vex
```

- Produce un ELF estándar de VexOS.
- Usa la libc de VexOS (`libvexos.so`).
- Ideal para aplicaciones de sistema (servidores, daemons).

### 2.2 JIT (Just-in-Time)

```bash
vex build --jit --target=x86_64-vexos src/hotpath.vex
```

- Compila las funciones calientes en código nativo en tiempo de ejecución.
- Se integra con VDP para actualizar dinámicamente las rutas críticas.
- Ideal para: renderizado gráfico, procesamiento de red, parsing.

### 2.3 Bare (standalone)

```bash
vex build --bare --target=x86_64-vexos src/init.vex
```

- Para procesos de arranque o servicios que corren sin libc.
- Igual que el kernel, pero en espacio de usuario.

---

## 3. JIT en caliente

El JIT de Vex permite recompilar funciones en caliente mientras el programa corre:

```rust
// En un servidor web VexOS:
let handler = vex::jit::compile!(|req: &Request| -> Response {
    // Esta función se recompila si cambia la ruta
    match req.path {
        "/api/data" => api::handle_data(req),
        "/api/stream" => api::handle_stream(req),
        _ => Response::not_found(),
    }
});

// En caliente, reemplazamos el handler:
vex::jit::update!(handler, |req: &Request| -> Response {
    // Nueva versión sin reiniciar el proceso
    process_request(req)
});
```

### 3.1 Perfil de compilación

| Modo | Tiempo | Optimización | Uso |
|---|---|---|---|
| `--jit` cold | ~5ms | -O1 | Primera ejecución |
| `--jit` hot | ~50ms | -O3 | Loop calientes (>100 ejecuciones) |
| `--release` | ~30s | -O3 + LTO | Distribución final |

---

## 4. IPC via VDP

VDP (Vex Distributed Protocol) es el protocolo de comunicación universal de VexOS.

### 4.1 Arquitectura

```
Proceso A                  Proceso B
┌──────────┐              ┌──────────┐
│  App     │              │  Service │
│  ┌────┐  │              │  ┌────┐  │
│  │VDP │──┼───channel───┼─▶│VDP │  │
│  └────┘  │  shm/ring   │  └────┘  │
└──────────┘              └──────────┘
```

### 4.2 Mensaje VDP

```rust
pub struct VdpMessage {
    pub version: u8,          // 1
    pub flags: VdpFlags,      // RELIABLE, ORDERED, ENCRYPTED, CRITICAL
    pub src: VdpAddress,      // node://<id>/<service>
    pub dst: VdpAddress,      // node://<id>/<service>
    pub payload: Vec<u8>,     // msgpack-encoded
    pub caps: Vec<Capability>,// [READ, WRITE, EXECUTE, ...]
    pub nonce: u64,           // Anti-replay
    pub timestamp: u64,       // Monotonic clock
}

impl VdpMessage {
    pub fn new(src: VdpAddress, dst: VdpAddress, payload: &impl Serialize) -> Self {
        VdpMessage {
            version: 1,
            flags: VdpFlags::RELIABLE | VdpFlags::ORDERED,
            src,
            dst,
            payload: msgpack::to_vec(payload),
            caps: vec![],
            nonce: rand::u64(),
            timestamp: clock::monotonic(),
        }
    }

    pub fn verify(&self) -> Result<(), VdpError> {
        // Verifica nonce, timestamp, caps
        // Retorna error si el mensaje es inválido
        Ok(())
    }
}
```

### 4.3 VDP Address

```rust
pub struct VdpAddress {
    pub node: Uuid,     // Identificador único del nodo
    pub service: String,// Nombre del servicio
}
```

- `node://a1b2c3/fs` — El servicio de archivos en el nodo `a1b2c3`.
- `node://a1b2c3/apps/editor` — El editor en el nodo `a1b2c3`.
- `node://*` — Broadcast a todos los nodos.

### 4.4 Comunicación local (memoria compartida)

```rust
pub struct SharedChannel {
    buffer: &'static mut RingBuffer<VdpMessage>,
    sender_sem: Semaphore,
    receiver_sem: Semaphore,
}

impl SharedChannel {
    pub fn send(&mut self, msg: VdpMessage) -> Result<(), VdpError> {
        self.sender_sem.wait();
        self.buffer.push(msg);
        self.receiver_sem.signal();
        Ok(())
    }

    pub fn recv(&mut self) -> Result<VdpMessage, VdpError> {
        self.receiver_sem.wait();
        let msg = self.buffer.pop().ok_or(VdpError::Empty)?;
        self.sender_sem.signal();
        Ok(msg)
    }
}
```

### 4.5 Comunicación remota (TCP/TLS)

```rust
pub struct RemoteVdpTransport {
    stream: TlsStream<TcpStream>,
    buffer: Vec<u8>,
}

impl RemoteVdpTransport {
    pub fn connect(addr: SocketAddr) -> Result<Self, VdpError> {
        let tcp = TcpStream::connect(addr)?;
        let tls = TlsStream::new(tcp, &VexOsCert::trust_store())?;
        Ok(RemoteVdpTransport { stream: tls, buffer: Vec::new() })
    }

    pub fn send(&mut self, msg: VdpMessage) -> Result<(), VdpError> {
        let bytes = msgpack::to_vec(&msg)?;
        // Formato: [len:u32][msgpack]
        let header = (bytes.len() as u32).to_le_bytes();
        self.stream.write_all(&header)?;
        self.stream.write_all(&bytes)?;
        Ok(())
    }
}
```

---

## 5. Graphics Stack: SolidScript + SolidEngine

### 5.1 SolidScript

Lenguaje de scripting visual para interfaces de usuario, interpretado por SolidEngine:

```solidscript
// app.solid — Interfaz de editor de texto
window "Editor" {
    width: 1280
    height: 720
    title: "VexOS Text Editor"
    
    menubar {
        menu "File" {
            item "New"       action: new_file
            item "Open..."   action: open_file
            separator
            item "Save"      action: save_file
            item "Save As..." action: save_file_as
            separator
            item "Exit"     action: exit_app
        }
        menu "Edit" {
            item "Undo"     action: undo
            item "Redo"    action: redo
        }
    }
    
    textarea "content" {
        x: 0
        y: 30
        width: fill
        height: fill
        font: "monospace/14"
        syntax: "vex"
        line_numbers: true
    }
    
    statusbar {
        label "cursor_pos"  text: "Ln 1, Col 1"
        label "encoding"    text: "UTF-8"
        label "mode"        text: "INSERT"
    }
}
```

### 5.2 SolidEngine

Motor gráfico que renderiza interfaces SolidScript:

```rust
pub struct SolidEngine {
    compositor: Compositor,
    windows: Vec<SolidWindow>,
    font_renderer: FontRenderer,
    gpu: GpuContext,
}

impl SolidEngine {
    pub fn new(framebuffer: &mut VgaFramebuffer) -> Self {
        SolidEngine {
            compositor: Compositor::new(),
            windows: Vec::new(),
            font_renderer: FontRenderer::new(),
            gpu: GpuContext::new(),
        }
    }

    pub fn load_script(&mut self, script: &str) -> Result<UiHandle, SolidError> {
        let ast = SolidParser::parse(script)?;
        let window = SolidWindow::from_ast(ast)?;
        let handle = UiHandle(self.windows.len());
        self.windows.push(window);
        Ok(handle)
    }

    pub fn render(&mut self) {
        self.compositor.begin_frame();
        for window in &self.windows {
            window.render(&mut self.compositor, &self.font_renderer);
        }
        self.compositor.present(&mut self.gpu);
    }

    pub fn handle_event(&mut self, event: &InputEvent) {
        for window in &mut self.windows {
            window.handle_event(event);
        }
    }
}
```

---

## 6. Package Management: vexpm

`vexpm` es el gestor de paquetes de VexOS, inspirado en npm, cargo y pacman.

### 6.1 Comandos básicos

```bash
vexpm install solid-engine   # Instala un paquete
vexpm remove vdp-tools       # Elimina un paquete
vexpm search "web server"    # Busca paquetes
vexpm update                 # Actualiza todos los paquetes
vexpm info ahci-driver       # Información de un paquete
vexpm publish                # Publica un paquete en el registro
```

### 6.2 Formato de paquete

```toml
# vexpm.toml
[package]
name = "net-http-server"
version = "2.1.0"
description = "HTTP/1.1 server for VexOS"
author = "VexOS Team"
license = "MIT"

[dependencies]
vdp = "^1.0"
vex-fs = "^0.5"
tls = { version = "1.2", optional = true }

[build]
type = "jit"  # o "aot" o "bare"
target = "x86_64-vexos"
```

### 6.3 Registro de paquetes

- Centralizado en `registry.vexos.org`.
- Paquetes firmados con ed25519.
- Las dependencias se resuelven y compilan con vexbuild.
- Los paquetes JIT se distribuyen como IR de Vex (`.vxir`) para compilación en el cliente.

---

## 7. Procesos y servicios típicos

```
Procesos de VexOS en ejecución:
  PID  NAME            STATUS  CPU%  MEM
  ──────────────────────────────────────
  1    kernel          idle    0.1%  8MB
  2    init            running 0.0%  2MB
  3    vdp-router      running 0.5%  12MB
  4    fs-server       running 0.3%  24MB
  5    net-stack       running 1.2%  32MB
  6    solid-engine    running 4.5%  64MB
  7    vexpm-daemon    idle    0.0%  4MB
  8    terminal        running 0.8%  16MB
  ──────────────────────────────────────
  Total:  8 procesos, 162MB used / 8GB total
```

| Servicio | Descripción | Compilación |
|---|---|---|
| **vdp-router** | Enruta mensajes VDP entre procesos/nodos | AOT |
| **fs-server** | Sistema de archivos VexFS | AOT |
| **net-stack** | TCP/IP + VDP remoto | JIT hot path |
| **solid-engine** | Servidor gráfico + compositor | JIT |
| **vdp-dns** | Resolución de nombres VDP | AOT |
| **logd** | Demonio de logs | AOT |
| **vexpm-daemon** | Gestor de paquetes | JIT |

---

## 8. Programa de usuario mínimo

```rust
// hello.vex — "Hello, World" en VexOS
use vexos::io;
use vexos::vdp;

fn main() -> ExitCode {
    io::println("¡Hola desde VexOS!");

    let msg = vdp::Message::new(
        "node://local/app/hello",
        "node://local/logd",
        &"Hello logged from VexOS!",
    );
    vdp::send(&msg).unwrap();

    ExitCode::SUCCESS
}
```

Compilación y ejecución:

```bash
vex build --release --target=x86_64-vexos hello.vex
vexpm run hello
```
