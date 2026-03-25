/*
 * FreeRTOS two-task demo for MPS2-AN385 (Cortex-M3) on QEMU
 *
 * Two tasks print to UART at different rates via a shared message queue.
 * A "sensor" task sends readings every 500ms; a "telemetry" task receives
 * and prints them. This models a basic spacecraft telemetry pipeline.
 *
 * Based on the FreeRTOS QEMU MPS2-AN385 demo.
 */

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

#include <stdio.h>
#include <stdint.h>

#include "board.h"

/* Task priorities */
#define SENSOR_TASK_PRIORITY    (tskIDLE_PRIORITY + 1)
#define TELEMETRY_TASK_PRIORITY (tskIDLE_PRIORITY + 2)

/* Sensor reading interval */
#define SENSOR_PERIOD_MS 500

/* Queue holds up to 5 sensor readings */
#define QUEUE_LENGTH 5

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

static void prvUARTInit(void)
{
    /* 25 MHz APB clock / 115200 baud = ~217 (matches Phase 1) */
    UART0_BAUDDIV = 217;
    UART0_CTRL = 1;
}

/*-----------------------------------------------------------*/

/*
 * Sensor task: generates fake sensor readings and sends them to the queue.
 * Runs every SENSOR_PERIOD_MS milliseconds using vTaskDelayUntil for
 * precise periodic timing.
 */
static void prvSensorTask(void *pvParameters)
{
    (void)pvParameters;
    TickType_t xLastWakeTime = xTaskGetTickCount();
    SensorReading_t xReading;
    uint32_t ulReadingCount = 0;

    for (;;) {
        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(SENSOR_PERIOD_MS));

        /* Simulate a temperature sensor: 20.00-20.99 C with variation */
        xReading.sensor_id = 1;
        xReading.value = 2000 + (ulReadingCount % 100);  /* centidegrees */
        xReading.timestamp = xLastWakeTime;

        /* Non-blocking send — drop reading if queue is full to preserve timing */
        if (xQueueSend(xSensorQueue, &xReading, 0) != pdPASS) {
            /* Raw printf to avoid blocking on xPrintMutex — preserves timing */
            printf("[SENSOR] Queue full, dropped reading %u\r\n",
                   (unsigned)ulReadingCount);
        }
        ulReadingCount++;
    }
}

/*-----------------------------------------------------------*/

/*
 * Telemetry task: receives sensor readings from the queue and prints them.
 * Higher priority than the sensor task, so it runs as soon as data arrives.
 */
static void prvTelemetryTask(void *pvParameters)
{
    (void)pvParameters;
    SensorReading_t xReceived;

    safe_printf("[TELEMETRY] Task started, waiting for sensor data...\r\n");

    for (;;) {
        /* portMAX_DELAY with INCLUDE_vTaskSuspend=1 guarantees pdPASS return */
        (void)xQueueReceive(xSensorQueue, &xReceived, portMAX_DELAY);

        safe_printf("[TELEMETRY] Sensor %u: %u.%02u C at tick %u\r\n",
                    (unsigned)xReceived.sensor_id,
                    (unsigned)(xReceived.value / 100),
                    (unsigned)(xReceived.value % 100),
                    (unsigned)xReceived.timestamp);
    }
}

/*-----------------------------------------------------------*/

int main(void)
{
    prvUARTInit();

    /* Raw printf — mutex not yet created, scheduler not running */
    printf("FreeRTOS Spacecraft Telemetry Demo\r\n");
    printf("==================================\r\n");

    /* Create the print mutex for thread-safe UART output */
    xPrintMutex = xSemaphoreCreateMutex();
    configASSERT(xPrintMutex != NULL);

    /* Create the message queue */
    xSensorQueue = xQueueCreate(QUEUE_LENGTH, sizeof(SensorReading_t));
    configASSERT(xSensorQueue != NULL);

    /* Create tasks */
    BaseType_t xResult;
    xResult = xTaskCreate(prvSensorTask, "Sensor", configMINIMAL_STACK_SIZE * 4,
                          NULL, SENSOR_TASK_PRIORITY, NULL);
    configASSERT(xResult == pdPASS);

    xResult = xTaskCreate(prvTelemetryTask, "Telemetry", configMINIMAL_STACK_SIZE * 4,
                          NULL, TELEMETRY_TASK_PRIORITY, NULL);
    configASSERT(xResult == pdPASS);

    /* Start the scheduler — this never returns */
    vTaskStartScheduler();

    /* Should never reach here */
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

/* vApplicationGetTimerTaskMemory removed — configUSE_TIMERS is 0 */

/*-----------------------------------------------------------*/
/* Timer interrupt stubs (referenced by startup_gcc.c)      */
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
