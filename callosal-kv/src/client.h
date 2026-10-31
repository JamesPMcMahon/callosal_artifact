// client.h — pread/pwrite-backed get/put with seqlock retry.
//
// A client instance owns open fds for every shard file under a populated
// directory. get() is a pread-of-bucket-page + slot scan + seqlock verify;
// put() is a three-phase pwrite that makes the odd/even version transition
// visible to concurrent readers on the same node and on shared-storage peers.
//
// The client has no hidden global state — all workers can share one
// `struct client` safely, but each thread issues its own syscalls.

#ifndef CALLOSAL_KV_CLIENT_H
#define CALLOSAL_KV_CLIENT_H

#include <stdint.h>

#include "bucket.h"
#include "table.h"

// Cap on overflow probing across the owner's bucket stripe during lookup.
// Populate rarely probes beyond 1-2 even at 75% LF; 8 gives plenty of headroom
// and preserves O(1)-ish lookup on pathological runs.
#define CLIENT_MAX_PROBES       8

// Seqlock retry budget per probe. If the slot is being pounded on by a
// writer faster than we can complete a clean read, we return CLIENT_RETRY.
#define CLIENT_MAX_SEQLOCK_RETRY 64

// Return codes.
enum {
    CLIENT_OK         = 0,
    CLIENT_NOT_FOUND  = 1,
    CLIENT_RETRY      = 2,
    CLIENT_ERR        = -1,
};

struct client {
    struct table_geometry geom;
    // Flat array of fds indexed by node_id * max_shards_per_owner + shard.
    // -1 entries mean "shard not present for this owner" (trailing slots
    //  on owners that have fewer shards than the max).
    int      *fds;
    uint64_t  max_shards_per_owner;
};

// client_open: open all shard files under `dir` for every owner in `g`.
// Returns 0 on success; on failure, closes any fds already opened.
int client_open(struct client *c, const char *dir, const struct table_geometry *g);

// client_close: release fds and internal buffers. Safe to call on a
// partially-initialized client.
void client_close(struct client *c);

// client_get: look up `key` and copy its VAL_SIZE-byte value into value_out.
// Returns CLIENT_OK on hit, CLIENT_NOT_FOUND if the key is absent,
// CLIENT_RETRY if the seqlock budget was exhausted, CLIENT_ERR on I/O error.
int client_get(struct client *c, uint64_t key, void *value_out);

// client_put: overwrite the slot for `key` with `value`. Caller must be the
// CREW writer for `key` (i.e. table_owner(hash(key)) == this_node) — the
// client does not enforce ownership because it's a correctness property
// of the workload layer.
// Returns CLIENT_OK on success, CLIENT_NOT_FOUND if the key was not pre-
// populated, CLIENT_ERR on I/O error.
int client_put(struct client *c, uint64_t key, const void *value);

#endif  // CALLOSAL_KV_CLIENT_H
