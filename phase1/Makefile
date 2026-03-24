CC         = arm-none-eabi-gcc
OBJCOPY    = arm-none-eabi-objcopy
QEMU       = qemu-system-arm
QEMU_FLAGS = -machine mps2-an385 -cpu cortex-m3 -nographic
TIMEOUT_CMD ?= timeout

CFLAGS  = -mcpu=cortex-m3 -mthumb -ffreestanding -ffunction-sections -fdata-sections -Wall -Wextra -Wpedantic -g -Os
LDFLAGS = -T mps2.ld -nostdlib -Wl,--gc-sections -Wl,-Map=$(BUILD)/firmware.map

SRCS    = startup.c main.c
BUILD   = build

all: $(BUILD)/firmware.elf

$(BUILD)/firmware.elf: $(SRCS) mps2.ld
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) $(SRCS) -o $@

run: $(BUILD)/firmware.elf
	$(QEMU) $(QEMU_FLAGS) -kernel $<

test: $(BUILD)/firmware.elf
	@echo "Running QEMU integration tests..."
	@OUTPUT=$$($(TIMEOUT_CMD) 3 $(QEMU) $(QEMU_FLAGS) -kernel $< 2>&1); \
	PASS=0; FAIL=0; \
	echo "$$OUTPUT" | grep -q "Hello from bare-metal Cortex-M3!" \
		&& { echo "  PASS: boot message"; PASS=$$((PASS+1)); } \
		|| { echo "  FAIL: boot message"; FAIL=$$((FAIL+1)); }; \
	echo "$$OUTPUT" | grep -q "No OS. No standard library. Just registers." \
		&& { echo "  PASS: second line"; PASS=$$((PASS+1)); } \
		|| { echo "  FAIL: second line"; FAIL=$$((FAIL+1)); }; \
	echo "$$PASS passed, $$FAIL failed"; \
	[ $$FAIL -eq 0 ]

clean:
	rm -rf $(BUILD)

.PHONY: all run test clean
