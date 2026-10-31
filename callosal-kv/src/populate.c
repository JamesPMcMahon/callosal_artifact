// populate.c — deterministic single-threaded populate.
//
// The populator materializes this node's shards fully in memory, then
// writes them out with one pwrite per page range. This trades RAM for
// I/O simplicity and avoids ping-ponging the page cache during populate.
// The memory footprint is bounded by (buckets_for_owner * PAGE_SIZE),
// which fits comfortably for the configurations we care about.

#include "populate.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// splitmix64: fast, well-mixed PRNG used both as the key generator and as
// a value-filler. Deterministic given the seed; nodes do not share state.
static uint64_t splitmix64(uint64_t x) {
    x += UINT64_C(0x9e3779b97f4a7c15);
    x = (x ^ (x >> 30U)) * UINT64_C(0xbf58476d1ce4e5b9);
    x = (x ^ (x >> 27U)) * UINT64_C(0x94d049bb133111eb);
    x ^= x >> 31U;
    return x;
}

uint64_t populate_key_at(uint64_t seed, uint64_t index) {
    // Mix seed and index into a single 64-bit value; splitmix it.
    return splitmix64(seed ^ (index + UINT64_C(0x9e3779b97f4a7c15)));
}

void populate_value_for(uint64_t key, void *value_out) {
    // Byte layout mirrors what the probe expects: [gen: u64][payload].
    // Initial populate stamps gen=0; writers bump it monotonically on put.
    uint8_t *p = (uint8_t *)value_out;
    uint64_t gen = 0;
    memcpy(p, &gen, sizeof gen);
    uint64_t state = key;
    for (size_t i = sizeof gen; i < VAL_SIZE; i += sizeof(uint64_t)) {
        state = splitmix64(state);
        size_t remaining = VAL_SIZE - i;
        size_t n = remaining < sizeof(uint64_t) ? remaining : sizeof(uint64_t);
        memcpy(p + i, &state, n);
    }
}

int populate_shard_path(char *path_out, size_t path_cap,
                        const char *dir, uint32_t node_id, uint64_t shard) {
    int written = snprintf(path_out, path_cap,
                           "%s/table-node%u-s%llu.db",
                           dir, node_id, (unsigned long long)shard);
    if (written < 0 || (size_t)written >= path_cap) {
        return -1;
    }
    return written;
}

static int ensure_directory(const char *dir) {
    struct stat st;
    if (stat(dir, &st) == 0) {
        return S_ISDIR(st.st_mode) ? 0 : -1;
    }
    if (errno != ENOENT) {
        return -1;
    }
    return mkdir(dir, 0755);
}

// Try to place (key, value) into `page`. Returns 1 on success, 0 if the
// page is full. On success the slot is initialized with version = 2.
static int place_in_page(struct bucket_page *page, uint64_t key, const void *value) {
    int idx = bucket_find_empty(page);
    if (idx < 0) {
        return 0;
    }
    slot_init(&page->slots[idx], key, value);
    return 1;
}

int populate_run(const char *dir,
                 uint32_t node_id,
                 uint64_t seed,
                 const struct table_geometry *g,
                 bool durable,
                 struct populate_stats *stats_out) {
    if (dir == NULL || g == NULL || node_id >= g->n_nodes) {
        fprintf(stderr, "populate: invalid arguments\n");
        return -1;
    }
    if (ensure_directory(dir) != 0) {
        fprintf(stderr, "populate: cannot use directory %s: %s\n",
                dir, strerror(errno));
        return -1;
    }

    uint64_t buckets_for_me = table_buckets_for_owner(g, node_id);
    uint64_t shards_for_me  = table_shards_for_owner(g, node_id);
    if (buckets_for_me == 0U) {
        fprintf(stderr,
                "populate: node %u owns 0 buckets (n_buckets=%llu, n_nodes=%u)\n",
                node_id, (unsigned long long)g->n_buckets, g->n_nodes);
        return -1;
    }

    // Allocate all of this node's pages contiguously, indexed by local rank.
    struct bucket_page *pages = calloc(buckets_for_me, sizeof(struct bucket_page));
    if (pages == NULL) {
        fprintf(stderr,
                "populate: calloc(%llu pages) failed: %s\n",
                (unsigned long long)buckets_for_me, strerror(errno));
        return -1;
    }

    struct populate_stats stats = {0};
    int rc = 0;

    for (uint64_t i = 0; i < g->n_records; ++i) {
        uint64_t key = populate_key_at(seed, i);
        uint64_t bucket_id = table_bucket_id(key, g->n_buckets);
        stats.records_considered++;
        if (table_owner(bucket_id, g->n_nodes) != node_id) {
            continue;
        }
        stats.records_owned++;

        uint64_t rank = table_local_rank(bucket_id, g->n_nodes);
        uint8_t value[VAL_SIZE];
        populate_value_for(key, value);

        // Linear probe: try the home bucket, then the next-in-stripe bucket
        // on this owner (rank+1), until we find space. Cross-bucket probing
        // is rare at the 75% default load factor.
        uint64_t probe_rank = rank;
        int placed = 0;
        while (probe_rank < buckets_for_me) {
            if (place_in_page(&pages[probe_rank], key, value)) {
                placed = 1;
                if (probe_rank != rank) {
                    stats.overflow_probes++;
                }
                break;
            }
            probe_rank++;
        }
        if (!placed) {
            fprintf(stderr,
                    "populate: node %u overflow starting at rank %llu "
                    "(n_buckets=%llu, LF=%llu%%). Increase --records budget "
                    "or lower --load-factor.\n",
                    node_id,
                    (unsigned long long)rank,
                    (unsigned long long)g->n_buckets,
                    (unsigned long long)g->load_factor_pct);
            rc = -1;
            break;
        }
        stats.records_placed++;
    }

    if (rc == 0) {
        // Flush each shard: a single pwrite (or repeated pwrite) of
        // buckets_per_shard pages, starting at offset 0.
        for (uint64_t s = 0; s < shards_for_me && rc == 0; ++s) {
            char path[1024];
            if (populate_shard_path(path, sizeof(path), dir, node_id, s) < 0) {
                fprintf(stderr, "populate: shard path too long\n");
                rc = -1;
                break;
            }
            int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
            if (fd < 0) {
                fprintf(stderr, "populate: open(%s): %s\n", path, strerror(errno));
                rc = -1;
                break;
            }
            uint64_t first_rank = s * g->buckets_per_shard;
            uint64_t shard_buckets = buckets_for_me - first_rank;
            if (g->buckets_per_shard != UINT64_MAX
                && shard_buckets > g->buckets_per_shard) {
                shard_buckets = g->buckets_per_shard;
            }
            size_t shard_bytes = (size_t)shard_buckets * PAGE_SIZE;
            size_t written = 0;
            while (written < shard_bytes) {
                ssize_t n = pwrite(fd,
                                   (const uint8_t *)&pages[first_rank] + written,
                                   shard_bytes - written,
                                   (off_t)written);
                if (n < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    fprintf(stderr, "populate: pwrite(%s): %s\n",
                            path, strerror(errno));
                    rc = -1;
                    break;
                }
                written += (size_t)n;
            }
            if (rc == 0 && durable) {
                if (fsync(fd) != 0) {
                    fprintf(stderr, "populate: fsync(%s): %s\n",
                            path, strerror(errno));
                    rc = -1;
                }
            }
            (void)close(fd);
        }
    }

    free(pages);
    if (stats_out != NULL) {
        *stats_out = stats;
    }
    return rc;
}
