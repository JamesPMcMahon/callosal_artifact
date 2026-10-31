// workload.h — op-stream generator.
//
// Workload produces (op_kind, key_index) pairs. The "key index" is an
// integer in [0, n_records); the bench layer turns it into the physical
// key via populate_key_at() so the workload layer stays decoupled from
// the on-disk layout.
//
// Distributions:
//   - DIST_UNIFORM: each index equally likely.
//   - DIST_ZIPF: YCSB-style zipf(theta). Indices are scrambled through
//     the key-generation PRF (not here, but in the bench layer) so
//     popularity does not align with bucket placement.
//
// CREW: write-ownership filtering happens in the bench layer, not here —
// the workload is an unfiltered stream.

#ifndef CALLOSAL_KV_WORKLOAD_H
#define CALLOSAL_KV_WORKLOAD_H

#include <stdint.h>

enum op_kind {
    OP_GET = 0,
    OP_PUT = 1,
};

enum dist_kind {
    DIST_UNIFORM = 0,
    DIST_ZIPF    = 1,
};

struct workload {
    enum dist_kind dist;
    uint64_t n_records;
    uint64_t state;            // PRNG state (splitmix64)
    uint32_t write_ratio_q32;  // write_ratio * 2^32, compared to top 32 bits

    // Zipf precomputed values (unused when dist == UNIFORM).
    double zipf_theta;
    double zipf_alpha;
    double zipf_zetan;
    double zipf_zeta2;
    double zipf_eta;
};

// workload_init: seed the generator. `write_ratio` is clamped to [0, 1].
// For ZIPF, theta must be in (0, 1); pass 0.99 for the YCSB default.
// Returns 0 on ok, -1 on bad args.
int workload_init(struct workload *w,
                  enum dist_kind dist,
                  double zipf_theta,
                  double write_ratio,
                  uint64_t n_records,
                  uint64_t seed);

// workload_next_op: produce the next (op_kind, key_index) pair.
void workload_next_op(struct workload *w,
                      enum op_kind *kind_out,
                      uint64_t *key_index_out);

uint64_t uniform_next(struct workload *w);
uint64_t zipf_next(struct workload *w);


#endif  // CALLOSAL_KV_WORKLOAD_H
