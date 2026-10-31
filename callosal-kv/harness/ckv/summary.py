"""JSON stats → pandas DataFrame → CSV.

Pure functions over dicts; no I/O except the two marked `write_` / `read_`
helpers. Unit tests live in ``tests/test_summary.py``.

The canonical per-run JSON format is produced by ``src/stats.c::stats_dump_json``.
A subset of fields is required; extras are preserved but not type-checked.
"""

from __future__ import annotations

import json
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any, cast

import pandas as pd

from ckv.config import RunConfig

# Fields required to be present in the JSON. If one is missing we treat the
# run as failed rather than silently producing a garbage row.
REQUIRED_FIELDS = (
    "duration_ns", "total_ops", "gets", "puts",
    "get_p50_ns", "get_p95_ns", "get_p99_ns",
    "put_p50_ns", "put_p95_ns", "put_p99_ns",
)


@dataclass(frozen=True, slots=True)
class Row:
    """One row in the summary table: sweep cell + measured results.

    Flat on purpose — pandas likes flat rows, plotnine likes tidy data.
    """

    # cell identifiers
    records: int
    threads: int
    duration_s: int
    distribution: str
    mix: str
    write_ratio: float
    # results
    total_ops: int
    mops: float
    get_p50_ns: int
    get_p95_ns: int
    get_p99_ns: int
    put_p50_ns: int
    put_p95_ns: int
    put_p99_ns: int
    get_retries: int
    errors: int
    probe_observations: int
    probe_violations: int


def row_from_stats(
    cfg: RunConfig,
    stats: dict[str, Any],
    *,
    threads_used: int,
    probe_observations: int = 0,
    probe_violations: int = 0,
) -> Row:
    """Build a :class:`Row` from a parsed stats JSON dict and its cell config.

    ``probe_*`` counts are passed in separately today because the C binary
    does not yet include them in the JSON. They are typically extracted
    from the run's stdout by the caller.
    """
    missing = [k for k in REQUIRED_FIELDS if k not in stats]
    if missing:
        raise ValueError(f"stats JSON missing fields: {missing}")

    duration_ns = int(stats["duration_ns"])
    total_ops = int(stats["total_ops"])
    secs = duration_ns / 1e9
    mops = total_ops / secs / 1e6 if secs > 0 else 0.0

    return Row(
        records=cfg.records,
        threads=threads_used,
        duration_s=cfg.duration_s,
        distribution=cfg.distribution.value,
        mix=cfg.mix.name,
        write_ratio=cfg.mix.write_ratio,
        total_ops=total_ops,
        mops=mops,
        get_p50_ns=int(stats["get_p50_ns"]),
        get_p95_ns=int(stats["get_p95_ns"]),
        get_p99_ns=int(stats["get_p99_ns"]),
        put_p50_ns=int(stats["put_p50_ns"]),
        put_p95_ns=int(stats["put_p95_ns"]),
        put_p99_ns=int(stats["put_p99_ns"]),
        get_retries=int(stats.get("get_retries", 0)),
        errors=int(stats.get("errors", 0)),
        probe_observations=probe_observations,
        probe_violations=probe_violations,
    )


def parse_probe_log(text: str) -> tuple[int, int]:
    """Extract (violations, observations) from bench stdout.

    The binary prints exactly one ``bench: probe monotonic: V violations / O
    observations`` line when the probe is enabled; returns (0, 0) if absent.
    """
    for line in text.splitlines():
        if "probe monotonic:" in line:
            # format: "bench: probe monotonic: V violations / O observations"
            parts = line.split()
            try:
                v = int(parts[3])
                o = int(parts[6])
            except (IndexError, ValueError):
                return (0, 0)
            return (v, o)
    return (0, 0)


def parse_threads_log(text: str) -> int | None:
    """Extract ``threads=N`` from the first bench node summary line."""
    for line in text.splitlines():
        if line.startswith("bench: node="):
            for tok in line.split():
                if tok.startswith("threads="):
                    try:
                        return int(tok.removeprefix("threads="))
                    except ValueError:
                        return None
    return None


def rows_to_dataframe(rows: list[Row]) -> pd.DataFrame:
    return pd.DataFrame([asdict(r) for r in rows])


def write_summary_csv(df: pd.DataFrame, path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    df.to_csv(path, index=False)


def read_summary_csv(path: Path) -> pd.DataFrame:
    return pd.read_csv(path)


def read_stats_json(path: Path) -> dict[str, Any]:
    return cast(dict[str, Any], json.loads(path.read_text()))
