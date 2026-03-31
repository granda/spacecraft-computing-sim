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

**Sequence wrap**: The seq field rolls from `9999` to `0000` and is indistinguishable from a firmware restart.  A future enhancement could use the tick field (monotonically increasing) to detect restarts vs normal wraps — not implemented in the current bridge since the bridge manages the QEMU lifecycle directly.

## DTN Ground Station

Two ION DTN nodes in Docker form the communication link between the spacecraft and ground station.  The spacecraft node receives telemetry from the UART bridge and forwards it over a DTN link to the ground station.

```
QEMU (FreeRTOS)          Host Python           Docker (integration-net)
┌──────────────┐    ┌──────────────────┐    ┌────────────┐    ┌─────────────────┐
│ firmware.elf │    │  uart_bridge.py  │    │ spacecraft │    │ ground-station  │
│ UART output  │───>│  TCP :4321       │───>│ ipn:1.3    │───>│ ipn:2.3         │
│              │sock│  batch + inject  │exec│            │LTP │                 │
└──────────────┘    └──────────────────┘    └────────────┘    └─────────────────┘
```

### UART Bridge

`uart_bridge.py` runs on the host and connects the two subsystems:

1. Launches QEMU with UART redirected to a TCP socket (`-chardev socket,server=on,wait=off`)
2. Connects to the socket and reads telemetry lines (filters for `$TELEM,*`)
3. Batches lines (2s interval or 30-line cap) and injects each batch as a DTN bundle via `docker compose exec` into the spacecraft container (`ipn:1.3` -> `ipn:2.3`)
4. Ground station receives bundles over LTP

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
- 100 KB/s bandwidth, 1s one-way light time (5s in Mars-delay mode)
- 24-hour contact window (resets on container restart)
- BPSec enabled but no keys configured — bundles pass unsigned (local testing only)

### Mars-Distance Delay Simulation

Simulates realistic Mars communication delays using two synchronized mechanisms:

1. **ION OWLT** (`node_delay.rc`): Range tables set 5s one-way light time so ION's contact-graph routing and LTP retransmission timers account for the delay.
2. **tc netem** (applied at test time): `tc qdisc add dev eth0 root netem delay 5000ms` on the spacecraft container adds actual network latency to match.

Both must agree — if tc netem delay is 5s but ION thinks OWLT is 1s, LTP will retransmit aggressively and waste bandwidth.

Real Mars OWLT ranges from ~4 minutes (closest approach) to ~24 minutes (conjunction). The 5s test value keeps CI fast while being long enough to be unambiguously measurable.

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

# UART bridge (end-to-end)
make bridge                 # run bridge: QEMU -> DTN (Ctrl-C to stop)
make test-bridge            # 11 assertions (QEMU -> bridge -> spacecraft -> ground station)
make test-bridge ARGS=--keep-on-failure

# Mars-delay (5s OWLT + tc netem)
make test-mars-delay        # 7 assertions (telemetry delivery under simulated Mars delay)
make test-mars-delay ARGS=--keep-on-failure
```
