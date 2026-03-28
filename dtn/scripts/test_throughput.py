#!/usr/bin/env python3
"""Throughput tests for DTN bundle delivery with larger payloads.

Sends multi-segment payloads (10 KB – 500 KB) over clean and degraded links,
exercising LTP fragmentation/reassembly and measuring sustained throughput.

Requires NET_ADMIN capability on node1 (set in docker-compose.yml).
"""

import re
import shlex
import signal
import subprocess
import sys
import time

from test_basic import (COMPOSE, run, compose_exec, poll_sleep,
                        cleanup, containers_running)
from test_degraded import (apply_netem, clear_netem, netem_stats,
                           wait_for_nodes, ltp_warmup)

SIZE_10K = 10 * 1024
SIZE_100K = 100 * 1024
SIZE_500K = 500 * 1024


def generate_test_file(node, path, size_bytes):
    """Create a random file inside a container and return its md5 hex digest."""
    if size_bytes % 1024 != 0:
        raise ValueError(f"size_bytes must be a multiple of 1024, got {size_bytes}")
    count = size_bytes // 1024
    compose_exec(node, f"dd if=/dev/urandom of={path} bs=1024 count={count} 2>/dev/null",
                 check=True)
    actual = compose_exec(node, f"stat -c %s {path}").strip()
    if int(actual) != size_bytes:
        raise RuntimeError(f"Generated file size mismatch: {actual} != {size_bytes}")
    output = compose_exec(node, f"md5sum {path}")
    return output.split()[0]


def send_file_and_verify(sender, sender_eid, receiver, receiver_eid,
                         file_path, expected_md5, size_bytes, timeout_sec=60):
    """Send a file via bpsendfile and poll for delivery with md5 verification.

    Returns dict with keys: delivered, elapsed_sec, throughput_bps, md5_match.
    """
    safe_src = shlex.quote(sender_eid)
    safe_dst = shlex.quote(receiver_eid)
    safe_path = shlex.quote(file_path)

    # Kill stale bprecvfile and clean output file
    compose_exec(receiver, "pkill -x bprecvfile 2>/dev/null || true")
    compose_exec(receiver, "cd /tmp && rm -f testfile1")

    recv = subprocess.Popen(
        [*COMPOSE, "exec", "-T", receiver, "bash", "-c",
         f"cd /tmp && exec timeout {timeout_sec + 10} bprecvfile {safe_dst} 1"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )

    result = {"delivered": False, "elapsed_sec": 0.0, "throughput_bps": 0.0,
              "md5_match": False}

    try:
        # Wait for bprecvfile to be listening.  The endpoint is statically
        # registered in node.rc, so bundles queue in ION even if bprecvfile
        # hasn't attached yet — we warn but proceed rather than aborting.
        recv_ready = False
        for i in range(10):
            pid_check = compose_exec(receiver,
                                     "pgrep -x bprecvfile > /dev/null && echo UP || echo DOWN")
            if "UP" in pid_check:
                recv_ready = True
                break
            poll_sleep(i, base=0.5, cap=2.0)
        if not recv_ready:
            print(f"  WARNING: bprecvfile on {receiver} not detected after 10 polls")

        t_start = time.monotonic()
        compose_exec(sender,
                     f"bpsendfile {safe_src} {safe_dst} {safe_path}",
                     check=True)

        # Poll for delivery
        deadline = time.monotonic() + timeout_sec
        while time.monotonic() < deadline:
            output = compose_exec(
                receiver,
                "test -f /tmp/testfile1 && md5sum /tmp/testfile1 || echo NOT_RECEIVED",
            )
            if expected_md5 in output:
                elapsed = time.monotonic() - t_start
                result["delivered"] = True
                result["elapsed_sec"] = elapsed
                result["throughput_bps"] = size_bytes / elapsed if elapsed > 0 else 0
                result["md5_match"] = True
                return result
            time.sleep(1)

        # Timed out — check if file arrived with wrong checksum
        output = compose_exec(
            receiver,
            "test -f /tmp/testfile1 && md5sum /tmp/testfile1 || echo NOT_RECEIVED",
        )
        if "NOT_RECEIVED" not in output:
            elapsed = time.monotonic() - t_start
            result["delivered"] = True
            result["elapsed_sec"] = elapsed
            result["throughput_bps"] = size_bytes / elapsed if elapsed > 0 else 0
        return result

    finally:
        recv.terminate()
        try:
            recv.wait(timeout=5)
        except subprocess.TimeoutExpired:
            recv.kill()
            recv.wait()


def format_size(size_bytes):
    """Human-readable size label."""
    if size_bytes >= 1024 * 1024:
        return f"{size_bytes // (1024 * 1024)} MB"
    return f"{size_bytes // 1024} KB"


def format_throughput(bps):
    """Human-readable throughput."""
    if bps >= 1024 * 1024:
        return f"{bps / (1024 * 1024):.1f} MB/s"
    if bps >= 1024:
        return f"{bps / 1024:.1f} KB/s"
    return f"{bps:.0f} B/s"


def main():
    passed = 0
    failed = 0
    throughput_log = []

    def check(desc, ok):
        nonlocal passed, failed
        if ok:
            print(f"  PASS: {desc}")
            passed += 1
        else:
            print(f"  FAIL: {desc}")
            failed += 1

    def log_result(scenario, size_bytes, result):
        throughput_log.append((scenario, size_bytes, result))

    print("Running throughput tests (larger payloads over degraded links)...")

    run([*COMPOSE, "up", "-d"], timeout=600, check=True)

    if not wait_for_nodes():
        print("0 passed, 1 failed")
        return False

    ltp_warmup()

    # Generate test files on node1
    print("Generating test files on node1...")
    files = {}
    for size in (SIZE_10K, SIZE_100K, SIZE_500K):
        path = f"/tmp/testpayload_{size}"
        md5 = generate_test_file("node1", path, size)
        files[size] = (path, md5)
        print(f"    {format_size(size)}: {md5[:12]}... at {path}")

    # ── Baseline: clean link ─────────────────────────────────────────
    print("Baseline: clean link transfers...")
    baseline_100k_time = None
    for size in (SIZE_10K, SIZE_100K, SIZE_500K):
        path, md5 = files[size]
        label = format_size(size)
        timeout = 60 if size == SIZE_500K else 30
        r = send_file_and_verify("node1", "ipn:1.2", "node2", "ipn:2.2",
                                 path, md5, size, timeout_sec=timeout)
        check(f"clean {label} delivered with correct md5",
              r["delivered"] and r["md5_match"])
        if r["delivered"]:
            print(f"    {label}: {r['elapsed_sec']:.1f}s, {format_throughput(r['throughput_bps'])}")
        log_result("clean", size, r)
        if size == SIZE_100K:
            baseline_100k_time = r["elapsed_sec"]

    # ── Latency: 500ms delay, 100ms jitter ───────────────────────────
    ltp_warmup()
    print("Test: latency (500ms delay, 100ms jitter) with 100 KB...")
    clear_netem("node1")
    apply_netem("node1", "delay 500ms 100ms")
    check("netem delay rule active",
          bool(re.search(r"delay\s+500", netem_stats("node1"))))

    path, md5 = files[SIZE_100K]
    r = send_file_and_verify("node1", "ipn:1.2", "node2", "ipn:2.2",
                             path, md5, SIZE_100K, timeout_sec=60)
    check("latency 100 KB delivered with correct md5",
          r["delivered"] and r["md5_match"])
    if baseline_100k_time and r["delivered"]:
        check("latency transfer slower than baseline",
              r["elapsed_sec"] > baseline_100k_time * 0.9)
        print(f"    100 KB: {r['elapsed_sec']:.1f}s (baseline was {baseline_100k_time:.1f}s)")
    log_result("latency 500ms", SIZE_100K, r)
    clear_netem("node1")

    # ── Packet loss: 10% ─────────────────────────────────────────────
    ltp_warmup()
    print("Test: packet loss (10%) with 100 KB...")
    apply_netem("node1", "loss 10%")
    check("netem loss-10% rule active",
          bool(re.search(r"loss\s+10%", netem_stats("node1"))))

    path, md5 = files[SIZE_100K]
    r = send_file_and_verify("node1", "ipn:1.2", "node2", "ipn:2.2",
                             path, md5, SIZE_100K, timeout_sec=60)
    check("loss-10% 100 KB delivered with correct md5",
          r["delivered"] and r["md5_match"])
    if r["delivered"]:
        print(f"    100 KB: {r['elapsed_sec']:.1f}s, {format_throughput(r['throughput_bps'])}")
    log_result("loss 10%", SIZE_100K, r)
    clear_netem("node1")

    # ── Packet loss: 25% ─────────────────────────────────────────────
    ltp_warmup()
    # Only test 10 KB here. At 25% loss, LTP retransmission cascades
    # make 100 KB (~72 segments) unreliable within reasonable timeouts.
    # 10 KB (~8 segments) still exercises multi-segment delivery under heavy loss.
    print("Test: packet loss (25%) with 10 KB...")
    apply_netem("node1", "loss 25%")
    check("netem loss-25% rule active",
          bool(re.search(r"loss\s+25%", netem_stats("node1"))))

    # Warm LTP under loss — push traffic so retransmission timers adjust
    compose_exec("node1", "bping -c 10 -i 1 ipn:1.2 ipn:2.1", timeout=60)

    path, md5 = files[SIZE_10K]
    r = send_file_and_verify("node1", "ipn:1.2", "node2", "ipn:2.2",
                             path, md5, SIZE_10K, timeout_sec=60)
    check("loss-25% 10 KB delivered with correct md5",
          r["delivered"] and r["md5_match"])
    if r["delivered"]:
        print(f"    10 KB: {r['elapsed_sec']:.1f}s, {format_throughput(r['throughput_bps'])}")
    log_result("loss 25%", SIZE_10K, r)
    clear_netem("node1")

    # ── Outage + recovery with 100 KB ────────────────────────────────
    ltp_warmup()
    print("Test: outage + recovery with 100 KB...")
    clear_netem("node1")
    apply_netem("node1", "loss 100%")
    check("netem blackout active",
          bool(re.search(r"loss\s+100%", netem_stats("node1"))))

    safe_src = shlex.quote("ipn:1.2")
    safe_dst = shlex.quote("ipn:2.2")
    path, md5 = files[SIZE_100K]

    compose_exec("node2", "pkill -x bprecvfile 2>/dev/null || true")
    compose_exec("node2", "cd /tmp && rm -f testfile1")

    recv = subprocess.Popen(
        [*COMPOSE, "exec", "-T", "node2", "bash", "-c",
         f"cd /tmp && exec timeout 130 bprecvfile {safe_dst} 1"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )

    try:
        # Endpoint is statically registered — bundles queue even without bprecvfile.
        recv_ready = False
        for i in range(10):
            pid_check = compose_exec("node2",
                                     "pgrep -x bprecvfile > /dev/null && echo UP || echo DOWN")
            if "UP" in pid_check:
                recv_ready = True
                break
            poll_sleep(i, base=0.5, cap=2.0)
        if not recv_ready:
            print("  WARNING: bprecvfile on node2 not detected after 10 polls")

        compose_exec("node1",
                     f"bpsendfile {safe_src} {safe_dst} {shlex.quote(path)}",
                     check=True)

        # Confirm bundle is held during outage
        bundle_blocked = True
        for i in range(5):
            poll_sleep(i, base=0.5, cap=2.0)
            output = compose_exec("node2",
                                  "test -f /tmp/testfile1 && echo ARRIVED || echo NOT_RECEIVED")
            if "ARRIVED" in output:
                bundle_blocked = False
                break
        check("100 KB bundle held during outage", bundle_blocked)
        if bundle_blocked:
            print("    confirmed: bundle queued (link is down)")

        # Restore link
        print("    restoring link...")
        t_start = time.monotonic()
        clear_netem("node1")

        delivered = False
        md5_ok = False
        for i in range(60):
            output = compose_exec(
                "node2",
                "test -f /tmp/testfile1 && md5sum /tmp/testfile1 || echo NOT_RECEIVED",
            )
            if md5 in output:
                delivered = True
                md5_ok = True
                break
            poll_sleep(i, base=1.0, cap=3.0)

        elapsed = time.monotonic() - t_start
        check("100 KB bundle delivered after recovery", delivered)
        check("100 KB bundle md5 correct after recovery", md5_ok)
        if delivered:
            print(f"    delivered {elapsed:.1f}s after link restored")
            log_result("outage recovery", SIZE_100K,
                       {"delivered": True, "elapsed_sec": elapsed,
                        "throughput_bps": None, "md5_match": md5_ok})

    finally:
        recv.terminate()
        try:
            recv.wait(timeout=5)
        except subprocess.TimeoutExpired:
            recv.kill()
            recv.wait()
        clear_netem("node1")

    # ── Summary ──────────────────────────────────────────────────────
    print()
    print(f"{'Scenario':<20} {'Size':>8} {'Time':>8} {'Throughput':>14}")
    print("-" * 54)
    for scenario, size, r in throughput_log:
        if not r["delivered"]:
            print(f"{scenario:<20} {format_size(size):>8} {'—':>8} {'not delivered':>14}")
        elif r["throughput_bps"] is None:
            # Outage recovery: elapsed is post-restore latency, not throughput
            print(f"{scenario:<20} {format_size(size):>8} "
                  f"{r['elapsed_sec']:>7.1f}s "
                  f"{'(recovery time)':>14}")
        else:
            print(f"{scenario:<20} {format_size(size):>8} "
                  f"{r['elapsed_sec']:>7.1f}s "
                  f"{format_throughput(r['throughput_bps']):>14}")

    print()
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
