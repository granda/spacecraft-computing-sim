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
	@OUTPUT=$$($(TIMEOUT_CMD) 10 $(QEMU) $(QEMU_FLAGS) -kernel $< 2>&1); \
	PASS=0; FAIL=0; \
	echo "$$OUTPUT" | grep -q "SysTick interrupt demo" \
		&& { echo "  PASS: boot banner"; PASS=$$((PASS+1)); } \
		|| { echo "  FAIL: boot banner"; FAIL=$$((FAIL+1)); }; \
	echo "$$OUTPUT" | grep -q "tick 1" \
		&& { echo "  PASS: first tick"; PASS=$$((PASS+1)); } \
		|| { echo "  FAIL: first tick"; FAIL=$$((FAIL+1)); }; \
	echo "$$OUTPUT" | grep -q "tick 5" \
		&& { echo "  PASS: mid tick"; PASS=$$((PASS+1)); } \
		|| { echo "  FAIL: mid tick"; FAIL=$$((FAIL+1)); }; \
	echo "$$OUTPUT" | grep -q "tick 10" \
		&& { echo "  PASS: final tick"; PASS=$$((PASS+1)); } \
		|| { echo "  FAIL: final tick"; FAIL=$$((FAIL+1)); }; \
	echo "$$OUTPUT" | grep -q "5 seconds counted by interrupt" \
		&& { echo "  PASS: completion message"; PASS=$$((PASS+1)); } \
		|| { echo "  FAIL: completion message"; FAIL=$$((FAIL+1)); }; \
	echo "$$OUTPUT" | grep -q "tick 11" \
		&& { echo "  FAIL: counted past 10"; FAIL=$$((FAIL+1)); } \
		|| { echo "  PASS: no extra ticks"; PASS=$$((PASS+1)); }; \
	TICK_COUNT=$$(echo "$$OUTPUT" | grep -c "^  tick "); \
	[ "$$TICK_COUNT" -eq 10 ] \
		&& { echo "  PASS: exactly 10 tick lines"; PASS=$$((PASS+1)); } \
		|| { echo "  FAIL: expected 10 tick lines, got $$TICK_COUNT"; FAIL=$$((FAIL+1)); }; \
	echo "$$PASS passed, $$FAIL failed"; \
	[ $$FAIL -eq 0 ]

clean:
	rm -rf $(BUILD)

.PHONY: all run test clean
