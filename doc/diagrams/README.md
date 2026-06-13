# VestaVM — Diagramas de Arquitectura

## 1. Arquitectura general del ecosistema

```mermaid
flowchart TB
    subgraph VestaVM["🔥 VestaVM"]
        Vex["Vex Language"] --> Compiler["Compiler SSA IR → Bytecode"]
        Compiler --> VM["Virtual Machine Interpreter ~340 MIPS"]
        VM --> JIT["JIT C1 20-60x speedup"]
        VM --> GC["Generational GC"]
        VM --> Dist["Distributed Runtime VDP"]
    end

    subgraph Photonic["💡 Photonic AGI System"]
        Laser["Laser Matrix RGBI 650/532/450/850/405nm"]
        Laser --> Crystal["Quartz Crystal n=1.544"]
        Crystal --> Interferometry["Self-Mixing Interferometry"]
        Interferometry --> Resonance["Standing Wave Detection 0-1000"]
    end

    subgraph Materia["🧠 M.A.T.E.R.I.A."]
        BaseMateria["BaseMateria JEPA Embedding Engine"]
        BaseMateria --> Predict["Predictive Projection"]
        BaseMateria --> History["Temporal Context Buffer"]
        Predict --> Validate["Resonance Validation"]
    end

    VestaVM <-->|"FFI (.so)"| Photonic
    VestaVM <-->|"FFI (.so)"| Materia
    Photonic <-->|"UTF-32 Protocol"| Materia
```

## 2. Pipeline del compilador Vex

```mermaid
flowchart TB
    Source[".vex Source Code"] --> LexParse["Pass 1: Lex + Parse"]
    LexParse --> TypeCheck["Pass 2: TypeCheck + Namespace Flatten"]
    TypeCheck --> Lower["Pass 3: Lowering AST → SSA IR"]
    Lower --> Optimize["Pass 4: Optimize IR"]
    Optimize --> Emit["Pass 5: Emit .vel bytecode"]
    Emit --> Assembler["Assembler → .velb"]
    Assembler --> Linker["Linker → merged .velb"]
```

## 3. Sistema Fotónico AGI

```mermaid
flowchart TB
    subgraph Controller["Controlador"]
        MCU["RP2350 / ESP32-S3"]
        VexVM["VestaVM"] -->|"FFI"| Plugin["vesta_photonic.so"]
    end
    subgraph Optics["Sistema Óptico"]
        LaserMatrix["Matriz Láseres RGBI+UV"]
        LaserMatrix --> Crystal["Prisma Cuarzo n=1.544"]
        Crystal --> Mirror["Espejo Alta Reflectividad"]
        Mirror --> Crystal
    end
    subgraph Detection["Self-Mixing"]
        Crystal -->|"Feedback"| LaserFeedback["Cambio voltaje láser"]
        LaserFeedback --> ADC["ADC"] --> Plugin
    end
```

## 4. BaseMateria + JEPA

```mermaid
flowchart TB
    Init["init(config)"] --> Embed["embed(data) → latente"]
    Embed --> Latent["current_embedding[]"]
    Latent --> Predict["predict(n) → values"]
    Latent --> Resonance["resonance() → 0-1000"]
    Latent --> Encode["encode_photonic(idx) → u32"]
    History["History Buffer"] --> Embed
```

## 5. Integración: Fotónico + M.A.T.E.R.I.A. + Vex

```mermaid
sequenceDiagram
    participant V as Vex/VM
    participant BM as BaseMateria
    participant PH as Photonic
    participant HW as Hardware

    V->>BM: embed(data)
    BM-->>V: latent dim
    V->>BM: predict(14)
    BM-->>V: prediction[]
    V->>PH: encode_photonic(i)
    PH-->>V: UTF-32 code
    V->>PH: photonic_emit(code)
    PH->>HW: pulso láser
    HW->>HW: self-mixing feedback
    HW-->>PH: mV
    PH-->>V: feedback
    V->>BM: resonance()
    BM-->>V: 0-1000
```
