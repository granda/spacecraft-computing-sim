# Bare Metal: C on QEMU

## Quick Start

### Prerequisites

```bash
sudo apt install gcc-arm-none-eabi qemu-system-arm build-essential
```

### Build and Run

```bash
make        # build firmware
make run    # run in QEMU (Ctrl-A, X to exit)
make test   # run integration tests
```

### Expected Output

```
SysTick interrupt demo
======================
Ticking at 2 Hz for 5 seconds (10 ticks)...

  tick 1
  tick 2
  # (ticks 3-9 omitted)
  tick 10

Done — 5 seconds counted by interrupt.
```

## Project Structure

```
startup.c   — Vector table, Reset_Handler, C runtime init (.data copy, .bss zero)
main.c      — CMSDK UART0 driver and application entry point
mps2.ld     — Linker script (flash at 0x0, RAM at 0x20000000)
Makefile    — Build, run, and test targets
```

## Diagrams

- [Boot Flow](docs/boot-flow.md) — how the CPU gets from power-on to `main()`

## How It Works

This is bare-metal C — no operating system, no standard library. The firmware:

1. **Boots** — CPU reads the vector table at address 0x0, loads the stack pointer and jumps to `Reset_Handler`
2. **Initializes** — copies `.data` from flash to RAM, zeroes `.bss`
3. **Prints** — writes bytes directly to the UART0 data register at `0x40004000`

The QEMU MPS2-AN385 machine emulates an ARM Cortex-M3 with a CMSDK APB UART, so serial output appears directly in your terminal.

## Toolchain

| Tool | Purpose |
|------|---------|
| `arm-none-eabi-gcc` | ARM cross-compiler (Cortex-M3, Thumb) |
| `qemu-system-arm` | ARM system emulator (MPS2-AN385 board) |
| `make` | Build system |

## Build Flags

- `-mcpu=cortex-m3 -mthumb` — target Cortex-M3 in Thumb mode
- `-ffreestanding` — no hosted environment assumptions
- `-nostdlib` — no standard library linking
- `-Os` — optimize for size (standard for embedded)
- `-ffunction-sections -fdata-sections` + `--gc-sections` — dead code elimination
- `-Wall -Wextra -Wpedantic` — strict warnings
