/*
 * Integration firmware: continuous spacecraft telemetry for MPS2-AN385
 *
 * Runs four sensor tasks that feed a processing pipeline, outputting
 * structured CSV telemetry over UART.  Designed to run indefinitely so
 * the Python bridge script (next milestone) can capture and forward
 * telemetry to the DTN ground station.
 *
 *   Temp Sensor  (pri 1, 1 Hz)   ──┐
 *   Batt Sensor  (pri 1, 0.5 Hz) ──┤
 *   Sun Sensor   (pri 1, 2 Hz)   ──┼──> Processor (pri 3) ──> Telemetry (pri 2)
 *   Gyro Sensor  (pri 4, 10 Hz)  ──┘
 *
 * Output format: $TELEM,seq,sensor,value,tick  (see telemetry.h)
 *
 * Note: FreeRTOSConfig.h defines INCLUDE_vTaskDelayUntil (old API name).
 * FreeRTOS V202212 maps both old and new names, so xTaskDelayUntil works.
 * If the config is migrated to INCLUDE_xTaskDelayUntil, the old key can
 * be removed.
 */

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

#include <stdio.h>
#include <stdint.h>

#include "board.h"
#include "telemetry.h"

/* Queues */
static QueueHandle_t xRawQueue = NULL;       /* sensors -> processor */
static QueueHandle_t xTelemetryQueue = NULL; /* processor -> telemetry */

/* Per-sensor drop counters — incremented locklessly from sensor tasks,
 * read by the processor task which prints periodic summaries.
 * Note: ++ is a read-modify-write (ldr/add/str) — not atomic even on
 * single-core CM3 (SysTick can interrupt between instructions).  This
 * is acceptable because these are diagnostic-only: a torn read or
 * dropped increment just means an occasional miscounted drop in the
 * summary; pipeline correctness is unaffected.  Do NOT use for anything
 * safety-critical. */
static volatile uint32_t ulDrops[SENSOR_ID_BOUND];  /* index 0 unused; sensor IDs are 1-based */

/* Telemetry queue full counter — incremented by processor task */
static volatile uint32_t ulTelemDrops;

/* Per-sensor deadline overrun counters — xTaskDelayUntil returns pdFALSE
 * when the task was already past its wake time (execution overran period).
 * Diagnostic-only, same torn-increment caveat as ulDrops. */
static volatile uint32_t ulOverruns[SENSOR_ID_BOUND];  /* [0] unused */

/* Print drop summary every N processed readings (~7s at ~13 readings/s) */
#define DROP_REPORT_INTERVAL  100

static SemaphoreHandle_t xPrintMutex = NULL;

/* Thread-safe printf — not ISR-safe, only call from task context.
 * WARNING: do not use safe_printf inside sensor for(;;) loops.
 * The mutex blocks the caller, so a high-priority task (e.g. GYRO)
 * would stall behind a lower-priority task holding the lock.
 * Priority inheritance bounds the delay but does not eliminate jitter.
 * One-shot startup messages are acceptable; hot-path logging is not. */
#define safe_printf(...)                                \
    do {                                                \
        xSemaphoreTake(xPrintMutex, portMAX_DELAY);     \
        printf(__VA_ARGS__);                            \
        xSemaphoreGive(xPrintMutex);                    \
    } while (0)

/*-----------------------------------------------------------*/

static void prvUARTInit(void)
{
    /* 25 MHz APB clock / 115200 baud = ~217 */
    UART0_BAUDDIV = 217;
    UART0_CTRL = 1;
}

/*-----------------------------------------------------------*/

/* Sensor name lookup for telemetry output */
static const char *prvSensorName(SensorId_t xId)
{
    switch (xId) {
        case SENSOR_TEMP: return "TEMP";
        case SENSOR_GYRO: return "GYRO";
        case SENSOR_BATT: return "BATT";
        case SENSOR_SUN:  return "SUN";
        default:          return "UNKN";
    }
}

/*-----------------------------------------------------------*/

/*
 * Temperature sensor: 1 Hz housekeeping.
 * Simulates spacecraft thermal readings (20-25 C range).
 */
static void prvTempSensorTask(void *pvParameters)
{
    (void)pvParameters;
    TickType_t xLastWakeTime = xTaskGetTickCount();
    RawReading_t xReading;

    safe_printf("# TEMP sensor online (1 Hz, priority %u)\r\n",
                (unsigned)uxTaskPriorityGet(NULL));

    for (;;) {
        if (xTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(TEMP_PERIOD_MS)) == pdFALSE) {
            ulOverruns[SENSOR_TEMP]++;
        }

        xReading.sensor_id = SENSOR_TEMP;
        xReading.raw_value = 2000 + (xLastWakeTime % 500);  /* centidegrees */
        xReading.timestamp = xLastWakeTime;

        if (xQueueSend(xRawQueue, &xReading, 0) != pdPASS) {
            ulDrops[SENSOR_TEMP]++;
        }
    }
}

/*-----------------------------------------------------------*/

/*
 * Gyroscope sensor: 10 Hz attitude control.
 * Highest priority — preempts all other tasks.
 */
static void prvGyroSensorTask(void *pvParameters)
{
    (void)pvParameters;
    TickType_t xLastWakeTime = xTaskGetTickCount();
    RawReading_t xReading;

    safe_printf("# GYRO sensor online (10 Hz, priority %u)\r\n",
                (unsigned)uxTaskPriorityGet(NULL));

    for (;;) {
        if (xTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(GYRO_PERIOD_MS)) == pdFALSE) {
            ulOverruns[SENSOR_GYRO]++;
        }

        xReading.sensor_id = SENSOR_GYRO;
        xReading.raw_value = 150 + (xLastWakeTime % 50);  /* millideg/s */
        xReading.timestamp = xLastWakeTime;

        if (xQueueSend(xRawQueue, &xReading, 0) != pdPASS) {
            ulDrops[SENSOR_GYRO]++;
        }
    }
}

/*-----------------------------------------------------------*/

/*
 * Battery voltage sensor: 0.5 Hz power subsystem.
 * Simulates a slowly varying Li-ion battery (2.8-2.85 V).
 */
static void prvBattSensorTask(void *pvParameters)
{
    (void)pvParameters;
    TickType_t xLastWakeTime = xTaskGetTickCount();
    RawReading_t xReading;

    safe_printf("# BATT sensor online (0.5 Hz, priority %u)\r\n",
                (unsigned)uxTaskPriorityGet(NULL));

    for (;;) {
        if (xTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(BATT_PERIOD_MS)) == pdFALSE) {
            ulOverruns[SENSOR_BATT]++;
        }

        xReading.sensor_id = SENSOR_BATT;
        xReading.raw_value = 2800 + (xLastWakeTime % 50);  /* millivolts */
        xReading.timestamp = xLastWakeTime;

        if (xQueueSend(xRawQueue, &xReading, 0) != pdPASS) {
            ulDrops[SENSOR_BATT]++;
        }
    }
}

/*-----------------------------------------------------------*/

/*
 * Solar array current sensor: 2 Hz power generation monitoring.
 * Simulates solar array output current (0.8-0.9 A).
 * Named SUN for brevity in telemetry output — this is a current
 * monitor, not a sun/star tracker (distinct instrument).
 */
static void prvSunSensorTask(void *pvParameters)
{
    (void)pvParameters;
    TickType_t xLastWakeTime = xTaskGetTickCount();
    RawReading_t xReading;

    safe_printf("# SUN sensor online (2 Hz, priority %u)\r\n",
                (unsigned)uxTaskPriorityGet(NULL));

    for (;;) {
        if (xTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(SUN_PERIOD_MS)) == pdFALSE) {
            ulOverruns[SENSOR_SUN]++;
        }

        xReading.sensor_id = SENSOR_SUN;
        xReading.raw_value = 800 + (xLastWakeTime % 100);  /* milliamps */
        xReading.timestamp = xLastWakeTime;

        if (xQueueSend(xRawQueue, &xReading, 0) != pdPASS) {
            ulDrops[SENSOR_SUN]++;
        }
    }
}

/*-----------------------------------------------------------*/

/*
 * Processor task: reads raw sensor data, applies calibration,
 * and forwards processed results to the telemetry task.
 */
static void prvProcessorTask(void *pvParameters)
{
    (void)pvParameters;
    RawReading_t xRaw;
    ProcessedReading_t xProcessed;
    /* Wraps at UINT32_MAX (~4.3 billion readings, ~10 years at 13/s).
     * Output uses seq % 10000 so the CSV field stays 4 digits. */
    uint32_t ulSeq = 0;

    safe_printf("# Processor online (priority %u)\r\n",
                (unsigned)uxTaskPriorityGet(NULL));

    for (;;) {
        if (xQueueReceive(xRawQueue, &xRaw,
                          pdMS_TO_TICKS(TEMP_PERIOD_MS * 2)) == pdPASS) {

            xProcessed.sensor_id = xRaw.sensor_id;
            xProcessed.timestamp = xRaw.timestamp;
            xProcessed.seq = ulSeq++;

            /* Periodic drop summary — uses modulo to avoid overflow of an
             * additive threshold.  Processor priority is below GYRO, so
             * safe_printf here does not invert the attitude-control task. */
            if ((ulSeq % DROP_REPORT_INTERVAL) == 0) {
                uint32_t t = ulDrops[SENSOR_TEMP];
                uint32_t g = ulDrops[SENSOR_GYRO];
                uint32_t b = ulDrops[SENSOR_BATT];
                uint32_t s = ulDrops[SENSOR_SUN];
                uint32_t td = ulTelemDrops;
                uint32_t ot = ulOverruns[SENSOR_TEMP];
                uint32_t og = ulOverruns[SENSOR_GYRO];
                uint32_t ob = ulOverruns[SENSOR_BATT];
                uint32_t os = ulOverruns[SENSOR_SUN];
                if (t | g | b | s | td) {
                    safe_printf("# Drops: TEMP=%u GYRO=%u BATT=%u SUN=%u TELEM=%u\r\n",
                                (unsigned)t, (unsigned)g,
                                (unsigned)b, (unsigned)s, (unsigned)td);
                }
                if (ot | og | ob | os) {
                    safe_printf("# Overruns: TEMP=%u GYRO=%u BATT=%u SUN=%u\r\n",
                                (unsigned)ot, (unsigned)og,
                                (unsigned)ob, (unsigned)os);
                }
            }

            switch (xRaw.sensor_id) {
                case SENSOR_TEMP:
                    xProcessed.value = xRaw.raw_value + TEMP_CAL_OFFSET;
                    break;
                case SENSOR_GYRO:
                    /* Max: 199 * 2 = 398, well within int32_t.  If
                     * GYRO_SCALE_FACTOR is raised, verify no overflow. */
                    xProcessed.value = xRaw.raw_value * GYRO_SCALE_FACTOR;
                    break;
                case SENSOR_BATT:
                    xProcessed.value = xRaw.raw_value + BATT_CAL_OFFSET;
                    break;
                case SENSOR_SUN:
                    xProcessed.value = xRaw.raw_value;  /* passthrough */
                    break;
                default:
                    xProcessed.value = xRaw.raw_value;
                    break;
            }

            if (xQueueSend(xTelemetryQueue, &xProcessed,
                           pdMS_TO_TICKS(100)) != pdPASS) {
                ulTelemDrops++;
            }
        }
    }
}

/*-----------------------------------------------------------*/

/*
 * Telemetry task: formats processed readings as $TELEM CSV lines.
 * Runs indefinitely — the bridge script captures this output.
 *
 * TODO: safe_printf here acquires the mutex ~13.5 times/sec.  Benign
 * at current throughput but consider a lock-free ring buffer or
 * dedicated print queue when the bridge milestone adds checksum
 * computation or higher-frequency forwarding.
 */
static void prvTelemetryTask(void *pvParameters)
{
    (void)pvParameters;
    ProcessedReading_t xData;

    safe_printf("# Telemetry online (priority %u)\r\n",
                (unsigned)uxTaskPriorityGet(NULL));

    for (;;) {
        if (xQueueReceive(xTelemetryQueue, &xData,
                          pdMS_TO_TICKS(TEMP_PERIOD_MS * 2)) == pdPASS) {

            safe_printf(TELEM_FMT,
                        (unsigned)(xData.seq % 10000),
                        prvSensorName(xData.sensor_id),
                        (int)xData.value,
                        (unsigned)xData.timestamp);
        }
    }
}

/*-----------------------------------------------------------*/

int main(void)
{
    prvUARTInit();

    /* Raw printf — mutex not yet created, scheduler not running */
    printf("# Spacecraft Telemetry Firmware v1.0\r\n");
    printf("# ===================================\r\n");

    xPrintMutex = xSemaphoreCreateMutex();
    configASSERT(xPrintMutex != NULL);

    /* Raw queue: 4 sensors -> processor (sized for 10 Hz gyro burst) */
    xRawQueue = xQueueCreate(30, sizeof(RawReading_t));
    configASSERT(xRawQueue != NULL);

    /* Telemetry queue: processor -> telemetry */
    xTelemetryQueue = xQueueCreate(10, sizeof(ProcessedReading_t));
    configASSERT(xTelemetryQueue != NULL);

    BaseType_t xResult;

    xResult = xTaskCreate(prvTempSensorTask, "Temp", configMINIMAL_STACK_SIZE * 4,
                          NULL, HOUSEKEEPING_PRIORITY, NULL);
    configASSERT(xResult == pdPASS);

    xResult = xTaskCreate(prvGyroSensorTask, "Gyro", configMINIMAL_STACK_SIZE * 4,
                          NULL, GYRO_TASK_PRIORITY, NULL);
    configASSERT(xResult == pdPASS);

    xResult = xTaskCreate(prvBattSensorTask, "Batt", configMINIMAL_STACK_SIZE * 4,
                          NULL, HOUSEKEEPING_PRIORITY, NULL);
    configASSERT(xResult == pdPASS);

    xResult = xTaskCreate(prvSunSensorTask, "Sun", configMINIMAL_STACK_SIZE * 4,
                          NULL, HOUSEKEEPING_PRIORITY, NULL);
    configASSERT(xResult == pdPASS);

    xResult = xTaskCreate(prvProcessorTask, "Proc", configMINIMAL_STACK_SIZE * 4,
                          NULL, PROCESSOR_TASK_PRIORITY, NULL);
    configASSERT(xResult == pdPASS);

    xResult = xTaskCreate(prvTelemetryTask, "Telem", configMINIMAL_STACK_SIZE * 4,
                          NULL, TELEMETRY_TASK_PRIORITY, NULL);
    configASSERT(xResult == pdPASS);

    vTaskStartScheduler();

    /* Should never reach here */
    for (;;);
}

/*-----------------------------------------------------------*/
/* FreeRTOS required hook functions                          */
/*-----------------------------------------------------------*/

void vApplicationMallocFailedHook(void)
{
    printf("\r\nMalloc failed\r\n");
    portDISABLE_INTERRUPTS();
    for (;;);
}

void vApplicationStackOverflowHook(TaskHandle_t pxTask, char *pcTaskName)
{
    (void)pxTask;
    printf("\r\nStack overflow in %s\r\n", pcTaskName);
    portDISABLE_INTERRUPTS();
    for (;;);
}

void vAssertCalled(const char *pcFileName, uint32_t ulLine)
{
    printf("ASSERT! Line %d, file %s\r\n", (int)ulLine, pcFileName);
    portDISABLE_INTERRUPTS();
    for (;;);
}

/* Required when configSUPPORT_STATIC_ALLOCATION is 1 */
void vApplicationGetIdleTaskMemory(StaticTask_t **ppxIdleTaskTCBBuffer,
                                   StackType_t **ppxIdleTaskStackBuffer,
                                   configSTACK_DEPTH_TYPE *pulIdleTaskStackSize)
{
    static StaticTask_t xIdleTaskTCB;
    static StackType_t uxIdleTaskStack[configMINIMAL_STACK_SIZE];
    *ppxIdleTaskTCBBuffer = &xIdleTaskTCB;
    *ppxIdleTaskStackBuffer = uxIdleTaskStack;
    *pulIdleTaskStackSize = configMINIMAL_STACK_SIZE;
}

/*-----------------------------------------------------------*/

void TIMER0_Handler(void) { configASSERT(0); }
void TIMER1_Handler(void) { configASSERT(0); }

/* Prevent accidental use of libc malloc — FreeRTOS uses pvPortMalloc.
 * Safe with our custom printf-stdarg.c which never calls malloc.
 * (newlib-nano's printf would call malloc for _REENT buffers, but
 * printf-stdarg.c replaces it entirely via the putchar macro.) */
void *malloc(size_t size)
{
    (void)size;
    printf("\r\nUnexpected malloc() call\r\n");
    portDISABLE_INTERRUPTS();
    for (;;);
}
