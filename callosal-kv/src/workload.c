// workload.c — uniform and zipf(theta) op-stream generators.

#include "workload.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>

// splitmix64 — local copy (same constants as table/populate). Keeping a
// per-thread `state` lets workload generators run lock-free in parallel.
static uint64_t splitmix64(uint64_t *state) {
    uint64_t x = (*state += UINT64_C(0x9e3779b97f4a7c15));
    x = (x ^ (x >> 30U)) * UINT64_C(0xbf58476d1ce4e5b9);
    x = (x ^ (x >> 27U)) * UINT64_C(0x94d049bb133111eb);
    x ^= x >> 31U;
    return x;
}

// zeta(n, theta) = sum_{i=1..n} 1/i^theta. O(n) — fine for init, called once.
static double zeta(uint64_t n, double theta) {
    double sum = 0.0;
    for (uint64_t i = 1; i <= n; ++i) {
        sum += 1.0 / pow((double)i, theta);
    }
    return sum;
}

int workload_init(struct workload *w,
                  enum dist_kind dist,
                  double zipf_theta,
                  double write_ratio,
                  uint64_t n_records,
                  uint64_t seed) {
    if (w == NULL || n_records == 0U) {
        return -1;
    }
    if (dist == DIST_ZIPF && !(zipf_theta > 0.0 && zipf_theta < 1.0)) {
        return -1;
    }
    if (write_ratio < 0.0) {
        write_ratio = 0.0;
    }
    if (write_ratio > 1.0) {
        write_ratio = 1.0;
    }
    w->dist = dist;
    w->n_records = n_records;
    w->state = seed == 0U ? UINT64_C(0x9e3779b97f4a7c15) : seed;
    // Compare the top 32 bits of a 64-bit PRNG draw against this threshold.
    // write_ratio == 1.0 must round up to 2^32 (all writes).
    double scaled = write_ratio * 4294967296.0;
    w->write_ratio_q32 = (scaled >= 4294967296.0)
                             ? UINT32_MAX
                             : (uint32_t)scaled;

    if (dist == DIST_ZIPF) {
        w->zipf_theta = zipf_theta;
        w->zipf_alpha = 1.0 / (1.0 - zipf_theta);
        w->zipf_zetan = zeta(n_records, zipf_theta);
        w->zipf_zeta2 = zeta(2U, zipf_theta);
        double n = (double)n_records;
        w->zipf_eta = (1.0 - pow(2.0 / n, 1.0 - zipf_theta)) /
                      (1.0 - w->zipf_zeta2 / w->zipf_zetan);
    } else {
        w->zipf_theta = 0.0;
        w->zipf_alpha = 0.0;
        w->zipf_zetan = 0.0;
        w->zipf_zeta2 = 0.0;
        w->zipf_eta   = 0.0;
    }
    return 0;
}

// Gray & Marchand zipf sampler (same shape as YCSB's ScrambledZipfianGenerator
// inner core): draw u ~ Uniform(0,1), compare against eta, return one of two
// closed-form expressions. Produces an index in [0, n_records).
uint64_t zipf_next(struct workload *w) {
    uint64_t r = splitmix64(&w->state);
    double u = (double)r / 18446744073709551616.0;  // [0, 1)
    double uz = u * w->zipf_zetan;
    if (uz < 1.0) {
        return 0;
    }
    if (uz < 1.0 + pow(0.5, w->zipf_theta)) {
        return 1;
    }
    double x = (double)w->n_records *
               pow(w->zipf_eta * u - w->zipf_eta + 1.0, w->zipf_alpha);
    uint64_t idx = (uint64_t)x;
    if (idx >= w->n_records) {
        idx = w->n_records - 1U;
    }
    return idx;
}

uint64_t uniform_next(struct workload *w) {
    uint64_t r = splitmix64(&w->state);
    return r % w->n_records;
}

void workload_next_op(struct workload *w,
                      enum op_kind *kind_out,
                      uint64_t *key_index_out) {
    uint64_t roll = splitmix64(&w->state);
    uint32_t top = (uint32_t)(roll >> 32U);
    *kind_out = (top < w->write_ratio_q32) ? OP_PUT : OP_GET;

    uint64_t idx;
    if (w->dist == DIST_ZIPF) {
        idx = zipf_next(w);
    } else {
        idx = uniform_next(w);
    }
    *key_index_out = idx;
}
