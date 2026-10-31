// stats.c — counters, latency histograms, aggregation, JSON emission.

#include "stats.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

void stats_init(struct thread_stats *s) {
    memset(s, 0, sizeof *s);
}

// ilog2 for 64-bit: returns 0 for x == 0, otherwise floor(log2(x)).
// Cap at LAT_HIST_BUCKETS - 1.
static unsigned ilog2_u64(uint64_t x) {
    if (x == 0U) {
        return 0U;
    }
    unsigned b = 0U;
    while ((x >> 1U) != 0U) {
        x >>= 1U;
        b++;
        if (b >= (unsigned)(LAT_HIST_BUCKETS - 1)) {
            return (unsigned)(LAT_HIST_BUCKETS - 1);
        }
    }
    return b;
}

void stats_record(struct thread_stats *s, enum op_kind kind, uint64_t latency_ns) {
    unsigned b = ilog2_u64(latency_ns);
    if (kind == OP_GET) {
        s->get_lat.buckets[b]++;
    } else {
        s->put_lat.buckets[b]++;
    }
}

static void hist_merge(struct lat_hist *dst, const struct lat_hist *src) {
    for (size_t i = 0; i < LAT_HIST_BUCKETS; ++i) {
        dst->buckets[i] += src->buckets[i];
    }
}

static uint64_t hist_total(const struct lat_hist *h) {
    uint64_t total = 0;
    for (size_t i = 0; i < LAT_HIST_BUCKETS; ++i) {
        total += h->buckets[i];
    }
    return total;
}

// Given a sorted histogram, find the upper bound of the bucket containing
// the pth percentile (0..100). Returns 2^(bucket+1) - 1 as an estimate of
// the upper edge, which is conservative but consistent.
static uint64_t hist_percentile(const struct lat_hist *h, unsigned p) {
    uint64_t total = hist_total(h);
    if (total == 0U) {
        return 0U;
    }
    uint64_t target = (total * (uint64_t)p + 99U) / 100U;
    if (target == 0U) {
        target = 1U;
    }
    uint64_t cum = 0;
    for (size_t i = 0; i < LAT_HIST_BUCKETS; ++i) {
        cum += h->buckets[i];
        if (cum >= target) {
            // Upper edge of bucket i is (1 << (i+1)) - 1, saturating.
            if (i + 1U >= 63U) {
                return UINT64_MAX;
            }
            return (UINT64_C(1) << (i + 1U)) - 1U;
        }
    }
    return UINT64_MAX;
}

void stats_aggregate(struct agg_stats *out,
                     const struct thread_stats *arr,
                     size_t n,
                     uint64_t duration_ns) {
    memset(out, 0, sizeof *out);
    out->duration_ns = duration_ns;
    for (size_t i = 0; i < n; ++i) {
        out->total_gets          += arr[i].gets;
        out->total_puts          += arr[i].puts;
        out->total_get_not_found += arr[i].get_not_found;
        out->total_put_not_found += arr[i].put_not_found;
        out->total_get_retries   += arr[i].get_retries;
        out->total_errors        += arr[i].errors;
        hist_merge(&out->get_lat, &arr[i].get_lat);
        hist_merge(&out->put_lat, &arr[i].put_lat);
    }
    out->total_ops = out->total_gets + out->total_puts;
    out->get_p50_ns = hist_percentile(&out->get_lat, 50);
    out->get_p95_ns = hist_percentile(&out->get_lat, 95);
    out->get_p99_ns = hist_percentile(&out->get_lat, 99);
    out->put_p50_ns = hist_percentile(&out->put_lat, 50);
    out->put_p95_ns = hist_percentile(&out->put_lat, 95);
    out->put_p99_ns = hist_percentile(&out->put_lat, 99);
}

static void emit_hist(FILE *f, const char *name, const struct lat_hist *h) {
    fprintf(f, "\"%s\":[", name);
    for (size_t i = 0; i < LAT_HIST_BUCKETS; ++i) {
        fprintf(f, "%s%llu", (i == 0U) ? "" : ",",
                (unsigned long long)h->buckets[i]);
    }
    fprintf(f, "]");
}

int stats_dump_json(const struct agg_stats *a, const char *path) {
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        fprintf(stderr, "stats: fopen(%s): %s\n", path, strerror(errno));
        return -1;
    }
    fprintf(f, "{");
    fprintf(f, "\"duration_ns\":%llu,",       (unsigned long long)a->duration_ns);
    fprintf(f, "\"total_ops\":%llu,",         (unsigned long long)a->total_ops);
    fprintf(f, "\"gets\":%llu,",              (unsigned long long)a->total_gets);
    fprintf(f, "\"puts\":%llu,",              (unsigned long long)a->total_puts);
    fprintf(f, "\"get_not_found\":%llu,",     (unsigned long long)a->total_get_not_found);
    fprintf(f, "\"put_not_found\":%llu,",     (unsigned long long)a->total_put_not_found);
    fprintf(f, "\"get_retries\":%llu,",       (unsigned long long)a->total_get_retries);
    fprintf(f, "\"errors\":%llu,",            (unsigned long long)a->total_errors);
    fprintf(f, "\"get_p50_ns\":%llu,",        (unsigned long long)a->get_p50_ns);
    fprintf(f, "\"get_p95_ns\":%llu,",        (unsigned long long)a->get_p95_ns);
    fprintf(f, "\"get_p99_ns\":%llu,",        (unsigned long long)a->get_p99_ns);
    fprintf(f, "\"put_p50_ns\":%llu,",        (unsigned long long)a->put_p50_ns);
    fprintf(f, "\"put_p95_ns\":%llu,",        (unsigned long long)a->put_p95_ns);
    fprintf(f, "\"put_p99_ns\":%llu,",        (unsigned long long)a->put_p99_ns);
    emit_hist(f, "get_hist", &a->get_lat);
    fprintf(f, ",");
    emit_hist(f, "put_hist", &a->put_lat);
    fprintf(f, "}\n");
    fclose(f);
    return 0;
}
