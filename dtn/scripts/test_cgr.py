#!/usr/bin/env python3
"""Contact-Graph Routing tests with scheduled link windows.

Demonstrates ION's CGR engine using time-bounded contacts rather than an
always-on link.  Bundles sent during gaps between contact windows are queued
locally and forwarded when the next window opens.

Contact schedule (offsets from ionadmin start):
  +0   to +60  : no contact (gap)  — bundles queue
  +60  to +90  : Window 1 (30s)    — queued bundles deliver
  +90  to +150 : no contact (gap)  — bundles queue again
  +150 to +210 : Window 2 (60s)    — second delivery

Uses docker-compose.cgr.yml override to load node_cgr.rc configs.
"""

import re
import shlex
import signal
import subprocess
import sys
import time
from pathlib import Path

from test_basic import run, poll_sleep, wait_for_nodes

PHASE3_DIR = Path(__file__).resolve().parent.parent

COMPOSE_CGR = [
    "docker", "compose", "--project-directory", str(PHASE3_DIR),
    "-f", str(PHASE3_DIR / "docker-compose.yml"),
    "-f", str(PHASE3_DIR / "docker-compose.cgr.yml"),
]


# ---------------------------------------------------------------------------
# Local compose helpers bound to COMPOSE_CGR.  These mirror the signatures in
# test_basic but avoid mutating the shared module-level COMPOSE list, which
# would be fragile if test_basic is ever refactored to capture COMPOSE by value.
# ---------------------------------------------------------------------------

def compose_exec(node, cmd, timeout=30, check=False):
    """Run a command inside a CGR-configured container."""
    result = subprocess.run(
        [*COMPOSE_CGR, "exec", "-T", node, "bash", "-c", cmd],
        capture_output=True, text=True, timeout=timeout,
    )
    if result.returncode != 0:
        if result.stderr:
            print(f"  [{node}] stderr: {result.stderr.strip()}", file=sys.stderr)
        if check:
            raise RuntimeError(
                f"[{node}] command failed (exit {result.returncode}): {cmd}")
    return result.stdout


def cleanup():
    subprocess.run([*COMPOSE_CGR, "down", "--timeout", "5", "--volumes"],
                   capture_output=True, timeout=30)


def send_and_wait_for_window(payload, window_offset, window_label,
                             t_ready, check, timings):
    """Send a bundle during a gap, verify it queues, then deliver in a window.

    1. Start bprecvfile on node2
    2. bpsendfile from node1 — should queue (no active contact)
    3. Poll to confirm the bundle is NOT delivered (gap assertion)
    4. Sleep until the target window opens
    5. Poll until the bundle arrives (delivery assertion)

    Timing note: window_offset is relative to t_ready, which approximates
    ionadmin start.  ionadmin actually starts a few seconds *before* the test
    detects "ION node ready", so windows open slightly earlier than
    t_ready + offset — this gives us extra margin rather than less.

    Args:
        payload: unique string to send and verify
        window_offset: seconds after t_ready when the contact window opens
        window_label: human-readable label for the timing summary
        t_ready: monotonic timestamp when ION daemons became ready
        check: assertion callback  check(description, bool)
        timings: list to append (label, elapsed) tuples
    """
    safe_src = shlex.quote("ipn:1.2")
    safe_dst = shlex.quote("ipn:2.2")

    compose_exec("node2", "cd /tmp && rm -f testfile1")

    recv = subprocess.Popen(
        [*COMPOSE_CGR, "exec", "-T", "node2", "bash", "-c",
         f"cd /tmp && exec timeout 120 bprecvfile {safe_dst} 1"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )

    try:
        # Wait for receiver process AND endpoint registration.
        # pgrep confirms the process is running, but the 2s base sleep gives
        # bprecvfile time to register its endpoint with BP — bundles arriving
        # before registration completes are silently discarded.
        for i in range(10):
            pid_check = compose_exec(
                "node2", "pgrep -x bprecvfile > /dev/null && echo UP || echo DOWN")
            if "UP" in pid_check:
                break
            poll_sleep(i, base=2.0, cap=3.0)

        compose_exec("node1",
                     f"printf '%s' {shlex.quote(payload)} > /tmp/msg.txt && "
                     f"bpsendfile {safe_src} {safe_dst} /tmp/msg.txt",
                     check=True)

        t_sent = time.monotonic()

        # Verify NOT delivered for several seconds (proves queuing during gap).
        # This loop consumes ~12-16s of elapsed time, which is accounted for in
        # the wait_needed calculation below (it uses current elapsed, not a
        # pre-computed value).
        bundle_blocked = True
        for i in range(8):
            poll_sleep(i, base=1.0, cap=2.0)
            output = compose_exec(
                "node2", "cat /tmp/testfile1 2>/dev/null || echo NOT_RECEIVED")
            if payload in output:
                bundle_blocked = False
                break
        check(f"bundle queued during gap (not yet delivered)", bundle_blocked)
        if bundle_blocked:
            print("    confirmed: bundle queued locally (no active contact)")

        # Wait for the contact window to open
        print(f"Waiting for {window_label} to open...")
        elapsed = time.monotonic() - t_ready
        wait_needed = max(0, window_offset + 10 - elapsed)  # +10s margin
        if wait_needed > 0:
            print(f"    sleeping {wait_needed:.0f}s until {window_label}...")
            time.sleep(wait_needed)

        delivered = False
        for i in range(30):
            output = compose_exec(
                "node2", "cat /tmp/testfile1 2>/dev/null || echo NOT_RECEIVED")
            if payload in output:
                delivered = True
                break
            poll_sleep(i, base=1.0, cap=3.0)

        dt = time.monotonic() - t_sent
        check(f"bundle delivered when {window_label} opened ({dt:.1f}s)",
              delivered)
        timings.append((window_label, f"{dt:.1f}s"))

    finally:
        recv.terminate()
        try:
            recv.wait(timeout=5)
        except subprocess.TimeoutExpired:
            recv.kill()
            recv.wait()


def main():
    passed = 0
    failed = 0
    timings = []

    def check(desc, ok):
        nonlocal passed, failed
        if ok:
            print(f"  PASS: {desc}")
            passed += 1
        else:
            print(f"  FAIL: {desc}")
            failed += 1

    print("Running CGR contact-window tests...")

    t_up = time.monotonic()
    run([*COMPOSE_CGR, "up", "-d"], timeout=600, check=True)

    if not wait_for_nodes(compose=COMPOSE_CGR):
        print(f"0 passed, 1 failed")
        return False

    # t_ready approximates ionadmin start.  In reality ionadmin processes the
    # contact plan a few seconds *before* "ION node ready" is printed (bpecho,
    # cfdpclock, bputa start in between), so contact windows open slightly
    # earlier than t_ready + offset.  This means our wait calculations have
    # built-in extra margin rather than less.
    t_ready = time.monotonic()
    print(f"  Nodes ready ({t_ready - t_up:.0f}s after compose-up)")

    # ── Test 1-2: Verify scheduled contact plan on both nodes ────────
    # ionadmin 'l contact' lists contacts as:
    #   "xmit rate from node X to node Y is  100000 bytes/sec"
    # We check for multiple inter-node contacts (not self-loops) and
    # confirm there is no 86400-second always-on contact.
    for node in ["node1", "node2"]:
        output = compose_exec(node, 'echo "l contact" | ionadmin', timeout=10)
        inter_node = re.findall(r'node [12] to node [12].*100000', output)
        # Filter out self-loops (node 1 to node 1, node 2 to node 2)
        cross_links = [m for m in inter_node if 'node 1 to node 2' in m
                       or 'node 2 to node 1' in m]
        has_windows = len(cross_links) >= 2
        no_always_on = ("86400" not in output)
        check(f"{node} has scheduled contact windows",
              has_windows and no_always_on)

    # ── Tests 3-4: Gap 0 → Window 1 ─────────────────────────────────
    print("Test: bundle queuing during initial gap...")
    send_and_wait_for_window(
        payload="cgr-gap0-bundle",
        window_offset=60,       # Window 1 opens at ionadmin +60s ≈ t_ready +60s
        window_label="Window 1",
        t_ready=t_ready,
        check=check,
        timings=timings,
    )

    # ── Tests 5-6: Gap 1 → Window 2 ─────────────────────────────────
    # Wait for Gap 1 to begin (Window 1 closes at +90s from ionadmin)
    print("Waiting for Gap 1 (between windows)...")
    elapsed = time.monotonic() - t_ready
    wait_needed = max(0, 100 - elapsed)  # +90s + 10s margin
    if wait_needed > 0:
        print(f"    sleeping {wait_needed:.0f}s until Gap 1...")
        time.sleep(wait_needed)

    print("Test: bundle queuing during second gap...")
    send_and_wait_for_window(
        payload="cgr-gap1-bundle",
        window_offset=150,      # Window 2 opens at ionadmin +150s ≈ t_ready +150s
        window_label="Window 2",
        t_ready=t_ready,
        check=check,
        timings=timings,
    )

    # ── Summary ──────────────────────────────────────────────────────
    total_time = time.monotonic() - t_up
    print(f"\n{'Scenario':<25} {'Delivery time':>15}")
    print(f"{'-'*25} {'-'*15}")
    for label, dt in timings:
        print(f"{label:<25} {dt:>15}")
    print(f"{'Total test time':<25} {total_time:>14.0f}s")

    print(f"\n{passed} passed, {failed} failed")
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
            print("  Containers left running for inspection."
                  " Run 'make -C dtn clean' when done.")
        else:
            cleanup()
    sys.exit(0 if success else 1)
