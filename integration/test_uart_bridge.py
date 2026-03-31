#!/usr/bin/env python3
"""End-to-end test: QEMU firmware -> UART bridge -> spacecraft -> ground station.

Verifies that real FreeRTOS telemetry flows from QEMU through the UART bridge
into the spacecraft ION node and arrives at the ground station over DTN.
"""

import argparse
import shlex
import signal
import subprocess
import sys
import time
from pathlib import Path

INTEGRATION_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = INTEGRATION_DIR.parent

# Shared helpers live in dtn/scripts/test_basic.py (sibling directory).
# Same import pattern as test_ground_station.py.
sys.path.insert(0, str(PROJECT_ROOT / "dtn" / "scripts"))
from test_basic import (  # noqa: E402
    compose_exec,
    poll_sleep,
    run,
    wait_for_nodes,
)

COMPOSE = ["docker", "compose", "--project-directory", str(INTEGRATION_DIR)]
NODES = ["spacecraft", "ground-station"]

BRIDGE_DURATION = 15  # seconds to run the bridge


def cleanup():
    result = subprocess.run(
        [*COMPOSE, "down", "--timeout", "5", "--volumes"],
        capture_output=True, text=True, timeout=30,
    )
    if result.returncode != 0 and result.stderr:
        print(f"  cleanup stderr: {result.stderr.strip()}", file=sys.stderr)


def main():
    passed = 0
    failed = 0

    def check(desc, ok):
        nonlocal passed, failed
        if ok:
            print(f"  PASS: {desc}")
            passed += 1
        else:
            print(f"  FAIL: {desc}")
            failed += 1

    print("Running UART bridge end-to-end tests...")

    # ── Phase 1: Start DTN containers ──
    run([*COMPOSE, "up", "-d"], timeout=600, check=True)

    if not wait_for_nodes(compose=COMPOSE, nodes=NODES):
        print("Startup failed — 0 tests run. Run 'docker compose logs' in integration/ to debug.")
        return False

    # LTP warmup
    ltp_ready = False
    for i in range(15):
        try:
            output = compose_exec(
                "spacecraft", "bping -c 1 -i 1 ipn:1.2 ipn:2.1",
                timeout=15, compose=COMPOSE,
            )
            if "bundles received" in output:
                ltp_ready = True
                break
        except (RuntimeError, subprocess.TimeoutExpired):
            pass
        poll_sleep(i)
    if not ltp_ready:
        print("  WARNING: LTP warmup did not converge — tests may fail")

    # ── Phase 2: Build firmware ──
    run(["make", "-C", str(INTEGRATION_DIR), "build/firmware.elf"], timeout=60, check=True)

    # ── Phase 3: Start bprecvfile on ground station ──
    # Clean stale testfiles from any previous --keep-on-failure run to avoid
    # false positives (bprecvfile resets its counter each invocation).
    compose_exec("ground-station", "rm -f /tmp/testfile*", compose=COMPOSE)

    # bprecvfile <eid> <count> receives up to <count> files as /tmp/testfile1, testfile2, ...
    # Runs backgrounded (&) in the container; the `timeout` wrapper ensures it
    # doesn't outlive the test.  On --keep-on-failure the container stays up and
    # the process will be cleaned up by `docker compose down` on the next run.
    compose_exec(
        "ground-station",
        f"cd /tmp && timeout {BRIDGE_DURATION + 30} bprecvfile ipn:2.3 100 &",
        timeout=10, compose=COMPOSE,
    )
    time.sleep(1)  # let bprecvfile register

    # ── Phase 4: Launch bridge ──
    bridge_proc = subprocess.Popen(
        [sys.executable, str(INTEGRATION_DIR / "uart_bridge.py"),
         "--kernel", str(INTEGRATION_DIR / "build" / "firmware.elf"),
         "--duration", str(BRIDGE_DURATION)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )

    # ── Phase 5: Wait for bridge to finish ──
    try:
        stdout, stderr = bridge_proc.communicate(timeout=BRIDGE_DURATION + 30)
    except subprocess.TimeoutExpired:
        bridge_proc.terminate()
        stdout, stderr = bridge_proc.communicate(timeout=10)

    print(f"  Bridge stderr:\n{stderr.strip()}" if stderr.strip() else "  (no bridge stderr)")

    # Parse bridge stats from the "STATS: k=v k=v ..." line on stdout.
    # Only that prefixed line is parsed, so other stdout output won't corrupt stats.
    stats = {}
    for output_line in stdout.strip().split("\n"):
        if output_line.startswith("STATS: "):
            for part in output_line[len("STATS: "):].split():
                if "=" in part:
                    k, v = part.split("=", 1)
                    try:
                        stats[k] = int(v)
                    except ValueError:
                        pass
            break

    # ── Phase 6: Assertions ──

    # Test 1: Bridge exited cleanly
    check("bridge exited cleanly", bridge_proc.returncode == 0)

    # Test 2: Bridge read telemetry lines
    check("bridge read telemetry lines", stats.get("telem_lines", 0) > 0)

    # Test 3: Bridge sent >= 2 bundles
    bundles_sent = stats.get("bundles_sent", 0)
    check(f"bridge sent >= 2 bundles (got {bundles_sent})", bundles_sent >= 2)

    # Poll for bundle delivery instead of a fixed sleep
    received_files = []
    for i in range(15):
        ls_output = compose_exec("ground-station", "ls /tmp/testfile* 2>/dev/null || true",
                                 compose=COMPOSE)
        received_files = [f.strip() for f in ls_output.strip().split("\n") if f.strip()]
        if received_files:
            break
        poll_sleep(i)

    # Test 4: Ground station received at least one file
    check(f"ground station received files (got {len(received_files)})", len(received_files) > 0)

    # Read all received telemetry
    telem_content = ""
    for fpath in received_files:
        content = compose_exec("ground-station", f"cat {shlex.quote(fpath)}", compose=COMPOSE)
        telem_content += content

    telem_lines = [line for line in telem_content.strip().split("\n") if line.startswith("$TELEM,")]

    # Test 5: Received content contains $TELEM lines
    check(f"received telemetry lines (got {len(telem_lines)})", len(telem_lines) > 0)

    # Test 6: Contains TEMP sensor data
    has_temp = any(",TEMP," in line for line in telem_lines)
    check("received TEMP sensor data", has_temp)

    # Test 7: Contains GYRO sensor data
    has_gyro = any(",GYRO," in line for line in telem_lines)
    check("received GYRO sensor data", has_gyro)

    # Test 8: Contains BATT sensor data
    has_batt = any(",BATT," in line for line in telem_lines)
    check("received BATT sensor data", has_batt)

    # Test 9: Contains SUN sensor data
    has_sun = any(",SUN," in line for line in telem_lines)
    check("received SUN sensor data", has_sun)

    # Test 10: Valid CSV format (5 comma-separated fields)
    csv_ok = all(len(line.split(",")) == 5 for line in telem_lines) if telem_lines else False
    check("telemetry CSV format valid (5 fields)", csv_ok)

    # Test 11: Calibrated value ranges
    # Ranges derived from telemetry.h calibration constants and raw sensor ranges:
    #   TEMP: raw 2000-2499 + TEMP_CAL_OFFSET(-10)  = 1990-2489
    #   GYRO: raw 150-199   * GYRO_SCALE_FACTOR(2)  = 300-398
    #   BATT: raw 2800-2849 + BATT_CAL_OFFSET(5)    = 2805-2854
    #   SUN:  raw 800-899   (passthrough)            = 800-899
    ranges_ok = bool(telem_lines)
    for line in telem_lines:
        parts = line.split(",")
        if len(parts) != 5:
            ranges_ok = False
            break
        try:
            sensor, value = parts[2], int(parts[3])
        except ValueError:
            ranges_ok = False
            break
        if sensor == "TEMP" and not (1990 <= value <= 2489):
            ranges_ok = False
        elif sensor == "GYRO" and not (300 <= value <= 398):
            ranges_ok = False
        elif sensor == "BATT" and not (2805 <= value <= 2854):
            ranges_ok = False
        elif sensor == "SUN" and not (800 <= value <= 899):
            ranges_ok = False
    check("calibrated values in expected ranges", ranges_ok)

    print(f"{passed} passed, {failed} failed")
    return failed == 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="UART bridge end-to-end test")
    parser.add_argument("--keep-on-failure", action="store_true",
                        help="Leave containers running on failure for inspection")
    args = parser.parse_args()

    signal.signal(signal.SIGINT, lambda *_: sys.exit(130))
    success = False
    try:
        success = main()
    except (RuntimeError, subprocess.TimeoutExpired) as e:
        print(f"FATAL: {e}", file=sys.stderr)
        success = False
    finally:
        if args.keep_on_failure and not success:
            print("  Containers left running for inspection. Run 'make -C integration clean-ground-station' when done.")
        else:
            cleanup()
    sys.exit(0 if success else 1)
