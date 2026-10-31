// stats.h — per-thread op counters and latency histograms.
//
// Each worker owns a `struct thread_stats`; there is no cross-thread
// sharing on the hot path. Aggregation and JSON emission happen once,
// post-join, in the main thread.
//
// Latency histograms are log2-bucketed in nanoseconds: bucket i covers
// [2^i, 2^(i+1)) ns. 64 buckets span 1 ns to ~584 years, which is plenty.

#ifndef CALLOSAL_KV_STATS_H
#define CALLOSAL_KV_STATS_H

#include <stddef.h>
#include <stdint.h>

#include "workload.h"

#define LAT_HIST_BUCKETS 64

struct lat_hist {
    uint64_t buckets[LAT_HIST_BUCKETS];
};

struct thread_stats {
    uint64_t gets;
    uint64_t puts;
    uint64_t get_not_found;
    uint64_t put_not_found;
    uint64_t get_retries;     // CLIENT_RETRY returns
    uint64_t errors;
    struct lat_hist get_lat;
    struct lat_hist put_lat;
};

struct agg_stats {
    uint64_t total_ops;
    uint64_t total_gets;
    uint64_t total_puts;
    uint64_t total_get_not_found;
    uint64_t total_put_not_found;
    uint64_t total_get_retries;
    uint64_t total_errors;
    uint64_t duration_ns;       // wall clock of the measurement window
    struct lat_hist get_lat;
    struct lat_hist put_lat;
    // Derived percentiles in ns (p50, p95, p99) for convenience.
    uint64_t get_p50_ns, get_p95_ns, get_p99_ns;
    uint64_t put_p50_ns, put_p95_ns, put_p99_ns;
};

void stats_init(struct thread_stats *s);
void stats_record(struct thread_stats *s, enum op_kind kind, uint64_t latency_ns);

void stats_aggregate(struct agg_stats *out,
                     const struct thread_stats *arr,
                     size_t n,
                     uint64_t duration_ns);

// stats_dump_json: write a single-line JSON object capturing the aggregate
// and per-operation histograms to `path`. Returns 0 on success.
int stats_dump_json(const struct agg_stats *a, const char *path);

#endif  // CALLOSAL_KV_STATS_H
