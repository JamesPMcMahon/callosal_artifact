// bench.c — worker thread orchestration for --run.

#include "bench.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __linux__
#include <sched.h>
#endif

#include "client.h"
#include "populate.h"
#include "stats.h"
#include "table.h"

// Monotonic nanosecond clock. CLOCK_MONOTONIC is immune to NTP steps
// which is what we want across a --start-epoch wait.
static uint64_t now_ns(void) {
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

struct worker {
    uint32_t             id;
    const struct bench_config *cfg;
    struct client       *client;
    struct table_geometry *geom;
    pthread_barrier_t   *start_barrier;
    pthread_barrier_t   *warmup_barrier;  // NULL when warmup_sec == 0
    atomic_bool         *warmup_flag;
    atomic_bool         *stop_flag;
    struct thread_stats  stats;
    struct probe_ctx     probe;
    // Generation source for PUT-stamped values. Two modes:
    //
    //  - probe_mode != NONE: all writers share one atomic counter so the
    //    reader's monotonic-gen invariant holds per key. Without that,
    //    two threads writing key K with private counters produce apparent
    //    retrogrades that look like violations but are workload-construction
    //    artifacts — see the CREW/intra-node-partition comment below for
    //    why key ownership alone isn't enough.
    //
    //  - probe_mode == NONE: each worker bumps its own counter. The gen
    //    is unread in this mode, and the shared atomic was a global cache
    //    line bouncing on every write — a measurement-instrument ceiling,
    //    not a system-under-test property. Skipping it cleans up the
    //    perf numbers so they reflect seqlock + page I/O, not RMW contention.
    _Atomic uint64_t    *shared_gen;
    uint64_t             local_gen;
};

static void pin_self(uint32_t core) {
#ifdef __linux__
    cpu_set_t set;
    CPU_ZERO(&set);
    // CPU_SET's second arg is size_t; the int cast inside the macro
    // triggers -Wsign-conversion, so we feed it a size_t directly.
    size_t c = (size_t)core;
    CPU_SET(c, &set);
    (void)pthread_setaffinity_np(pthread_self(), sizeof set, &set);
#else
    (void)core;
#endif
}

// worker_do_op: execute one op from the workload stream, updating stats
// and probe state. Factored so the warmup and measurement phases share
// the identical hot path — the only difference between phases is that
// the per-thread stats and probe counters are reset at the phase boundary.
static inline void worker_do_op(struct worker *w, struct workload *wl,
                                uint8_t *val_buf) {
    const struct bench_config *cfg = w->cfg;
    enum op_kind kind;
    uint64_t idx;
    workload_next_op(wl, &kind, &idx);
    uint64_t key = populate_key_at(cfg->seed, idx);

    // CREW filter: writes only for keys this node owns, and further
    // partitioned among this node's writer threads so no two threads
    // ever race on the same slot. Without the intra-node partition,
    // two threads on the same node running client_put(K) can have
    // their three-phase pwrites interleave, producing an ABA on the
    // seqlock version — reader sees v1==v2 yet gets either writer's
    // payload. That's fine for page-level coherence but breaks the
    // monotonic-gen invariant the probe asserts.
    if (kind == OP_PUT) {
	    while(1){
		    uint64_t bucket_id = table_bucket_id(key, w->geom->n_buckets);
		    if (table_owner(bucket_id, w->geom->n_nodes) != cfg->node_id) {
			    if (wl->dist == DIST_ZIPF) {
				    idx = zipf_next(wl);
			    } else {
				    idx = uniform_next(wl);
			    }
			    key = populate_key_at(cfg->seed, idx);
			    continue;
		    }
		    if ((idx % (uint64_t)cfg->threads) != (uint64_t)w->id) {
			    if (wl->dist == DIST_ZIPF) {
				    idx = zipf_next(wl);
			    } else {
				    idx = uniform_next(wl);
			    }
			    key = populate_key_at(cfg->seed, idx);

			    continue;
		    }
		    break;
	    }	
    }

    uint64_t t0 = now_ns();
    if (kind == OP_GET) {
        int rc = client_get(w->client, key, val_buf);
        uint64_t dt = now_ns() - t0;
        if (rc == CLIENT_OK) {
            w->stats.gets++;
            stats_record(&w->stats, OP_GET, dt);
            uint64_t gen = probe_decode_gen(val_buf);
            probe_observe(&w->probe, idx, gen);
        } else if (rc == CLIENT_NOT_FOUND) {
            w->stats.get_not_found++;
        } else if (rc == CLIENT_RETRY) {
            w->stats.get_retries++;
        } else {
            w->stats.errors++;
        }
    } else {
        uint64_t gen;
        if (cfg->probe_mode == PROBE_MODE_NONE) {
            // Per-thread: no cross-core traffic. Gen is unread here.
            gen = ++w->local_gen;
        } else {
            gen = atomic_fetch_add_explicit(w->shared_gen, 1U,
                                            memory_order_relaxed) + 1U;
        }
        // High bit set distinguishes writer-stamped gens from
        // populate's gen=0 across the entire run.
        gen |= UINT64_C(1) << 62;
        probe_encode_value(key, gen, val_buf);
        int rc = client_put(w->client, key, val_buf);
        uint64_t dt = now_ns() - t0;
        if (rc == CLIENT_OK) {
            w->stats.puts++;
            stats_record(&w->stats, OP_PUT, dt);
        } else if (rc == CLIENT_NOT_FOUND) {
            w->stats.put_not_found++;
        } else {
            w->stats.errors++;
        }
    }
}

static void *worker_main(void *arg) {
    struct worker *w = (struct worker *)arg;
    const struct bench_config *cfg = w->cfg;

    if (cfg->pin) {
        pin_self(w->id);
    }

    struct workload wl;
    if (workload_init(&wl, cfg->dist, cfg->zipf_theta, cfg->write_ratio,
                      cfg->records, cfg->seed ^ ((uint64_t)w->id * 0x9e3779b97f4a7c15ULL)) != 0) {
        fprintf(stderr, "bench: workload_init failed (thread %u)\n", w->id);
        return NULL;
    }

    uint8_t val_buf[VAL_SIZE];

    (void)pthread_barrier_wait(w->start_barrier);

    // Warmup phase: run identical ops, then zero this thread's stats and
    // probe counters at the barrier. The probe's hash table of last-seen
    // gens is intentionally NOT cleared — keeping it preserves monotonic
    // reference points across the phase boundary so a stale reader that
    // crosses the barrier cannot hide a retrograde.
    if (cfg->warmup_sec > 0U) {
        // Check stop_flag too so the pthread_create error path — which
        // can't feed a warmup_barrier that expects every worker — can
        // still abort cleanly without waiting on the phase barrier.
        while (!atomic_load_explicit(w->warmup_flag, memory_order_relaxed)
               && !atomic_load_explicit(w->stop_flag, memory_order_relaxed)) {
            worker_do_op(w, &wl, val_buf);
        }
        if (atomic_load_explicit(w->stop_flag, memory_order_relaxed)) {
            return NULL;
        }
        (void)pthread_barrier_wait(w->warmup_barrier);
        stats_init(&w->stats);
        w->probe.observations = 0;
        w->probe.violations = 0;
    }

    while (!atomic_load_explicit(w->stop_flag, memory_order_relaxed)) {
        worker_do_op(w, &wl, val_buf);
    }
    return NULL;
}

int bench_run(const struct bench_config *cfg) {
    if (cfg == NULL || cfg->data_dir == NULL || cfg->threads == 0U) {
        fprintf(stderr, "bench: invalid config\n");
        return -1;
    }

    struct table_geometry geom;
    if (table_geometry_init(&geom, cfg->records, cfg->n_nodes,
                            cfg->load_factor_pct, cfg->max_file_pages) != 0) {
        fprintf(stderr, "bench: bad geometry\n");
        return -1;
    }

    struct client cl;
    if (client_open(&cl, cfg->data_dir, &geom) != 0) {
        fprintf(stderr, "bench: client_open failed\n");
        return -1;
    }

    struct worker *workers = calloc(cfg->threads, sizeof(*workers));
    pthread_t     *tids    = calloc(cfg->threads, sizeof(*tids));
    if (workers == NULL || tids == NULL) {
        free(workers);
        free(tids);
        client_close(&cl);
        return -1;
    }

    pthread_barrier_t start_barrier;
    (void)pthread_barrier_init(&start_barrier, NULL, cfg->threads + 1U);
    pthread_barrier_t warmup_barrier;
    bool warmup_barrier_inited = false;
    if (cfg->warmup_sec > 0U) {
        (void)pthread_barrier_init(&warmup_barrier, NULL, cfg->threads + 1U);
        warmup_barrier_inited = true;
    }
    atomic_bool warmup_flag;
    atomic_init(&warmup_flag, false);
    atomic_bool stop_flag;
    atomic_init(&stop_flag, false);
    _Atomic uint64_t shared_gen;
    // Seed from CLOCK_REALTIME nanoseconds so a repeated --run against a
    // reused data directory cannot fabricate probe violations: every new
    // run's PUT gens sit strictly above any writer-stamped gens left in
    // slots by the previous run. Lower 62 bits (ns-since-epoch) stay
    // below bit 62 well past year 2116, so the bit-62 writer sentinel
    // does not alias into the counter.
    struct timespec tseed;
    (void)clock_gettime(CLOCK_REALTIME, &tseed);
    uint64_t seed_ns = (uint64_t)tseed.tv_sec * UINT64_C(1000000000) +
                       (uint64_t)tseed.tv_nsec;
    atomic_init(&shared_gen, seed_ns);

    for (uint32_t i = 0; i < cfg->threads; ++i) {
        workers[i].id            = i;
        workers[i].cfg           = cfg;
        workers[i].client        = &cl;
        workers[i].geom          = &geom;
        workers[i].start_barrier = &start_barrier;
        workers[i].warmup_barrier = warmup_barrier_inited ? &warmup_barrier : NULL;
        workers[i].warmup_flag   = &warmup_flag;
        workers[i].stop_flag     = &stop_flag;
        workers[i].shared_gen    = &shared_gen;
        // Seed local_gen like shared_gen so a reused data-dir can't
        // fabricate retrogrades in probe-off runs either, and offset by
        // thread id so the bytes written to a given slot differ per-thread
        // (keeps the payload format consistent between probe modes).
        workers[i].local_gen     = seed_ns + (uint64_t)i * 0x9e3779b97f4a7c15ULL;
        stats_init(&workers[i].stats);
        if (probe_init(&workers[i].probe, cfg->probe_mode, cfg->probe_capacity) != 0) {
            fprintf(stderr, "bench: probe_init failed (thread %u)\n", i);
            // Best-effort cleanup: spawn nothing, tear down what exists.
            for (uint32_t j = 0; j < i; ++j) {
                probe_free(&workers[j].probe);
            }
            pthread_barrier_destroy(&start_barrier);
            if (warmup_barrier_inited) {
                pthread_barrier_destroy(&warmup_barrier);
            }
            free(workers);
            free(tids);
            client_close(&cl);
            return -1;
        }
        if (pthread_create(&tids[i], NULL, worker_main, &workers[i]) != 0) {
            fprintf(stderr, "bench: pthread_create failed\n");
            // Unblock any already-started workers by releasing barrier,
            // then set stop so they exit immediately.
            atomic_store(&stop_flag, true);
            (void)pthread_barrier_wait(&start_barrier);
            for (uint32_t j = 0; j < i; ++j) {
                (void)pthread_join(tids[j], NULL);
                probe_free(&workers[j].probe);
            }
            for (uint32_t j = i; j < cfg->threads; ++j) {
                probe_free(&workers[j].probe);
            }
            pthread_barrier_destroy(&start_barrier);
            if (warmup_barrier_inited) {
                pthread_barrier_destroy(&warmup_barrier);
            }
            free(workers);
            free(tids);
            client_close(&cl);
            return -1;
        }
    }

    (void)pthread_barrier_wait(&start_barrier);

    // Optional warmup phase: workers run the same op loop but discard stats
    // at the phase barrier so the measurement window below starts from a
    // warm cache / steady-state generation counter.
    if (cfg->warmup_sec > 0U) {
        struct timespec wts = {
            .tv_sec  = (time_t)cfg->warmup_sec,
            .tv_nsec = 0,
        };
        (void)nanosleep(&wts, NULL);
        atomic_store(&warmup_flag, true);
        (void)pthread_barrier_wait(&warmup_barrier);
    }

    // Time-bounded measurement window. Capture t_start *after* any warmup
    // so duration_ns reflects only the measured phase.
    uint64_t t_start = now_ns();
    struct timespec ts = {
        .tv_sec  = (time_t)cfg->duration_sec,
        .tv_nsec = 0,
    };
    (void)nanosleep(&ts, NULL);
    atomic_store(&stop_flag, true);

    for (uint32_t i = 0; i < cfg->threads; ++i) {
        (void)pthread_join(tids[i], NULL);
    }
    uint64_t t_end = now_ns();

    // Aggregate stats.
    struct thread_stats *ts_arr = calloc(cfg->threads, sizeof(*ts_arr));
    uint64_t total_violations = 0, total_observations = 0;
    for (uint32_t i = 0; i < cfg->threads; ++i) {
        ts_arr[i] = workers[i].stats;
        total_violations   += workers[i].probe.violations;
        total_observations += workers[i].probe.observations;
    }
    struct agg_stats agg;
    stats_aggregate(&agg, ts_arr, cfg->threads, t_end - t_start);

    // Human-readable summary.
    double secs = (double)agg.duration_ns / 1e9;
    double mops = (double)agg.total_ops / secs / 1e6;
    printf("bench: node=%u threads=%u duration=%.2fs ops=%" PRIu64
           " (%.2f Mops/s)\n",
           cfg->node_id, cfg->threads, secs, agg.total_ops, mops);
    printf("bench: gets=%" PRIu64 " puts=%" PRIu64
           " not_found=(%" PRIu64 ",%" PRIu64 ")"
           " retries=%" PRIu64 " errors=%" PRIu64 "\n",
           agg.total_gets, agg.total_puts,
           agg.total_get_not_found, agg.total_put_not_found,
           agg.total_get_retries, agg.total_errors);
    printf("bench: get p50/p95/p99 = %" PRIu64 "/%" PRIu64 "/%" PRIu64 " ns\n",
           agg.get_p50_ns, agg.get_p95_ns, agg.get_p99_ns);
    printf("bench: put p50/p95/p99 = %" PRIu64 "/%" PRIu64 "/%" PRIu64 " ns\n",
           agg.put_p50_ns, agg.put_p95_ns, agg.put_p99_ns);
    if (cfg->probe_mode != PROBE_MODE_NONE) {
        printf("bench: probe monotonic: %" PRIu64 " violations / %" PRIu64
               " observations\n",
               total_violations, total_observations);
    }

    int rc = 0;
    if (cfg->out_path != NULL && cfg->out_path[0] != '\0') {
        if (stats_dump_json(&agg, cfg->out_path) != 0) {
            rc = -1;
        }
    }

    for (uint32_t i = 0; i < cfg->threads; ++i) {
        probe_free(&workers[i].probe);
    }
    free(ts_arr);
    free(workers);
    free(tids);
    pthread_barrier_destroy(&start_barrier);
    if (warmup_barrier_inited) {
        pthread_barrier_destroy(&warmup_barrier);
    }
    client_close(&cl);
    return rc;
}

