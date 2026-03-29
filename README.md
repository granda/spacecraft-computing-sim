# Spacecraft Computing Simulator

A hands-on learning project for spacecraft computer systems — real-time operating systems, embedded C, and delay-tolerant networking — built entirely in virtual environments using QEMU.

No physical hardware required.

## Roadmap

### Bare Metal

Set up an ARM cross-compilation environment and write bare-metal C targeting an emulated Cortex-M3 (MPS2-AN385). Software talks directly to hardware registers without an OS.

- [x] UART output via memory-mapped I/O
- [x] Interrupt handlers (SysTick timer)

[Code and docs](bare-metal/)

### FreeRTOS

Clone FreeRTOS and run it on the same QEMU Cortex-M3 target. Learn RTOS fundamentals through progressive exercises.

- [x] Two tasks printing to UART at different rates
- [x] Share data between tasks via a FreeRTOS queue
- [x] Watchdog timer that resets if a task hangs
- [x] Sensor-reading pipeline with priority-based scheduling
- [x] Trigger and resolve a priority inversion scenario

### DTN (Delay-Tolerant Networking)

Build NASA JPL's delay-tolerant networking stack and simulate space-like network conditions.

- [x] Two-node DTN network in Docker containers
- [x] Simulated degraded links with `tc netem` (latency, packet loss, intermittent connectivity)
- [x] Bundle transfer over degraded link (sustained throughput, larger payloads)
- [x] File transfer using CFDP
- [ ] Contact-graph routing with scheduled link windows

### Integration

FreeRTOS "spacecraft" sends telemetry over DTN to a "ground station" — a miniature version of real mission architecture.

- [ ] FreeRTOS collects fake sensor data in QEMU
- [ ] ION DTN node acts as ground station in Docker
- [ ] UART bridge: telemetry flows from QEMU to ION via host script
- [ ] Artificial Mars-distance delays on the virtual network

### Blog

- [ ] Document each module on Hugo blog
- [ ] Angle: "Building a spacecraft computer simulator at home with Claude Code"

## Quick Start

```bash
sudo apt install gcc-arm-none-eabi qemu-system-arm build-essential
make -C bare-metal run  # bare-metal: build and run in QEMU (Ctrl-A, X to exit)
make -C freertos run    # FreeRTOS: sensor pipeline demo
make -C freertos test   # run integration tests (7 assertions)
make -C dtn test        # DTN: build ION, run two-node network, 6 tests
make -C dtn test-degraded  # DTN: degraded links (latency, loss, intermittent), 3 tests
make -C dtn test-throughput  # DTN: larger payloads (10-500 KB) over degraded links
make -C dtn test-cfdp      # DTN: CFDP file transfer (1-100 KB, integrity checks)
```

## Key Concepts

| Domain | What | Why It Matters |
|--------|------|----------------|
| RTOS | Deterministic scheduling, priority preemption, queues, mutexes | Tasks must execute within guaranteed time windows |
| Spacecraft | Radiation hardening, TMR, ECC, strict C standards (JPL/MISRA) | Cosmic rays flip bits; software must be bulletproof |
| DTN | Store-and-forward, Bundle Protocol, contact-graph routing | TCP/IP breaks in space; DTN handles latency and link disruption |

## Reading List

- "Digital Apollo" by David Mindell — Apollo Guidance Computer history
- "Mastering the FreeRTOS Real Time Kernel" by Richard Barry — [free PDF](https://www.freertos.org/Documentation/RTOS_book.html)
- JPL "Power of 10" coding rules — safety-critical C style guide
- RFC 9171 — Bundle Protocol specification
- [NASA cFS](https://github.com/nasa/cFS) — open-source flight software framework
- [ION DTN docs](https://ion-dtn.readthedocs.io/)
