// bucket.c — slot operations and the intra-page seqlock.
//
// The seqlock uses acquire/release loads and stores on the version word,
// bracketing a memcpy of the payload. It's deliberately the classical
// Linux-kernel seqlock, not a per-cache-line variant: remote readers pull
// whole pages, while same-node readers and writers can still race within
// the page. The seqlock is for that intra-page race.

#include "bucket.h"

#include <stdatomic.h>
#include <string.h>

void key_to_bytes(uint64_t key, uint8_t out[KEY_SIZE]) {
    for (size_t i = 0; i < KEY_SIZE; ++i) {
        out[i] = (uint8_t)((key >> (8U * i)) & 0xFFU);
    }
}

uint64_t key_from_bytes(const uint8_t in[KEY_SIZE]) {
    uint64_t key = 0;
    for (size_t i = 0; i < KEY_SIZE; ++i) {
        key |= ((uint64_t)in[i]) << (8U * i);
    }
    return key;
}

int slot_is_empty(const struct slot *s) {
    return s->version == SLOT_EMPTY_VERSION ? 1 : 0;
}

void slot_init(struct slot *s, uint64_t key, const void *value) {
    key_to_bytes(key, s->key);
    memcpy(s->value, value, VAL_SIZE);
    s->version = SLOT_FIRST_VERSION;
}

int bucket_find_key(const struct bucket_page *page, uint64_t key) {
    for (int i = 0; i < SLOTS_PER_BUCKET; ++i) {
        const struct slot *s = &page->slots[i];
        if (s->version == SLOT_EMPTY_VERSION) {
            continue;
        }
        if (key_from_bytes(s->key) == key) {
            return i;
        }
    }
    return -1;
}

int bucket_find_empty(const struct bucket_page *page) {
    for (int i = 0; i < SLOTS_PER_BUCKET; ++i) {
        if (page->slots[i].version == SLOT_EMPTY_VERSION) {
            return i;
        }
    }
    return -1;
}

// Wrappers over __atomic builtins so the seqlock idiom reads cleanly. These
// operate on plain `uint64_t` fields — C11 atomics would force a different
// on-disk type and risk subtle layout changes.
static inline uint64_t version_load_acquire(const uint64_t *v) {
    return __atomic_load_n(v, __ATOMIC_ACQUIRE);
}

static inline void version_store_release(uint64_t *v, uint64_t new_value) {
    __atomic_store_n(v, new_value, __ATOMIC_RELEASE);
}

int slot_seqlock_read(const struct slot *s, uint64_t *key_out,
                      void *value_out, int max_retries) {
    for (int attempt = 0; attempt < max_retries; ++attempt) {
        uint64_t v1 = version_load_acquire(&s->version);
        if ((v1 & 1U) != 0U || v1 == SLOT_EMPTY_VERSION) {
            // Writer in flight, or slot never populated. Spin and retry.
            continue;
        }
        // The acquire fence on v1 orders the following payload reads after
        // the version load, so a reader will not observe payload stores
        // that happened after v1 was published.
        uint8_t key_bytes[KEY_SIZE];
        memcpy(key_bytes, s->key, KEY_SIZE);
        memcpy(value_out, s->value, VAL_SIZE);
        atomic_thread_fence(memory_order_acquire);
        uint64_t v2 = version_load_acquire(&s->version);
        if (v1 == v2) {
            if (key_out != NULL) {
                *key_out = key_from_bytes(key_bytes);
            }
            return 0;
        }
    }
    return -1;
}

void slot_seqlock_write(struct slot *s, uint64_t key, const void *value) {
    uint64_t current = __atomic_load_n(&s->version, __ATOMIC_RELAXED);
    // An empty slot (version 0) uses SLOT_FIRST_VERSION-1 = 1 as its
    // transient odd value, committing at SLOT_FIRST_VERSION on release.
    uint64_t in_flight = (current == SLOT_EMPTY_VERSION) ? 1U : current + 1U;
    uint64_t committed = in_flight + 1U;

    version_store_release(&s->version, in_flight);
    // The release fence above is what readers synchronize with. Stores
    // issued after it are not reordered before the version bump.
    atomic_thread_fence(memory_order_release);

    key_to_bytes(key, s->key);
    memcpy(s->value, value, VAL_SIZE);

    version_store_release(&s->version, committed);
}
