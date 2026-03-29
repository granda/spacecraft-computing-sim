#!/usr/bin/env python3
"""CFDP file transfer tests.

Verifies CFDP (CCSDS File Delivery Protocol) file transfer between the
spacecraft and ground station nodes using cfdptest over BP+LTP.

CFDP runs on top of the Bundle Protocol and provides file-oriented
transfer with metadata, checksums, and transaction tracking — the way
real missions transfer files between spacecraft and ground.
"""

import shlex
import signal
import sys
import time

from test_basic import (COMPOSE, run, compose_exec, poll_sleep,
                        cleanup, containers_running)
from test_degraded import wait_for_nodes, ltp_warmup
from test_throughput import generate_test_file, format_size


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

    print("Running CFDP file transfer tests...")

    run([*COMPOSE, "up", "-d"], timeout=600, check=True)

    if not wait_for_nodes():
        for node in ["node1", "node2"]:
            print(run([*COMPOSE, "logs", node], timeout=10, quiet=True))
        print("0 passed, 1 failed")
        return False

    ltp_warmup()

    # ── CFDP daemons running ─────────────────────────────────────────
    # Mid-test crash detection relies on Docker healthcheck (pgrep -x cfdpclock
    # && pgrep -x bputa) rather than polling containers_running() inline.
    print("Checking CFDP daemons...")
    for node in ["node1", "node2"]:
        output = compose_exec(
            node,
            "pgrep -x cfdpclock > /dev/null && pgrep -x bputa > /dev/null "
            "&& echo CFDP_OK || echo CFDP_FAIL",
        )
        check(f"{node} CFDP daemons running", "CFDP_OK" in output)

    # ── File transfers: node1 → node2 (spacecraft to ground) ────────
    sizes = [1024, 50 * 1024, 100 * 1024]

    print("Generating test files on node1...")
    files = {}
    for size in sizes:
        path = f"/tmp/cfdp_test_{size}"
        md5 = generate_test_file("node1", path, size)
        files[size] = (path, md5)
        print(f"    {format_size(size)}: {md5[:12]}...")

    # Send all files in a single exec session — ION's CFDP entity state
    # doesn't survive well across separate docker exec invocations.
    dst_paths = {s: f"/tmp/cfdp_recv_{s}" for s in sizes}
    send_cmds = []
    for size in sizes:
        path, _ = files[size]
        dst = dst_paths[size]
        # sleep 5: CFDP needs time between transactions for EOF/finish PDUs over 1s OWLT
        safe_path = shlex.quote(path)
        safe_dst = shlex.quote(dst)
        send_cmds.append(f"printf 'd 2\\nf {safe_path}\\nt {safe_dst}\\n&\\n' | cfdptest; sleep 5")
    batch_cmd = " && ".join(send_cmds)

    for dst in dst_paths.values():
        compose_exec("node2", f"rm -f {dst}")

    print("  Sending all sizes node1 → node2 via CFDP...")
    compose_exec("node1", batch_cmd, timeout=120, check=True)

    # Poll for each file (60s matches test_throughput.py for 100 KB)
    for size in sizes:
        path, md5 = files[size]
        label = format_size(size)
        dst = dst_paths[size]
        delivered = False
        md5_match = False
        for _ in range(60):
            output = compose_exec("node2",
                                  f"test -f {dst} && md5sum {dst} || echo NOT_RECEIVED")
            if "NOT_RECEIVED" not in output:
                delivered = True
                md5_match = md5 in output
                break
            time.sleep(1)
        check(f"CFDP {label} node1→node2 delivered", delivered)
        check(f"CFDP {label} node1→node2 md5 correct", md5_match)

    # ── Reverse: node2 → node1 (ground to spacecraft) ───────────────
    # File generation and cfdptest MUST run in a single compose_exec session —
    # ION's CFDP entity state breaks across separate docker exec invocations.
    print("  Sending 50 KB node2 → node1 via CFDP...")
    size = 50 * 1024
    dst_path = f"/tmp/cfdp_recv_reverse_{size}"
    compose_exec("node1", f"rm -f {dst_path}")
    compose_exec("node2",
                 f"dd if=/dev/urandom of=/tmp/cfdp_rev bs=1024 count=50 2>/dev/null && "
                 f"md5sum /tmp/cfdp_rev > /tmp/cfdp_rev_md5 && "
                 f"printf 'd 1\\nf /tmp/cfdp_rev\\nt {shlex.quote(dst_path)}\\n&\\n' | cfdptest && sleep 5",
                 timeout=60, check=True)
    md5_output = compose_exec("node2", "cat /tmp/cfdp_rev_md5").strip()
    if not md5_output:
        print("  WARNING: could not read reverse file md5")
        rev_md5 = ""
    else:
        rev_md5 = md5_output.split()[0]
    delivered = False
    md5_match = False
    for _ in range(60):
        output = compose_exec("node1",
                              f"test -f {dst_path} && md5sum {dst_path} || echo NOT_RECEIVED")
        if "NOT_RECEIVED" not in output:
            delivered = True
            md5_match = bool(rev_md5) and rev_md5 in output
            break
        time.sleep(1)
    check("CFDP 50 KB node2→node1 delivered", delivered)
    check("CFDP 50 KB node2→node1 md5 correct", md5_match)

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
