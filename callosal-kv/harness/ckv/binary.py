"""Typed wrapper around the ``./callosal-kv`` C binary.

The C binary has two modes — ``--populate`` and ``--run`` — and emits stats
as JSON (``--out PATH`` for ``--run``). Callers should go through this
module rather than assembling argv strings inline: it centralizes flag
spelling, keeps the subprocess plumbing in one place, and exposes typed
return values.

Nothing here ssh's anywhere. Remote execution is layered on top via
``ckv.remote``.
"""

from __future__ import annotations

import json
import logging
import shlex
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from ckv.config import RunConfig

_LOG = logging.getLogger(__name__)


@dataclass(frozen=True, slots=True)
class PopulateResult:
    returncode: int
    stdout: str
    stderr: str


@dataclass(frozen=True, slots=True)
class RunResult:
    """Parsed JSON + context from a single ``--run`` invocation."""

    returncode: int
    stats: dict[str, Any]   # parsed JSON from --out (may be empty on failure)
    stdout: str
    stderr: str

    def mops_per_sec(self) -> float:
        d_ns = float(self.stats.get("duration_ns", 0))
        ops = float(self.stats.get("total_ops", 0))
        if d_ns <= 0:
            return 0.0
        return ops / (d_ns / 1e9) / 1e6


def populate_argv(binary: str, cfg: RunConfig, *, node_id: int, n_nodes: int) -> list[str]:
    """argv for a populate invocation. Pure — no subprocess."""
    return [
        binary,
        "--populate",
        "--path", cfg.data_dir,
        "--records", str(cfg.records),
        "--node-id", str(node_id),
        "--num-nodes", str(n_nodes),
        "--seed", str(cfg.seed),
        "--load-factor", str(cfg.load_factor_pct),
        "--max-file-pages", str(cfg.max_file_pages),
    ]


def run_argv(
    binary: str,
    cfg: RunConfig,
    *,
    node_id: int,
    n_nodes: int,
    out_path: str,
    coordinator: str | None = None,
    threads_override: int | None = None,
) -> list[str]:
    """argv for a measurement ``--run`` invocation. Pure — no subprocess.

    ``coordinator`` is ``"host:port"`` when the worker should wait on the
    UDP barrier; ``None`` for a standalone run with no barrier.

    ``threads_override`` takes precedence over ``cfg.threads`` when set.
    The scaling sweep uses this to pass a different thread count to each
    host while sharing a single ``RunConfig`` cell.
    """
    argv: list[str] = [
        binary,
        "--run",
        "--path", cfg.data_dir,
        "--records", str(cfg.records),
        "--node-id", str(node_id),
        "--num-nodes", str(n_nodes),
        "--seed", str(cfg.seed),
        "--load-factor", str(cfg.load_factor_pct),
        "--max-file-pages", str(cfg.max_file_pages),
        "--duration", str(cfg.duration_s),
        "--dist", cfg.distribution.value,
        "--zipf-theta", str(cfg.zipf_theta),
        "--write-ratio", str(cfg.mix.write_ratio),
        "--probe", cfg.probe.value,
        "--out", out_path,
    ]
    threads = threads_override if threads_override is not None else cfg.threads
    if threads is not None:
        argv.extend(["--threads", str(threads)])
    if cfg.warmup_s > 0:
        argv.extend(["--warmup", str(cfg.warmup_s)])
    argv.append("--pin" if cfg.pin else "--no-pin")
    if coordinator is not None:
        argv.extend(["--coordinator", coordinator])
    return argv


def invoke_local(
    argv: list[str], *, timeout_s: float | None = None,
) -> subprocess.CompletedProcess[str]:
    """Run a command locally, capturing stdout/stderr."""
    _LOG.info("invoke_local: %s", shlex.join(argv))
    return subprocess.run(
        argv,
        capture_output=True,
        text=True,
        timeout=timeout_s,
        check=False,
    )


def populate_local(binary: str, cfg: RunConfig, *, node_id: int, n_nodes: int) -> PopulateResult:
    cp = invoke_local(populate_argv(binary, cfg, node_id=node_id, n_nodes=n_nodes))
    return PopulateResult(returncode=cp.returncode, stdout=cp.stdout, stderr=cp.stderr)


def run_local(
    binary: str,
    cfg: RunConfig,
    *,
    node_id: int,
    n_nodes: int,
    out_path: str,
    coordinator: str | None = None,
) -> RunResult:
    argv = run_argv(
        binary, cfg,
        node_id=node_id, n_nodes=n_nodes,
        out_path=out_path, coordinator=coordinator,
    )
    # run_local is the standalone-smoke path; worker_timeout accounts for
    # warmup + measurement + a generous slack.
    cp = invoke_local(argv, timeout_s=(cfg.duration_s + cfg.warmup_s) * 3 + 60)
    stats: dict[str, Any] = {}
    if cp.returncode == 0 and Path(out_path).exists():
        stats = json.loads(Path(out_path).read_text())
    return RunResult(returncode=cp.returncode, stats=stats, stdout=cp.stdout, stderr=cp.stderr)
