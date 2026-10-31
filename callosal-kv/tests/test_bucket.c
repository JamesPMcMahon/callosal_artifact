// test_bucket.c — unit tests for slot and page primitives, including a
// multi-threaded seqlock stress test that pounds on a shared in-memory
// slot with one writer and many readers.

#include "test.h"
#include "tests.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "bucket.h"

void test_bucket_slot_init(void) {
    struct slot s;
    memset(&s, 0, sizeof(s));
    ASSERT_TRUE(slot_is_empty(&s));

    uint8_t value[VAL_SIZE];
    for (size_t i = 0; i < VAL_SIZE; ++i) {
        value[i] = (uint8_t)(i & 0xFFU);
    }
    slot_init(&s, 0xDEADBEEFCAFEBABEULL, value);
    ASSERT_EQ_U64(s.version, SLOT_FIRST_VERSION);
    ASSERT_FALSE(slot_is_empty(&s));
    ASSERT_EQ_U64(key_from_bytes(s.key), 0xDEADBEEFCAFEBABEULL);
    ASSERT_EQ_MEM(s.value, value, VAL_SIZE);
}

void test_bucket_find_empty_and_key(void) {
    struct bucket_page page;
    memset(&page, 0, sizeof(page));

    ASSERT_EQ_INT(bucket_find_empty(&page), 0);
    ASSERT_EQ_INT(bucket_find_key(&page, 42), -1);

    uint8_t value[VAL_SIZE];
    memset(value, 0xAB, VAL_SIZE);
    slot_init(&page.slots[0], 100, value);
    slot_init(&page.slots[1], 200, value);
    slot_init(&page.slots[3], 400, value);

    ASSERT_EQ_INT(bucket_find_empty(&page), 2);
    ASSERT_EQ_INT(bucket_find_key(&page, 100), 0);
    ASSERT_EQ_INT(bucket_find_key(&page, 200), 1);
    ASSERT_EQ_INT(bucket_find_key(&page, 400), 3);
    ASSERT_EQ_INT(bucket_find_key(&page, 300), -1);
}

void test_bucket_find_empty_full(void) {
    struct bucket_page page;
    memset(&page, 0, sizeof(page));
    uint8_t value[VAL_SIZE];
    memset(value, 0, VAL_SIZE);
    for (int i = 0; i < SLOTS_PER_BUCKET; ++i) {
        slot_init(&page.slots[i], (uint64_t)(i + 1), value);
    }
    ASSERT_EQ_INT(bucket_find_empty(&page), -1);
}

void test_bucket_seqlock_roundtrip(void) {
    struct slot s;
    memset(&s, 0, sizeof(s));

    uint8_t v1[VAL_SIZE];
    memset(v1, 0x11, VAL_SIZE);
    slot_seqlock_write(&s, 0xABCDEF01, v1);
    ASSERT_EQ_U64(s.version & 1U, 0U);
    ASSERT_TRUE(s.version >= SLOT_FIRST_VERSION);

    uint64_t key_out = 0;
    uint8_t value_out[VAL_SIZE];
    int rc = slot_seqlock_read(&s, &key_out, value_out, 16);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_U64(key_out, 0xABCDEF01);
    ASSERT_EQ_MEM(value_out, v1, VAL_SIZE);

    uint8_t v2[VAL_SIZE];
    memset(v2, 0x22, VAL_SIZE);
    uint64_t before = s.version;
    slot_seqlock_write(&s, 0xABCDEF01, v2);
    ASSERT_TRUE(s.version > before);

    rc = slot_seqlock_read(&s, &key_out, value_out, 16);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_MEM(value_out, v2, VAL_SIZE);
}

void test_bucket_seqlock_retries_on_odd(void) {
    struct slot s;
    memset(&s, 0, sizeof(s));
    uint8_t v[VAL_SIZE];
    memset(v, 0x55, VAL_SIZE);
    slot_seqlock_write(&s, 5, v);
    // Force version odd to simulate an in-flight writer.
    s.version |= 1U;
    uint8_t out[VAL_SIZE];
    int rc = slot_seqlock_read(&s, NULL, out, 4);
    ASSERT_EQ_INT(rc, -1);
}

// ----------------- multi-threaded stress -------------------------

struct stress_args {
    struct slot *s;
    int          iterations;
    atomic_int  *stop;
    atomic_long *torn;
    atomic_long *successes;
};

// The writer rotates through a small set of (key, value) pairs that encode
// the value so the reader can detect any tearing: all bytes of value[] are
// set to the same tag byte, and that tag equals key & 0xFF. A torn read
// will show a value with mixed tags.
static void writer_put(struct slot *s, uint64_t key) {
    uint8_t value[VAL_SIZE];
    memset(value, (int)(key & 0xFFU), VAL_SIZE);
    slot_seqlock_write(s, key, value);
}

static void *stress_writer(void *arg) {
    struct stress_args *a = arg;
    for (int i = 0; i < a->iterations && !atomic_load(a->stop); ++i) {
        uint64_t key = (uint64_t)(i & 0xFF);
        writer_put(a->s, key);
    }
    atomic_store(a->stop, 1);
    return NULL;
}

static void *stress_reader(void *arg) {
    struct stress_args *a = arg;
    uint8_t out[VAL_SIZE];
    uint64_t key_out = 0;
    while (!atomic_load(a->stop)) {
        int rc = slot_seqlock_read(a->s, &key_out, out, 256);
        if (rc != 0) {
            continue;
        }
        uint8_t tag = (uint8_t)(key_out & 0xFFU);
        for (size_t i = 0; i < VAL_SIZE; ++i) {
            if (out[i] != tag) {
                atomic_fetch_add(a->torn, 1);
                break;
            }
        }
        atomic_fetch_add(a->successes, 1);
    }
    return NULL;
}

void test_bucket_seqlock_stress(void) {
    struct slot s;
    memset(&s, 0, sizeof(s));

    uint8_t seed_value[VAL_SIZE];
    memset(seed_value, 0, VAL_SIZE);
    slot_seqlock_write(&s, 0, seed_value);

    atomic_int stop = 0;
    atomic_long torn = 0;
    atomic_long successes = 0;

    struct stress_args args = {
        .s = &s,
        .iterations = 200000,
        .stop = &stop,
        .torn = &torn,
        .successes = &successes,
    };

    pthread_t writer;
    pthread_t readers[3];
    pthread_create(&writer, NULL, stress_writer, &args);
    for (size_t i = 0; i < 3; ++i) {
        pthread_create(&readers[i], NULL, stress_reader, &args);
    }
    pthread_join(writer, NULL);
    // Writer has stopped. Let readers drain their current loop.
    atomic_store(&stop, 1);
    for (size_t i = 0; i < 3; ++i) {
        pthread_join(readers[i], NULL);
    }

    long torn_final = atomic_load(&torn);
    long success_final = atomic_load(&successes);
    if (torn_final != 0) {
        fprintf(stderr, "    seqlock torn reads observed: %ld / %ld\n",
                torn_final, success_final);
    }
    ASSERT_EQ_INT(torn_final, 0);
    ASSERT_TRUE(success_final > 0);
}
