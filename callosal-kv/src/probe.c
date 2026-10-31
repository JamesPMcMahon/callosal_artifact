// probe.c — monotonic-read generation tracking.

#include "probe.h"

#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "populate.h"

static uint64_t round_up_pow2(uint64_t n) {
    if (n <= 1U) {
        return 1U;
    }
    // Shift up until we cover n.
    uint64_t r = 1U;
    while (r < n) {
        r <<= 1U;
    }
    return r;
}

int probe_init(struct probe_ctx *p, enum probe_mode mode, uint64_t capacity) {
    if (p == NULL) {
        return -1;
    }
    p->mode = mode;
    p->capacity = 0;
    p->keys = NULL;
    p->gens = NULL;
    p->observations = 0;
    p->violations = 0;
    if (mode == PROBE_MODE_NONE) {
        return 0;
    }
    uint64_t cap = round_up_pow2(capacity == 0U ? 1024U : capacity);
    p->keys = calloc(cap, sizeof(uint64_t));
    p->gens = calloc(cap, sizeof(uint64_t));
    if (p->keys == NULL || p->gens == NULL) {
        probe_free(p);
        return -1;
    }
    p->capacity = cap;
    return 0;
}

void probe_free(struct probe_ctx *p) {
    if (p == NULL) {
        return;
    }
    free(p->keys);
    free(p->gens);
    p->keys = NULL;
    p->gens = NULL;
    p->capacity = 0;
}

void probe_encode_value(uint64_t key, uint64_t gen, void *value_out) {
    populate_value_for(key, value_out);
    memcpy(value_out, &gen, sizeof gen);
}

uint64_t probe_decode_gen(const void *value_in) {
    uint64_t gen;
    memcpy(&gen, value_in, sizeof gen);
    return gen;
}

void probe_observe(struct probe_ctx *p, uint64_t key, uint64_t gen) {
    if (p == NULL || p->mode == PROBE_MODE_NONE || p->capacity == 0U) {
        return;
    }
    p->observations++;
    uint64_t idx = key & (p->capacity - 1U);
    if (p->keys[idx] == key) {
        if (gen < p->gens[idx]) {
            p->violations++;
            return;  // don't regress the stored gen
        }
        p->gens[idx] = gen;
    } else {
        // New slot or eviction: record fresh (key, gen).
        p->keys[idx] = key;
        p->gens[idx] = gen;
    }
}
