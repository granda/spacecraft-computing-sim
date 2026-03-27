CC          = arm-none-eabi-gcc
SIZE        = arm-none-eabi-size
QEMU        = qemu-system-arm
QEMU_FLAGS  = -machine mps2-an385 -cpu cortex-m3 -nographic
TIMEOUT_CMD ?= timeout
TIMEOUT     ?= 10

BUILD       = build

# FreeRTOS kernel (git submodule — kernel-only repo)
KERNEL      = freertos-kernel
PORT        = $(KERNEL)/portable/GCC/ARM_CM3

# Source files
SRCS  = main.c
SRCS += startup_gcc.c
SRCS += printf-stdarg.c
# FreeRTOS kernel
SRCS += $(KERNEL)/tasks.c
SRCS += $(KERNEL)/list.c
SRCS += $(KERNEL)/queue.c
# timers.c omitted — configUSE_TIMERS is 0, no software timers in this demo
# event_groups.c and stream_buffer.c omitted — not used in this demo
SRCS += $(KERNEL)/portable/MemMang/heap_4.c
SRCS += $(PORT)/port.c

# Compiler flags
CFLAGS  = -mthumb -mcpu=cortex-m3 -ffreestanding
CFLAGS += -ffunction-sections -fdata-sections
CFLAGS += -Wall -Wextra -Werror -g -Os
CFLAGS += -I. -I$(KERNEL)/include -I$(PORT)

# Linker flags
LDFLAGS  = -T mps2_m3.ld
LDFLAGS += -nostartfiles -specs=nano.specs -specs=nosys.specs
LDFLAGS += -Wl,--gc-sections -Wl,-Map=$(BUILD)/firmware.map

# Object files in build dir
OBJS = $(SRCS:%.c=$(BUILD)/%.o)

all: $(BUILD)/firmware.elf
	$(SIZE) $<

$(BUILD)/firmware.elf: $(OBJS) mps2_m3.ld
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) -o $@

$(BUILD)/%.o: %.c | dirs
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

dirs:
	@mkdir -p $(BUILD)

run: $(BUILD)/firmware.elf
	$(QEMU) $(QEMU_FLAGS) -kernel $<

test: $(BUILD)/firmware.elf
	@echo "Running QEMU integration tests..."
	@OUTPUT=$$($(TIMEOUT_CMD) $(TIMEOUT) $(QEMU) $(QEMU_FLAGS) -kernel $< 2>&1); \
	PASS=0; FAIL=0; \
	echo "$$OUTPUT" | grep -q "Sensor Pipeline Demo" \
		&& { echo "  PASS: boot banner"; PASS=$$((PASS+1)); } \
		|| { echo "  FAIL: boot banner"; FAIL=$$((FAIL+1)); }; \
	echo "$$OUTPUT" | grep -q "\[TEMP\].*Sensor online" \
		&& { echo "  PASS: temp sensor started"; PASS=$$((PASS+1)); } \
		|| { echo "  FAIL: temp sensor started"; FAIL=$$((FAIL+1)); }; \
	echo "$$OUTPUT" | grep -q "\[GYRO\].*Sensor online" \
		&& { echo "  PASS: gyro sensor started"; PASS=$$((PASS+1)); } \
		|| { echo "  FAIL: gyro sensor started"; FAIL=$$((FAIL+1)); }; \
	echo "$$OUTPUT" | grep -q "\[TELEM\].*TEMP:" \
		&& { echo "  PASS: temp telemetry received"; PASS=$$((PASS+1)); } \
		|| { echo "  FAIL: temp telemetry received"; FAIL=$$((FAIL+1)); }; \
	echo "$$OUTPUT" | grep -q "\[TELEM\].*GYRO:" \
		&& { echo "  PASS: gyro telemetry received"; PASS=$$((PASS+1)); } \
		|| { echo "  FAIL: gyro telemetry received"; FAIL=$$((FAIL+1)); }; \
	echo "$$OUTPUT" | grep -q "Pipeline complete.*processed.*dropped" \
		&& { echo "  PASS: pipeline completed"; PASS=$$((PASS+1)); } \
		|| { echo "  FAIL: pipeline completed"; FAIL=$$((FAIL+1)); }; \
	TELEM_COUNT=$$(echo "$$OUTPUT" | grep -c "\[TELEM\] #"); \
	[ "$$TELEM_COUNT" -eq 30 ] \
		&& { echo "  PASS: all 30 telemetry readings printed (got $$TELEM_COUNT)"; PASS=$$((PASS+1)); } \
		|| { echo "  FAIL: expected 30 telemetry readings, got $$TELEM_COUNT"; FAIL=$$((FAIL+1)); }; \
	echo "$$PASS passed, $$FAIL failed"; \
	[ $$FAIL -eq 0 ]

clean:
	rm -rf $(BUILD)

.PHONY: all run test clean dirs
