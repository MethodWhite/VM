# Photonic AGI Laser System — RP2350 Firmware

## Overview

This firmware runs on a Raspberry Pi Pico 2 (RP2350) and provides the
real-time hardware interface for the photonic AGI laser system.  It
communicates with **VestaVM** over UART and directly controls five
laser diodes with PWM via PIO state machines, reads self-mixing
interferometry feedback from an ADC, and monitors a quartz crystal
temperature sensor.

## Hardware Pin Map

| Signal               | GPIO | Notes                          |
|----------------------|------|--------------------------------|
| Laser R              | 2    | PIO0 SM0                       |
| Laser G              | 3    | PIO0 SM1                       |
| Laser B              | 4    | PIO0 SM2                       |
| Laser IR             | 5    | PIO0 SM3                       |
| Laser UV             | 6    | PIO1 SM0                       |
| UART TX (to host)    | 0    | Hardware UART0                 |
| UART RX (from host)  | 7    | PIO1 SM1 (PIO UART receiver)   |
| ADC – feedback       | 26   | ADC0 (self-mixing photodiode)  |
| ADC – crystal temp   | 27   | ADC1 (quartz thermistor)       |

## UART Protocol

**Baud rate:** 115200 8N1

### Command format (host → device)

```
Byte 0        : command code
Bytes 1–4     : payload (32-bit, little-endian)
```

### Response format (device → host)

```
Bytes 0–3     : 32-bit value (little-endian)
```

### Commands

| Code | Name            | Payload                           | Response                    |
|------|-----------------|-----------------------------------|-----------------------------|
| 0x01 | `EMIT`          | [15:0] freq (0.1 nm), [31:16] duty cycle (ns) | 0 = OK, 0xFFFFFFFF = error |
| 0x02 | `READ_FEEDBACK` | ignored                           | feedback voltage in mV      |
| 0x03 | `RESONANCE`     | reserved                          | current feedback mV         |
| 0x04 | `LASER_OFF`     | byte 0 = laser index (0–4)       | 0 = OK, 0xFFFFFFFE = error |
| 0x05 | `SET_TEMP`      | target temperature in millidegrees C | current temperature in m°C |

### Laser Selection

The EMIT command selects the laser whose wavelength range contains
the requested frequency.  If multiple lasers match, the one whose
centre wavelength is closest to the request is chosen.

| Laser | Wavelength (nm) | Freq range (0.1 nm) |
|-------|-----------------|---------------------|
| R     | ~680            | 6200 – 7500         |
| G     | ~532            | 4950 – 5700         |
| B     | ~473            | 4500 – 4950         |
| IR    | ~808            | 7000 – 10000        |
| UV    | ~355            | 1000 – 4000         |

## PIO Programs

### `photonic_laser.pio`

Each laser is driven by a dedicated PIO state machine.  A single
32-bit FIFO word encodes both the on-time and off-time:

- `[15:0]`  = on_cycles (0 = skip, minimum non-zero pulse = 3 sysclk cycles)
- `[31:16]` = off_cycles (0 = skip, minimum gap = 3 sysclk cycles)

The SM uses `pull noblock` so it continues with the last parameters
indefinitely until a new word arrives — this guarantees glitch-free
PWM updates.

Synchronised firing is achieved by disabling all SMs, preloading their
FIFOs, then re-enabling them in rapid succession.

### `photonic_uart.pio`

A PIO-based UART receiver that samples an asynchronous serial line
(8N1) at 8× oversampling.  Each received byte is pushed into the
RX FIFO and immediately transferred to a 256-byte ring buffer via
DMA (ring mode).

Flow control is handled in software: when the ring buffer reaches
a high-water mark, the firmware transmits XOFF; when it drains,
it transmits XON.

## Build Instructions

```bash
# Set the Pico SDK path
export PICO_SDK_PATH=/path/to/pico-sdk

# Configure
cmake -B build -S .

# Build
cmake --build build

# Flash (copy UF2 to Pico 2)
cp build/photonic_controller.uf2 /media/$USER/RPI-RP2/
```

## Files

| File                    | Description                              |
|-------------------------|------------------------------------------|
| `photonic_controller.c` | Main firmware entry point and protocol   |
| `photonic_controller.h` | Header with constants and declarations   |
| `photonic_laser.pio`    | PIO program for laser PWM control        |
| `photonic_uart.pio`     | PIO program for UART reception           |
| `CMakeLists.txt`        | CMake build configuration                |
| `pico_sdk_import.cmake` | Pico SDK import helper                   |
| `README.md`             | This file                                |

## License

Proprietary — VestaVM / anomalyco
