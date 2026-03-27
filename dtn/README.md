# DTN: ION on Linux

Two-node delay-tolerant network using NASA JPL's ION stack in Docker containers. Bundles travel between a "spacecraft" node and a "ground station" node over a simulated space link.

## Architecture

```
+------------------+       Docker bridge        +------------------+
|   ion-node1      |      network (dtn-net)     |   ion-node2      |
|   "spacecraft"   | <-- LTP over UDP :1113 --> | "ground station" |
|   ipn:1.*        |     1s one-way delay       |   ipn:2.*        |
|                  |     100 KB/s bandwidth     |                  |
+------------------+                            +------------------+
```

- **Node 1** (spacecraft): ION node ID 1, endpoints `ipn:1.0` through `ipn:1.2`
- **Node 2** (ground station): ION node ID 2, endpoints `ipn:2.0` through `ipn:2.2`
- **LTP** (Licklider Transmission Protocol) over UDP on Docker bridge networking (port 1113)
- Contact window: 24 hours at 100 KB/s with 1-second one-way light time

## Quick Start

```bash
# Build the ION Docker image (first time takes 3-8 minutes)
make -C dtn build

# Start the two-node network interactively
make -C dtn run        # Ctrl-C to stop

# Run integration tests
make -C dtn test

# Clean up containers and images
make -C dtn clean

# Note: if you `make run` then later `make test` without cleaning,
# the 24-hour contact window may have expired. Run `make clean` first.
```

## Output

```
Running ION DTN integration tests...
Waiting for ION daemons...
  PASS: node1 ION running
  PASS: node2 ION running
  PASS: bping node1->node2
  PASS: bping node2->node1
  PASS: bundle node1->node2 delivered
  PASS: bundle node2->node1 delivered
6 passed, 0 failed
```

## Key Concepts

### How this fits in the learning path

The FreeRTOS module ran tasks inside a single QEMU-emulated microcontroller. This module introduces network communication: two Linux processes in Docker containers exchange data using NASA's DTN protocols.

### DTN vs TCP/IP

TCP/IP assumes low latency, continuous connectivity, and symmetric links. Space links have none of these: light takes 3-22 minutes to reach Mars, and orbiting relays have intermittent contact windows. DTN (Delay-Tolerant Networking) uses **store-and-forward**: bundles are held at each hop until the next link is available, like a postal system.

### Bundle Protocol

The Bundle Protocol (RFC 9171) is to DTN what IP is to the internet. A "bundle" is a self-contained message with source/destination addresses, lifetime, and priority. ION implements BPv7 with IPN (Interplanetary) addressing: `ipn:<node>.<service>`.

### LTP (Licklider Transmission Protocol)

LTP is the convergence layer designed for deep space. Unlike TCP, LTP expects long round-trip times and handles retransmission with timers tuned to one-way light time. It runs over UDP and breaks bundles into segments that are individually acknowledged. In the bping output you'll see ~1.5s round-trip times — that's the 1-second OWLT from the contact/range config being enforced by LTP's retransmission timing.

Real missions (Mars relay, Lunar Gateway) use LTP. STCP is simpler but hides the delay characteristics that make space networking interesting.

### Contact-Graph Routing

ION doesn't use routing tables. Instead, contacts (scheduled communication windows) are declared in advance. ION's routing engine computes the best path for each bundle based on when links are available, how much bandwidth they have, and the bundle's deadline. In this demo, a simple always-on contact is used; later exercises add realistic scheduled windows.

## Project Structure

```
Dockerfile          - Multi-stage build: compile ION 4.1.4 from source, slim runtime
docker-compose.yml  - Two-node network with bridge networking
configs/
  node1/node.rc     - Combined ION config (ionadmin, ionsecadmin, ltpadmin, bpadmin, ipnadmin)
  node2/node.rc     - Same structure, different node ID and routing
scripts/
  ionstart.sh       - Container entrypoint: starts ION daemons via ionstart
  test.py           - Integration tests (ION status, bping, bpsendfile/bprecvfile)
Makefile            - build, run, test, clean targets
```

## ION Configuration Highlights

| Setting | Value | Why |
|---------|-------|-----|
| Convergence layer | LTP over UDP | What real deep-space missions use |
| Contact bandwidth | 100,000 bytes/sec | Realistic low-bandwidth space link |
| One-way light time | 1 second | Simulates propagation delay |
| Contact duration | 24 hours | Relative to ionadmin start; restart resets the clock |
| Bundle Protocol | BPv7 (ION 4.1.4) | Current standard (RFC 9171) |
| Config format | Combined `.rc` file | ION's `ionstart -I` handles ordering |

> **Note:** Containers run as root because ION's shared memory requires `IPC_OWNER`.
