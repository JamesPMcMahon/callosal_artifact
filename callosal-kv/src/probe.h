// probe.h — monotonic-read correctness probe.
//
// Every value carries a 64-bit generation counter in the first 8 bytes of
// its payload. Writers strictly increment the counter on every put; readers
// assert that the generation observed for a given key never decreases over
// the reader's lifetime.
//
// Per-key tracking for 10 M keys is memory-heavy, and the probe's job is
// to *flag* violations rather than to prove their absence. We keep a
// direct-mapped hash array of recent observations (CAPACITY entries of
// 16 bytes each). Collisions evict, so the probe has false negatives but
// zero false positives — any reported violation is real.

#ifndef CALLOSAL_KV_PROBE_H
#define CALLOSAL_KV_PROBE_H

#include <stdint.h>
#include <stddef.h>

enum probe_mode {
    PROBE_MODE_NONE      = 0,
    PROBE_MODE_MONOTONIC = 1,
};

struct probe_ctx {
    enum probe_mode mode;
    uint64_t capacity;     // must be a power of two, 0 when mode == NONE
    uint64_t *keys;        // capacity entries; 0 means empty slot
    uint64_t *gens;        // last observed gen for keys[i]
    uint64_t observations; // total observe() calls
    uint64_t violations;   // monotonicity violations
};

// probe_init: allocate internal state for `mode`. `capacity` is the number
// of tracking slots; rounded up to next power of two. Returns 0 on ok,
// -1 on allocation failure. Safe to call with mode == NONE (no alloc).
int probe_init(struct probe_ctx *p, enum probe_mode mode, uint64_t capacity);

// probe_free: release probe memory. Safe on partially-initialized ctx.
void probe_free(struct probe_ctx *p);

// probe_encode_value: fill value_out with VAL_SIZE bytes carrying `gen`
// at bytes [0..7] and a deterministic key-derived tail at [8..VAL_SIZE-1].
// The tail matches populate_value_for's, so populate corresponds to gen=0.
void probe_encode_value(uint64_t key, uint64_t gen, void *value_out);

// probe_decode_gen: extract the generation counter from an encoded value.
uint64_t probe_decode_gen(const void *value_in);

// probe_observe: record the (key, gen) pair. If the slot for this key
// already tracks a gen > the one passed, increments violations.
// Safe to call when mode == NONE (no-op).
void probe_observe(struct probe_ctx *p, uint64_t key, uint64_t gen);

#endif  // CALLOSAL_KV_PROBE_H
