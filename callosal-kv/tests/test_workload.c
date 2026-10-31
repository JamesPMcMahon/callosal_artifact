// test_workload.c — op-stream distribution sanity checks.

#include "test.h"
#include "tests.h"

#include <stdint.h>
#include <stdlib.h>

#include "workload.h"

void test_workload_uniform_write_ratio(void) {
    struct workload w;
    ASSERT_EQ_INT(workload_init(&w, DIST_UNIFORM, 0.0, 0.25, 1000, 1234), 0);
    uint64_t puts = 0, gets = 0;
    for (int i = 0; i < 100000; ++i) {
        enum op_kind k;
        uint64_t idx;
        workload_next_op(&w, &k, &idx);
        if (k == OP_PUT) { puts++; } else { gets++; }
    }
    // Expect ~25000 puts; tolerance +/- 1000 (~4%).
    ASSERT_TRUE(puts > 24000U && puts < 26000U);
    ASSERT_TRUE(gets > 73000U && gets < 76000U);
}

void test_workload_uniform_coverage(void) {
    // Every index in a small universe should be seen at least once with
    // high probability over 100x coverage.
    struct workload w;
    ASSERT_EQ_INT(workload_init(&w, DIST_UNIFORM, 0.0, 0.0, 100, 42), 0);
    int seen[100] = {0};
    for (int i = 0; i < 10000; ++i) {
        enum op_kind k;
        uint64_t idx;
        workload_next_op(&w, &k, &idx);
        ASSERT_TRUE(idx < 100U);
        seen[idx] = 1;
    }
    int covered = 0;
    for (int i = 0; i < 100; ++i) { covered += seen[i]; }
    ASSERT_EQ_INT(covered, 100);
}

void test_workload_zipf_skew(void) {
    // Zipf(theta=0.99) should concentrate probability mass at low indices.
    // Check that the single most-popular index accounts for >5% of ops —
    // a very loose bound; YCSB's zipfian-0.99 puts the head near ~20%.
    struct workload w;
    ASSERT_EQ_INT(workload_init(&w, DIST_ZIPF, 0.99, 0.0, 10000, 7), 0);
    enum { N = 20000 };
    int counts[10000] = {0};
    for (int i = 0; i < N; ++i) {
        enum op_kind k;
        uint64_t idx;
        workload_next_op(&w, &k, &idx);
        ASSERT_TRUE(idx < 10000U);
        counts[idx]++;
    }
    int head_max = 0;
    for (int i = 0; i < 10; ++i) {
        if (counts[i] > head_max) { head_max = counts[i]; }
    }
    ASSERT_TRUE(head_max > N / 20);  // >5% of ops on a head item
}

void test_workload_rejects_bad_theta(void) {
    struct workload w;
    ASSERT_EQ_INT(workload_init(&w, DIST_ZIPF, 0.0, 0.5, 1000, 1), -1);
    ASSERT_EQ_INT(workload_init(&w, DIST_ZIPF, 1.0, 0.5, 1000, 1), -1);
    ASSERT_EQ_INT(workload_init(&w, DIST_ZIPF, 1.5, 0.5, 1000, 1), -1);
    // Uniform ignores theta.
    ASSERT_EQ_INT(workload_init(&w, DIST_UNIFORM, 99.0, 0.5, 1000, 1), 0);
}
