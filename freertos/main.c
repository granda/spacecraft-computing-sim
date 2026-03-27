/*
 * FreeRTOS sensor pipeline demo for MPS2-AN385 (Cortex-M3) on QEMU
 *
 * Models a spacecraft sensor-to-telemetry pipeline with priority scheduling:
 *
 *   Temp Sensor (pri 1, 1 Hz)  ──┐
 *                                 ├──> Processor (pri 3) ──> Telemetry (pri 2)
 *   Gyro Sensor (pri 4, 10 Hz) ──┘
 *
 * The gyroscope runs at higher priority and rate than the temperature sensor,
 * mirroring real spacecraft where attitude control sensors are more critical
 * than housekeeping sensors. The processor task aggregates readings from both
 * sensors via a shared queue and forwards processed results to telemetry.
 *
 * Key concepts demonstrated:
 *   - Multiple producers at different priorities sharing a single queue
 *   - Priority-based preemption ensuring critical sensors are never starved
 *   - A processing stage between raw sensors and telemetry output
 */

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

#include <stdio.h>
#include <stdint.h>

#include "board.h"

/* Task priorities — gyro is highest (most time-critical) */
#define TEMP_TASK_PRIORITY       (tskIDLE_PRIORITY + 1)
#define TELEMETRY_TASK_PRIORITY  (tskIDLE_PRIORITY + 2)
#define PROCESSOR_TASK_PRIORITY  (tskIDLE_PRIORITY + 3)
#define GYRO_TASK_PRIORITY       (tskIDLE_PRIORITY + 4)

/* Sensor rates */
#define TEMP_PERIOD_MS  1000  /* 1 Hz — housekeeping */
#define GYRO_PERIOD_MS  100   /* 10 Hz — attitude control */

/* Stop after this many processed readings */
#define MAX_PROCESSED  30

/* Sentinel sequence number — signals telemetry task to shut down */
#define SENTINEL_SEQ   UINT32_MAX

/* Sensor IDs */
#define SENSOR_TEMP  1
#define SENSOR_GYRO  2

/* Calibration parameters */
#define TEMP_CAL_OFFSET  (-10)   /* centidegrees bias correction */
#define GYRO_SCALE_FACTOR  2     /* raw-to-calibrated multiplier */

/* Raw sensor reading — sent from sensors to processor */
typedef struct {
    uint32_t sensor_id;
    int32_t  raw_value;
    uint32_t timestamp;
} RawReading_t;

/* Processed reading — sent from processor to telemetry */
typedef struct {
    uint32_t sensor_id;
    int32_t  value;       /* Processed/calibrated value */
    uint32_t timestamp;
    uint32_t seq;         /* Sequence number */
} ProcessedReading_t;

/* Queues */
static QueueHandle_t xRawQueue = NULL;       /* sensors → processor */
static QueueHandle_t xTelemetryQueue = NULL; /* processor → telemetry */

static SemaphoreHandle_t xPrintMutex = NULL;

/* Thread-safe printf: takes the mutex, prints, releases.
 * Not ISR-safe — only call from task context. */
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

/*
 * Temperature sensor: low priority, low rate (1 Hz).
 * Housekeeping — not time-critical. Can be preempted by the gyro.
 */
static void prvTempSensorTask(void *pvParameters)
{
    (void)pvParameters;
    TickType_t xLastWakeTime = xTaskGetTickCount();
    RawReading_t xReading;

    safe_printf("[TEMP]  Sensor online (1 Hz, priority %u)\r\n",
                (unsigned)uxTaskPriorityGet(NULL));

    for (;;) {
        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(TEMP_PERIOD_MS));

        /* Simulate temperature: 20-25 C range */
        xReading.sensor_id = SENSOR_TEMP;
        xReading.raw_value = 2000 + (xLastWakeTime % 500);  /* centidegrees */
        xReading.timestamp = xLastWakeTime;

        /* Non-blocking send to preserve timing */
        if (xQueueSend(xRawQueue, &xReading, 0) != pdPASS) {
            safe_printf("[TEMP]  Queue full, dropped\r\n");
        }
    }
}

/*-----------------------------------------------------------*/

/*
 * Gyroscope sensor: high priority, high rate (10 Hz).
 * Attitude control — time-critical. Preempts the temp sensor.
 */
static void prvGyroSensorTask(void *pvParameters)
{
    (void)pvParameters;
    TickType_t xLastWakeTime = xTaskGetTickCount();
    RawReading_t xReading;

    safe_printf("[GYRO]  Sensor online (10 Hz, priority %u)\r\n",
                (unsigned)uxTaskPriorityGet(NULL));

    for (;;) {
        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(GYRO_PERIOD_MS));

        /* Simulate gyroscope: angular rate in millidegrees/sec */
        xReading.sensor_id = SENSOR_GYRO;
        xReading.raw_value = 150 + (xLastWakeTime % 50);  /* small oscillation */
        xReading.timestamp = xLastWakeTime;

        if (xQueueSend(xRawQueue, &xReading, 0) != pdPASS) {
            safe_printf("[GYRO]  Queue full, dropped\r\n");
        }
    }
}

/*-----------------------------------------------------------*/

/*
 * Processor task: reads raw sensor data, applies calibration/transform,
 * and forwards processed results to telemetry. In a real spacecraft this
 * would apply Kalman filtering, unit conversion, limit checking, etc.
 */
static void prvProcessorTask(void *pvParameters)
{
    (void)pvParameters;
    RawReading_t xRaw;
    ProcessedReading_t xProcessed;
    uint32_t ulSeq = 0;
    uint32_t ulDropped = 0;

    safe_printf("[PROC]  Processor online (priority %u)\r\n",
                (unsigned)uxTaskPriorityGet(NULL));

    for (;;) {
        /* Block until a raw reading arrives */
        if (xQueueReceive(xRawQueue, &xRaw,
                          pdMS_TO_TICKS(TEMP_PERIOD_MS * 2)) == pdPASS) {

            /* Simple "calibration": apply offset (real systems do much more) */
            xProcessed.sensor_id = xRaw.sensor_id;
            xProcessed.timestamp = xRaw.timestamp;
            xProcessed.seq = ulSeq++;

            if (xRaw.sensor_id == SENSOR_TEMP) {
                xProcessed.value = xRaw.raw_value + TEMP_CAL_OFFSET;
            } else if (xRaw.sensor_id == SENSOR_GYRO) {
                xProcessed.value = xRaw.raw_value * GYRO_SCALE_FACTOR;
            } else {
                safe_printf("[PROC]  Unknown sensor_id %u\r\n",
                            (unsigned)xRaw.sensor_id);
                xProcessed.value = xRaw.raw_value;
            }

            /* Forward to telemetry — block briefly to let telemetry drain */
            if (xQueueSend(xTelemetryQueue, &xProcessed,
                           pdMS_TO_TICKS(100)) != pdPASS) {
                ulDropped++;
                safe_printf("[PROC]  Telemetry queue full, dropped #%u\r\n",
                            (unsigned)ulSeq - 1);
            }

            /* Stop after MAX_PROCESSED readings — send sentinel so
             * the lower-priority telemetry task can drain its queue
             * before the system halts. */
            if (ulSeq >= MAX_PROCESSED) {
                safe_printf("\r\n[PROC]  === Pipeline complete: %u processed, %u dropped ===\r\n",
                            (unsigned)ulSeq, (unsigned)ulDropped);
                ProcessedReading_t xSentinel = { 0, 0, 0, SENTINEL_SEQ };
                xQueueSend(xTelemetryQueue, &xSentinel, portMAX_DELAY);
                /* Block forever — telemetry task will halt after draining.
                 * Sensor tasks and this task remain allocated (TCBs not freed);
                 * acceptable for a demo that halts shortly after. */
                vTaskSuspend(NULL);
            }
        }
    }
}

/*-----------------------------------------------------------*/

/*
 * Telemetry task: receives processed readings and outputs them.
 * In a real spacecraft this would format CCSDS packets for downlink.
 */
static void prvTelemetryTask(void *pvParameters)
{
    (void)pvParameters;
    ProcessedReading_t xData;

    safe_printf("[TELEM] Telemetry online (priority %u)\r\n",
                (unsigned)uxTaskPriorityGet(NULL));

    for (;;) {
        if (xQueueReceive(xTelemetryQueue, &xData,
                          pdMS_TO_TICKS(TEMP_PERIOD_MS * 2)) == pdPASS) {

            /* Sentinel from processor — all real data has been printed */
            if (xData.seq == SENTINEL_SEQ) {
                safe_printf("[TELEM] All readings transmitted\r\n");
                portDISABLE_INTERRUPTS();
                for (;;);
            }

            const char *pcName;
            if (xData.sensor_id == SENSOR_TEMP) {
                pcName = "TEMP";
            } else if (xData.sensor_id == SENSOR_GYRO) {
                pcName = "GYRO";
            } else {
                pcName = "UNKN";
            }

            safe_printf("[TELEM] #%03u %s: %d at tick %u\r\n",
                        (unsigned)xData.seq,
                        pcName,
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
    printf("FreeRTOS Sensor Pipeline Demo\r\n");
    printf("=============================\r\n\r\n");

    xPrintMutex = xSemaphoreCreateMutex();
    configASSERT(xPrintMutex != NULL);

    /* Raw queue: sensors → processor (large enough for 10 Hz gyro burst) */
    xRawQueue = xQueueCreate(20, sizeof(RawReading_t));
    configASSERT(xRawQueue != NULL);

    /* Telemetry queue: processor → telemetry */
    xTelemetryQueue = xQueueCreate(10, sizeof(ProcessedReading_t));
    configASSERT(xTelemetryQueue != NULL);

    BaseType_t xResult;
    xResult = xTaskCreate(prvTempSensorTask, "Temp", configMINIMAL_STACK_SIZE * 4,
                          NULL, TEMP_TASK_PRIORITY, NULL);
    configASSERT(xResult == pdPASS);

    xResult = xTaskCreate(prvGyroSensorTask, "Gyro", configMINIMAL_STACK_SIZE * 4,
                          NULL, GYRO_TASK_PRIORITY, NULL);
    configASSERT(xResult == pdPASS);

    xResult = xTaskCreate(prvProcessorTask, "Proc", configMINIMAL_STACK_SIZE * 4,
                          NULL, PROCESSOR_TASK_PRIORITY, NULL);
    configASSERT(xResult == pdPASS);

    xResult = xTaskCreate(prvTelemetryTask, "Telem", configMINIMAL_STACK_SIZE * 4,
                          NULL, TELEMETRY_TASK_PRIORITY, NULL);
    configASSERT(xResult == pdPASS);

    vTaskStartScheduler();

    /* Should never reach here — scheduler runs indefinitely */
    for (;;);
}

/*-----------------------------------------------------------*/
/* FreeRTOS required hook functions                          */
/*-----------------------------------------------------------*/

void vApplicationMallocFailedHook(void)
{
    /* Raw printf intentional — system is fatally broken, mutex may be held */
    printf("\r\nMalloc failed\r\n");
    portDISABLE_INTERRUPTS();
    for (;;);
}

void vApplicationStackOverflowHook(TaskHandle_t pxTask, char *pcTaskName)
{
    (void)pxTask;
    /* Raw printf intentional — system is fatally broken, mutex may be held */
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

/* Prevent accidental use of libc malloc — FreeRTOS uses pvPortMalloc */
void *malloc(size_t size)
{
    (void)size;
    printf("\r\nUnexpected malloc() call\r\n");
    portDISABLE_INTERRUPTS();
    for (;;);
}
