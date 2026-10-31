// table.h — hash, geometry, and bucket-to-file placement.
//
// The benchmark's physical layout is determined by a handful of pure
// functions over (records, nodes, load factor, max-file cap). Each node
// computes its own view locally; there is no central coordinator.
//
//   bucket_id = hash(key) % n_buckets
//   owner     = bucket_id % n_nodes              // striped
//   rank(b)   = bucket_id / n_nodes              // local rank within owner
//   shard     = rank / buckets_per_shard
//   offset    = (rank % buckets_per_shard) * PAGE_SIZE

#ifndef CALLOSAL_KV_TABLE_H
#define CALLOSAL_KV_TABLE_H

#include <stddef.h>
#include <stdint.h>

#include "config.h"

struct table_geometry {
    uint64_t n_records;
    uint64_t n_buckets;
    uint64_t buckets_per_shard;   // per owner-file cap; UINT64_MAX = no cap
    uint64_t load_factor_pct;
    uint32_t n_nodes;
};

// table_num_buckets: derive the total bucket count from records and load
// factor. Rounded up so that n_records / (SLOTS_PER_BUCKET * LF) fits.
uint64_t table_num_buckets(uint64_t n_records, uint64_t load_factor_pct);

// table_buckets_per_shard: given a maximum per-file size in bucket-pages
// (0 = no cap), return the number of buckets that fit in a single shard file.
// When 0 is passed, returns UINT64_MAX.
uint64_t table_buckets_per_shard(uint64_t max_file_pages);

// table_geometry_init: derive layout geometry from records, nodes, load
// factor, and per-shard file cap. Returns 0 on success or -1 on a
// parameter violation (e.g. 0 records, 0 nodes, impossible load factor).
int table_geometry_init(struct table_geometry *g,
                        uint64_t n_records,
                        uint32_t n_nodes,
                        uint64_t load_factor_pct,
                        uint64_t max_file_pages);

// Hash a scalar key to 64 bits. The current implementation is the
// Murmur3 / splitmix64 finalizer — small, fast, and good enough
// dispersal for uniformly pseudo-random keys (which is what the
// deterministic populator produces).
uint64_t table_hash(uint64_t key);

// table_bucket_id: which bucket does this key live in?
uint64_t table_bucket_id(uint64_t key, uint64_t n_buckets);

// table_owner: which node owns a given bucket? Striped.
uint32_t table_owner(uint64_t bucket_id, uint32_t n_nodes);

// table_local_rank: position of a bucket within its owner's sequence.
uint64_t table_local_rank(uint64_t bucket_id, uint32_t n_nodes);

// table_shard_index: which shard file holds a bucket at the given local rank?
uint64_t table_shard_index(uint64_t local_rank, uint64_t buckets_per_shard);

// table_bucket_offset: byte offset of a bucket within its shard file.
uint64_t table_bucket_offset(uint64_t local_rank, uint64_t buckets_per_shard);

// table_buckets_for_owner: how many buckets does node `node_id` own, in
// total across all its shards? Used for sizing shard files on populate.
uint64_t table_buckets_for_owner(const struct table_geometry *g, uint32_t node_id);

// table_shards_for_owner: how many shard files does `node_id` need?
uint64_t table_shards_for_owner(const struct table_geometry *g, uint32_t node_id);

#endif  // CALLOSAL_KV_TABLE_H
