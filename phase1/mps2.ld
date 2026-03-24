/* MPS2 AN385 memory layout: 4MB flash, 4MB RAM */
MEMORY
{
    FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 4M
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 4M
}

/* Entry point - the reset handler */
ENTRY(Reset_Handler)

SECTIONS
{
    /* Vector table and code go in flash */
    .text : {
        KEEP(*(.vector_table))   /* Vector table must be first */
        *(.text*)                /* All code */
        *(.rodata*)              /* Read-only data (strings, constants) */
        . = ALIGN(4);
        _etext = .;              /* End of code in flash — used by debuggers and memory map tools */
    } > FLASH

    /* Initialized global variables: stored in flash, copied to RAM at startup */
    .data : {
        . = ALIGN(4);
        _sdata = .;
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT > FLASH
    _sidata = LOADADDR(.data);

    /* Zero-initialized globals: just reserve space in RAM */
    .bss : {
        . = ALIGN(4);
        _sbss = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM

    /* Stack at top of RAM */
    _stack_top = ORIGIN(RAM) + LENGTH(RAM);

}

/* Ensure minimum stack space between end of BSS and top of RAM */
_min_stack_size = 0x400;
ASSERT((_stack_top - _ebss) >= _min_stack_size, "Not enough RAM for stack");
