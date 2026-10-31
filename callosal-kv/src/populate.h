// populate.h — deterministic pre-population of this node's owned buckets.
//
// All nodes run populate in parallel, producing disjoint bucket ranges.
// The key stream is generated identically on every node from (seed, index)
// via splitmix64, so there is no cross-node agreement needed — each node
// filters to its own owned buckets and writes only those.
//
// The populator builds its shards fully in memory before flushing, which
// keeps the I/O pattern to a single sequential pwrite per shard. This is
// fine up to ~10 M records (≈ 1.9 GB per node at the default LF); beyond
// that we'll revisit.

#ifndef CALLOSAL_KV_POPULATE_H
#define CALLOSAL_KV_POPULATE_H

#include <stdbool.h>
#include <stdint.h>

#include "bucket.h"
#include "table.h"

struct populate_stats {
    uint64_t records_considered;   // all keys in [0, n_records)
    uint64_t records_owned;        // subset whose bucket's owner == node_id
    uint64_t records_placed;       // actually written into a slot
    uint64_t overflow_probes;      // cross-bucket probes (next bucket)
};

// populate_key_at: deterministic key for index i under seed. Independent
// of n_records, so all nodes agree on the mapping.
uint64_t populate_key_at(uint64_t seed, uint64_t index);

// populate_value_for: fills exactly VAL_SIZE bytes derived from key.
// Used so a reader can verify value integrity without extra metadata.
void populate_value_for(uint64_t key, void *value_out);

// populate_shard_path: format the shard file path into path_out.
// Returns the number of bytes written (excluding NUL), or -1 on overflow.
// Layout: "<dir>/table-node<node>-s<shard>.db".
int populate_shard_path(char *path_out, size_t path_cap,
                        const char *dir, uint32_t node_id, uint64_t shard);

// populate_run: synchronously populate this node's shards on disk.
// Creates/truncates each shard file in `dir`, writes all owned records,
// and fsync()s the files (optional — controlled by `durable`).
// Returns 0 on success, -1 on error (details to stderr).
int populate_run(const char *dir,
                 uint32_t node_id,
                 uint64_t seed,
                 const struct table_geometry *g,
                 bool durable,
                 struct populate_stats *stats_out);

#endif  // CALLOSAL_KV_POPULATE_H
