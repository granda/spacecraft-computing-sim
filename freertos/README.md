# FreeRTOS on QEMU

A sensor-to-telemetry pipeline with priority-based scheduling on QEMU MPS2-AN385 (Cortex-M3) — four FreeRTOS tasks modeling how a real spacecraft processes sensor data.

## Architecture

```
Temp Sensor (pri 1, 1 Hz)  ──┐              ┌──> Telemetry (pri 2)
                             ├──> Raw Queue ├──> Processor (pri 3) ──> Telemetry Queue
Gyro Sensor (pri 4, 10 Hz) ──┘
```

- **Gyro sensor** (priority 4, 10 Hz): attitude control — highest priority, highest rate. Preempts everything except interrupts.
- **Processor** (priority 3): reads raw queue, applies calibration (`TEMP_CAL_OFFSET`, `GYRO_SCALE_FACTOR`), forwards to telemetry queue. Blocking send with 100ms timeout prevents overflow. Tracks drop count.
- **Telemetry** (priority 2): formats and prints processed readings over UART.
- **Temp sensor** (priority 1, 1 Hz): housekeeping — lowest priority, lowest rate. Can be preempted by everything.

Shutdown uses a sentinel value in the telemetry queue so all readings are printed before halt.

## Quick Start

```bash
# First time: clone with submodules
git clone --recurse-submodules https://github.com/granda/spacecraft-sim.git

# Or if already cloned:
git submodule update --init --recursive

# Build and run
make -C freertos run    # Ctrl-A, X to exit QEMU
make -C freertos test   # 7 integration tests
```

## Output

```
FreeRTOS Sensor Pipeline Demo
=============================

[GYRO]  Sensor online (10 Hz, priority 4)
[PROC]  Processor online (priority 3)
[TELEM] Telemetry online (priority 2)
[TEMP]  Sensor online (1 Hz, priority 1)
[TELEM] #000 GYRO: 300 at tick 100
[TELEM] #001 GYRO: 302 at tick 200
...
[TELEM] #010 TEMP: 1991 at tick 1001    ← temp reading between gyro bursts
...
[PROC]  === Pipeline complete: 30 processed, 0 dropped ===
[TELEM] All readings transmitted
```

## Key Concepts

### Priority-based preemption

The gyro sensor runs at 10 Hz and priority 4 — it will always preempt the processor (3), telemetry (2), and temp sensor (1). This mirrors real spacecraft where attitude control sensors are more time-critical than housekeeping sensors.

### vTaskDelayUntil vs vTaskDelay

`vTaskDelay(100)` sleeps for 100ms *after* the task finishes its work — so the actual period drifts. `vTaskDelayUntil` wakes at absolute tick intervals regardless of work time. Both sensor tasks use this for drift-resistant scheduling.

### Non-blocking sensor sends

Sensor tasks use zero-timeout queue sends. If the raw queue is full, they drop the reading and log it rather than blocking — this preserves timing guarantees. A blocked send could cause a sensor to miss its next deadline.

### Blocking processor sends

The processor uses a 100ms timeout when sending to the telemetry queue. This gives the lower-priority telemetry task time to drain without dropping readings unnecessarily. Drops are counted and reported at shutdown.

### Sentinel-based shutdown

`taskYIELD()` only yields to equal-or-higher priority tasks, so it can't hand control to the lower-priority telemetry task. Instead, the processor sends a sentinel value through the telemetry queue and suspends. The telemetry task processes all real readings, then halts when it sees the sentinel.

### Thread-safe output

All task-context UART output uses `safe_printf`, which wraps `printf` in a mutex. Fatal error hooks (malloc failed, stack overflow) use raw `printf` since the mutex may be held.

## Project Structure

```
main.c            — Application: tasks, queues, calibration, UART init, FreeRTOS hooks
FreeRTOSConfig.h  — RTOS configuration (tick rate, stack sizes, features)
startup_gcc.c     — Vector table, Reset_Handler with .data/.bss init
printf-stdarg.c   — Lightweight printf for UART output
mps2_m3.ld        — Linker script
board.h           — MPS2-AN385 UART register definitions
Makefile           — Build system with QEMU integration tests
freertos-kernel/   — Git submodule → FreeRTOS/FreeRTOS-Kernel
```

## FreeRTOS Configuration Highlights

| Setting | Value | Why |
|---------|-------|-----|
| `configTICK_RATE_HZ` | 1000 | 1ms tick resolution |
| `configCHECK_FOR_STACK_OVERFLOW` | 2 | Watermark + fill pattern detection |
| `configUSE_MALLOC_FAILED_HOOK` | 1 | Halt on allocation failure |
| `configUSE_PREEMPTION` | 1 | Higher priority tasks preempt lower ones |
| `configTOTAL_HEAP_SIZE` | 100 KB | FreeRTOS heap for task stacks and queues |
