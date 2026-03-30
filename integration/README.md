# Integration: Spacecraft Telemetry

Continuous telemetry firmware for MPS2-AN385 (Cortex-M3) on QEMU — six FreeRTOS tasks modeling a spacecraft that collects sensor data and outputs structured telemetry over UART.  This is the first piece of the integration pipeline that will eventually bridge FreeRTOS telemetry to a DTN ground station.

## Architecture

```
Temp Sensor  (pri 1, 1 Hz)   ──┐
Batt Sensor  (pri 1, 0.5 Hz) ──┤
Sun Sensor   (pri 1, 2 Hz)   ──┼──> Raw Queue (30) ──> Processor (pri 3) ──> Telemetry Queue (10) ──> Telemetry (pri 2) ──> UART
Gyro Sensor  (pri 4, 10 Hz)  ──┘
```

## Sensors

| Sensor | ID | Rate | Priority | Raw Range | Calibration | Unit |
|--------|-----|------|----------|-----------|-------------|------|
| TEMP | 1 | 1 Hz | 1 | 2000-2499 | offset -10 | centidegrees C |
| GYRO | 2 | 10 Hz | 4 | 150-199 | scale x2 | millideg/s |
| BATT | 3 | 0.5 Hz | 1 | 2800-2849 | offset +5 | millivolts |
| SUN | 4 | 2 Hz | 1 | 800-899 | passthrough | milliamps (solar array current) |

## Telemetry Format

NMEA-style CSV lines parseable by `line.split(",")` in Python:

```
$TELEM,0001,TEMP,1990,1000
$TELEM,0002,GYRO,320,1100
$TELEM,0003,BATT,2805,2000
$TELEM,0004,SUN,850,1500
```

| Field | Description |
|-------|-------------|
| `$TELEM` | Frame marker — lets bridge script skip non-telemetry lines |
| seq | 4-digit zero-padded sequence number (wraps at 9999) |
| sensor | Sensor name: TEMP, GYRO, BATT, SUN |
| value | Signed calibrated integer |
| tick | FreeRTOS tick count at time of reading |

Status lines are prefixed with `#` and can be ignored by downstream parsers.

**Sequence wrap**: The seq field rolls from `9999` to `0000` and is indistinguishable from a firmware restart.  The bridge script (next milestone) should use the tick field (monotonically increasing) to detect restarts vs normal wraps.

## DTN Ground Station

Two ION DTN nodes in Docker form the communication link between the spacecraft and ground station.  The spacecraft node will receive telemetry from the UART bridge (next milestone) and forward it over a DTN link to the ground station.

```
QEMU (FreeRTOS)                    Docker (integration-net)
┌──────────────┐      ┌──────────────────┐      ┌───────────────────┐
│  Firmware     │      │  ION spacecraft  │      │  ION ground-station│
│  UART output  │─ ─ ─>│  ipn:1.*         │─────>│  ipn:2.*           │
│              │bridge │  (node 1)        │ LTP  │  (node 2)          │
└──────────────┘(TODO) └──────────────────┘      └───────────────────┘
```

### Endpoints

| Service | Spacecraft | Ground Station | Purpose |
|---------|------------|----------------|---------|
| 0 | ipn:1.0 | ipn:2.0 | Control (discard) |
| 1 | ipn:1.1 | ipn:2.1 | bpecho / ping |
| 2 | ipn:1.2 | ipn:2.2 | File transfer |
| 3 | ipn:1.3 | ipn:2.3 | **Telemetry** |
| 64/65 | ipn:1.64/65 | ipn:2.64/65 | CFDP |

### DTN link parameters

- LTP over UDP on Docker bridge network
- 100 KB/s bandwidth, 1s one-way light time
- 24-hour contact window (resets on container restart)
- BPSec enabled but no keys configured — bundles pass unsigned (local testing only)

## Build and Run

Depends on the `freertos/` directory for board support files and the FreeRTOS kernel submodule.  Ground station requires Docker and reuses the ION image from `../dtn/`.

```bash
# Firmware
make              # build firmware
make run          # run in QEMU (Ctrl-A, X to exit)
make test         # 15 firmware assertions (15s capture)
make clean

# Ground station
make ground-station-up      # start spacecraft + ground-station containers
make ground-station-down    # stop containers
make test-ground-station    # 8 DTN assertions (ION health, bping, bundle delivery)
make test-ground-station ARGS=--keep-on-failure  # leave containers up on failure
make clean-ground-station   # tear down containers, images, volumes
```
