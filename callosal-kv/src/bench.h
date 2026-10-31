// bench.h — worker-loop driver for the measurement phase.
//
// Owns thread spawn, pthread_barrier-based start synchronization, the
// time-bounded op loop (reading from workload, filtering CREW writes,
// timing via clock_gettime(MONOTONIC)), and probe observation. Finalizes
// by aggregating per-thread stats and writing the output JSON.

#ifndef CALLOSAL_KV_BENCH_H
#define CALLOSAL_KV_BENCH_H

#include <stdbool.h>
#include <stdint.h>

#include "probe.h"
#include "workload.h"

struct bench_config {
    const char *data_dir;
    uint32_t    node_id;
    uint32_t    n_nodes;
    uint64_t    records;
    uint64_t    load_factor_pct;
    uint64_t    max_file_pages;

    uint32_t    threads;
    uint32_t    duration_sec;
    uint32_t    warmup_sec;  // 0 disables warmup entirely
    uint64_t    seed;

    enum dist_kind  dist;
    double          zipf_theta;
    double          write_ratio;

    enum probe_mode probe_mode;
    uint64_t        probe_capacity;

    bool        pin;
    const char *out_path;
};

// bench_run: execute the measurement phase end-to-end. Returns 0 on clean
// completion (regardless of monotonic violations — those are reported in
// the output JSON). Returns -1 on I/O or setup errors.
int bench_run(const struct bench_config *cfg);

#endif  // CALLOSAL_KV_BENCH_H
