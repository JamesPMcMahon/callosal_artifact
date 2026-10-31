// bucket.h — on-disk / in-memory slot layout and seqlock primitives.
//
// A bucket is one 4 KiB page of SLOTS_PER_BUCKET fixed-size slots. Each slot
// carries a version word at its head that implements a Silo/Linux-kernel
// style seqlock for reader-writer coordination.
//
// CREW invariant: at most one writer per slot at any time. Readers may be
// concurrent and spread across nodes. The seqlock catches reader-writer
// races on the same page; the surrounding file system may serialize page
// ownership across nodes, but it does not make a single slot read atomic.

#ifndef CALLOSAL_KV_BUCKET_H
#define CALLOSAL_KV_BUCKET_H

#include <stddef.h>
#include <stdint.h>

#include "config.h"

#define SLOT_EMPTY_VERSION   UINT64_C(0)
#define SLOT_FIRST_VERSION   UINT64_C(2)

// A slot is laid out with the version at offset 0 so that in-order memcpy
// (e.g. pread via copy_to_user) sees the version word before the payload.
// The struct is intentionally unpacked: natural alignment happens to match
// the on-disk layout exactly (8 + 8 + 128 = 144) and lets atomic loads on
// the version field be single-instruction.
struct slot {
    uint64_t version;
    uint8_t  key[KEY_SIZE];
    uint8_t  value[VAL_SIZE];
};
static_assert(sizeof(struct slot) == SLOT_SIZE, "slot size must equal SLOT_SIZE");
static_assert(offsetof(struct slot, version) == 0, "version must be at slot head");

// A bucket_page is exactly one page with a contiguous slot array followed
// by slack to round up to PAGE_SIZE.
struct bucket_page {
    struct slot slots[SLOTS_PER_BUCKET];
    uint8_t     slack[PAGE_SIZE - SLOTS_PER_BUCKET * SLOT_SIZE];
};
static_assert(sizeof(struct bucket_page) == PAGE_SIZE, "bucket_page must be PAGE_SIZE");

// slot_is_empty: predicate on a slot snapshot. A slot with version 0 is
// considered unoccupied.
int slot_is_empty(const struct slot *s);

// slot_init: install (key, value) into an empty slot with version=2.
// Intended for single-threaded pre-population. Does not run the seqlock
// protocol — there are no concurrent readers during populate.
void slot_init(struct slot *s, uint64_t key, const void *value);

// bucket_find_key: linear scan of a page for an occupied slot matching key.
// Returns the slot index in [0, SLOTS_PER_BUCKET) or -1 if no match.
// Does not honor the seqlock — the caller is responsible for validating
// the snapshot around any subsequent read.
int bucket_find_key(const struct bucket_page *page, uint64_t key);

// bucket_find_empty: return the index of the first empty slot in page,
// or -1 if the page is fully occupied.
int bucket_find_empty(const struct bucket_page *page);

// slot_seqlock_read: read (key, value) from a slot that may be concurrently
// written by one other thread on the same address. Retries up to max_retries
// times on odd or changing versions. Returns 0 on success, -1 on exhaustion.
//
// value_out must have room for VAL_SIZE bytes. key_out may be NULL.
// Intended for *in-memory* shared access; the file-backed read path in
// client.c layers its own re-read of the page on top of this primitive.
int slot_seqlock_read(const struct slot *s, uint64_t *key_out,
                      void *value_out, int max_retries);

// slot_seqlock_write: publish (key, value) into a slot using the
// odd/even version protocol. CREW precondition: the caller is the slot's
// sole writer. Safe to call with version=0 (empty slot first write) — in
// that case the first committed version is SLOT_FIRST_VERSION.
void slot_seqlock_write(struct slot *s, uint64_t key, const void *value);

// key_to_bytes / bytes_to_key: little-endian pack/unpack helpers for
// the slot key field. Exposed for tests and tools.
void     key_to_bytes(uint64_t key, uint8_t out[KEY_SIZE]);
uint64_t key_from_bytes(const uint8_t in[KEY_SIZE]);

#endif  // CALLOSAL_KV_BUCKET_H
