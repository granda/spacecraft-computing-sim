#!/usr/bin/env python3
"""UART bridge: captures FreeRTOS telemetry from QEMU and forwards as DTN bundles.

Launches QEMU with UART redirected to a TCP socket, reads telemetry lines,
batches them, and injects each batch as a bundle into the spacecraft ION node
(ipn:1.3 -> ipn:2.3) via docker compose exec.
"""

import argparse
import os
import select
import shlex
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path

INTEGRATION_DIR = Path(__file__).resolve().parent

# QEMU configuration
QEMU = "qemu-system-arm"
UART_HOST = "localhost"
UART_PORT = 4321

# Batching parameters
BATCH_INTERVAL = 2.0   # seconds between bundle sends
BATCH_MAX_LINES = 30   # cap per bundle
RECV_BUF_MAX = 64 * 1024  # discard recv buffer if no newline after 64 KB

# DTN endpoints
SOURCE_EID = "ipn:1.3"
DEST_EID = "ipn:2.3"

COMPOSE = ["docker", "compose", "--project-directory", str(INTEGRATION_DIR)]

# Global for signal handler cleanup
_bridge_state = {"qemu_proc": None, "sock": None, "stopping": False}


def launch_qemu(kernel_path, port=UART_PORT):
    """Launch QEMU with UART redirected to a TCP socket."""
    cmd = [
        QEMU,
        "-machine", "mps2-an385",
        "-cpu", "cortex-m3",
        "-nographic",
        "-chardev", f"socket,id=uart0,host={UART_HOST},port={port},server=on,wait=off",
        "-serial", "chardev:uart0",
        "-kernel", str(kernel_path),
    ]
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    _bridge_state["qemu_proc"] = proc
    return proc


def connect_uart(host, port, retries=50, delay=0.1):
    """Connect to QEMU's UART TCP socket with retries."""
    qemu_proc = _bridge_state["qemu_proc"]
    for i in range(retries):
        if _bridge_state["stopping"]:
            raise RuntimeError("Interrupted during UART connect")
        if qemu_proc and qemu_proc.poll() is not None:
            raise RuntimeError(
                f"QEMU exited (code {qemu_proc.returncode}) before UART connected. "
                f"Port {port} may be in use."
            )
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            sock.connect((host, port))
            _bridge_state["sock"] = sock
            return sock
        except OSError:
            sock.close()
            if i < retries - 1:
                time.sleep(delay)
    raise RuntimeError(f"Could not connect to QEMU UART at {host}:{port} after {retries} retries")


def inject_bundle(lines, compose_cmd):
    """Write telemetry batch into spacecraft container and send as a bundle.

    The temp file is PID-scoped to avoid races between concurrent bridge runs.
    It's overwritten each batch and cleaned up on container teardown.
    """
    payload = "\n".join(lines) + "\n"
    escaped = shlex.quote(payload)
    tmp_file = f"/tmp/telem_batch_{os.getpid()}.txt"
    cmd = f"printf '%s' {escaped} > {tmp_file} && bpsendfile {SOURCE_EID} {DEST_EID} {tmp_file}"
    result = subprocess.run(
        [*compose_cmd, "exec", "-T", "spacecraft", "bash", "-c", cmd],
        capture_output=True, text=True, timeout=30,
    )
    if result.returncode != 0 and result.stderr:
        print(f"  inject stderr: {result.stderr.strip()}", file=sys.stderr)
    return result.returncode == 0


def run_bridge(kernel_path, port=UART_PORT, duration=None, compose=None):
    """Main bridge loop: read UART, batch telemetry, inject bundles.

    Returns a stats dict: {lines_read, telem_lines, bundles_sent}.
    """
    if compose is None:
        compose = COMPOSE
    stats = {"lines_read": 0, "telem_lines": 0, "bundles_sent": 0}

    qemu_proc = launch_qemu(kernel_path, port=port)
    print(f"QEMU launched (PID {qemu_proc.pid}), connecting to UART socket...", file=sys.stderr)

    sock = None
    try:
        sock = connect_uart(UART_HOST, port)
        sock.setblocking(False)
        print(f"Connected to UART at {UART_HOST}:{port}", file=sys.stderr)

        batch = []
        batch_start = time.monotonic()
        bridge_start = time.monotonic()
        recv_buf = ""
        while not _bridge_state["stopping"]:
            # Check duration limit
            if duration and (time.monotonic() - bridge_start) >= duration:
                break

            # Check QEMU still running
            if qemu_proc.poll() is not None:
                print(f"QEMU exited (code {qemu_proc.returncode})", file=sys.stderr)
                break

            # Wait for data with timeout for batch flush
            remaining = BATCH_INTERVAL - (time.monotonic() - batch_start)
            timeout = max(0.1, min(remaining, 0.5))
            ready, _, _ = select.select([sock], [], [], timeout)

            if ready:
                try:
                    chunk = sock.recv(4096)
                except OSError:
                    break
                if not chunk:
                    break  # EOF
                recv_buf += chunk.decode("ascii", errors="replace")

                # Guard against unbounded growth if QEMU sends binary/malformed
                # data without newlines (e.g. misconfigured chardev).
                if len(recv_buf) > RECV_BUF_MAX and "\n" not in recv_buf:
                    recv_buf = ""

                # Process complete lines from buffer
                while "\n" in recv_buf:
                    line, recv_buf = recv_buf.split("\n", 1)
                    line = line.strip()
                    if not line:
                        continue
                    stats["lines_read"] += 1

                    # Forward only telemetry data lines; diagnostic lines
                    # (starting with #) and any other output are skipped.
                    if line.startswith("$TELEM,"):
                        batch.append(line)
                        stats["telem_lines"] += 1

            # Flush batch on interval or size cap
            elapsed = time.monotonic() - batch_start
            if batch and (elapsed >= BATCH_INTERVAL or len(batch) >= BATCH_MAX_LINES):
                if inject_bundle(batch, compose):
                    stats["bundles_sent"] += 1
                    if stats["bundles_sent"] % 5 == 0:
                        print(
                            f"  [{stats['bundles_sent']} bundles sent, "
                            f"{stats['telem_lines']} telem lines]",
                            file=sys.stderr,
                        )
                batch = []
                batch_start = time.monotonic()

        # Flush remaining (including on SIGINT — don't silently drop the last batch)
        if batch:
            if inject_bundle(batch, compose):
                stats["bundles_sent"] += 1

    finally:
        if sock:
            sock.close()
            _bridge_state["sock"] = None
        qemu_proc.terminate()
        try:
            qemu_proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            qemu_proc.kill()
            qemu_proc.wait()
        _bridge_state["qemu_proc"] = None

    return stats


def _signal_handler(signum, frame):
    _bridge_state["stopping"] = True


def main():
    parser = argparse.ArgumentParser(description="UART bridge: QEMU telemetry to DTN")
    parser.add_argument("--kernel", default="build/firmware.elf",
                        help="Path to firmware ELF (default: build/firmware.elf)")
    parser.add_argument("--port", type=int, default=UART_PORT,
                        help=f"QEMU UART TCP port (default: {UART_PORT})")
    parser.add_argument("--duration", type=float, default=None,
                        help="Run for N seconds then exit (default: run forever)")
    args = parser.parse_args()

    kernel = Path(args.kernel)
    if not kernel.is_absolute():
        kernel = INTEGRATION_DIR / kernel
    if not kernel.exists():
        print(f"Firmware not found: {kernel}", file=sys.stderr)
        print("Run 'make' first to build.", file=sys.stderr)
        sys.exit(1)

    signal.signal(signal.SIGINT, _signal_handler)
    signal.signal(signal.SIGTERM, _signal_handler)

    print(f"Starting UART bridge (kernel={kernel}, port={args.port}, duration={args.duration})", file=sys.stderr)
    stats = run_bridge(kernel, port=args.port, duration=args.duration)

    print(f"STATS: lines_read={stats['lines_read']} "
          f"telem_lines={stats['telem_lines']} "
          f"bundles_sent={stats['bundles_sent']}")
    print(f"Bridge stopped. {stats['bundles_sent']} bundles sent.", file=sys.stderr)


if __name__ == "__main__":
    main()
