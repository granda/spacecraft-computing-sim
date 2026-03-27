#!/usr/bin/env python3
"""Integration tests for the ION DTN two-node network."""

import random
import signal
import shlex
import subprocess
import sys
import time
from pathlib import Path

PHASE3_DIR = Path(__file__).resolve().parent.parent
COMPOSE = ["docker", "compose", "--project-directory", str(PHASE3_DIR)]


def poll_sleep(attempt, base=0.5, cap=3.0):
    """Sleep with exponential backoff + jitter."""
    delay = min(base * (2 ** attempt), cap)
    time.sleep(delay * (0.5 + random.random() * 0.5))


def run(cmd, timeout=30, check=False, quiet=False):
    """Run a command and return stdout.

    Most callers use check=False because they're polling for readiness
    or capturing output to grep — a non-zero exit is expected in those cases.
    Use check=True for infrastructure commands (compose up) where failure
    means the test environment is broken. Use quiet=True during polling loops
    where failure is expected and stderr would be noise.
    """
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        raise RuntimeError(f"Command timed out after {timeout}s: {' '.join(cmd)}")
    if result.returncode != 0:
        if check:
            print(f"Command failed (exit {result.returncode}): {' '.join(cmd)}")
            print(result.stdout)
            if result.stderr:
                print(result.stderr, file=sys.stderr)
            raise RuntimeError(f"Command failed: {' '.join(cmd)}")
        elif result.stderr and not quiet:
            print(f"  stderr: {result.stderr.strip()}", file=sys.stderr)
    return result.stdout


def compose_exec(node, cmd, timeout=30, check=False):
    """Run a command inside a container. Returns stdout."""
    result = subprocess.run(
        [*COMPOSE, "exec", "-T", node, "bash", "-c", cmd],
        capture_output=True, text=True, timeout=timeout,
    )
    if result.returncode != 0:
        if result.stderr:
            print(f"  [{node}] stderr: {result.stderr.strip()}", file=sys.stderr)
        if check:
            raise RuntimeError(f"[{node}] command failed (exit {result.returncode}): {cmd}")
    return result.stdout


def cleanup():
    subprocess.run([*COMPOSE, "down", "--timeout", "5", "--volumes"], capture_output=True, timeout=30)


def containers_running():
    """Check that both containers are still running (not exited/crashed)."""
    result = subprocess.run(
        [*COMPOSE, "ps", "--status", "running", "--quiet"],
        capture_output=True, text=True, timeout=10,
    )
    return len(result.stdout.strip().splitlines()) >= 2


def send_and_recv(sender, sender_eid, receiver, receiver_eid, message):
    """Send a file via bpsendfile and poll for delivery. Returns received content or None.

    Endpoint convention: ipn:N.1 = bpecho (ping), ipn:N.2 = file transfer tests.
    """
    safe_msg = shlex.quote(message)
    safe_sender = shlex.quote(sender_eid)
    safe_receiver = shlex.quote(receiver_eid)

    def _cleanup_recv(proc):
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        if proc.returncode not in (0, -signal.SIGTERM, -signal.SIGKILL):
            print(f"    warning: bprecvfile exited with {proc.returncode}", file=sys.stderr)

    # Each attempt starts a fresh bprecvfile so the receiver is always listening.
    # The endpoint is statically registered in node.rc, so polling bpadmin is
    # unreliable — instead we retry the full send+receive cycle.
    for attempt in range(3):
        compose_exec(receiver, "cd /tmp && rm -f testfile1")
        recv = subprocess.Popen(
            [*COMPOSE, "exec", "-T", receiver, "bash", "-c",
             f"cd /tmp && exec timeout 25 bprecvfile {safe_receiver} 1"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        try:
            poll_sleep(attempt, base=2.0, cap=5.0)
            compose_exec(sender,
                         f"printf '%s' {safe_msg} > /tmp/msg.txt && "
                         f'bpsendfile {safe_sender} {safe_receiver} /tmp/msg.txt',
                         check=True)
            for i in range(10):
                output = compose_exec(receiver, "cat /tmp/testfile1 2>/dev/null || echo NOT_RECEIVED")
                if message in output:
                    return message
                poll_sleep(i)
        finally:
            _cleanup_recv(recv)
    return None


def main():
    passed = 0
    failed = 0

    def check(desc, output, pattern):
        nonlocal passed, failed
        if pattern.lower() in output.lower():
            print(f"  PASS: {desc}")
            passed += 1
        else:
            print(f"  FAIL: {desc}")
            failed += 1

    print("Running ION DTN integration tests...")

    # Build and start — exit early if build fails
    run([*COMPOSE, "up", "-d"], timeout=600, check=True)

    # Wait for both nodes (check alternating, detect crashes early)
    print("Waiting for ION daemons...")
    ready = {"node1": False, "node2": False}
    container_crash = False
    deadline = time.monotonic() + 90
    attempt = 0
    while time.monotonic() < deadline:
        if attempt % 3 == 2 and not containers_running():  # throttle: docker ps is expensive
            print("  FAIL: container(s) exited unexpectedly")
            failed += 1
            container_crash = True
            break
        for node in ready:
            if not ready[node]:
                output = run([*COMPOSE, "logs", node], timeout=10, quiet=True)
                if "ION node ready" in output:
                    ready[node] = True
        if all(ready.values()):
            break
        poll_sleep(attempt)
        attempt += 1

    if not container_crash:
        for node, is_ready in ready.items():
            if not is_ready:
                print(f"  FAIL: {node} did not start in time")
                print(run([*COMPOSE, "logs", node], timeout=10))
                failed += 1

    if not all(ready.values()):
        print(f"0 passed, {failed} failed")
        return False

    # Wait for LTP/UDP connections to establish (poll with bping)
    ltp_ready = False
    output = ""
    for i in range(15):
        try:
            output = compose_exec("node1", "bping -c 1 -i 1 ipn:1.2 ipn:2.1", timeout=15)
            if "bundles received" in output:
                ltp_ready = True
                break
        except subprocess.TimeoutExpired:
            pass
        poll_sleep(i)
    if not ltp_ready:
        print("  WARNING: LTP warmup did not converge — bping tests may fail")
        print(f"  Last bping output: {output.strip()}")

    # Test 1-2: ION running on both nodes (check for version string)
    for node in ["node1", "node2"]:
        output = compose_exec(node, 'echo "v" | ionadmin')
        check(f"{node} ION running", output, "ion-open-source")

    # Test 3-4: bping both directions
    # ipn:N.2 is reused as source for both bping and file transfer — safe because tests
    # run sequentially and _cleanup_recv terminates bprecvfile before the next test starts.
    # Verify bpecho is alive first — if it crashed, bping fails with opaque errors
    for node in ["node1", "node2"]:
        pid_check = compose_exec(node, "pgrep -x bpecho > /dev/null && echo OK || echo DEAD")
        if "DEAD" in pid_check:
            print(f"  WARNING: bpecho on {node} is not running")

    output = compose_exec("node1", "bping -c 3 -i 2 ipn:1.2 ipn:2.1", timeout=30)
    # Match on "3 bundles received" only — ION output format may vary between versions
    check("bping node1->node2", output, "3 bundles received")

    output = compose_exec("node2", "bping -c 3 -i 2 ipn:2.2 ipn:1.1", timeout=30)
    check("bping node2->node1", output, "3 bundles received")

    # Test 5: File transfer node1 -> node2
    result = send_and_recv("node1", "ipn:1.2", "node2", "ipn:2.2", "hello from spacecraft")
    check("bundle node1->node2 delivered", result or "NOT_RECEIVED", "hello from spacecraft")

    # Test 6: File transfer node2 -> node1
    result = send_and_recv("node2", "ipn:2.2", "node1", "ipn:1.2", "hello from ground")
    check("bundle node2->node1 delivered", result or "NOT_RECEIVED", "hello from ground")

    print(f"{passed} passed, {failed} failed")
    return failed == 0


if __name__ == "__main__":
    signal.signal(signal.SIGINT, lambda *_: sys.exit(130))
    success = False
    try:
        success = main()
    except RuntimeError as e:
        print(f"FATAL: {e}", file=sys.stderr)
        success = False
    finally:
        if "--keep-on-failure" in sys.argv and not success:
            print("  Containers left running for inspection. Run 'make -C dtn clean' when done.")
        else:
            cleanup()
    sys.exit(0 if success else 1)
