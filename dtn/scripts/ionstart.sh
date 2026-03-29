#!/bin/bash
set -euo pipefail

CONFIG_DIR="${ION_CONFIG_DIR:-/ion/configs}"
BPECHO_PID=
CFDPCLOCK_PID=
BPUTA_PID=
SLEEP_PID=
ION_STARTED=false

# Graceful shutdown on SIGTERM/SIGINT (registered early to avoid race)
cleanup() {
    echo "Stopping ION..."
    [ -n "$BPECHO_PID" ] && kill "$BPECHO_PID" 2>/dev/null || true
    [ -n "$CFDPCLOCK_PID" ] && kill "$CFDPCLOCK_PID" 2>/dev/null || true
    [ -n "$BPUTA_PID" ] && kill "$BPUTA_PID" 2>/dev/null || true
    [ -n "$SLEEP_PID" ] && kill "$SLEEP_PID" 2>/dev/null || true
    if [ "$ION_STARTED" = true ]; then timeout 10 ionstop 2>/dev/null || true; fi
}
trap cleanup SIGTERM SIGINT

echo "Starting ION node ${ION_NODE_NUM}..."

# Use combined config file with ionstart -I
if ! ionstart -I "$CONFIG_DIR/node.rc"; then
    echo "ionstart failed. Config: $CONFIG_DIR/node.rc"
    cat /ion/ion.log 2>/dev/null || true
    exit 1
fi
ION_STARTED=true

# Endpoint convention: ipn:N.1 = bpecho (ping), ipn:N.2 = file transfer tests
# The Docker healthcheck (pgrep -x bpecho) is the canonical liveness signal.
# This brief check catches immediate startup crashes only.
bpecho ipn:"${ION_NODE_NUM}".1 &
BPECHO_PID=$!
sleep 2
if ! kill -0 "$BPECHO_PID" 2>/dev/null; then
    echo "ERROR: bpecho crashed on startup" >&2
    exit 1
fi

# Start CFDP daemons — cfdpclock manages timers, bputa bridges CFDP to BP.
# Note: 's bputa N' in cfdpadmin only sets the UTL command name; it does NOT
# launch bputa. The daemon must be started explicitly here.
# Both are checked in the Docker healthcheck, so treat failures as fatal.
cfdpclock &
CFDPCLOCK_PID=$!
bputa &
BPUTA_PID=$!

# Poll for CFDP daemons rather than bare sleep (matches bpecho pattern above)
for _ in {1..4}; do
    sleep 1
    kill -0 "$CFDPCLOCK_PID" 2>/dev/null && kill -0 "$BPUTA_PID" 2>/dev/null && break
done
if ! kill -0 "$CFDPCLOCK_PID" 2>/dev/null; then
    echo "ERROR: cfdpclock crashed on startup" >&2
    exit 1
fi
if ! kill -0 "$BPUTA_PID" 2>/dev/null; then
    echo "ERROR: bputa crashed on startup" >&2
    exit 1
fi

echo "ION node ready (node ${ION_NODE_NUM})"

# Keep container running
sleep infinity &
SLEEP_PID=$!

wait $SLEEP_PID || true
