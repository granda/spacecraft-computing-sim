#!/usr/bin/env python3
"""End-to-end test: telemetry delivery under simulated Mars-distance delay.

Starts ION nodes with 5s OWLT config, applies matching tc netem delay to
the spacecraft container's eth0, then verifies telemetry flows from QEMU
through the UART bridge and arrives at the ground station despite the delay.

Requires NET_ADMIN capability on spacecraft (set in docker-compose.delay.yml).
"""

import argparse
import re
import shlex
import signal
import subprocess
import sys
import time
from pathlib import Path

INTEGRATION_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = INTEGRATION_DIR.parent

sys.path.insert(0, str(PROJECT_ROOT / "dtn" / "scripts"))
from test_basic import (  # noqa: E402
    compose_exec as _compose_exec,
    poll_sleep,
    run,
    wait_for_nodes,
)

COMPOSE_DELAY = [
    "docker", "compose", "--project-directory", str(INTEGRATION_DIR),
    "-f", str(INTEGRATION_DIR / "docker-compose.yml"),
    "-f", str(INTEGRATION_DIR / "docker-compose.delay.yml"),
]
NODES = ["spacecraft", "ground-station"]

MARS_DELAY_MS = 5000   # tc netem delay — must match node_delay.rc OWLT (5s)
BRIDGE_DURATION = 30   # seconds to run the bridge (longer than baseline to allow LTP round-trips)
RECV_TIMEOUT = 300     # bprecvfile timeout — must outlast bridge + delivery polling (bridge=30s + poll≤150s)


def compose_exec(node, cmd, timeout=30, check=False):
    """Run a command inside a delay-configured container."""
    return _compose_exec(node, cmd, timeout=timeout, check=check, compose=COMPOSE_DELAY)


def cleanup():
    result = subprocess.run(
        [*COMPOSE_DELAY, "down", "--timeout", "5", "--volumes"],
        capture_output=True, text=True, timeout=30,
    )
    if result.returncode != 0 and result.stderr:
        print(f"  cleanup stderr: {result.stderr.strip()}", file=sys.stderr)


# ---------------------------------------------------------------------------
# tc netem helpers (same pattern as dtn/scripts/test_degraded.py).
# ---------------------------------------------------------------------------

def apply_netem(node, rule):
    """Add a tc netem qdisc to the container's eth0 (idempotent)."""
    compose_exec(node, "tc qdisc del dev eth0 root 2>/dev/null || true")
    compose_exec(node, f"tc qdisc add dev eth0 root netem {rule}", check=True)


def netem_stats(node):
    """Return tc -s qdisc output for eth0."""
    return compose_exec(node, "tc -s qdisc show dev eth0")


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

    print("Running Mars-delay end-to-end tests...")

    # ── Phase 1: Start DTN containers with delay config ──
    run([*COMPOSE_DELAY, "up", "-d"], timeout=600, check=True)

    if not wait_for_nodes(compose=COMPOSE_DELAY, nodes=NODES):
        print("Startup failed — 0 tests run. Run 'docker compose logs' in integration/ to debug.")
        return False

    # LTP warmup — round trip is now ~10s, so allow more time per attempt
    ltp_ready = False
    for i in range(20):
        try:
            output = compose_exec(
                "spacecraft", "bping -c 1 -i 1 ipn:1.2 ipn:2.1",
                timeout=30,
            )
            if "bundles received" in output:
                ltp_ready = True
                break
        except (RuntimeError, subprocess.TimeoutExpired):
            pass
        poll_sleep(i, base=2.0, cap=5.0)
    if not ltp_ready:
        print("  WARNING: LTP warmup did not converge — tests may fail")

    # ── Phase 2: Apply tc netem delay (both directions for symmetric simulation) ──
    apply_netem("spacecraft", f"delay {MARS_DELAY_MS}ms")
    apply_netem("ground-station", f"delay {MARS_DELAY_MS}ms")
    netem_output = netem_stats("spacecraft")

    # Test 1: netem delay rule is active with expected value
    expected_delay = f"{MARS_DELAY_MS // 1000}s" if MARS_DELAY_MS >= 1000 else f"{MARS_DELAY_MS}ms"
    netem_ok = bool(re.search(rf"delay\s+{re.escape(expected_delay)}", netem_output))
    if not netem_ok:
        print(f"  tc qdisc output: {netem_output.strip()}")
    check(f"tc netem delay {expected_delay} applied on spacecraft", netem_ok)

    # ── Phase 3: Build firmware ──
    run(["make", "-C", str(INTEGRATION_DIR), "build/firmware.elf"], timeout=60, check=True)

    # ── Phase 4: Start bprecvfile on ground station ──
    compose_exec("ground-station", "rm -f /tmp/testfile*")
    compose_exec(
        "ground-station",
        f"cd /tmp && timeout {RECV_TIMEOUT} bprecvfile ipn:2.3 100 &",
        timeout=10,
    )
    time.sleep(1)
    recv_check = compose_exec("ground-station", "pgrep -x bprecvfile > /dev/null && echo ok || echo missing")
    if "missing" in recv_check:
        print("  WARNING: bprecvfile not running — bundle delivery tests may fail")

    # ── Phase 5: Launch bridge ──
    bridge_proc = subprocess.Popen(
        [sys.executable, str(INTEGRATION_DIR / "uart_bridge.py"),
         "--kernel", str(INTEGRATION_DIR / "build" / "firmware.elf"),
         "--duration", str(BRIDGE_DURATION)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )

    # ── Phase 6: Wait for bridge to finish ──
    try:
        stdout, stderr = bridge_proc.communicate(timeout=BRIDGE_DURATION + 60)
    except subprocess.TimeoutExpired:
        bridge_proc.terminate()
        stdout, stderr = bridge_proc.communicate(timeout=10)

    print(f"  Bridge stderr:\n{stderr.strip()}" if stderr.strip() else "  (no bridge stderr)")

    # Parse bridge stats
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

    # Test 2: Bridge exited cleanly
    check("bridge exited cleanly", bridge_proc.returncode == 0)

    # Test 3: Bridge read telemetry lines
    check("bridge read telemetry lines", stats.get("telem_lines", 0) > 0)

    # Test 4: Bridge sent >= 1 bundle
    bundles_sent = stats.get("bundles_sent", 0)
    check(f"bridge sent >= 1 bundle (got {bundles_sent})", bundles_sent >= 1)

    # ── Phase 7: Poll for bundle delivery (bundles may still be in flight) ──
    # Snapshot immediately after bridge exits — if delay is working, some bundles
    # should still be in-flight and not yet delivered.
    ls_immediate = compose_exec("ground-station", "ls /tmp/testfile* 2>/dev/null || true")
    files_immediate = [f.strip() for f in ls_immediate.strip().split("\n") if f.strip()]

    received_files = list(files_immediate)
    if not received_files:
        for i in range(30):
            ls_output = compose_exec("ground-station", "ls /tmp/testfile* 2>/dev/null || true")
            received_files = [f.strip() for f in ls_output.strip().split("\n") if f.strip()]
            if received_files:
                break
            poll_sleep(i, base=2.0, cap=5.0)

    # Test 5: Ground station received at least one file
    check(f"ground station received files (got {len(received_files)})", len(received_files) > 0)

    # Read all received telemetry
    telem_content = ""
    for fpath in received_files:
        content = compose_exec("ground-station", f"cat {shlex.quote(fpath)}")
        telem_content += content

    telem_lines = [line for line in telem_content.strip().split("\n") if line.startswith("$TELEM,")]

    # Test 6: Received content contains $TELEM lines
    check(f"received telemetry lines (got {len(telem_lines)})", len(telem_lines) > 0)

    # Test 7: Valid CSV format (5 comma-separated fields)
    csv_ok = all(len(line.split(",")) == 5 for line in telem_lines) if telem_lines else False
    check("telemetry CSV format valid (5 fields)", csv_ok)

    # Informational: check whether delay was observable (not a hard assertion —
    # Test 1 already verifies netem is applied, and Tests 5-7 verify delivery).
    if bundles_sent > 0:
        if len(files_immediate) < bundles_sent:
            print(f"  INFO: delay observable ({len(files_immediate)} delivered at bridge exit < {bundles_sent} sent)")
        else:
            print(f"  INFO: all {bundles_sent} bundles arrived before bridge exit (fast CI runner)")

    print(f"{passed} passed, {failed} failed")
    return failed == 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Mars-delay end-to-end test")
    parser.add_argument("--keep-on-failure", action="store_true",
                        help="Leave containers running on failure for inspection")
    args = parser.parse_args()

    signal.signal(signal.SIGINT, lambda *_: sys.exit(130))
    success = False
    try:
        success = main()
    except Exception as e:
        print(f"FATAL: {e}", file=sys.stderr)
        success = False
    finally:
        if args.keep_on_failure and not success:
            print("  Containers left running for inspection. Run 'make -C integration clean-ground-station' when done.")
        else:
            cleanup()
    sys.exit(0 if success else 1)
