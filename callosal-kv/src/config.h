// config.h — compile-time defaults for callosal-kv.
//
// Runtime behavior is controlled via CLI flags in main.c.
// The constants here define the on-disk slot layout and a handful of default
// values that rarely change at runtime.
//
// WARNING: changing KEY_SIZE, VAL_SIZE, or VERSION_SIZE changes the on-disk
// layout and is incompatible with existing populated tables. Repopulate.

#ifndef CALLOSAL_KV_CONFIG_H
#define CALLOSAL_KV_CONFIG_H

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#define KEY_SIZE           8
#define VAL_SIZE           128
#define VERSION_SIZE       8
#define SLOT_SIZE          (VERSION_SIZE + KEY_SIZE + VAL_SIZE)

#define PAGE_SIZE          4096
#define SLOTS_PER_BUCKET   (PAGE_SIZE / SLOT_SIZE)

#define DEFAULT_RECORDS          (10ULL * 1000ULL * 1000ULL)
#define DEFAULT_LOAD_FACTOR_PCT  75
#define DEFAULT_DURATION_SEC     30
#define DEFAULT_ZIPF_THETA       0.99
#define DEFAULT_WRITE_RATIO      0.50
#define DEFAULT_MAX_FILE_MB      0  // 0 = no cap

static_assert(VERSION_SIZE == sizeof(uint64_t),
              "VERSION_SIZE must match uint64_t");
static_assert(SLOTS_PER_BUCKET * SLOT_SIZE <= PAGE_SIZE,
              "slots must fit within a page");
static_assert(SLOTS_PER_BUCKET > 0,
              "must have at least one slot per bucket");

#endif  // CALLOSAL_KV_CONFIG_H
