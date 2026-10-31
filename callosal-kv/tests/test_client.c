// test_client.c — end-to-end get/put over a populated directory.

#include "test.h"
#include "tests.h"

#include <dirent.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "client.h"
#include "populate.h"
#include "probe.h"
#include "table.h"

static int make_tmp_dir(char *out, size_t cap) {
    const char *base = getenv("TMPDIR");
    if (base == NULL || base[0] == '\0') {
        base = "/tmp";
    }
    int n = snprintf(out, cap, "%s/callosal-kv-cli-XXXXXX", base);
    if (n < 0 || (size_t)n >= cap) {
        return -1;
    }
    return mkdtemp(out) == NULL ? -1 : 0;
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

// Populate a small table and return (dir, geometry, client) the caller must
// tear down. On failure the caller's assertions fire; we return a best-effort
// state either way.
static int setup_populated(char *dir, size_t dir_cap,
                           struct table_geometry *g, struct client *c,
                           uint64_t records, uint64_t seed) {
    if (make_tmp_dir(dir, dir_cap) != 0) {
        return -1;
    }
    if (table_geometry_init(g, records, 1, 50, 0) != 0) {
        return -1;
    }
    struct populate_stats stats;
    if (populate_run(dir, 0, seed, g, false, &stats) != 0) {
        return -1;
    }
    if (client_open(c, dir, g) != 0) {
        return -1;
    }
    return 0;
}

void test_client_get_after_populate(void) {
    char dir[256];
    struct table_geometry g;
    struct client c;
    ASSERT_EQ_INT(setup_populated(dir, sizeof dir, &g, &c, 1024, 42), 0);

    uint8_t expected[VAL_SIZE];
    uint8_t actual[VAL_SIZE];
    for (uint64_t i = 0; i < g.n_records; ++i) {
        uint64_t key = populate_key_at(42, i);
        int rc = client_get(&c, key, actual);
        ASSERT_EQ_INT(rc, CLIENT_OK);
        populate_value_for(key, expected);
        ASSERT_EQ_MEM(actual, expected, VAL_SIZE);
    }
    client_close(&c);
    rm_rf(dir);
}

void test_client_put_then_get(void) {
    char dir[256];
    struct table_geometry g;
    struct client c;
    ASSERT_EQ_INT(setup_populated(dir, sizeof dir, &g, &c, 512, 7), 0);

    uint8_t value[VAL_SIZE];
    uint8_t readback[VAL_SIZE];
    // Write a fresh gen=1234 value, read it back, confirm gen round-trips.
    for (uint64_t i = 0; i < 32; ++i) {
        uint64_t key = populate_key_at(7, i);
        uint64_t gen = 1000U + i;
        probe_encode_value(key, gen, value);
        ASSERT_EQ_INT(client_put(&c, key, value), CLIENT_OK);
        ASSERT_EQ_INT(client_get(&c, key, readback), CLIENT_OK);
        ASSERT_EQ_U64(probe_decode_gen(readback), gen);
        ASSERT_EQ_MEM(readback, value, VAL_SIZE);
    }
    client_close(&c);
    rm_rf(dir);
}

void test_client_get_overflow_key(void) {
    // Regression test for a lookup bug: when a key's home-rank
    // page is fully occupied (so populate placed the key at home+N for
    // some N>=1), the reader must advance to the next probe rank rather
    // than returning CLIENT_RETRY. LF=75 + 4000 records + seed=17
    // reliably produces ~26 overflow placements — high enough that the
    // lookup path is exercised, low enough that placement doesn't fail.
    char dir[256];
    struct table_geometry g;
    struct client c;
    ASSERT_EQ_INT(make_tmp_dir(dir, sizeof dir), 0);
    ASSERT_EQ_INT(table_geometry_init(&g, 4000, 1, 75, 0), 0);

    struct populate_stats stats;
    ASSERT_EQ_INT(populate_run(dir, 0, 17, &g, false, &stats), 0);
    // Sanity-check that this configuration actually exercises overflow;
    // if not, the test would silently pass for the wrong reason.
    ASSERT_TRUE(stats.overflow_probes > 0U);

    ASSERT_EQ_INT(client_open(&c, dir, &g), 0);
    uint8_t actual[VAL_SIZE];
    uint8_t expected[VAL_SIZE];
    for (uint64_t i = 0; i < g.n_records; ++i) {
        uint64_t key = populate_key_at(17, i);
        int rc = client_get(&c, key, actual);
        ASSERT_EQ_INT(rc, CLIENT_OK);
        populate_value_for(key, expected);
        ASSERT_EQ_MEM(actual, expected, VAL_SIZE);
    }
    client_close(&c);
    rm_rf(dir);
}

void test_client_get_not_found(void) {
    char dir[256];
    struct table_geometry g;
    struct client c;
    ASSERT_EQ_INT(setup_populated(dir, sizeof dir, &g, &c, 256, 99), 0);

    uint8_t buf[VAL_SIZE];
    // Keys drawn with a different seed are (virtually) certain to be absent.
    for (uint64_t i = 0; i < 16; ++i) {
        uint64_t absent_key = populate_key_at(0xDEADBEEFULL, i);
        int rc = client_get(&c, absent_key, buf);
        ASSERT_EQ_INT(rc, CLIENT_NOT_FOUND);
    }
    client_close(&c);
    rm_rf(dir);
}

struct cc_ctx {
    struct client *c;
    uint64_t       key;
    atomic_bool   *stop;
    uint64_t       iters;
    uint64_t       torn_observed;  // gen decreased between reads
    uint64_t       last_gen;
};

static void *cc_writer(void *arg) {
    struct cc_ctx *ctx = arg;
    uint8_t value[VAL_SIZE];
    uint64_t gen = 1;
    while (!atomic_load(ctx->stop)) {
        probe_encode_value(ctx->key, gen, value);
        (void)client_put(ctx->c, ctx->key, value);
        gen++;
        ctx->iters++;
    }
    return NULL;
}

static void *cc_reader(void *arg) {
    struct cc_ctx *ctx = arg;
    uint8_t value[VAL_SIZE];
    uint64_t last = 0;
    while (!atomic_load(ctx->stop)) {
        if (client_get(ctx->c, ctx->key, value) != CLIENT_OK) {
            continue;
        }
        uint64_t gen = probe_decode_gen(value);
        if (gen < last) {
            ctx->torn_observed++;
        }
        last = gen;
        ctx->iters++;
    }
    ctx->last_gen = last;
    return NULL;
}

void test_client_put_concurrent_seqlock(void) {
    // One writer, three readers pounding the same key for ~150 ms. The
    // seqlock must keep the readers from ever observing a gen that is
    // older than one they've already seen — that would mean they saw a
    // torn payload whose header-gen did not match its tail-bytes.
    char dir[256];
    struct table_geometry g;
    struct client c;
    ASSERT_EQ_INT(setup_populated(dir, sizeof dir, &g, &c, 64, 3), 0);

    uint64_t key = populate_key_at(3, 0);
    atomic_bool stop;
    atomic_init(&stop, false);

    struct cc_ctx w_ctx = { .c = &c, .key = key, .stop = &stop };
    struct cc_ctx r_ctx[3];
    for (int i = 0; i < 3; ++i) {
        r_ctx[i].c = &c;
        r_ctx[i].key = key;
        r_ctx[i].stop = &stop;
        r_ctx[i].iters = 0;
        r_ctx[i].torn_observed = 0;
        r_ctx[i].last_gen = 0;
    }

    pthread_t writer;
    pthread_t readers[3];
    pthread_create(&writer, NULL, cc_writer, &w_ctx);
    for (int i = 0; i < 3; ++i) {
        pthread_create(&readers[i], NULL, cc_reader, &r_ctx[i]);
    }

    struct timespec ts = { .tv_sec = 0, .tv_nsec = 150 * 1000 * 1000 };
    nanosleep(&ts, NULL);
    atomic_store(&stop, true);

    pthread_join(writer, NULL);
    for (int i = 0; i < 3; ++i) {
        pthread_join(readers[i], NULL);
    }

    for (int i = 0; i < 3; ++i) {
        ASSERT_EQ_U64(r_ctx[i].torn_observed, 0U);
    }
    client_close(&c);
    rm_rf(dir);
}
