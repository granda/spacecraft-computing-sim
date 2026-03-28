#!/usr/bin/env python3
"""Integration tests for DTN bundle delivery over degraded links.

Applies tc netem rules to simulate latency, packet loss, and intermittent
connectivity, then verifies ION's store-and-forward delivers bundles through
each condition.

Requires NET_ADMIN capability on node1 (set in docker-compose.yml).
"""

import re
import shlex
import signal
import subprocess
import sys
import time

from test_basic import (COMPOSE, run, compose_exec, poll_sleep,
                        send_and_recv, cleanup, containers_running)


def apply_netem(node, rule):
    """Add a tc netem qdisc to the container's eth0."""
    compose_exec(node, f"tc qdisc add dev eth0 root netem {rule}", check=True)


def clear_netem(node):
    """Remove any tc netem qdisc (ignore errors if none set)."""
    compose_exec(node, "tc qdisc del dev eth0 root 2>/dev/null || true")


def netem_stats(node):
    """Return tc -s qdisc output for eth0."""
    return compose_exec(node, "tc -s qdisc show dev eth0")


def parse_dropped(stats):
    """Extract dropped packet count from tc -s output."""
    m = re.search(r"dropped (\d+)", stats)
    return int(m.group(1)) if m else 0


def wait_for_nodes():
    """Block until both ION daemons report ready. Returns True on success."""
    print("Waiting for ION daemons...")
    ready = {"node1": False, "node2": False}
    deadline = time.monotonic() + 90
    attempt = 0
    while time.monotonic() < deadline:
        if attempt % 3 == 2 and not containers_running():
            print("  FAIL: container(s) exited unexpectedly")
            return False
        for node in ready:
            if not ready[node]:
                output = run([*COMPOSE, "logs", node], timeout=10, quiet=True)
                if "ION node ready" in output:
                    ready[node] = True
        if all(ready.values()):
            return True
        poll_sleep(attempt)
        attempt += 1
    for node, is_ready in ready.items():
        if not is_ready:
            print(f"  FAIL: {node} did not start in time")
    return False


def ltp_warmup():
    """Poll with bping until LTP/UDP connections establish."""
    for i in range(15):
        try:
            output = compose_exec("node1", "bping -c 1 -i 1 ipn:1.2 ipn:2.1",
                                  timeout=15)
            if "bundles received" in output:
                return
        except subprocess.TimeoutExpired:
            pass
        poll_sleep(i)
    print("  WARNING: LTP warmup did not converge")


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

    print("Running degraded-link tests...")

    run([*COMPOSE, "up", "-d"], timeout=600, check=True)

    if not wait_for_nodes():
        print(f"0 passed, 1 failed")
        return False

    ltp_warmup()

    # ── Baseline transfer time (clean link) ─────────────────────────
    print("Measuring baseline transfer time...")
    t0 = time.monotonic()
    send_and_recv("node1", "ipn:1.2", "node2", "ipn:2.2", "baseline-payload")
    baseline_time = time.monotonic() - t0
    print(f"    baseline: {baseline_time:.1f}s")

    # ── Test 1: high latency ──────────────────────────────────────────
    print("Test: latency (500ms delay, 100ms jitter)...")
    clear_netem("node1")
    apply_netem("node1", "delay 500ms 100ms")

    stats = netem_stats("node1")
    check("netem delay rule active", bool(re.search(r"delay\s+500", stats)))
    print(f"    qdisc: {stats.strip().splitlines()[0]}")

    t0 = time.monotonic()
    result = send_and_recv("node1", "ipn:1.2", "node2", "ipn:2.2",
                           "latency-test-payload")
    latency_time = time.monotonic() - t0
    print(f"    transfer: {latency_time:.1f}s (baseline was {baseline_time:.1f}s)")

    check("latency test slower than baseline",
          latency_time > baseline_time)
    check("bundle delivered under 500ms latency", result is not None)
    clear_netem("node1")

    # ── Test 2: packet loss ───────────────────────────────────────────
    print("Test: packet loss (25%)...")
    apply_netem("node1", "loss 25%")

    stats = netem_stats("node1")
    check("netem loss rule active", bool(re.search(r"loss\s+25%", stats)))
    print(f"    qdisc: {stats.strip().splitlines()[0]}")

    # Push traffic through the degraded interface so netem has packets to drop.
    # A tiny payload produces few LTP segments — bping burst ensures enough
    # UDP packets flow for the 25% loss to be statistically measurable.
    compose_exec("node1", "bping -c 10 -i 1 ipn:1.2 ipn:2.1", timeout=60)

    result = send_and_recv("node1", "ipn:1.2", "node2", "ipn:2.2",
                           "loss-test-payload")

    stats_after = netem_stats("node1")
    drops = parse_dropped(stats_after)
    print(f"    packets dropped by netem: {drops}")
    check("netem dropped packets", drops > 0)

    check("bundle delivered despite packet loss", result is not None)
    clear_netem("node1")

    # ── Test 3: intermittent connectivity ─────────────────────────────
    # Send a bundle while the link is completely down, then restore it.
    # Proves DTN store-and-forward: bundle is queued during the outage
    # and LTP retransmits once connectivity returns.
    print("Test: intermittent link (send during outage, deliver on recovery)...")
    clear_netem("node1")
    apply_netem("node1", "loss 100%")

    stats = netem_stats("node1")
    check("netem blackout active", bool(re.search(r"loss\s+100%", stats)))
    print(f"    qdisc: {stats.strip().splitlines()[0]}")

    safe_src = shlex.quote("ipn:1.2")
    safe_dst = shlex.quote("ipn:2.2")
    payload = "intermittent-test-payload"

    compose_exec("node2", "cd /tmp && rm -f testfile1")

    recv = subprocess.Popen(
        [*COMPOSE, "exec", "-T", "node2", "bash", "-c",
         f"cd /tmp && exec timeout 60 bprecvfile {safe_dst} 1"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )

    try:
        # Poll until bprecvfile is listening (visible as a process inside the container)
        for i in range(10):
            pid_check = compose_exec("node2", "pgrep -x bprecvfile > /dev/null && echo UP || echo DOWN")
            if "UP" in pid_check:
                break
            poll_sleep(i, base=0.5, cap=2.0)

        compose_exec("node1",
                     f"printf '%s' {shlex.quote(payload)} > /tmp/msg.txt && "
                     f"bpsendfile {safe_src} {safe_dst} /tmp/msg.txt",
                     check=True)

        # Poll to confirm bundle has NOT arrived (proves the outage is real).
        # Check several times to be confident it's truly blocked, not just slow.
        bundle_blocked = True
        for i in range(5):
            poll_sleep(i, base=0.5, cap=2.0)
            output = compose_exec("node2",
                                  "cat /tmp/testfile1 2>/dev/null || echo NOT_RECEIVED")
            if payload in output:
                bundle_blocked = False
                break
        check("bundle held during outage", bundle_blocked)
        if bundle_blocked:
            print("    confirmed: bundle queued (link is down)")

        # Restore link — LTP retransmits
        print("    restoring link...")
        clear_netem("node1")

        delivered = False
        for i in range(20):
            output = compose_exec("node2",
                                  "cat /tmp/testfile1 2>/dev/null || echo NOT_RECEIVED")
            if payload in output:
                delivered = True
                break
            poll_sleep(i, base=1.0, cap=5.0)

        check("bundle delivered after link recovery", delivered)

    finally:
        recv.terminate()
        try:
            recv.wait(timeout=5)
        except subprocess.TimeoutExpired:
            recv.kill()
            recv.wait()

    clear_netem("node1")

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
            print("  Containers left running for inspection."
                  " Run 'make -C dtn clean' when done.")
        else:
            cleanup()
    sys.exit(0 if success else 1)
