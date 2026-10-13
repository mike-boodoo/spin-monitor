CC     ?= gcc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -pthread

all: spinmon test_contention test_monitor_lease test_rendezvous

spinmon: spinmon.c
	$(CC) $(CFLAGS) $< -o $@

test_contention: test_contention.c spinmon.c
	$(CC) $(CFLAGS) $< -o $@

test_monitor_lease: test_monitor_lease.c spinmon.c
	$(CC) $(CFLAGS) $< -o $@

test_rendezvous: test_rendezvous.c spinmon.c
	$(CC) $(CFLAGS) $< -o $@

# Lease/monitor tests are deterministic; the contention gate needs a baseline (run `make baseline` first).
test: test_monitor_lease test_rendezvous
	./test_monitor_lease
	./test_rendezvous

baseline: test_contention
	./test_contention record

gate: test_contention
	./test_contention

demo: spinmon
	./spinmon

clean:
	rm -f spinmon test_contention test_monitor_lease test_rendezvous

.PHONY: all test baseline gate demo clean
