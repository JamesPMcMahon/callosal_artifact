#!/usr/bin/env bash
# Local benchmark sweep.
#
# Populates the KV store once per records-count, then runs each combination
# of (distribution, mix) and captures per-run stats JSON plus a summary
# table. YCSB-style mixes:
#   A: 50% writes, 50% reads
#   B:  5% writes, 95% reads
#   C:  0% writes (read-only)
# Distributions: uniform, zipf(theta=0.99).
#
# Usage (on the machine where the binary runs):
#   scripts/sweep.sh [--records "100000 10000000"] [--duration 10]
#                    [--threads N] [--data-dir DIR] [--out-dir DIR]
#
# Outputs, under $out_dir:
#   stats-<records>-<dist>-<mix>.json   raw per-run JSON
#   run-<records>-<dist>-<mix>.log      captured stdout for the run
#   summary.csv                          one row per run
#   summary.txt                          pretty-printed table
#
# Dependencies: jq. awk/column from coreutils.

set -euo pipefail

RECORDS_LIST="100000 10000000"
DURATION=10
THREADS=""
DATA_DIR="/tmp/callosal-kv-data"
TS="$(date -u +%Y%m%d-%H%M%S)"
OUT_DIR="results-${TS}"
SEED=1
LOAD_FACTOR=75
BINARY="./callosal-kv"

usage() {
    sed -n '2,22p' "$0"
    exit 2
}

while [ $# -gt 0 ]; do
    case "$1" in
        --records)    RECORDS_LIST="$2"; shift 2;;
        --duration)   DURATION="$2";     shift 2;;
        --threads)    THREADS="$2";      shift 2;;
        --data-dir)   DATA_DIR="$2";     shift 2;;
        --out-dir)    OUT_DIR="$2";      shift 2;;
        --seed)       SEED="$2";         shift 2;;
        --binary)     BINARY="$2";       shift 2;;
        -h|--help)    usage;;
        *) echo "unknown arg: $1" >&2; usage;;
    esac
done

command -v jq >/dev/null 2>&1 || {
    echo "sweep: jq not installed (run 'make setup' or apt-get install jq)" >&2
    exit 1
}
[ -x "$BINARY" ] || {
    echo "sweep: $BINARY not executable — build first (make all)" >&2
    exit 1
}

mkdir -p "$OUT_DIR"
mkdir -p "$DATA_DIR"

# mixes: label → write-ratio
declare -A MIX
MIX[A]=0.5
MIX[B]=0.05
MIX[C]=0.0

DISTS="uniform zipf"
MIXES="A B C"

SUMMARY_CSV="$OUT_DIR/summary.csv"
SUMMARY_TXT="$OUT_DIR/summary.txt"
printf "records,dist,mix,write_ratio,threads,duration_s,total_ops,mops,get_p50_ns,get_p95_ns,get_p99_ns,put_p50_ns,put_p95_ns,put_p99_ns,retries,errors,probe_observations,probe_violations\n" > "$SUMMARY_CSV"

echo "sweep: starting $TS"
echo "sweep: records=[$RECORDS_LIST] duration=${DURATION}s threads=${THREADS:-auto} out=$OUT_DIR"

for records in $RECORDS_LIST; do
    echo "[populate] records=$records -> $DATA_DIR"
    rm -f "$DATA_DIR"/table-node0-s*.db
    "$BINARY" --populate \
              --path "$DATA_DIR" \
              --records "$records" \
              --node-id 0 \
              --num-nodes 1 \
              --seed "$SEED" \
              --load-factor "$LOAD_FACTOR" \
        > "$OUT_DIR/populate-$records.log" 2>&1

    for dist in $DISTS; do
        for mix in $MIXES; do
            wr="${MIX[$mix]}"
            tag="${records}-${dist}-${mix}"
            stats_path="$OUT_DIR/stats-${tag}.json"
            log_path="$OUT_DIR/run-${tag}.log"
            echo "[run] records=$records dist=$dist mix=$mix (write_ratio=$wr)"

            args=(
                --run
                --path "$DATA_DIR"
                --records "$records"
                --node-id 0
                --num-nodes 1
                --seed "$SEED"
                --load-factor "$LOAD_FACTOR"
                --duration "$DURATION"
                --dist "$dist"
                --write-ratio "$wr"
                --probe monotonic
                --no-pin
                --out "$stats_path"
            )
            [ -n "$THREADS" ] && args+=(--threads "$THREADS")

            "$BINARY" "${args[@]}" > "$log_path" 2>&1 || {
                echo "sweep: run failed (see $log_path)" >&2
                continue
            }

            # Pull threads/duration/probe from the human-readable log (not
            # in the JSON today). Throughput we re-derive from the JSON.
            threads_used="$(awk '/^bench: node=/ { for (i=1;i<=NF;i++) if ($i~"^threads=") { sub("threads=","",$i); print $i } }' "$log_path")"
            probe_obs="$(awk '/^bench: probe monotonic:/ { print $7 }' "$log_path")"
            probe_viol="$(awk '/^bench: probe monotonic:/ { print $4 }' "$log_path")"

            jq_expr='[
                .duration_ns/1e9,
                .total_ops,
                (.total_ops / (.duration_ns/1e9) / 1e6),
                .get_p50_ns, .get_p95_ns, .get_p99_ns,
                .put_p50_ns, .put_p95_ns, .put_p99_ns,
                .get_retries, .errors
            ] | @csv'
            row="$(jq -r "$jq_expr" "$stats_path")"

            printf "%d,%s,%s,%s,%s,%s,%s,%s,%s\n" \
                "$records" "$dist" "$mix" "$wr" "${threads_used:-?}" \
                "$row" \
                "${probe_obs:-0}" "${probe_viol:-0}" \
                | sed 's/"//g' >> "$SUMMARY_CSV"
        done
    done
done

# Pretty table: align CSV columns for humans.
if command -v column >/dev/null 2>&1; then
    column -t -s, "$SUMMARY_CSV" > "$SUMMARY_TXT"
else
    cp "$SUMMARY_CSV" "$SUMMARY_TXT"
fi

echo
echo "sweep: done"
echo "sweep: results in $OUT_DIR"
echo
cat "$SUMMARY_TXT"
