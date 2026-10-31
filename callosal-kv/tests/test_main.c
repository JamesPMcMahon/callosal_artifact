// test_main.c — registry + runner for all unit tests.
//
// Each new test function is declared in tests/tests.h and defined in the
// corresponding tests/test_<module>.c. Register it in REGISTRY below.

#include "test.h"
#include "tests.h"

int g_test_failures = 0;

static const struct test_case REGISTRY[] = {
    { "bucket.slot_init",                   test_bucket_slot_init },
    { "bucket.find_empty_and_key",          test_bucket_find_empty_and_key },
    { "bucket.find_empty_full",             test_bucket_find_empty_full },
    { "bucket.seqlock_roundtrip",           test_bucket_seqlock_roundtrip },
    { "bucket.seqlock_retries_on_odd",      test_bucket_seqlock_retries_on_odd },
    { "bucket.seqlock_stress",              test_bucket_seqlock_stress },

    { "table.num_buckets_rounds_up",        test_table_num_buckets_rounds_up },
    { "table.num_buckets_rejects_bad_lf",   test_table_num_buckets_rejects_bad_lf },
    { "table.buckets_per_shard",            test_table_buckets_per_shard },
    { "table.geometry_init",                test_table_geometry_init },
    { "table.geometry_init_rejects_zero",   test_table_geometry_init_rejects_zero },
    { "table.hash_avalanche",               test_table_hash_avalanche },
    { "table.hash_distribution",            test_table_hash_distribution },
    { "table.owner_striping",               test_table_owner_striping },
    { "table.shard_placement",              test_table_shard_placement },
    { "table.buckets_for_owner_striped",    test_table_buckets_for_owner_striped },

    { "populate.key_deterministic",         test_populate_key_deterministic },
    { "populate.value_for_key_stable",      test_populate_value_for_key_stable },
    { "populate.shard_path_formats",        test_populate_shard_path_formats },
    { "populate.end_to_end_small",          test_populate_end_to_end_small },
    { "populate.two_nodes_disjoint",        test_populate_two_nodes_disjoint },
    { "populate.sharded_small_cap",         test_populate_sharded_small_cap },

    { "client.get_after_populate",          test_client_get_after_populate },
    { "client.put_then_get",                test_client_put_then_get },
    { "client.get_not_found",               test_client_get_not_found },
    { "client.get_overflow_key",            test_client_get_overflow_key },
    { "client.put_concurrent_seqlock",      test_client_put_concurrent_seqlock },

    { "workload.uniform_write_ratio",       test_workload_uniform_write_ratio },
    { "workload.uniform_coverage",          test_workload_uniform_coverage },
    { "workload.zipf_skew",                 test_workload_zipf_skew },
    { "workload.rejects_bad_theta",         test_workload_rejects_bad_theta },

    { "probe.encode_decode_gen",            test_probe_encode_decode_gen },
    { "probe.monotonic_no_violation",       test_probe_monotonic_no_violation },
    { "probe.monotonic_catches_regress",    test_probe_monotonic_catches_regress },
    { "probe.none_is_noop",                 test_probe_none_is_noop },

    { "stats.record_and_aggregate",         test_stats_record_and_aggregate },
    { "stats.percentiles",                  test_stats_percentiles },
};

int main(void) {
    size_t n = sizeof(REGISTRY) / sizeof(REGISTRY[0]);
    for (size_t i = 0; i < n; ++i) {
        int before = g_test_failures;
        printf("[ RUN      ] %s\n", REGISTRY[i].name);
        REGISTRY[i].fn();
        if (g_test_failures == before) {
            printf("[       OK ] %s\n", REGISTRY[i].name);
        } else {
            printf("[  FAILED  ] %s\n", REGISTRY[i].name);
        }
    }
    if (g_test_failures == 0) {
        printf("[  PASSED  ] %zu test%s\n", n, n == 1 ? "" : "s");
        return 0;
    }
    printf("[  FAILED  ] %d failure(s)\n", g_test_failures);
    return 1;
}
