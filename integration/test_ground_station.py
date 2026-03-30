#!/usr/bin/env python3
"""Integration tests for the ION DTN ground station.

Verifies that the spacecraft and ground-station ION nodes can communicate
over the DTN link and that the telemetry endpoint (ipn:2.3) is ready to
receive bundles from the UART bridge (next milestone).
"""

import signal
import subprocess
import sys
from pathlib import Path

INTEGRATION_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = INTEGRATION_DIR.parent

# Reuse helpers from the DTN test suite, passing compose= explicitly
# so each call targets integration's docker-compose.yml.
# Assumes layout: integration/ and dtn/ are siblings under PROJECT_ROOT.
sys.path.insert(0, str(PROJECT_ROOT / "dtn" / "scripts"))
from test_basic import (  # noqa: E402
    compose_exec,
    poll_sleep,
    run,
    send_and_recv,
    wait_for_nodes,
)

COMPOSE = ["docker", "compose", "--project-directory", str(INTEGRATION_DIR)]
NODES = ["spacecraft", "ground-station"]


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

    print("Running ground station integration tests...")

    # Build and start
    run([*COMPOSE, "up", "-d"], timeout=600, check=True)

    if not wait_for_nodes(compose=COMPOSE, nodes=NODES):
        print("Startup failed — 0 tests run. Run 'docker compose logs' in integration/ to debug.")
        return False

    # LTP warmup (poll with bping)
    ltp_ready = False
    output = ""
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
        print("  WARNING: LTP warmup did not converge — bping tests may fail")
        print(f"  Last bping output: {output.strip()}")

    # Test 1-2: ION running on both nodes
    for node in NODES:
        output = compose_exec(node, 'echo "v" | ionadmin', compose=COMPOSE)
        check(f"{node} ION running", "ion-open-source" in output.lower())

    # Pre-check: verify bpecho is alive before bping tests
    for node in NODES:
        pid_check = compose_exec(node, "pgrep -x bpecho > /dev/null && echo OK || echo DEAD",
                                 compose=COMPOSE)
        if "DEAD" in pid_check:
            print(f"  WARNING: bpecho on {node} is not running")

    # Test 3-4: bping both directions
    output = compose_exec("spacecraft", "bping -c 3 -i 2 ipn:1.2 ipn:2.1",
                          timeout=30, compose=COMPOSE)
    check("bping spacecraft->ground-station", "3 bundles received" in output)

    output = compose_exec("ground-station", "bping -c 3 -i 2 ipn:2.2 ipn:1.1",
                          timeout=30, compose=COMPOSE)
    check("bping ground-station->spacecraft", "3 bundles received" in output)

    # Test 5-6: file transfer both directions
    result = send_and_recv(
        "spacecraft", "ipn:1.2", "ground-station", "ipn:2.2",
        "hello from spacecraft", compose=COMPOSE,
    )
    check("bundle spacecraft->ground-station delivered", result == "hello from spacecraft")

    result = send_and_recv(
        "ground-station", "ipn:2.2", "spacecraft", "ipn:1.2",
        "hello from ground", compose=COMPOSE,
    )
    check("bundle ground-station->spacecraft delivered", result == "hello from ground")

    # Test 7: telemetry endpoint exists on ground station
    output = compose_exec("ground-station", 'echo "l endpoint" | bpadmin', compose=COMPOSE)
    check("ground-station has telemetry endpoint ipn:2.3", "ipn:2.3" in output)

    # Test 8: mock telemetry bundle delivery via telemetry endpoints
    result = send_and_recv(
        "spacecraft", "ipn:1.3", "ground-station", "ipn:2.3",
        "$TELEM,0001,TEMP,1990,1000", compose=COMPOSE,
    )
    check(
        "telemetry bundle delivered to ground station",
        result == "$TELEM,0001,TEMP,1990,1000",
    )

    print(f"{passed} passed, {failed} failed")
    return failed == 0


if __name__ == "__main__":
    signal.signal(signal.SIGINT, lambda *_: sys.exit(130))
    success = False
    try:
        success = main()
    except (RuntimeError, subprocess.TimeoutExpired) as e:
        print(f"FATAL: {e}", file=sys.stderr)
        success = False
    finally:
        if "--keep-on-failure" in sys.argv and not success:
            print("  Containers left running for inspection. Run 'make -C integration clean-ground-station' when done.")
        else:
            cleanup()
    sys.exit(0 if success else 1)
