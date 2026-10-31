// test_probe.c — generation encoding + monotonic observation.

#include "test.h"
#include "tests.h"

#include <stdint.h>
#include <string.h>

#include "config.h"
#include "populate.h"
#include "probe.h"

void test_probe_encode_decode_gen(void) {
    uint8_t buf[VAL_SIZE];
    probe_encode_value(0xABCDEF, 7777, buf);
    ASSERT_EQ_U64(probe_decode_gen(buf), 7777U);

    // Tail (bytes 8..VAL_SIZE) must match populate_value_for's tail for the
    // same key — that's the invariant that lets populate seed the table
    // with gen=0 while keeping value integrity checks simple.
    uint8_t pop_buf[VAL_SIZE];
    populate_value_for(0xABCDEF, pop_buf);
    ASSERT_EQ_MEM(buf + 8, pop_buf + 8, VAL_SIZE - 8);

    // Populate's own gen is 0.
    ASSERT_EQ_U64(probe_decode_gen(pop_buf), 0U);
}

void test_probe_monotonic_no_violation(void) {
    struct probe_ctx p;
    ASSERT_EQ_INT(probe_init(&p, PROBE_MODE_MONOTONIC, 256), 0);
    // Strictly ascending gens for 1000 keys -> zero violations.
    for (uint64_t k = 1; k < 1000; ++k) {
        probe_observe(&p, k, k * 10U);
        probe_observe(&p, k, k * 10U + 1U);
    }
    ASSERT_EQ_U64(p.violations, 0U);
    ASSERT_TRUE(p.observations >= 1998U);
    probe_free(&p);
}

void test_probe_monotonic_catches_regress(void) {
    struct probe_ctx p;
    ASSERT_EQ_INT(probe_init(&p, PROBE_MODE_MONOTONIC, 256), 0);
    uint64_t k = 42;
    probe_observe(&p, k, 100U);
    probe_observe(&p, k, 101U);
    probe_observe(&p, k,  50U);  // regression!
    ASSERT_EQ_U64(p.violations, 1U);
    probe_observe(&p, k, 101U);  // still below the 101 we saw, but same -> ok
    ASSERT_EQ_U64(p.violations, 1U);
    probe_observe(&p, k,  49U);  // another regression
    ASSERT_EQ_U64(p.violations, 2U);
    probe_free(&p);
}

void test_probe_none_is_noop(void) {
    struct probe_ctx p;
    ASSERT_EQ_INT(probe_init(&p, PROBE_MODE_NONE, 256), 0);
    probe_observe(&p, 1U, 100U);
    probe_observe(&p, 1U,  50U);  // would be a violation under MONOTONIC
    ASSERT_EQ_U64(p.violations, 0U);
    ASSERT_EQ_U64(p.observations, 0U);
    probe_free(&p);
}
