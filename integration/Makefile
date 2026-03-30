CC          = arm-none-eabi-gcc
SIZE        = arm-none-eabi-size
QEMU        = qemu-system-arm
QEMU_FLAGS  = -machine mps2-an385 -cpu cortex-m3 -nographic
TIMEOUT_CMD ?= timeout
TIMEOUT     ?= 15

BUILD       = build

# Shared board support and FreeRTOS kernel from freertos/.
# This firmware reuses startup_gcc.c, printf-stdarg.c, board.h,
# FreeRTOSConfig.h, and mps2_m3.ld from the freertos/ directory
# to avoid duplicating hardware-level plumbing.  CI triggers on
# freertos/** changes to catch breakage from shared-file edits.
FREERTOS    = ../freertos
KERNEL      = $(FREERTOS)/freertos-kernel
PORT        = $(KERNEL)/portable/GCC/ARM_CM3

# Object files (flat in build/)
OBJS  = $(BUILD)/main.o
OBJS += $(BUILD)/startup_gcc.o
OBJS += $(BUILD)/printf-stdarg.o
OBJS += $(BUILD)/tasks.o
OBJS += $(BUILD)/list.o
OBJS += $(BUILD)/queue.o
OBJS += $(BUILD)/heap_4.o
OBJS += $(BUILD)/port.o

# vpath: let make find .c files in shared directories
vpath %.c . $(FREERTOS) $(KERNEL) $(KERNEL)/portable/MemMang $(PORT)

# Compiler flags
CFLAGS  = -mthumb -mcpu=cortex-m3 -ffreestanding
CFLAGS += -ffunction-sections -fdata-sections
CFLAGS += -Wall -Wextra -Werror -g -Os
CFLAGS += -I. -I$(FREERTOS) -I$(KERNEL)/include -I$(PORT)

# Linker flags
LDFLAGS  = -T $(FREERTOS)/mps2_m3.ld
LDFLAGS += -nostartfiles -specs=nano.specs -specs=nosys.specs
LDFLAGS += -Wl,--gc-sections -Wl,-Map=$(BUILD)/firmware.map

all: $(BUILD)/firmware.elf
	$(SIZE) $<

$(BUILD)/firmware.elf: $(OBJS) $(FREERTOS)/mps2_m3.ld
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) -o $@

$(BUILD)/%.o: %.c | dirs
	$(CC) $(CFLAGS) -c $< -o $@

dirs:
	@mkdir -p $(BUILD)

run: $(BUILD)/firmware.elf
	$(QEMU) $(QEMU_FLAGS) -kernel $<

test: $(BUILD)/firmware.elf
	@echo "Running integration telemetry tests..."
	@$(TIMEOUT_CMD) $(TIMEOUT) $(QEMU) $(QEMU_FLAGS) -kernel $< 2>/dev/null \
		| bash test.sh

clean:
	rm -rf $(BUILD)

.PHONY: all run test clean dirs
