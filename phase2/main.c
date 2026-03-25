/*
 * FreeRTOS watchdog timer demo for MPS2-AN385 (Cortex-M3) on QEMU
 *
 * Builds on the two-task telemetry demo by adding a software watchdog:
 *   - Each task must "kick" the watchdog periodically to prove it's alive
 *   - A high-priority watchdog task checks the kick counters
 *   - If a task stops kicking (hangs), the watchdog detects it and alerts
 *   - The sensor task deliberately hangs after 5 readings to trigger the watchdog
 *
 * This models how spacecraft detect and recover from stuck software.
 * On real hardware, the watchdog would trigger a system reset.
 */

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

#include <stdio.h>
#include <stdint.h>

#include "board.h"

/* Task priorities — watchdog is highest */
#define SENSOR_TASK_PRIORITY    (tskIDLE_PRIORITY + 1)
#define TELEMETRY_TASK_PRIORITY (tskIDLE_PRIORITY + 2)
#define WATCHDOG_TASK_PRIORITY  (tskIDLE_PRIORITY + 3)

/* Timing */
#define SENSOR_PERIOD_MS              500
#define WATCHDOG_PERIOD_MS            2000  /* Check every 2 seconds */
#define TELEMETRY_RECEIVE_TIMEOUT_MS  (SENSOR_PERIOD_MS * 2)

_Static_assert(TELEMETRY_RECEIVE_TIMEOUT_MS < WATCHDOG_PERIOD_MS,
               "Telemetry timeout must be shorter than watchdog period");

/* Queue holds up to 5 sensor readings */
#define QUEUE_LENGTH 5

/* After this many readings, the sensor task will deliberately hang */
#define HANG_AFTER_READING 5

/* Message passed between tasks */
typedef struct {
    uint32_t sensor_id;
    uint32_t value;
    uint32_t timestamp;
} SensorReading_t;

static QueueHandle_t xSensorQueue = NULL;
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
/* Watchdog kick counters                                    */
/*-----------------------------------------------------------*/

/*
 * Each monitored task increments its counter every cycle.
 * The watchdog task snapshots these counters periodically.
 * If a counter hasn't changed since the last snapshot, that task is stuck.
 *
 * volatile prevents the compiler from caching counter values in registers
 * across loop iterations. On single-core Cortex-M3, 32-bit aligned
 * reads/writes are natively atomic so no torn-read concern exists.
 */
static volatile uint32_t ulSensorKicks = 0;
static volatile uint32_t ulTelemetryKicks = 0;

/*-----------------------------------------------------------*/

static void prvUARTInit(void)
{
    /* 25 MHz APB clock / 115200 baud = ~217 */
    UART0_BAUDDIV = 217;
    UART0_CTRL = 1;
}

/*-----------------------------------------------------------*/

static void prvSensorTask(void *pvParameters)
{
    (void)pvParameters;
    TickType_t xLastWakeTime = xTaskGetTickCount();
    SensorReading_t xReading;
    uint32_t ulReadingCount = 0;

    for (;;) {
        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(SENSOR_PERIOD_MS));

        xReading.sensor_id = 1;
        xReading.value = 2000 + (ulReadingCount % 100);
        xReading.timestamp = xLastWakeTime;

        /* Non-blocking send — drop reading if queue is full to preserve timing */
        if (xQueueSend(xSensorQueue, &xReading, 0) != pdPASS) {
            /* Raw printf to avoid blocking on xPrintMutex */
            printf("[SENSOR] Queue full, dropped reading %u\r\n",
                   (unsigned)ulReadingCount);
        }
        ulReadingCount++;

        /* Kick the watchdog — "I'm still alive".
         * This fires before the hang check, so the watchdog sees exactly
         * HANG_AFTER_READING kicks before the task stops responding. */
        ulSensorKicks++;

        /* Deliberately hang after HANG_AFTER_READING readings
         * to demonstrate the watchdog catching a stuck task */
        if (ulReadingCount >= HANG_AFTER_READING) {
            safe_printf("[SENSOR] Simulating hang after %u readings...\r\n",
                        (unsigned)ulReadingCount);
            for (;;);  /* stuck! */
        }
    }
}

/*-----------------------------------------------------------*/

static void prvTelemetryTask(void *pvParameters)
{
    (void)pvParameters;
    SensorReading_t xReceived;

    safe_printf("[TELEMETRY] Task started, waiting for sensor data...\r\n");

    for (;;) {
        /* Bounded timeout so we kick the watchdog even when no data arrives.
         * Without this, a stopped sensor would make telemetry look stuck too. */
        if (xQueueReceive(xSensorQueue, &xReceived,
                          pdMS_TO_TICKS(TELEMETRY_RECEIVE_TIMEOUT_MS)) == pdPASS) {
            safe_printf("[TELEMETRY] Sensor %u: %u.%02u C at tick %u\r\n",
                        (unsigned)xReceived.sensor_id,
                        (unsigned)(xReceived.value / 100),
                        (unsigned)(xReceived.value % 100),
                        (unsigned)xReceived.timestamp);
        }

        /* Kick unconditionally — proves this task is alive regardless of data flow */
        ulTelemetryKicks++;
    }
}

/*-----------------------------------------------------------*/

/*
 * Watchdog task: runs at the highest priority, checks that every
 * monitored task has made progress since the last check.
 *
 * On real spacecraft, a watchdog timeout would trigger a system reset.
 * Here we just print an alert and halt — enough to demonstrate the concept.
 */
static void prvWatchdogTask(void *pvParameters)
{
    (void)pvParameters;
    safe_printf("[WATCHDOG] Monitoring started (checking every %u ms)\r\n",
                (unsigned)WATCHDOG_PERIOD_MS);

    /* Warm up: let tasks run at least one cycle before checking.
     * Without this, reducing WATCHDOG_PERIOD_MS below SENSOR_PERIOD_MS
     * would cause a false alert on the very first check.
     * Note: first real check fires at ~2x WATCHDOG_PERIOD_MS from boot
     * (one delay here + one in the loop). This is intentional. */
    vTaskDelay(pdMS_TO_TICKS(WATCHDOG_PERIOD_MS));
    uint32_t ulLastSensorKicks = ulSensorKicks;
    uint32_t ulLastTelemetryKicks = ulTelemetryKicks;
    TickType_t xLastWakeTime = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(WATCHDOG_PERIOD_MS));

        /* Snapshot current kick counts */
        uint32_t ulCurrentSensor = ulSensorKicks;
        uint32_t ulCurrentTelemetry = ulTelemetryKicks;

        /* Check all tasks before halting so every failure is reported */
        BaseType_t xAllOk = pdTRUE;
        if (ulCurrentSensor == ulLastSensorKicks) {
            safe_printf("[WATCHDOG] ALERT: Sensor task not responding!\r\n");
            xAllOk = pdFALSE;
        }
        if (ulCurrentTelemetry == ulLastTelemetryKicks) {
            safe_printf("[WATCHDOG] ALERT: Telemetry task not responding!\r\n");
            xAllOk = pdFALSE;
        }
        if (xAllOk == pdFALSE) {
            safe_printf("[WATCHDOG] System would reset on real hardware.\r\n");
            safe_printf("[WATCHDOG] Halted. Awaiting reset.\r\n");
            /* Halt — this starves all lower-priority tasks intentionally.
             * On real hardware a reset would follow immediately. */
            for (;;);
        }

        safe_printf("[WATCHDOG] All tasks healthy (sensor +%u, telemetry +%u)\r\n",
                    (unsigned)(ulCurrentSensor - ulLastSensorKicks),
                    (unsigned)(ulCurrentTelemetry - ulLastTelemetryKicks));

        /* Save for next comparison */
        ulLastSensorKicks = ulCurrentSensor;
        ulLastTelemetryKicks = ulCurrentTelemetry;
    }
}

/*-----------------------------------------------------------*/

int main(void)
{
    prvUARTInit();

    /* Raw printf — mutex not yet created, scheduler not running */
    printf("FreeRTOS Watchdog Timer Demo\r\n");
    printf("============================\r\n");

    xPrintMutex = xSemaphoreCreateMutex();
    configASSERT(xPrintMutex != NULL);

    xSensorQueue = xQueueCreate(QUEUE_LENGTH, sizeof(SensorReading_t));
    configASSERT(xSensorQueue != NULL);

    BaseType_t xResult;
    xResult = xTaskCreate(prvSensorTask, "Sensor", configMINIMAL_STACK_SIZE * 4,
                          NULL, SENSOR_TASK_PRIORITY, NULL);
    configASSERT(xResult == pdPASS);

    xResult = xTaskCreate(prvTelemetryTask, "Telemetry", configMINIMAL_STACK_SIZE * 4,
                          NULL, TELEMETRY_TASK_PRIORITY, NULL);
    configASSERT(xResult == pdPASS);

    xResult = xTaskCreate(prvWatchdogTask, "Watchdog", configMINIMAL_STACK_SIZE * 4,
                          NULL, WATCHDOG_TASK_PRIORITY, NULL);
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
