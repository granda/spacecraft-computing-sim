/*
 * Bare-metal Hello UART for MPS2-AN385 (Cortex-M3)
 *
 * This writes directly to hardware registers — no OS, no standard library,
 * no printf. Just poking a memory address to send bytes out a serial port.
 *
 * The CMSDK UART on MPS2-AN385 is dead simple:
 *   - Write a byte to 0x40004000 (data register) and it gets transmitted
 *   - Read 0x40004004 (state register) bit 0 to check if TX buffer is full
 */

#include <stdint.h>

/*
 * UART0 register addresses — defined by the CMSDK APB UART hardware.
 *
 * Register map:
 *   0x00  DATA      Write a byte here to transmit it
 *   0x04  STATE     Bit 0 = TX buffer full, Bit 1 = RX buffer full
 *   0x08  CTRL      Bit 0 = TX enable, Bit 1 = RX enable
 *   0x0C  INTSTATUS Write to clear interrupt flags
 *   0x10  BAUDDIV   Baud rate divisor (must be >= 16)
 *
 * volatile tells the compiler "this memory location can change outside
 * my control (it's hardware), so don't optimize away reads/writes to it."
 */
#define UART0_BASE    0x40004000
#define UART0_DATA    (*(volatile uint32_t *)(UART0_BASE + 0x00))
#define UART0_STATE   (*(volatile uint32_t *)(UART0_BASE + 0x04))
#define UART0_CTRL    (*(volatile uint32_t *)(UART0_BASE + 0x08))
#define UART0_BAUDDIV (*(volatile uint32_t *)(UART0_BASE + 0x10))

#define UART_STATE_TXFULL  (1 << 0)
#define UART_CTRL_TX_EN    (1 << 0)

static void uart_init(void)
{
    /* 25 MHz APB clock / 115200 baud = ~217 (QEMU doesn't enforce exact rate) */
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

int main(void)
{
    uart_init();
    uart_puts("Hello from bare-metal Cortex-M3!\r\n");
    uart_puts("No OS. No standard library. Just registers.\r\n");

    /* Nothing else to do — hang forever */
    while (1);
}
