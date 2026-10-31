// tests.h — prototypes for every test function.
//
// Included both by each test module's .c file (so -Wmissing-prototypes is
// satisfied) and by tests/test_main.c (which registers them into a single
// REGISTRY table). To add a test: declare it here, define it in the
// relevant test_<module>.c, then register it in tests/test_main.c.

#ifndef CALLOSAL_KV_TESTS_LIST_H
#define CALLOSAL_KV_TESTS_LIST_H

// bucket
void test_bucket_slot_init(void);
void test_bucket_find_empty_and_key(void);
void test_bucket_find_empty_full(void);
void test_bucket_seqlock_roundtrip(void);
void test_bucket_seqlock_retries_on_odd(void);
void test_bucket_seqlock_stress(void);

// table
void test_table_num_buckets_rounds_up(void);
void test_table_num_buckets_rejects_bad_lf(void);
void test_table_buckets_per_shard(void);
void test_table_geometry_init(void);
void test_table_geometry_init_rejects_zero(void);
void test_table_hash_avalanche(void);
void test_table_hash_distribution(void);
void test_table_owner_striping(void);
void test_table_shard_placement(void);
void test_table_buckets_for_owner_striped(void);

// populate
void test_populate_key_deterministic(void);
void test_populate_value_for_key_stable(void);
void test_populate_shard_path_formats(void);
void test_populate_end_to_end_small(void);
void test_populate_two_nodes_disjoint(void);
void test_populate_sharded_small_cap(void);

// client
void test_client_get_after_populate(void);
void test_client_put_then_get(void);
void test_client_get_not_found(void);
void test_client_get_overflow_key(void);
void test_client_put_concurrent_seqlock(void);

// workload
void test_workload_uniform_write_ratio(void);
void test_workload_uniform_coverage(void);
void test_workload_zipf_skew(void);
void test_workload_rejects_bad_theta(void);

// probe
void test_probe_encode_decode_gen(void);
void test_probe_monotonic_no_violation(void);
void test_probe_monotonic_catches_regress(void);
void test_probe_none_is_noop(void);

// stats
void test_stats_record_and_aggregate(void);
void test_stats_percentiles(void);

#endif  // CALLOSAL_KV_TESTS_LIST_H
