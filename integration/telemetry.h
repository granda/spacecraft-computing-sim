/*
 * Telemetry format definitions for the integration firmware.
 *
 * Defines sensor IDs, sampling rates, calibration parameters, and the
 * structured CSV telemetry format used for UART output.  The format is
 * designed to be trivially parseable by the Python bridge script that
 * will forward telemetry to the DTN ground station.
 */

#ifndef TELEMETRY_H
#define TELEMETRY_H

#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"

/* ── Sensor IDs ────────────────────────────────────────────── */

typedef enum {
    SENSOR_TEMP = 1,
    SENSOR_GYRO = 2,
    SENSOR_BATT = 3,
    SENSOR_SUN  = 4,
    SENSOR_ID_BOUND     /* past-the-end for array sizing (IDs are 1-based) */
} SensorId_t;

/* ── Sampling periods (ms) ─────────────────────────────────── */

#define TEMP_PERIOD_MS  1000  /* 1 Hz   — housekeeping */
#define GYRO_PERIOD_MS  100   /* 10 Hz  — attitude control */
#define BATT_PERIOD_MS  2000  /* 0.5 Hz — power subsystem */
#define SUN_PERIOD_MS   500   /* 2 Hz   — solar array current */

/* ── Task priorities ───────────────────────────────────────── */

#define HOUSEKEEPING_PRIORITY    (tskIDLE_PRIORITY + 1)  /* TEMP, BATT, SUN */
#define TELEMETRY_TASK_PRIORITY  (tskIDLE_PRIORITY + 2)
#define PROCESSOR_TASK_PRIORITY  (tskIDLE_PRIORITY + 3)
#define GYRO_TASK_PRIORITY       (tskIDLE_PRIORITY + 4)

/* ── Calibration parameters ────────────────────────────────── */

#define TEMP_CAL_OFFSET    (-10)  /* centidegrees bias correction */
#define GYRO_SCALE_FACTOR  2      /* raw-to-calibrated multiplier */
#define BATT_CAL_OFFSET    5      /* millivolt bias correction */
/* SUN sensor is passthrough (already calibrated) — no constant needed */

/* ── Telemetry output format ───────────────────────────────── */

/*
 * NMEA-style CSV: $TELEM,seq,sensor,value,tick
 *
 * Example: $TELEM,0001,TEMP,1990,1000
 *
 * Fields:
 *   $TELEM  — frame marker (lets bridge skip non-telemetry lines)
 *   seq     — 4-digit zero-padded (display wraps at 9999 via % 10000;
 *              the underlying uint32_t counter wraps at UINT32_MAX)
 *   sensor  — sensor name: TEMP, GYRO, BATT, SUN
 *   value   — signed calibrated integer
 *   tick    — FreeRTOS tick count at time of reading
 */
/* TODO: add XOR checksum field (*XX) before bridge milestone —
 * QEMU UART is error-free but DTN forwarding could corrupt values. */
/* %04u and % 10000 are intentionally paired: the modulo keeps the
 * sequence number in 0-9999 so %04u always produces exactly 4 digits.
 * %d is correct for int32_t on 32-bit ARM (sizeof(int)==4). */
#define TELEM_FMT  "$TELEM,%04u,%s,%d,%u\r\n"

/* ── Data structures ───────────────────────────────────────── */

/* Raw sensor reading — sent from sensors to processor */
typedef struct {
    SensorId_t sensor_id;
    int32_t    raw_value;
    uint32_t   timestamp;
} RawReading_t;

/* Processed reading — sent from processor to telemetry */
typedef struct {
    SensorId_t sensor_id;
    int32_t    value;       /* calibrated */
    uint32_t   timestamp;
    uint32_t   seq;
} ProcessedReading_t;

#endif /* TELEMETRY_H */
