"""Tests for the pure parts of ckv.summary."""

from __future__ import annotations

import pytest

from ckv.config import Distribution, Mix, Probe, RunConfig
from ckv.summary import (
    parse_probe_log,
    parse_threads_log,
    row_from_stats,
    rows_to_dataframe,
)


def _cfg() -> RunConfig:
    return RunConfig(
        records=1000, threads=None, duration_s=10,
        mix=Mix.ycsb_a(), distribution=Distribution.UNIFORM, probe=Probe.MONOTONIC,
    )


def _stats(**overrides: object) -> dict[str, object]:
    base: dict[str, object] = {
        "duration_ns": 10_000_000_000,
        "total_ops": 5_000_000,
        "gets": 2_500_000, "puts": 2_500_000,
        "get_p50_ns": 1000, "get_p95_ns": 4000, "get_p99_ns": 9000,
        "put_p50_ns": 2000, "put_p95_ns": 8000, "put_p99_ns": 18000,
        "get_retries": 0, "errors": 0,
    }
    base.update(overrides)
    return base


def test_row_from_stats_happy_path() -> None:
    row = row_from_stats(_cfg(), _stats(), threads_used=8,
                         probe_observations=1_000_000, probe_violations=0)
    assert row.mops == pytest.approx(0.5)
    assert row.mix == "A"
    assert row.distribution == "uniform"
    assert row.probe_violations == 0


def test_row_from_stats_rejects_missing_fields() -> None:
    incomplete = _stats()
    del incomplete["put_p99_ns"]
    with pytest.raises(ValueError, match="missing"):
        row_from_stats(_cfg(), incomplete, threads_used=8)


def test_parse_probe_log_extracts_counts() -> None:
    text = (
        "bench: node=0 threads=8 duration=10.00s ops=5000000 (0.50 Mops/s)\n"
        "bench: probe monotonic: 0 violations / 2500000 observations\n"
    )
    assert parse_probe_log(text) == (0, 2_500_000)


def test_parse_probe_log_missing_returns_zero() -> None:
    assert parse_probe_log("no probe line here") == (0, 0)


def test_parse_threads_log_extracts_thread_count() -> None:
    text = "bench: node=0 threads=12 duration=10.00s ops=1 (0.00 Mops/s)"
    assert parse_threads_log(text) == 12


def test_rows_to_dataframe_has_expected_columns() -> None:
    row = row_from_stats(_cfg(), _stats(), threads_used=8)
    df = rows_to_dataframe([row])
    assert list(df.columns) == [
        "records", "threads", "duration_s", "distribution", "mix", "write_ratio",
        "total_ops", "mops",
        "get_p50_ns", "get_p95_ns", "get_p99_ns",
        "put_p50_ns", "put_p95_ns", "put_p99_ns",
        "get_retries", "errors",
        "probe_observations", "probe_violations",
    ]
    assert df.iloc[0]["mops"] == pytest.approx(0.5)
