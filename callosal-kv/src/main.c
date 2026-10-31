// main.c — entry point for callosal-kv.
//
// Supports --populate (build this node's shards) and --run (measurement
// phase).

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "barrier.h"
#include "bench.h"
#include "config.h"
#include "populate.h"
#include "table.h"
#include "workload.h"
#include "probe.h"

enum mode {
    MODE_UNSET,
    MODE_POPULATE,
    MODE_RUN,
};

struct args {
    enum mode   mode;
    const char *path;
    uint64_t    records;
    uint32_t    node_id;
    uint32_t    num_nodes;
    uint64_t    seed;
    uint64_t    load_factor_pct;
    uint64_t    max_file_pages;
    bool        durable;

    // --run only:
    uint32_t    threads;
    uint32_t    duration_sec;
    uint32_t    warmup_sec;
    bool        pin;
    double      write_ratio;
    double      zipf_theta;
    const char *dist_name;
    const char *probe_name;
    const char *out_path;
    const char *coordinator;  // "host:port" or NULL for standalone
};

static void print_help(const char *prog) {
    printf(
        "usage: %s [options] --populate --path DIR\n"
        "       %s [options] --run      --path DIR\n"
        "\n"
        "callosal-kv: shared-file key-value benchmark over pread/pwrite.\n"
        "\n"
        "mode (choose one):\n"
        "  --populate              build this node's shards on disk and exit\n"
        "  --run                   measurement phase\n"
        "\n"
        "layout (must match between populate and run):\n"
        "  --path DIR              directory holding shard files\n"
        "  --records N             total records across cluster (default %llu)\n"
        "  --node-id I             this node's id, 0 <= I < num-nodes\n"
        "  --num-nodes N           cluster size (default 1)\n"
        "  --seed S                PRNG seed for key generation (default 1)\n"
        "  --load-factor PCT       target per-bucket load factor (default %d)\n"
        "  --max-file-pages N      cap per-shard file size in 4 KiB pages (0=no cap, default 0)\n"
        "\n"
        "populate-only:\n"
        "  --fsync                 fsync() each shard after writing\n"
        "\n"
        "run-only:\n"
        "  --threads N             worker threads (default: online CPUs)\n"
        "  --duration S            measurement duration in seconds (default %d)\n"
        "  --warmup S              warmup seconds (discarded, default 0)\n"
        "  --dist {uniform,zipf}   op-stream distribution (default uniform)\n"
        "  --zipf-theta T          zipf theta when --dist zipf (default %.2f)\n"
        "  --write-ratio R         fraction of ops that are puts (default %.2f)\n"
        "  --probe {none,monotonic} correctness probe (default monotonic)\n"
        "  --pin / --no-pin        pin worker threads to cores (default pin)\n"
        "  --out PATH              stats JSON output (default stats-node<I>.json)\n"
        "  --coordinator H:P       UDP barrier rendezvous (default: standalone)\n"
        "\n"
        "general:\n"
        "  -h, --help              show this message and exit\n",
        prog, prog,
        (unsigned long long)DEFAULT_RECORDS,
        DEFAULT_LOAD_FACTOR_PCT,
        DEFAULT_DURATION_SEC,
        DEFAULT_ZIPF_THETA,
        DEFAULT_WRITE_RATIO);
}

static int parse_u64(const char *s, uint64_t *out) {
    if (s == NULL || s[0] == '\0' || s[0] == '-') {
        return -1;
    }
    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno != 0 || end == NULL || *end != '\0') {
        return -1;
    }
    *out = (uint64_t)v;
    return 0;
}

static int parse_u32(const char *s, uint32_t *out) {
    uint64_t v = 0;
    if (parse_u64(s, &v) != 0 || v > UINT32_MAX) {
        return -1;
    }
    *out = (uint32_t)v;
    return 0;
}

static int parse_double(const char *s, double *out) {
    if (s == NULL || s[0] == '\0') {
        return -1;
    }
    char *end = NULL;
    errno = 0;
    double v = strtod(s, &end);
    if (errno != 0 || end == NULL || *end != '\0') {
        return -1;
    }
    *out = v;
    return 0;
}

static int take_arg_value(int argc, char **argv, int *i, const char *flag,
                          const char **value_out) {
    if (*i + 1 >= argc) {
        fprintf(stderr, "error: %s requires a value\n", flag);
        return -1;
    }
    *i += 1;
    *value_out = argv[*i];
    return 0;
}

static int parse_args(int argc, char **argv, struct args *out) {
    out->mode = MODE_UNSET;
    out->path = NULL;
    out->records = DEFAULT_RECORDS;
    out->node_id = 0;
    out->num_nodes = 1;
    out->seed = 1;
    out->load_factor_pct = (uint64_t)DEFAULT_LOAD_FACTOR_PCT;
    out->max_file_pages = (uint64_t)DEFAULT_MAX_FILE_MB;
    out->durable = false;
    out->threads = 0;  // 0 → auto
    out->duration_sec = (uint32_t)DEFAULT_DURATION_SEC;
    out->warmup_sec = 0U;
    out->pin = true;
    out->write_ratio = DEFAULT_WRITE_RATIO;
    out->zipf_theta = DEFAULT_ZIPF_THETA;
    out->dist_name = "uniform";
    out->probe_name = "monotonic";
    out->out_path = NULL;
    out->coordinator = NULL;

    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        const char *v = NULL;
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            print_help(argv[0]);
            exit(0);
        } else if (strcmp(a, "--populate") == 0) {
            out->mode = MODE_POPULATE;
        } else if (strcmp(a, "--run") == 0) {
            out->mode = MODE_RUN;
        } else if (strcmp(a, "--fsync") == 0) {
            out->durable = true;
        } else if (strcmp(a, "--pin") == 0) {
            out->pin = true;
        } else if (strcmp(a, "--no-pin") == 0) {
            out->pin = false;
        } else if (strcmp(a, "--path") == 0) {
            if (take_arg_value(argc, argv, &i, a, &v) != 0) { return -1; }
            out->path = v;
        } else if (strcmp(a, "--records") == 0) {
            if (take_arg_value(argc, argv, &i, a, &v) != 0) { return -1; }
            if (parse_u64(v, &out->records) != 0) {
                fprintf(stderr, "error: bad --records: %s\n", v); return -1;
            }
        } else if (strcmp(a, "--node-id") == 0) {
            if (take_arg_value(argc, argv, &i, a, &v) != 0) { return -1; }
            if (parse_u32(v, &out->node_id) != 0) {
                fprintf(stderr, "error: bad --node-id: %s\n", v); return -1;
            }
        } else if (strcmp(a, "--num-nodes") == 0) {
            if (take_arg_value(argc, argv, &i, a, &v) != 0) { return -1; }
            if (parse_u32(v, &out->num_nodes) != 0 || out->num_nodes == 0U) {
                fprintf(stderr, "error: bad --num-nodes: %s\n", v); return -1;
            }
        } else if (strcmp(a, "--seed") == 0) {
            if (take_arg_value(argc, argv, &i, a, &v) != 0) { return -1; }
            if (parse_u64(v, &out->seed) != 0) {
                fprintf(stderr, "error: bad --seed: %s\n", v); return -1;
            }
        } else if (strcmp(a, "--load-factor") == 0) {
            if (take_arg_value(argc, argv, &i, a, &v) != 0) { return -1; }
            if (parse_u64(v, &out->load_factor_pct) != 0
                || out->load_factor_pct == 0U
                || out->load_factor_pct > 100U) {
                fprintf(stderr, "error: bad --load-factor: %s (1..100)\n", v);
                return -1;
            }
        } else if (strcmp(a, "--max-file-pages") == 0) {
            if (take_arg_value(argc, argv, &i, a, &v) != 0) { return -1; }
            if (parse_u64(v, &out->max_file_pages) != 0) {
                fprintf(stderr, "error: bad --max-file-pages: %s\n", v); return -1;
            }
        } else if (strcmp(a, "--threads") == 0) {
            if (take_arg_value(argc, argv, &i, a, &v) != 0) { return -1; }
            if (parse_u32(v, &out->threads) != 0) {
                fprintf(stderr, "error: bad --threads: %s\n", v); return -1;
            }
        } else if (strcmp(a, "--duration") == 0) {
            if (take_arg_value(argc, argv, &i, a, &v) != 0) { return -1; }
            if (parse_u32(v, &out->duration_sec) != 0 || out->duration_sec == 0U) {
                fprintf(stderr, "error: bad --duration: %s\n", v); return -1;
            }
        } else if (strcmp(a, "--warmup") == 0) {
            if (take_arg_value(argc, argv, &i, a, &v) != 0) { return -1; }
            if (parse_u32(v, &out->warmup_sec) != 0) {
                fprintf(stderr, "error: bad --warmup: %s\n", v); return -1;
            }
        } else if (strcmp(a, "--dist") == 0) {
            if (take_arg_value(argc, argv, &i, a, &v) != 0) { return -1; }
            out->dist_name = v;
        } else if (strcmp(a, "--zipf-theta") == 0) {
            if (take_arg_value(argc, argv, &i, a, &v) != 0) { return -1; }
            if (parse_double(v, &out->zipf_theta) != 0) {
                fprintf(stderr, "error: bad --zipf-theta: %s\n", v); return -1;
            }
        } else if (strcmp(a, "--write-ratio") == 0) {
            if (take_arg_value(argc, argv, &i, a, &v) != 0) { return -1; }
            if (parse_double(v, &out->write_ratio) != 0) {
                fprintf(stderr, "error: bad --write-ratio: %s\n", v); return -1;
            }
        } else if (strcmp(a, "--probe") == 0) {
            if (take_arg_value(argc, argv, &i, a, &v) != 0) { return -1; }
            out->probe_name = v;
        } else if (strcmp(a, "--out") == 0) {
            if (take_arg_value(argc, argv, &i, a, &v) != 0) { return -1; }
            out->out_path = v;
        } else if (strcmp(a, "--coordinator") == 0) {
            if (take_arg_value(argc, argv, &i, a, &v) != 0) { return -1; }
            out->coordinator = v;
        } else {
            fprintf(stderr, "error: unknown argument: %s\n", a);
            return -1;
        }
    }

    if (out->mode == MODE_UNSET) {
        fprintf(stderr, "error: must specify --populate or --run (try --help)\n");
        return -1;
    }
    if (out->path == NULL) {
        fprintf(stderr, "error: --path is required\n");
        return -1;
    }
    if (out->node_id >= out->num_nodes) {
        fprintf(stderr,
                "error: --node-id %u must be < --num-nodes %u\n",
                out->node_id, out->num_nodes);
        return -1;
    }
    return 0;
}

static int cmd_populate(const struct args *a) {
    struct table_geometry g;
    if (table_geometry_init(&g, a->records, a->num_nodes,
                            a->load_factor_pct, a->max_file_pages) != 0) {
        fprintf(stderr, "populate: invalid geometry\n");
        return 2;
    }
    uint64_t shards = table_shards_for_owner(&g, a->node_id);
    uint64_t owned_buckets = table_buckets_for_owner(&g, a->node_id);
    printf("populate: node=%u/%u records=%llu n_buckets=%llu "
           "owner_buckets=%llu shards=%llu lf=%llu%% max_file_pages=%llu "
           "dir=%s seed=%llu\n",
           a->node_id, a->num_nodes,
           (unsigned long long)a->records,
           (unsigned long long)g.n_buckets,
           (unsigned long long)owned_buckets,
           (unsigned long long)shards,
           (unsigned long long)a->load_factor_pct,
           (unsigned long long)a->max_file_pages,
           a->path,
           (unsigned long long)a->seed);

    struct populate_stats stats;
    int rc = populate_run(a->path, a->node_id, a->seed, &g, a->durable, &stats);
    if (rc != 0) {
        return 1;
    }
    printf("populate: placed=%llu owned=%llu considered=%llu overflow=%llu\n",
           (unsigned long long)stats.records_placed,
           (unsigned long long)stats.records_owned,
           (unsigned long long)stats.records_considered,
           (unsigned long long)stats.overflow_probes);
    return 0;
}

static int cmd_run(const struct args *a) {
    // If a coordinator is configured, rendezvous via UDP before we open
    // any shards or start threads. The start_ns we receive is currently
    // only logged — bench.c still uses CLOCK_MONOTONIC for measurement.
    if (a->coordinator != NULL) {
        char host[256];
        uint16_t port = 0;
        if (barrier_parse_spec(a->coordinator, host, sizeof host, &port) != 0) {
            fprintf(stderr, "error: bad --coordinator %s (expected host:port)\n",
                    a->coordinator);
            return 2;
        }
        uint64_t start_ns = 0;
        // Timeout generously: the coordinator waits for all peers, which
        // may take tens of seconds in a cold launch. Keep this well below
        // the driver-side barrier_timeout so we fail first with a clear
        // message rather than getting killed from the outside.
        int timeout_ms = 25 * 1000;
        if (barrier_wait(host, port, a->node_id, timeout_ms, &start_ns) != 0) {
            fprintf(stderr, "error: barrier_wait failed (%s:%u node=%u)\n",
                    host, port, a->node_id);
            return 1;
        }
        fprintf(stderr, "bench: barrier released node=%u start_ns=%llu\n",
                a->node_id, (unsigned long long)start_ns);
    }

    enum dist_kind dist;
    if (strcmp(a->dist_name, "uniform") == 0) {
        dist = DIST_UNIFORM;
    } else if (strcmp(a->dist_name, "zipf") == 0) {
        dist = DIST_ZIPF;
    } else {
        fprintf(stderr, "error: --dist must be uniform|zipf (got %s)\n",
                a->dist_name);
        return 2;
    }
    enum probe_mode pm;
    if (strcmp(a->probe_name, "none") == 0) {
        pm = PROBE_MODE_NONE;
    } else if (strcmp(a->probe_name, "monotonic") == 0) {
        pm = PROBE_MODE_MONOTONIC;
    } else {
        fprintf(stderr, "error: --probe must be none|monotonic (got %s)\n",
                a->probe_name);
        return 2;
    }

    uint32_t threads = a->threads;
    if (threads == 0U) {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        threads = (n > 0) ? (uint32_t)n : 1U;
    }

    char default_out[256];
    const char *out_path = a->out_path;
    if (out_path == NULL) {
        snprintf(default_out, sizeof default_out, "stats-node%u.json", a->node_id);
        out_path = default_out;
    }

    struct bench_config cfg = {
        .data_dir        = a->path,
        .node_id         = a->node_id,
        .n_nodes         = a->num_nodes,
        .records         = a->records,
        .load_factor_pct = a->load_factor_pct,
        .max_file_pages  = a->max_file_pages,
        .threads         = threads,
        .duration_sec    = a->duration_sec,
        .warmup_sec      = a->warmup_sec,
        .seed            = a->seed,
        .dist            = dist,
        .zipf_theta      = a->zipf_theta,
        .write_ratio     = a->write_ratio,
        .probe_mode      = pm,
        .probe_capacity  = a->records,
        .pin             = a->pin,
        .out_path        = out_path,
    };
    return bench_run(&cfg) == 0 ? 0 : 1;
}

int main(int argc, char **argv) {
    struct args a;
    if (parse_args(argc, argv, &a) != 0) {
        return 2;
    }
    switch (a.mode) {
    case MODE_POPULATE:
        return cmd_populate(&a);
    case MODE_RUN:
        return cmd_run(&a);
    case MODE_UNSET:
    default:
        return 2;
    }
}
