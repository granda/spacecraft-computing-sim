# Phase 3 — Two-node DTN network
#   make build        Build the ION Docker image
#   make run          Build + launch both nodes
#   make test         Build + run the test suite
#   make clean        Tear down containers, images, and volumes
SHELL = /bin/bash
COMPOSE = docker compose

.PHONY: all build run test test-degraded check-configs clean

all: build

build:
	$(COMPOSE) build

run: build
	$(COMPOSE) up

check-configs:
	@diff <(grep -E '^a (contact|range) ' configs/node1/node.rc | sort) \
	      <(grep -E '^a (contact|range) ' configs/node2/node.rc | sort) \
	  || { echo "ERROR: topology tables differ between node1 and node2"; exit 1; }
	@echo "  Config topology tables are in sync"

test: build check-configs
	@PYTHONPATH=scripts python3 scripts/test_basic.py

test-degraded: build check-configs
	@PYTHONPATH=scripts python3 scripts/test_degraded.py

clean:
	$(COMPOSE) down --rmi local --volumes --remove-orphans 2>/dev/null || true
