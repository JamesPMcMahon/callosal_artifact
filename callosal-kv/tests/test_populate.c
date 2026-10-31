// test_populate.c — deterministic key generation + end-to-end populate
// into a temp directory, re-read from disk, and verify every owned
// record lands in the expected bucket with the expected value.

#include "test.h"
#include "tests.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "bucket.h"
#include "populate.h"
#include "table.h"

void test_populate_key_deterministic(void) {
    uint64_t a = populate_key_at(0xCAFEULL, 0);
    uint64_t b = populate_key_at(0xCAFEULL, 0);
    ASSERT_EQ_U64(a, b);

    uint64_t c = populate_key_at(0xCAFEULL, 1);
    ASSERT_TRUE(a != c);

    uint64_t d = populate_key_at(0xBABEULL, 0);
    ASSERT_TRUE(a != d);
}

void test_populate_value_for_key_stable(void) {
    uint8_t v1[VAL_SIZE];
    uint8_t v2[VAL_SIZE];
    populate_value_for(123, v1);
    populate_value_for(123, v2);
    ASSERT_EQ_MEM(v1, v2, VAL_SIZE);

    uint8_t v3[VAL_SIZE];
    populate_value_for(124, v3);
    ASSERT_TRUE(memcmp(v1, v3, VAL_SIZE) != 0);
}

void test_populate_shard_path_formats(void) {
    char buf[256];
    int n = populate_shard_path(buf, sizeof(buf), "/tmp/x", 3, 7);
    ASSERT_TRUE(n > 0);
    ASSERT_TRUE(strcmp(buf, "/tmp/x/table-node3-s7.db") == 0);

    // Overflow detection.
    char small[8];
    int rc = populate_shard_path(small, sizeof(small), "/tmp/x", 3, 7);
    ASSERT_EQ_INT(rc, -1);
}

// Create a unique temp directory under /tmp (or TMPDIR).
static int make_tmp_dir(char *out, size_t cap) {
    const char *base = getenv("TMPDIR");
    if (base == NULL || base[0] == '\0') {
        base = "/tmp";
    }
    int n = snprintf(out, cap, "%s/callosal-kv-test-XXXXXX", base);
    if (n < 0 || (size_t)n >= cap) {
        return -1;
    }
    if (mkdtemp(out) == NULL) {
        return -1;
    }
    return 0;
}

static void rm_rf(const char *path) {
    DIR *d = opendir(path);
    if (d == NULL) {
        (void)unlink(path);
        return;
    }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        }
        char sub[1024];
        snprintf(sub, sizeof(sub), "%s/%s", path, de->d_name);
        rm_rf(sub);
    }
    (void)closedir(d);
    (void)rmdir(path);
}

// Read a shard page back from disk.
static int read_page_from_file(const char *path, uint64_t page_index,
                               struct bucket_page *out) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    ssize_t rd = pread(fd, out, PAGE_SIZE,
                       (off_t)(page_index * (uint64_t)PAGE_SIZE));
    (void)close(fd);
    return rd == PAGE_SIZE ? 0 : -1;
}

void test_populate_end_to_end_small(void) {
    // Small run: 1K records on a single node, verify every key is
    // placed in its expected bucket with its expected value.
    // A 50% target LF keeps per-bucket variance comfortably under the
    // SLOTS_PER_BUCKET ceiling at this record count — the 75% default
    // only has enough law-of-large-numbers smoothing at 10M-scale runs.
    char dir[256];
    ASSERT_EQ_INT(make_tmp_dir(dir, sizeof(dir)), 0);

    struct table_geometry g;
    ASSERT_EQ_INT(table_geometry_init(&g, 1024, 1, 50, 0), 0);

    struct populate_stats stats;
    int rc = populate_run(dir, /*node_id=*/0, /*seed=*/42, &g,
                          /*durable=*/false, &stats);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_U64(stats.records_considered, 1024U);
    ASSERT_EQ_U64(stats.records_owned, 1024U);
    ASSERT_EQ_U64(stats.records_placed, 1024U);

    // Every key should read back with the right value.
    char shard_path[512];
    ASSERT_TRUE(populate_shard_path(shard_path, sizeof(shard_path),
                                    dir, 0, 0) > 0);

    for (uint64_t i = 0; i < g.n_records; ++i) {
        uint64_t key = populate_key_at(42, i);
        uint64_t bucket_id = table_bucket_id(key, g.n_buckets);
        uint64_t rank = table_local_rank(bucket_id, g.n_nodes);

        struct bucket_page page;
        ASSERT_EQ_INT(read_page_from_file(shard_path, rank, &page), 0);

        int slot_idx = bucket_find_key(&page, key);
        if (slot_idx < 0) {
            // The record could have been displaced by linear probe into
            // the next bucket. Check rank+1 if overflow occurred.
            if (stats.overflow_probes > 0U && rank + 1U < g.n_buckets) {
                struct bucket_page next_page;
                ASSERT_EQ_INT(read_page_from_file(shard_path, rank + 1U,
                                                  &next_page), 0);
                slot_idx = bucket_find_key(&next_page, key);
                if (slot_idx >= 0) {
                    uint8_t expected[VAL_SIZE];
                    populate_value_for(key, expected);
                    ASSERT_EQ_MEM(next_page.slots[slot_idx].value,
                                  expected, VAL_SIZE);
                    continue;
                }
            }
        }
        ASSERT_TRUE(slot_idx >= 0);
        uint8_t expected[VAL_SIZE];
        populate_value_for(key, expected);
        ASSERT_EQ_MEM(page.slots[slot_idx].value, expected, VAL_SIZE);
    }

    rm_rf(dir);
}

void test_populate_two_nodes_disjoint(void) {
    // Two nodes populating in parallel must produce disjoint key sets;
    // their union equals the full record stream.
    char dir0[256], dir1[256];
    ASSERT_EQ_INT(make_tmp_dir(dir0, sizeof(dir0)), 0);
    ASSERT_EQ_INT(make_tmp_dir(dir1, sizeof(dir1)), 0);

    struct table_geometry g;
    ASSERT_EQ_INT(table_geometry_init(&g, 512, 2, 50, 0), 0);

    struct populate_stats s0, s1;
    ASSERT_EQ_INT(populate_run(dir0, 0, 7, &g, false, &s0), 0);
    ASSERT_EQ_INT(populate_run(dir1, 1, 7, &g, false, &s1), 0);

    ASSERT_EQ_U64(s0.records_owned + s1.records_owned, 512U);
    ASSERT_EQ_U64(s0.records_placed + s1.records_placed, 512U);
    // Each node should see roughly half — allow wide tolerance.
    ASSERT_TRUE(s0.records_owned > 200U);
    ASSERT_TRUE(s1.records_owned > 200U);

    rm_rf(dir0);
    rm_rf(dir1);
}

void test_populate_sharded_small_cap(void) {
    // Tight per-shard size cap: force multiple shards per owner.
    char dir[256];
    ASSERT_EQ_INT(make_tmp_dir(dir, sizeof(dir)), 0);

    struct table_geometry g;
    // 16 page cap → 16 buckets per shard. Use enough records to force
    // at least 2 shards.
    ASSERT_EQ_INT(table_geometry_init(&g, 20000, 1, 75, 16), 0);
    ASSERT_EQ_U64(g.buckets_per_shard, 16U);
    ASSERT_TRUE(table_shards_for_owner(&g, 0) >= 2U);

    struct populate_stats stats;
    ASSERT_EQ_INT(populate_run(dir, 0, 0xABCD, &g, false, &stats), 0);

    // Every shard file must exist with the expected size.
    for (uint64_t s = 0; s < table_shards_for_owner(&g, 0); ++s) {
        char path[512];
        ASSERT_TRUE(populate_shard_path(path, sizeof(path), dir, 0, s) > 0);
        struct stat st;
        ASSERT_EQ_INT(stat(path, &st), 0);
        ASSERT_TRUE(st.st_size > 0);
        ASSERT_EQ_U64((uint64_t)st.st_size % (uint64_t)PAGE_SIZE, 0U);
    }

    rm_rf(dir);
}
