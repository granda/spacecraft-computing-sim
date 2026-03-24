/*
 * Cortex-M3 startup code for MPS2-AN385
 *
 * The vector table sits at address 0x00000000. On reset, the CPU:
 *   1. Loads the stack pointer from address 0x00000000
 *   2. Loads the PC (program counter) from address 0x00000004 (Reset_Handler)
 *   3. Starts executing Reset_Handler
 *
 * This is fundamentally different from x86/Linux where the OS sets up your
 * stack and calls main(). Here, WE are the first code that runs.
 */

#include <stdint.h>

/* These symbols are defined in the linker script (mps2.ld) */
extern uint32_t _stack_top;
extern uint32_t _sidata;  /* Start of .data in flash */
extern uint32_t _sdata;   /* Start of .data in RAM */
extern uint32_t _edata;   /* End of .data in RAM */
extern uint32_t _sbss;    /* Start of .bss */
extern uint32_t _ebss;    /* End of .bss */

/* Forward declarations */
extern int main(void);
__attribute__((noreturn)) void Reset_Handler(void);
__attribute__((noreturn)) void Default_Handler(void);

/*
 * Vector table: an array of function pointers.
 * The Cortex-M3 has a fixed layout — first entry is the initial stack pointer,
 * then the exception handlers in a specific order.
 *
 * __attribute__((section(".vector_table"), used)) places this in the section
 * that our linker script puts at the very start of flash. "used" prevents
 * the compiler/LTO from discarding the symbol before the linker sees it.
 */
__attribute__((section(".vector_table"), used))
const uint32_t vector_table[] = {
    (uint32_t)(uintptr_t)&_stack_top,       /* Initial stack pointer */
    (uint32_t)(uintptr_t)Reset_Handler,     /* Reset handler — entry point */
    (uint32_t)(uintptr_t)Default_Handler,   /* NMI */
    (uint32_t)(uintptr_t)Default_Handler,   /* HardFault */
    (uint32_t)(uintptr_t)Default_Handler,   /* MemManage */
    (uint32_t)(uintptr_t)Default_Handler,   /* BusFault */
    (uint32_t)(uintptr_t)Default_Handler,   /* UsageFault */
    0, 0, 0, 0,                             /* Reserved */
    (uint32_t)(uintptr_t)Default_Handler,   /* SVCall */
    (uint32_t)(uintptr_t)Default_Handler,   /* DebugMonitor */
    0,                                       /* Reserved */
    (uint32_t)(uintptr_t)Default_Handler,   /* PendSV */
    (uint32_t)(uintptr_t)Default_Handler,   /* SysTick */
    /* Device-specific IRQs (MPS2-AN385 / CMSDK) */
    (uint32_t)(uintptr_t)Default_Handler,   /* IRQ 0:  UART0 RX */
    (uint32_t)(uintptr_t)Default_Handler,   /* IRQ 1:  UART0 TX */
    (uint32_t)(uintptr_t)Default_Handler,   /* IRQ 2:  UART1 RX */
    (uint32_t)(uintptr_t)Default_Handler,   /* IRQ 3:  UART1 TX */
    (uint32_t)(uintptr_t)Default_Handler,   /* IRQ 4:  UART2 RX */
    (uint32_t)(uintptr_t)Default_Handler,   /* IRQ 5:  UART2 TX */
    (uint32_t)(uintptr_t)Default_Handler,   /* IRQ 6:  GPIO 0 combined */
    (uint32_t)(uintptr_t)Default_Handler,   /* IRQ 7:  GPIO 1 combined */
    (uint32_t)(uintptr_t)Default_Handler,   /* IRQ 8:  Timer 0 */
    (uint32_t)(uintptr_t)Default_Handler,   /* IRQ 9:  Timer 1 */
    (uint32_t)(uintptr_t)Default_Handler,   /* IRQ 10: Dual Timer */
    (uint32_t)(uintptr_t)Default_Handler,   /* IRQ 11: SPI */
    (uint32_t)(uintptr_t)Default_Handler,   /* IRQ 12: UART overflow */
    (uint32_t)(uintptr_t)Default_Handler,   /* IRQ 13: Ethernet */
    (uint32_t)(uintptr_t)Default_Handler,   /* IRQ 14: Audio I2S */
    (uint32_t)(uintptr_t)Default_Handler,   /* IRQ 15: Touch Screen */
    (uint32_t)(uintptr_t)Default_Handler,   /* IRQ 16: GPIO 0 bit 0 */
    (uint32_t)(uintptr_t)Default_Handler,   /* IRQ 17: GPIO 0 bit 1 */
    (uint32_t)(uintptr_t)Default_Handler,   /* IRQ 18: GPIO 0 bit 2 */
    (uint32_t)(uintptr_t)Default_Handler,   /* IRQ 19: GPIO 0 bit 3 */
    (uint32_t)(uintptr_t)Default_Handler,   /* IRQ 20: GPIO 0 bit 4 */
    (uint32_t)(uintptr_t)Default_Handler,   /* IRQ 21: GPIO 0 bit 5 */
    (uint32_t)(uintptr_t)Default_Handler,   /* IRQ 22: GPIO 0 bit 6 */
    (uint32_t)(uintptr_t)Default_Handler,   /* IRQ 23: GPIO 0 bit 7 */
    (uint32_t)(uintptr_t)Default_Handler,   /* IRQ 24: GPIO 0 bit 8 */
    (uint32_t)(uintptr_t)Default_Handler,   /* IRQ 25: GPIO 0 bit 9 */
    (uint32_t)(uintptr_t)Default_Handler,   /* IRQ 26: GPIO 0 bit 10 */
    (uint32_t)(uintptr_t)Default_Handler,   /* IRQ 27: GPIO 0 bit 11 */
    (uint32_t)(uintptr_t)Default_Handler,   /* IRQ 28: GPIO 0 bit 12 */
    (uint32_t)(uintptr_t)Default_Handler,   /* IRQ 29: GPIO 0 bit 13 */
    (uint32_t)(uintptr_t)Default_Handler,   /* IRQ 30: GPIO 0 bit 14 */
    (uint32_t)(uintptr_t)Default_Handler,   /* IRQ 31: GPIO 0 bit 15 */
};

/*
 * Reset_Handler: the C runtime setup.
 *
 * In Python, the interpreter handles all of this for you.
 * In bare-metal C, we must manually:
 *   1. Copy initialized globals from flash to RAM
 *   2. Zero out uninitialized globals (BSS)
 *   3. Then call main()
 */
__attribute__((noreturn)) void Reset_Handler(void)
{
    /* Copy .data section from flash to RAM */
    uint32_t *src = &_sidata;
    uint32_t *dst = &_sdata;
    while (dst < &_edata) {
        *dst++ = *src++;
    }

    /* Zero out .bss section */
    dst = &_sbss;
    while (dst < &_ebss) {
        *dst++ = 0;
    }

    main();

    /* If main() returns, hang here */
    while (1);
}

/*
 * Default handler: halt under debugger, then hang.
 * Note: on real hardware without a debugger, bkpt escalates to HardFault.
 * Replace with WFI when targeting physical boards.
 */
__attribute__((noreturn)) void Default_Handler(void)
{
    __asm volatile("bkpt #0");
    while (1);
}
