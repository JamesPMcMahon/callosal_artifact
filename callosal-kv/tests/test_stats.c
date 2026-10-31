// test_stats.c — counter aggregation and histogram percentile math.

#include "test.h"
#include "tests.h"

#include <stdint.h>

#include "stats.h"
#include "workload.h"

void test_stats_record_and_aggregate(void) {
    struct thread_stats a, b;
    stats_init(&a);
    stats_init(&b);
    a.gets = 100; a.puts = 50;
    b.gets = 200; b.puts = 10;
    stats_record(&a, OP_GET, 1000U);   // ~1 us
    stats_record(&a, OP_PUT, 1000000U); // 1 ms
    stats_record(&b, OP_GET, 500U);
    stats_record(&b, OP_PUT, 2000000U);

    struct thread_stats arr[2] = { a, b };
    struct agg_stats agg;
    stats_aggregate(&agg, arr, 2, 1000000000ULL);
    ASSERT_EQ_U64(agg.total_gets, 300U);
    ASSERT_EQ_U64(agg.total_puts, 60U);
    ASSERT_EQ_U64(agg.total_ops, 360U);
    ASSERT_EQ_U64(agg.duration_ns, 1000000000ULL);
    // Two samples each in get_lat/put_lat histograms.
    uint64_t get_samples = 0, put_samples = 0;
    for (size_t i = 0; i < LAT_HIST_BUCKETS; ++i) {
        get_samples += agg.get_lat.buckets[i];
        put_samples += agg.put_lat.buckets[i];
    }
    ASSERT_EQ_U64(get_samples, 2U);
    ASSERT_EQ_U64(put_samples, 2U);
}

void test_stats_percentiles(void) {
    // Feed a known distribution, verify p99 lands in a sensible bucket.
    struct thread_stats s;
    stats_init(&s);
    // 98 samples at ~1 us + 2 outliers at ~1 ms. The 99th-percentile
    // target lands in the outlier bucket (cum hits 99 only past them).
    for (int i = 0; i < 98; ++i) {
        stats_record(&s, OP_GET, 1000U);
    }
    stats_record(&s, OP_GET, 1000000U);
    stats_record(&s, OP_GET, 1000000U);
    struct agg_stats agg;
    stats_aggregate(&agg, &s, 1, 1);
    // p50 should land in the 1 us range.
    ASSERT_TRUE(agg.get_p50_ns < 10000U);
    // p99 must be >= 1 ms (the one outlier is the 100th sample).
    ASSERT_TRUE(agg.get_p99_ns >= 1000000U);
}
