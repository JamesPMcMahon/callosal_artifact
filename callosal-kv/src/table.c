// table.c — pure geometry and placement math. No I/O, no globals.

#include "table.h"

#include <stddef.h>
#include <stdint.h>

// Derive n_buckets by ceiling division: n_records / (SLOTS_PER_BUCKET * LF).
uint64_t table_num_buckets(uint64_t n_records, uint64_t load_factor_pct) {
    if (load_factor_pct == 0U || load_factor_pct > 100U) {
        return 0U;
    }
    // Scale load factor to percent so integer math stays exact.
    // Effective slots per bucket under LF = (SLOTS_PER_BUCKET * LF) / 100.
    uint64_t denom_num = (uint64_t)SLOTS_PER_BUCKET * load_factor_pct;
    if (denom_num == 0U) {
        return 0U;
    }
    // n_buckets = ceil(n_records * 100 / denom_num)
    uint64_t num = n_records * 100U;
    uint64_t n = (num + denom_num - 1U) / denom_num;
    return n == 0U ? 1U : n;
}

uint64_t table_buckets_per_shard(uint64_t max_file_pages) {
    if (max_file_pages == 0U) {
        return UINT64_MAX;
    }
    return max_file_pages;
}

int table_geometry_init(struct table_geometry *g,
                        uint64_t n_records,
                        uint32_t n_nodes,
                        uint64_t load_factor_pct,
                        uint64_t max_file_pages) {
    if (g == NULL || n_records == 0U || n_nodes == 0U) {
        return -1;
    }
    uint64_t n_buckets = table_num_buckets(n_records, load_factor_pct);
    if (n_buckets == 0U) {
        return -1;
    }
    g->n_records = n_records;
    g->n_buckets = n_buckets;
    g->buckets_per_shard = table_buckets_per_shard(max_file_pages);
    g->load_factor_pct = load_factor_pct;
    g->n_nodes = n_nodes;
    return 0;
}

// splitmix64 generator step. Not cryptographic. Adding the golden-ratio
// constant before the finalizer removes the 0 → 0 fixed point of the
// pure Murmur3 finalizer, which matters when callers pass small integer
// keys directly.
uint64_t table_hash(uint64_t key) {
    uint64_t h = key + UINT64_C(0x9e3779b97f4a7c15);
    h = (h ^ (h >> 30U)) * UINT64_C(0xbf58476d1ce4e5b9);
    h = (h ^ (h >> 27U)) * UINT64_C(0x94d049bb133111eb);
    h ^= h >> 31U;
    return h;
}

uint64_t table_bucket_id(uint64_t key, uint64_t n_buckets) {
    return table_hash(key) % n_buckets;
}

uint32_t table_owner(uint64_t bucket_id, uint32_t n_nodes) {
    return (uint32_t)(bucket_id % (uint64_t)n_nodes);
}

uint64_t table_local_rank(uint64_t bucket_id, uint32_t n_nodes) {
    return bucket_id / (uint64_t)n_nodes;
}

uint64_t table_shard_index(uint64_t local_rank, uint64_t buckets_per_shard) {
    return buckets_per_shard == UINT64_MAX ? 0U : local_rank / buckets_per_shard;
}

uint64_t table_bucket_offset(uint64_t local_rank, uint64_t buckets_per_shard) {
    uint64_t within_shard = (buckets_per_shard == UINT64_MAX)
                                ? local_rank
                                : local_rank % buckets_per_shard;
    return within_shard * (uint64_t)PAGE_SIZE;
}

uint64_t table_buckets_for_owner(const struct table_geometry *g, uint32_t node_id) {
    // Stripe: node_id owns buckets {node_id, node_id + N, node_id + 2N, ...}.
    // Count = ceil((n_buckets - node_id) / N) when node_id < n_buckets, else 0.
    if ((uint64_t)node_id >= g->n_buckets) {
        return 0U;
    }
    uint64_t rem = g->n_buckets - (uint64_t)node_id;
    uint64_t count = (rem + (uint64_t)g->n_nodes - 1U) / (uint64_t)g->n_nodes;
    return count;
}

uint64_t table_shards_for_owner(const struct table_geometry *g, uint32_t node_id) {
    uint64_t buckets = table_buckets_for_owner(g, node_id);
    if (buckets == 0U) {
        return 0U;
    }
    if (g->buckets_per_shard == UINT64_MAX) {
        return 1U;
    }
    return (buckets + g->buckets_per_shard - 1U) / g->buckets_per_shard;
}
