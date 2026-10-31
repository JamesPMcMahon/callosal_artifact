// client.c — read/write path over POSIX pread/pwrite with seqlock retry.

#include "client.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "populate.h"

// pread_full / pwrite_full: retry on EINTR and short reads/writes. Return
// 0 on a complete transfer, -1 on error. Short read at EOF also -> -1.
static int pread_full(int fd, void *buf, size_t len, off_t off) {
    uint8_t *p = (uint8_t *)buf;
    size_t remaining = len;
    while (remaining > 0U) {
        ssize_t n = pread(fd, p, remaining, off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;  // unexpected EOF
        }
        p += (size_t)n;
        remaining -= (size_t)n;
        off += (off_t)n;
    }
    return 0;
}

static int pwrite_full(int fd, const void *buf, size_t len, off_t off) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t remaining = len;
    while (remaining > 0U) {
        ssize_t n = pwrite(fd, p, remaining, off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        p += (size_t)n;
        remaining -= (size_t)n;
        off += (off_t)n;
    }
    return 0;
}

static inline int fd_index(const struct client *c, uint32_t node, uint64_t shard) {
    return (int)((uint64_t)node * c->max_shards_per_owner + shard);
}

int client_open(struct client *c, const char *dir, const struct table_geometry *g) {
    if (c == NULL || dir == NULL || g == NULL) {
        return CLIENT_ERR;
    }
    c->geom = *g;

    // Every owner's shard count fits in [0, max_shards_per_owner]. Using
    // the max for all owners keeps the fd table rectangular (simpler
    // indexing; trailing slots are -1).
    uint64_t max_shards = 0;
    for (uint32_t n = 0; n < g->n_nodes; ++n) {
        uint64_t s = table_shards_for_owner(g, n);
        if (s > max_shards) {
            max_shards = s;
        }
    }
    if (max_shards == 0U) {
        // No buckets anywhere — nothing to open. Still valid.
        c->fds = NULL;
        c->max_shards_per_owner = 0;
        return CLIENT_OK;
    }

    size_t nfds = (size_t)g->n_nodes * (size_t)max_shards;
    c->fds = malloc(nfds * sizeof(int));
    if (c->fds == NULL) {
        return CLIENT_ERR;
    }
    for (size_t i = 0; i < nfds; ++i) {
        c->fds[i] = -1;
    }
    c->max_shards_per_owner = max_shards;

    for (uint32_t n = 0; n < g->n_nodes; ++n) {
        uint64_t shards = table_shards_for_owner(g, n);
        for (uint64_t s = 0; s < shards; ++s) {
            char path[1024];
            if (populate_shard_path(path, sizeof(path), dir, n, s) < 0) {
                fprintf(stderr, "client: shard path too long\n");
                client_close(c);
                return CLIENT_ERR;
            }
            int fd = open(path, O_RDWR);
            if (fd < 0) {
                fprintf(stderr, "client: open(%s): %s\n", path, strerror(errno));
                client_close(c);
                return CLIENT_ERR;
            }
            c->fds[fd_index(c, n, s)] = fd;
        }
    }
    return CLIENT_OK;
}

void client_close(struct client *c) {
    if (c == NULL || c->fds == NULL) {
        return;
    }
    size_t nfds = (size_t)c->geom.n_nodes * (size_t)c->max_shards_per_owner;
    for (size_t i = 0; i < nfds; ++i) {
        if (c->fds[i] >= 0) {
            (void)close(c->fds[i]);
            c->fds[i] = -1;
        }
    }
    free(c->fds);
    c->fds = NULL;
    c->max_shards_per_owner = 0;
}

// Resolve (owner, probe_rank) to (fd, byte-offset of page within shard).
// Returns 0 on success, -1 if probe_rank is past the owner's last bucket.
static int resolve_rank(const struct client *c, uint32_t owner,
                        uint64_t probe_rank, int *fd_out, off_t *off_out) {
    uint64_t owner_buckets = table_buckets_for_owner(&c->geom, owner);
    if (probe_rank >= owner_buckets) {
        return -1;
    }
    uint64_t shard = table_shard_index(probe_rank, c->geom.buckets_per_shard);
    off_t off = (off_t)table_bucket_offset(probe_rank, c->geom.buckets_per_shard);
    int fd = c->fds[fd_index(c, owner, shard)];
    if (fd < 0) {
        return -1;
    }
    *fd_out = fd;
    *off_out = off;
    return 0;
}

int client_get(struct client *c, uint64_t key, void *value_out) {
    if (c == NULL || value_out == NULL) {
        return CLIENT_ERR;
    }
    uint64_t bucket_id = table_bucket_id(key, c->geom.n_buckets);
    uint32_t owner    = table_owner(bucket_id, c->geom.n_nodes);
    uint64_t home_rank = table_local_rank(bucket_id, c->geom.n_nodes);

    //for (int probe = 0; probe < CLIENT_MAX_PROBES; ++probe) {
    for (uint64_t probe = 0; 0 < 1; ++probe) {

        uint64_t probe_rank = home_rank + (uint64_t)probe;
        int fd;
        off_t page_off;
        if (resolve_rank(c, owner, probe_rank, &fd, &page_off) != 0) {
            return CLIENT_NOT_FOUND;
        }

        bool advance_probe = false;
        //for (int retry = 0; retry < CLIENT_MAX_SEQLOCK_RETRY; ++retry) {
	for (;;) {

            struct bucket_page page;
            if (pread_full(fd, &page, PAGE_SIZE, page_off) < 0) {
                return CLIENT_ERR;
            }
            int slot_idx = bucket_find_key(&page, key);
            if (slot_idx < 0) {
                // If there's any empty slot, the key does not exist — there
                // is no way a linear-probe populate would have stepped past
                // a vacancy. Short-circuit.
                if (bucket_find_empty(&page) >= 0) {
                    return CLIENT_NOT_FOUND;
                }
                // Page is fully occupied and key not here → overflow into
                // next rank.
                advance_probe = true;
                break;
            }
            uint64_t v1 = page.slots[slot_idx].version;
            if ((v1 & 1U) != 0U || v1 == SLOT_EMPTY_VERSION) {
                continue;  // writer in flight; retry
            }
            // Re-read only the 8-byte version word — cheapest consistent
            // check. If unchanged, the page snapshot we already copied
            // with pread() was taken entirely between publishes.
            uint64_t v2;
            off_t v_off = page_off + (off_t)(slot_idx * SLOT_SIZE);
            if (pread_full(fd, &v2, sizeof v2, v_off) < 0) {
                return CLIENT_ERR;
            }
            if (v1 == v2) {
                memcpy(value_out, page.slots[slot_idx].value, VAL_SIZE);
                return CLIENT_OK;
            }
            // Torn — retry with fresh pread.
        }
        if (!advance_probe) {
            // Exhausted seqlock retries on this page without deciding to
            // advance — surface upward so caller can count the event.
            return CLIENT_RETRY;
        }
    }
    return CLIENT_NOT_FOUND;
}

int client_put(struct client *c, uint64_t key, const void *value) {
    if (c == NULL || value == NULL) {
        return CLIENT_ERR;
    }
    uint64_t bucket_id = table_bucket_id(key, c->geom.n_buckets);
    uint32_t owner    = table_owner(bucket_id, c->geom.n_nodes);
    uint64_t home_rank = table_local_rank(bucket_id, c->geom.n_nodes);

    //for (int probe = 0; probe < CLIENT_MAX_PROBES; ++probe) {
    for (uint64_t probe = 0; 0 < 1 ; ++probe) {

        uint64_t probe_rank = home_rank + (uint64_t)probe;
        int fd;
        off_t page_off;
        if (resolve_rank(c, owner, probe_rank, &fd, &page_off) != 0) {
            return CLIENT_NOT_FOUND;
        }
        struct bucket_page page;
        if (pread_full(fd, &page, PAGE_SIZE, page_off) < 0) {
            return CLIENT_ERR;
        }
        int slot_idx = bucket_find_key(&page, key);
        if (slot_idx < 0) {
            if (bucket_find_empty(&page) >= 0) {
                return CLIENT_NOT_FOUND;
            }
            continue;  // full, try next rank
        }

        off_t slot_off = page_off + (off_t)(slot_idx * SLOT_SIZE);
        uint64_t current = page.slots[slot_idx].version;
        // First transient odd = current + 1; committed even = current + 2.
        // If the slot was somehow observed odd (should not happen under
        // CREW), advance an extra step to keep the sequence monotonic.
        if ((current & 1U) != 0U) {
            current += 1U;
        }
        uint64_t odd_v  = current + 1U;
        uint64_t even_v = current + 2U;

        if (pwrite_full(fd, &odd_v, sizeof odd_v, slot_off) < 0) {
            return CLIENT_ERR;
        }
        if (pwrite_full(fd, value, VAL_SIZE,
                        slot_off + (off_t)(VERSION_SIZE + KEY_SIZE)) < 0) {
            return CLIENT_ERR;
        }
        if (pwrite_full(fd, &even_v, sizeof even_v, slot_off) < 0) {
            return CLIENT_ERR;
        }
        return CLIENT_OK;
    }
    return CLIENT_NOT_FOUND;
}
