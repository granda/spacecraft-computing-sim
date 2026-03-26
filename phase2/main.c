/*
 * FreeRTOS priority inversion demo for MPS2-AN385 (Cortex-M3) on QEMU
 *
 * Demonstrates the classic priority inversion problem and its fix:
 *
 * THREE TASKS share a resource (simulated by a semaphore/mutex):
 *   - LOW priority:  acquires the lock, does "slow work" (long delay while holding it)
 *   - HIGH priority: needs the lock to do its work, blocks waiting for it
 *   - MEDIUM priority: doesn't use the lock, just runs compute work
 *
 * THE BUG (priority inversion):
 *   1. LOW acquires the lock
 *   2. HIGH wakes up, tries to acquire the lock, blocks
 *   3. MEDIUM wakes up — it doesn't need the lock, so it runs
 *   4. MEDIUM preempts LOW (higher priority), preventing LOW from releasing the lock
 *   5. HIGH is stuck waiting for LOW, but LOW can't run because MEDIUM is hogging the CPU
 *   => HIGH is effectively running at LOW's priority — "inverted"
 *
 * THE FIX (priority inheritance):
 *   FreeRTOS mutexes (not binary semaphores) implement priority inheritance:
 *   when HIGH blocks on a mutex held by LOW, the kernel temporarily boosts
 *   LOW to HIGH's priority so it can finish and release the mutex.
 *   MEDIUM can no longer preempt LOW, and HIGH gets the lock promptly.
 *
 * This bug nearly killed the Mars Pathfinder mission in 1997.
 */

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include <stdio.h>
#include <stdint.h>

#include "board.h"

/* Task priorities */
#define LOW_PRIORITY    (tskIDLE_PRIORITY + 1)
#define MEDIUM_PRIORITY (tskIDLE_PRIORITY + 2)
#define HIGH_PRIORITY   (tskIDLE_PRIORITY + 3)

/* The shared resource lock — will be either a binary semaphore (buggy)
 * or a mutex (fixed), controlled by USE_MUTEX_FIX */
#define USE_MUTEX_FIX  0  /* Set to 1 to enable priority inheritance fix */

static SemaphoreHandle_t xSharedLock = NULL;
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

/* Burn CPU cycles to simulate work (busy-wait, not a FreeRTOS delay).
 * Iteration counts are empirically tuned for QEMU on desktop hardware —
 * adjust if tests become flaky on slow CI runners. */
static void prvBusyWork(uint32_t iterations)
{
    volatile uint32_t i;
    for (i = 0; i < iterations; i++);
}

/*-----------------------------------------------------------*/

/*
 * LOW priority task: acquires the shared lock, does slow work while
 * holding it, then releases. This simulates a task that legitimately
 * holds a resource for a long time (e.g., writing to flash).
 */
static void prvLowTask(void *pvParameters)
{
    (void)pvParameters;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));

        safe_printf("[LOW]  Acquiring lock...\r\n");
        xSemaphoreTake(xSharedLock, portMAX_DELAY);
        safe_printf("[LOW]  Lock acquired — doing slow work\r\n");

        /* Simulate slow work while holding the lock.
         * Using busy-wait (not vTaskDelay) so the task stays runnable
         * and can be preempted by medium — that's the whole point.
         * Must be longer than MEDIUM's wakeup period (200ms) so MEDIUM
         * gets scheduled while LOW holds the lock. */
        prvBusyWork(5000000);

        safe_printf("[LOW]  Releasing lock\r\n");
        xSemaphoreGive(xSharedLock);
    }
}

/*-----------------------------------------------------------*/

/*
 * MEDIUM priority task: does NOT use the shared lock.
 * It just runs compute work. During priority inversion, this task
 * preempts LOW and prevents it from releasing the lock.
 */
static void prvMediumTask(void *pvParameters)
{
    (void)pvParameters;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(200));

        safe_printf("[MED]  Running (no lock needed)\r\n");
        prvBusyWork(3000000);
        safe_printf("[MED]  Done\r\n");
    }
}

/*-----------------------------------------------------------*/

/*
 * HIGH priority task: needs the shared lock to do its work.
 * If LOW holds the lock and MEDIUM preempts LOW, HIGH is stuck.
 */
static void prvHighTask(void *pvParameters)
{
    (void)pvParameters;
    uint32_t ulCycle = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(300));

        safe_printf("[HIGH] Acquiring lock...\r\n");
        TickType_t xStart = xTaskGetTickCount();
        xSemaphoreTake(xSharedLock, portMAX_DELAY);
        TickType_t xWait = xTaskGetTickCount() - xStart;

        safe_printf("[HIGH] Lock acquired (waited %u ticks)\r\n",
                    (unsigned)xWait);

        /* Do quick work with the shared resource */
        prvBusyWork(100000);

        xSemaphoreGive(xSharedLock);
        safe_printf("[HIGH] Released lock\r\n");

        ulCycle++;
        if (ulCycle >= 3) {
            safe_printf("\r\n[HIGH] === Demo complete after 3 cycles ===\r\n");
#if USE_MUTEX_FIX
            safe_printf("[HIGH] Priority inheritance was ENABLED (mutex)\r\n");
            safe_printf("[HIGH] HIGH waited minimal ticks — MEDIUM could not block LOW\r\n");
#else
            safe_printf("[HIGH] Priority inheritance was DISABLED (binary semaphore)\r\n");
            safe_printf("[HIGH] HIGH waited many ticks — MEDIUM preempted LOW\r\n");
#endif
            safe_printf("[HIGH] Done.\r\n");
            /* Halt — on real hardware a reset would follow */
            portDISABLE_INTERRUPTS();
            for (;;);
        }
    }
}

/*-----------------------------------------------------------*/

int main(void)
{
    prvUARTInit();

    /* Raw printf — mutex not yet created, scheduler not running */
    printf("FreeRTOS Priority Inversion Demo\r\n");
    printf("================================\r\n");
#if USE_MUTEX_FIX
    printf("Mode: MUTEX (priority inheritance enabled)\r\n\r\n");
#else
    printf("Mode: BINARY SEMAPHORE (no priority inheritance)\r\n\r\n");
#endif

    xPrintMutex = xSemaphoreCreateMutex();
    configASSERT(xPrintMutex != NULL);

#if USE_MUTEX_FIX
    /* Mutex: FreeRTOS temporarily boosts LOW to HIGH's priority
     * when HIGH blocks on it — this prevents priority inversion */
    xSharedLock = xSemaphoreCreateMutex();
#else
    /* Binary semaphore: NO priority inheritance.
     * MEDIUM can preempt LOW while it holds the lock, blocking HIGH */
    xSharedLock = xSemaphoreCreateBinary();
#endif
    configASSERT(xSharedLock != NULL);
#if !USE_MUTEX_FIX
    BaseType_t xGiven = xSemaphoreGive(xSharedLock);  /* Start in "available" state */
    configASSERT(xGiven == pdPASS);
#endif

    BaseType_t xResult;
    xResult = xTaskCreate(prvLowTask, "Low", configMINIMAL_STACK_SIZE * 4,
                          NULL, LOW_PRIORITY, NULL);
    configASSERT(xResult == pdPASS);

    xResult = xTaskCreate(prvMediumTask, "Medium", configMINIMAL_STACK_SIZE * 4,
                          NULL, MEDIUM_PRIORITY, NULL);
    configASSERT(xResult == pdPASS);

    xResult = xTaskCreate(prvHighTask, "High", configMINIMAL_STACK_SIZE * 4,
                          NULL, HIGH_PRIORITY, NULL);
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
