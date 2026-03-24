/*
 * Bare-metal SysTick interrupt demo for MPS2-AN385 (Cortex-M3)
 *
 * Builds on the UART hello world by adding an interrupt:
 *   - SysTick timer fires at 2 Hz (every 500ms)
 *   - The handler increments a counter
 *   - main() watches the counter and prints each tick
 *   - Runs for 10 ticks (5 seconds) then stops
 *
 * This demonstrates the fundamental alternative to polling:
 * instead of the CPU checking "is it time yet?" in a loop,
 * the hardware INTERRUPTS the CPU when it's time.
 */

#include <stdint.h>

/* ---- UART0 registers ---- */
#define UART0_BASE    0x40004000
#define UART0_DATA    (*(volatile uint32_t *)(UART0_BASE + 0x00))
#define UART0_STATE   (*(volatile uint32_t *)(UART0_BASE + 0x04))
#define UART0_CTRL    (*(volatile uint32_t *)(UART0_BASE + 0x08))
#define UART0_BAUDDIV (*(volatile uint32_t *)(UART0_BASE + 0x10))

#define UART_STATE_TXFULL  (1 << 0)
#define UART_CTRL_TX_EN    (1 << 0)

/* ---- SysTick registers (built into every Cortex-M3) ---- */
/*
 * SysTick is a 24-bit countdown timer wired directly into the CPU core.
 * When it hits zero, it fires the SysTick exception (vector table entry 15),
 * reloads, and counts down again. No external hardware needed.
 *
 * Register map (fixed addresses, defined by ARM, same on every Cortex-M):
 *   0xE000E010  CTRL    — enable, interrupt enable, clock source, count flag
 *   0xE000E014  LOAD    — reload value (counts down from this to 0)
 *   0xE000E018  VAL     — current value (write to clear)
 */
#define SYSTICK_CTRL  (*(volatile uint32_t *)0xE000E010)
#define SYSTICK_LOAD  (*(volatile uint32_t *)0xE000E014)
#define SYSTICK_VAL   (*(volatile uint32_t *)0xE000E018)

#define SYSTICK_ENABLE     (1 << 0)  /* Start counting */
#define SYSTICK_TICKINT    (1 << 1)  /* Fire interrupt when hitting zero */
#define SYSTICK_CLKSOURCE  (1 << 2)  /* Use processor clock (not external) */

/* CPU clock: 25 MHz on MPS2-AN385 */
#define CPU_CLOCK_HZ   25000000
#define SYSTICK_HZ     2  /* Tick rate — 2 Hz fits the 24-bit LOAD register at 25 MHz */
#define DEMO_SECONDS   5
#define TOTAL_TICKS    (DEMO_SECONDS * SYSTICK_HZ)

/*
 * Tick counter — incremented by the interrupt handler.
 *
 * volatile is critical here: without it, the compiler sees that main()
 * never writes to tick_count and optimizes the while loop into
 * an infinite loop that never re-reads the variable. volatile forces
 * a memory read on every access.
 */
static volatile uint32_t tick_count = 0;

/* ---- UART functions (same as before) ---- */

static void uart_init(void)
{
    /* 25 MHz APB clock / 115200 baud = ~217 */
    UART0_BAUDDIV = 217;
    UART0_CTRL = UART_CTRL_TX_EN;
}

static void uart_putc(char c)
{
    /* Spins forever if UART TX is stuck — fine for QEMU, needs a timeout on real hardware */
    while (UART0_STATE & UART_STATE_TXFULL);
    UART0_DATA = c;
}

static void uart_puts(const char *s)
{
    if (!s) return;
    while (*s) {
        uart_putc(*s++);
    }
}

/* Print an unsigned integer in decimal (no printf needed) */
static void uart_put_uint(uint32_t n)
{
    char buf[10];  /* UINT32_MAX = 4,294,967,295 = 10 digits */
    int i = 0;
    if (n == 0) {
        uart_putc('0');
        return;
    }
    while (n > 0) {
        buf[i++] = '0' + (n % 10);
        n /= 10;
    }
    while (i > 0) {
        uart_putc(buf[--i]);
    }
}

/* ---- SysTick setup ---- */

static void systick_init(uint32_t ticks_per_second)
{
    /*
     * LOAD = (clock / desired_rate) - 1
     * SysTick LOAD is 24-bit (max 0xFFFFFF = 16,777,215).
     * At 25 MHz the slowest rate that fits is ~1.49 Hz.
     * We use 2 Hz (reload = 12,499,999) to stay within range.
     * The timer counts down from LOAD to 0, fires, reloads.
     */
    uint32_t reload = (CPU_CLOCK_HZ / ticks_per_second) - 1;
    /* SysTick LOAD is 24-bit — halt if the value doesn't fit.
     * bkpt halts under a debugger; on real hardware without one,
     * it escalates to HardFault (same caveat as Default_Handler). */
    if (reload > 0xFFFFFF) {
        __asm volatile("bkpt #0");
        while (1);
    }
    SYSTICK_LOAD = reload;
    SYSTICK_VAL = 0;  /* Clear to avoid a spurious first interrupt (ARM DDI 0337: any write clears VAL) */
    SYSTICK_CTRL = SYSTICK_ENABLE | SYSTICK_TICKINT | SYSTICK_CLKSOURCE;
}

/*
 * SysTick interrupt handler.
 *
 * This function is called by the CPU automatically — NOT by our code.
 * When SysTick counts down to zero:
 *   1. CPU saves registers onto the stack
 *   2. CPU looks up entry 15 in the vector table
 *   3. CPU jumps here
 *   4. We do our work (increment counter)
 *   5. We return — CPU restores registers, resumes whatever was running
 *
 * This is the key difference from polling: main() doesn't have to check
 * anything. The hardware interrupts it, runs this handler, and resumes.
 */
void SysTick_Handler(void)
{
    /* Safe: tick_count has a single writer (this ISR). No higher-priority ISR
     * can preempt SysTick on this target, so the LDR-ADD-STR sequence is
     * never torn. The volatile ensures main() re-reads from memory each iteration. */
    tick_count++;
}

/* ---- Main ---- */

int main(void)
{
    uart_init();

    uart_puts("SysTick interrupt demo\r\n");
    uart_puts("======================\r\n");
    uart_puts("Ticking at ");
    uart_put_uint(SYSTICK_HZ);
    uart_puts(" Hz for ");
    uart_put_uint(DEMO_SECONDS);
    uart_puts(" seconds (");
    uart_put_uint(TOTAL_TICKS);
    uart_puts(" ticks)...\r\n\r\n");

    systick_init(SYSTICK_HZ);

    uint32_t last_tick = 0;

    /* Main loop: print each tick in order. Advance last_tick one at a time
     * so no tick is skipped even if the ISR fires multiple times during
     * a slow UART print.
     * Note: on Cortex-M3, aligned 32-bit reads are naturally atomic, so
     * reading tick_count without a critical section is safe here. */
    while (1) {
        /* WFI: sleep until the next interrupt fires, saving power.
         * On QEMU this is a no-op, but on real hardware it stops
         * the CPU clock until SysTick (or any interrupt) wakes it. */
        __asm volatile("wfi");
        if (tick_count > last_tick) {
            last_tick++;
            uart_puts("  tick ");
            uart_put_uint(last_tick);
            uart_puts("\r\n");

            if (last_tick >= TOTAL_TICKS) {
                SYSTICK_CTRL = 0;  /* Disable SysTick before halting */
                uart_puts("\r\nDone — ");
                uart_put_uint(DEMO_SECONDS);
                uart_puts(" seconds counted by interrupt.\r\n");
                while (1);
            }
        }
    }
}
