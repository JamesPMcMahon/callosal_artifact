// barrier.h — UDP client side of the coordinator barrier.
//
// Protocol (implemented by harness/ckv/coordinator.py):
//   worker → coord : "READY <node_id>\n"
//   coord  → worker: "GO <start_ns>\n"
//
// The coordinator sits on one cluster-visible host. Each worker process
// calls barrier_wait() once, immediately before starting measurement. On
// success *start_ns_out receives the coordinator's broadcast timestamp
// (opaque — used only for log correlation; the measurement clock is still
// CLOCK_MONOTONIC inside bench.c).

#ifndef CKV_BARRIER_H
#define CKV_BARRIER_H

#include <stddef.h>
#include <stdint.h>

// Parse "host:port" → (host_out, port_out). ``host_out`` must be at
// least 256 bytes. Returns 0 on success, -1 on malformed spec.
int barrier_parse_spec(const char *spec, char *host_out, size_t host_cap,
                       uint16_t *port_out);

// Rendezvous with the coordinator at ``host:port``:
//   - send READY <node_id>
//   - wait for GO <start_ns>, retransmitting READY every ~500 ms until
//     either GO arrives or ``timeout_ms`` elapses.
// Returns 0 on success (``*start_ns_out`` set), -1 on error or timeout.
int barrier_wait(const char *host, uint16_t port, uint32_t node_id,
                 int timeout_ms, uint64_t *start_ns_out);

#endif
