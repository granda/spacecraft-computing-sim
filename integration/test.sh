#!/bin/bash
# Integration telemetry test assertions.
# Reads QEMU output from stdin, checks boot, format, and calibration values.
# No -e: we accumulate failures and exit via [ $FAIL -eq 0 ] at the end.
set -uo pipefail

OUTPUT=$(cat)
PASS=0
FAIL=0

pass() { echo "  PASS: $1"; PASS=$((PASS+1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }

# --- Early guard: detect empty QEMU output ---

if [ -z "$OUTPUT" ]; then
    fail "no QEMU output captured — UART init or QEMU launch may have failed"
    echo "$PASS passed, $FAIL failed"
    exit 1
fi

# --- Boot ---

printf '%s\n' "$OUTPUT" | grep -q "# Spacecraft Telemetry Firmware" \
    && pass "boot banner" \
    || fail "boot banner"

# --- Telemetry presence ---

printf '%s\n' "$OUTPUT" | grep -q '^\$TELEM,' \
    && pass "telemetry lines present" \
    || fail "telemetry lines present"

printf '%s\n' "$OUTPUT" | grep -q '^\$TELEM,[0-9]*,TEMP,' \
    && pass "TEMP telemetry" \
    || fail "TEMP telemetry"

printf '%s\n' "$OUTPUT" | grep -q '^\$TELEM,[0-9]*,GYRO,' \
    && pass "GYRO telemetry" \
    || fail "GYRO telemetry"

printf '%s\n' "$OUTPUT" | grep -q '^\$TELEM,[0-9]*,BATT,' \
    && pass "BATT telemetry" \
    || fail "BATT telemetry"

printf '%s\n' "$OUTPUT" | grep -q '^\$TELEM,[0-9]*,SUN,' \
    && pass "SUN telemetry" \
    || fail "SUN telemetry"

# --- Volume (proves continuous operation, not fixed-count halt) ---

TELEM_COUNT=$(printf '%s\n' "$OUTPUT" | grep -c '^\$TELEM,')
[ "$TELEM_COUNT" -ge 20 ] \
    && pass "at least 20 telemetry lines (got $TELEM_COUNT)" \
    || fail "expected >=20 telemetry lines, got $TELEM_COUNT"

# --- CSV format (5 comma-separated fields) ---

FIELD_CHECK=$(printf '%s\n' "$OUTPUT" | grep '^\$TELEM,' | head -1 | awk -F, '{print NF}')
if [ -n "$FIELD_CHECK" ] && [ "$FIELD_CHECK" -eq 5 ]; then
    pass "telemetry has 5 CSV fields"
else
    fail "expected 5 fields, got '${FIELD_CHECK:-<none>}'"
fi

# --- Sequence number format (4-digit zero-padded, 0000-9999) ---

SEQ_FIELD=$(printf '%s\n' "$OUTPUT" | grep '^\$TELEM,' | head -1 | cut -d, -f2)
if printf '%s\n' "$SEQ_FIELD" | grep -qE '^[0-9]{4}$'; then
    pass "seq field is 4-digit zero-padded (got $SEQ_FIELD)"
else
    fail "seq field not 4-digit zero-padded (got $SEQ_FIELD)"
fi

# --- Calibrated value ranges ---
# Each range is derived from the raw simulation range + calibration constant
# in telemetry.h and main.c.  Update these if those constants change.

check_range() {
    local name=$1 min=$2 max=$3 source=$4
    local val
    val=$(printf '%s\n' "$OUTPUT" | grep "^\$TELEM,[0-9]*,${name}," | head -1 | cut -d, -f4)
    if [ -z "$val" ]; then
        fail "$name value not found"
    elif [ "$val" -ge "$min" ] && [ "$val" -le "$max" ]; then
        pass "$name value in range (got $val)"
    else
        fail "$name value out of range (got $val, expected $min-$max) [$source]"
    fi
}

# TEMP: raw 2000-2499 + TEMP_CAL_OFFSET(-10) = 1990-2489
check_range TEMP 1990 2489 "raw 2000-2499 + offset -10"

# GYRO: raw 150-199 * GYRO_SCALE_FACTOR(2) = 300-398
check_range GYRO 300 398 "raw 150-199 * scale 2"

# BATT: raw 2800-2849 + BATT_CAL_OFFSET(5) = 2805-2854
check_range BATT 2805 2854 "raw 2800-2849 + offset 5"

# SUN: raw 800-899, passthrough (no calibration)
check_range SUN 800 899 "raw 800-899, passthrough"

# --- Pipeline health (no drops or overruns) ---

printf '%s\n' "$OUTPUT" | grep -q '^# Drops:' \
    && fail "queue drops detected" \
    || pass "no queue drops"

printf '%s\n' "$OUTPUT" | grep -q '^# Overruns:' \
    && fail "deadline overruns detected" \
    || pass "no deadline overruns"

# --- Summary ---

echo "$PASS passed, $FAIL failed"
[ $FAIL -eq 0 ]
