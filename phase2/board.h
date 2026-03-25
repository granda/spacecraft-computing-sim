#ifndef BOARD_H
#define BOARD_H

#include <stdint.h>

/* MPS2-AN385 UART0 base address */
#define UART0_BASE    0x40004000UL

/* UART0 register accessors */
#define UART0_DATA    (*(volatile uint32_t *)(UART0_BASE + 0x00))
#define UART0_STATE   (*(volatile uint32_t *)(UART0_BASE + 0x04))
#define UART0_CTRL    (*(volatile uint32_t *)(UART0_BASE + 0x08))
#define UART0_BAUDDIV (*(volatile uint32_t *)(UART0_BASE + 0x10))

#endif /* BOARD_H */
