# Phase 2: FreeRTOS on QEMU

Two FreeRTOS tasks communicating via a queue on the same QEMU MPS2-AN385 (Cortex-M3) target from Phase 1 — now with an RTOS managing task scheduling.

## Architecture

```
+-------------+       +-----------+       +---------------+
| Sensor Task |  -->  |   Queue   |  -->  | Telemetry Task|  -->  UART
| (pri 1)     |       | (5 slots) |       | (pri 2)       |
| 500ms cycle |       |           |       | blocks on rx  |
+-------------+       +-----------+       +---------------+
```

- **Sensor task** (priority 1): wakes every 500ms via `vTaskDelayUntil`, generates a fake temperature reading, sends it to the queue (non-blocking)
- **Telemetry task** (priority 2): blocks on the queue, prints each reading over UART as it arrives
- Higher priority means the telemetry task runs immediately when data is available, preempting the sensor task

## Quick Start

```bash
# First time: clone with submodules
git clone --recurse-submodules https://github.com/granda/spacecraft-sim.git

# Or if already cloned:
git submodule update --init --recursive

# Build and run
make -C phase2 run    # Ctrl-A, X to exit QEMU
make -C phase2 test   # 3 integration tests
```

## Output

```
FreeRTOS Spacecraft Telemetry Demo
==================================
[TELEMETRY] Task started, waiting for sensor data...
[TELEMETRY] Sensor 1: 20.00 C at tick 500
[TELEMETRY] Sensor 1: 20.01 C at tick 1000
[TELEMETRY] Sensor 1: 20.02 C at tick 1500
```

## Key Concepts

### What changed from Phase 1

In Phase 1, `main()` runs sequentially — one thread of execution, no concurrency. Here, `main()` creates tasks and starts the FreeRTOS scheduler. From that point, FreeRTOS owns the CPU and switches between tasks based on priority and timing.

### vTaskDelayUntil vs vTaskDelay

`vTaskDelay(500)` sleeps for 500ms *after* the task finishes its work — so the actual period is 500ms + work time, and it drifts. `vTaskDelayUntil` wakes at absolute tick intervals regardless of work time. That's why the ticks in the output are exactly 500 apart.

### Queues

Tasks don't share memory directly — they communicate through FreeRTOS queues. The sensor task puts a `SensorReading_t` struct into the queue; the telemetry task blocks until one arrives. This decouples the producer from the consumer and is the standard RTOS inter-task communication pattern.

### Non-blocking sends

The sensor task uses a zero-timeout queue send. If the queue is full, it drops the reading and logs it rather than blocking — this preserves the 500ms timing guarantee. A blocked send could cause the task to miss its next deadline.

## Project Structure

```
main.c            — Application: tasks, queue, UART init, FreeRTOS hooks
FreeRTOSConfig.h  — RTOS configuration (tick rate, stack sizes, features)
startup_gcc.c     — Vector table, Reset_Handler with .data/.bss init (from FreeRTOS demo, customized)
printf-stdarg.c   — Lightweight printf for UART output (from FreeRTOS demo, customized)
mps2_m3.ld        — Linker script (from FreeRTOS demo, customized)
Makefile           — Build system
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
