// test_table.c — geometry, hashing, and placement math.

#include "test.h"
#include "tests.h"

#include <stdint.h>

#include "table.h"

void test_table_num_buckets_rounds_up(void) {
    // With SLOTS_PER_BUCKET=28 and LF=75, one bucket holds ~21 records.
    // 1 record → at least 1 bucket.
    ASSERT_EQ_U64(table_num_buckets(1, 75), 1U);
    // 21 records → 1 bucket, 22 records → 2 buckets (ceil boundary).
    ASSERT_EQ_U64(table_num_buckets(21, 75), 1U);
    ASSERT_EQ_U64(table_num_buckets(22, 75), 2U);
    // 0 records → 1 bucket (degenerate but parameters valid).
    ASSERT_EQ_U64(table_num_buckets(0, 75), 1U);
}

void test_table_num_buckets_rejects_bad_lf(void) {
    ASSERT_EQ_U64(table_num_buckets(1000, 0), 0U);
    ASSERT_EQ_U64(table_num_buckets(1000, 101), 0U);
}

void test_table_buckets_per_shard(void) {
    ASSERT_EQ_U64(table_buckets_per_shard(0), UINT64_MAX);
    // The cap is now expressed directly in 4 KiB bucket-pages.
    ASSERT_EQ_U64(table_buckets_per_shard(1), 1U);
    ASSERT_EQ_U64(table_buckets_per_shard(4), 4U);
}

void test_table_geometry_init(void) {
    struct table_geometry g = {0};
    ASSERT_EQ_INT(table_geometry_init(&g, 1000, 1, 75, 0), 0);
    ASSERT_EQ_U64(g.n_records, 1000U);
    ASSERT_EQ_U64(g.n_nodes, 1U);
    ASSERT_EQ_U64(g.load_factor_pct, 75U);
    ASSERT_EQ_U64(g.buckets_per_shard, UINT64_MAX);
    ASSERT_TRUE(g.n_buckets > 0U);
}

void test_table_geometry_init_rejects_zero(void) {
    struct table_geometry g = {0};
    ASSERT_EQ_INT(table_geometry_init(&g, 0, 1, 75, 0), -1);
    ASSERT_EQ_INT(table_geometry_init(&g, 1000, 0, 75, 0), -1);
    ASSERT_EQ_INT(table_geometry_init(&g, 1000, 1, 0, 0), -1);
    ASSERT_EQ_INT(table_geometry_init(NULL, 1000, 1, 75, 0), -1);
}

void test_table_hash_avalanche(void) {
    // Different keys must produce different hashes; consecutive keys
    // should not collapse to the same hash.
    ASSERT_TRUE(table_hash(0) != table_hash(1));
    ASSERT_TRUE(table_hash(1) != table_hash(2));
    ASSERT_TRUE(table_hash(0xABCD) != table_hash(0xABCE));

    // Hash of zero is not zero (splitmix finalizer breaks the fixed point).
    ASSERT_TRUE(table_hash(0) != 0U);
}

void test_table_hash_distribution(void) {
    // Hash N keys into B buckets; count per bucket; check no bucket gets
    // more than 4x the expected share. This is a loose sanity check, not
    // a rigorous chi-square.
    enum { N = 10000, B = 128 };
    int counts[B] = {0};
    for (int i = 0; i < N; ++i) {
        uint64_t h = table_hash((uint64_t)i);
        counts[h % (uint64_t)B]++;
    }
    int expected = N / B;
    int max_seen = 0;
    int min_seen = N;
    for (int i = 0; i < B; ++i) {
        if (counts[i] > max_seen) {
            max_seen = counts[i];
        }
        if (counts[i] < min_seen) {
            min_seen = counts[i];
        }
    }
    ASSERT_TRUE(max_seen < expected * 4);
    ASSERT_TRUE(min_seen > 0);
}

void test_table_owner_striping(void) {
    ASSERT_EQ_U64(table_owner(0, 4), 0U);
    ASSERT_EQ_U64(table_owner(1, 4), 1U);
    ASSERT_EQ_U64(table_owner(2, 4), 2U);
    ASSERT_EQ_U64(table_owner(3, 4), 3U);
    ASSERT_EQ_U64(table_owner(4, 4), 0U);
    ASSERT_EQ_U64(table_owner(5, 4), 1U);

    ASSERT_EQ_U64(table_local_rank(0, 4), 0U);
    ASSERT_EQ_U64(table_local_rank(1, 4), 0U);
    ASSERT_EQ_U64(table_local_rank(4, 4), 1U);
    ASSERT_EQ_U64(table_local_rank(5, 4), 1U);
    ASSERT_EQ_U64(table_local_rank(9, 4), 2U);
}

void test_table_shard_placement(void) {
    // buckets_per_shard = 4 means rank 0 → shard 0 off 0,
    // rank 3 → shard 0 off 3*PAGE_SIZE, rank 4 → shard 1 off 0.
    ASSERT_EQ_U64(table_shard_index(0, 4), 0U);
    ASSERT_EQ_U64(table_shard_index(3, 4), 0U);
    ASSERT_EQ_U64(table_shard_index(4, 4), 1U);
    ASSERT_EQ_U64(table_shard_index(15, 4), 3U);

    ASSERT_EQ_U64(table_bucket_offset(0, 4), 0U);
    ASSERT_EQ_U64(table_bucket_offset(1, 4), (uint64_t)PAGE_SIZE);
    ASSERT_EQ_U64(table_bucket_offset(3, 4), 3U * (uint64_t)PAGE_SIZE);
    ASSERT_EQ_U64(table_bucket_offset(4, 4), 0U);

    // Uncapped.
    ASSERT_EQ_U64(table_shard_index(123456, UINT64_MAX), 0U);
    ASSERT_EQ_U64(table_bucket_offset(5, UINT64_MAX), 5U * (uint64_t)PAGE_SIZE);
}

void test_table_buckets_for_owner_striped(void) {
    struct table_geometry g = {0};
    (void)table_geometry_init(&g, 10000, 4, 75, 0);
    uint64_t total = 0;
    for (uint32_t i = 0; i < 4; ++i) {
        total += table_buckets_for_owner(&g, i);
    }
    ASSERT_EQ_U64(total, g.n_buckets);

    // Each owner's count differs from any other's by at most 1 for the
    // striped distribution.
    uint64_t min = UINT64_MAX;
    uint64_t max = 0;
    for (uint32_t i = 0; i < 4; ++i) {
        uint64_t c = table_buckets_for_owner(&g, i);
        if (c < min) { min = c; }
        if (c > max) { max = c; }
    }
    ASSERT_TRUE(max - min <= 1U);
}
