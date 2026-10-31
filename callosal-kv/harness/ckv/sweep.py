"""Iterate over a :class:`SweepConfig`, driving populate once per records-count
then running each cell and writing a combined ``summary.csv``.

Populate is orchestrated here — not in ``ckv.run`` — because it is a
per-records-count fixture that amortizes across many measurement cells. A
fresh populate is required when records-count changes, but not when
distribution/mix/write-ratio change.
"""

from __future__ import annotations

import argparse
import logging
import time
from dataclasses import replace
from pathlib import Path
from typing import Any

from ckv import binary, remote
from ckv.config import Host, RunConfig, SweepConfig, default_m3_sweep
from ckv.run import RunOutcome, run_single
from ckv.summary import (
    Row,
    parse_probe_log,
    parse_threads_log,
    read_stats_json,
    row_from_stats,
    rows_to_dataframe,
    write_summary_csv,
)

_LOG = logging.getLogger(__name__)


def _populate_on(host: Host, cfg: RunConfig, node_id: int, n_nodes: int,
                 *, binary_path: str) -> None:
    """Populate this node's shards. Raises on failure."""
    argv = binary.populate_argv(binary_path, cfg, node_id=node_id, n_nodes=n_nodes)
    if host.is_local:
        cp = binary.invoke_local(argv)
        if cp.returncode != 0:
            raise RuntimeError(f"populate failed on {host.name}: {cp.stderr}")
    else:
        r = remote.Remote(host)
        res = r.run(argv, cwd=host.remote_root)
        if not res.ok():
            raise RuntimeError(f"populate failed on {host.name}: {res.stderr}")


def clear_data_dir(host: Host, path: str) -> None:
    """Remove and recreate ``path`` on ``host``. Idempotent; raises on failure.

    The scaling sweep re-populates the NFS mount for each (num_nodes)
    grouping, and stale shards from a previous populate would still have
    their old owner encoded in the filename. Wipe first so we never read
    them back from a freshly-routed worker set.
    """
    # Clear *contents*, not the directory itself — the directory may live
    # under a root-owned parent (e.g. /var/lib) and we only own the leaf.
    argv = ["sh", "-c",
            f"mkdir -p {path!r} && find {path!r} -mindepth 1 -delete"]
    if host.is_local:
        cp = binary.invoke_local(argv)
        if cp.returncode != 0:
            raise RuntimeError(f"clear_data_dir failed on {host.name}: {cp.stderr}")
    else:
        r = remote.Remote(host)
        res = r.run(argv, cwd=host.remote_root)
        if not res.ok():
            raise RuntimeError(f"clear_data_dir failed on {host.name}: {res.stderr}")


def _records_cell_key(cfg: RunConfig) -> int:
    """Sweep cells are grouped by records-count for populate amortization."""
    return cfg.records


def _aggregate(cfg: RunConfig, outcome: RunOutcome) -> Row:
    """Combine per-node JSONs into a single Row. For now we sum total_ops
    across nodes and take the max of percentiles (a conservative summary);
    the per-node JSONs remain on disk for finer analysis."""
    total_ops = 0
    duration_ns = 0
    get_p50 = get_p95 = get_p99 = 0
    put_p50 = put_p95 = put_p99 = 0
    retries = errors = 0
    probe_obs = probe_viol = 0
    threads_used = 0

    def _i(d: dict[str, Any], key: str, default: int = 0) -> int:
        """Pull ``key`` out of a stats dict as int. JSON numbers come back
        as int/float at runtime — ``int()`` handles both."""
        return int(d.get(key, default))

    for w in outcome.workers:
        if not w.stats_path.exists():
            raise RuntimeError(f"missing stats for {w.host.name} node{w.node_id}")
        stats = read_stats_json(w.stats_path)
        total_ops += _i(stats, "total_ops")
        duration_ns = max(duration_ns, _i(stats, "duration_ns"))
        get_p50 = max(get_p50, _i(stats, "get_p50_ns"))
        get_p95 = max(get_p95, _i(stats, "get_p95_ns"))
        get_p99 = max(get_p99, _i(stats, "get_p99_ns"))
        put_p50 = max(put_p50, _i(stats, "put_p50_ns"))
        put_p95 = max(put_p95, _i(stats, "put_p95_ns"))
        put_p99 = max(put_p99, _i(stats, "put_p99_ns"))
        retries += _i(stats, "get_retries")
        errors  += _i(stats, "errors")
        v, o = parse_probe_log(w.stdout)
        probe_viol += v
        probe_obs  += o
        t = parse_threads_log(w.stdout)
        if t is not None:
            threads_used = max(threads_used, t)

    # Build a synthetic stats dict for row_from_stats. Using its validation
    # keeps the field set in sync between the two paths.
    synth: dict[str, Any] = {
        "duration_ns": duration_ns,
        "total_ops": total_ops,
        "gets": 0, "puts": 0,
        "get_p50_ns": get_p50, "get_p95_ns": get_p95, "get_p99_ns": get_p99,
        "put_p50_ns": put_p50, "put_p95_ns": put_p95, "put_p99_ns": put_p99,
        "get_retries": retries, "errors": errors,
    }
    return row_from_stats(
        cfg, synth, threads_used=threads_used or (cfg.threads or 0),
        probe_observations=probe_obs, probe_violations=probe_viol,
    )


def _resolve_coordinator(sweep: SweepConfig) -> tuple[str, str]:
    """Return (coordinator_host, coordinator_ssh_target) with sensible defaults.

    ``coordinator_host`` is the name workers use in ``--coordinator H:P``;
    it must resolve on the cluster's experimental network. If unset we fall
    back to ``hosts[0].ssh_target`` — fine for single-node smoke, and the
    user typically overrides to an internal name (``node0``) for real runs.
    """
    first = sweep.hosts[0]
    host = sweep.coordinator_host if sweep.coordinator_host is not None else first.ssh_target
    ssh_target = (sweep.coordinator_ssh_target
                  if sweep.coordinator_ssh_target is not None
                  else first.ssh_target)
    return host, ssh_target


def run_sweep(sweep: SweepConfig) -> Path:
    """Run the full sweep, returning the path to the written ``summary.csv``."""
    run_id = time.strftime("%Y%m%d-%H%M%S", time.gmtime())
    out_root = sweep.results_root / f"{sweep.name}-{run_id}"
    out_root.mkdir(parents=True, exist_ok=True)
    coord_host, coord_ssh = _resolve_coordinator(sweep)
    _LOG.info("sweep %s → %s (hosts=%d, cells=%d, coord=%s via %s)",
              sweep.name, out_root, len(sweep.hosts), len(sweep.cells),
              coord_host, coord_ssh)

    # Group cells by records-count so we populate once per group.
    cells_by_records: dict[int, list[RunConfig]] = {}
    for cell in sweep.cells:
        cells_by_records.setdefault(_records_cell_key(cell), []).append(cell)

    rows: list[Row] = []
    total_cells = len(sweep.cells)
    completed_cells = 0
    for records, group in cells_by_records.items():
        _LOG.info("populate: records=%d across %d host(s)", records, len(sweep.hosts))
        populate_cfg = group[0]  # any cell with this records-count will do
        for node_id, host in enumerate(sweep.hosts):
            _populate_on(host, populate_cfg, node_id, len(sweep.hosts),
                         binary_path=sweep.binary_path)

        for cell in group:
            cell_dir = out_root / cell.tag()
            completed_cells += 1
            _LOG.info("run [%d/%d] cell=%s", completed_cells, total_cells, cell.tag())
            outcome = run_single(
                cell, sweep.hosts,
                coordinator_host=coord_host,
                coordinator_ssh_target=coord_ssh,
                coordinator_port=sweep.coordinator_port,
                run_dir=cell_dir,
                binary_path=sweep.binary_path,
                harness_path=sweep.harness_path,
            )
            if not outcome.ok():
                _LOG.warning("cell %s failed; continuing", cell.tag())
                continue
            rows.append(_aggregate(cell, outcome))

    df = rows_to_dataframe(rows)
    summary_path = out_root / "summary.csv"
    write_summary_csv(df, summary_path)
    _LOG.info("sweep done: %s", summary_path)
    return summary_path


# --- CLI -------------------------------------------------------------------


def add_cli(subparsers: argparse._SubParsersAction[argparse.ArgumentParser]) -> None:
    p = subparsers.add_parser("sweep", help="run the default benchmark sweep")
    p.add_argument("--name", default="m3")
    p.add_argument("--host-target", default="localhost",
                   help="ssh target for the single worker (default: localhost)")
    p.add_argument("--host-root", default=".",
                   help="remote working directory (ignored if host is local)")
    p.add_argument("--records", nargs="+", type=int, default=[100_000, 10_000_000])
    p.add_argument("--duration", type=int, default=10)
    p.add_argument("--threads", type=int, default=None)
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--results-root", type=Path, default=Path("results"))
    p.add_argument("--coordinator-port", type=int, default=47811)
    p.add_argument("--coordinator-host", default=None,
                   help="internal DNS name workers use for --coordinator (default: hosts[0])")
    p.add_argument("--coordinator-ssh-target", default=None,
                   help="ssh target for the coordinator node (default: hosts[0])")
    p.add_argument("--binary-path", default="./callosal-kv",
                   help="relative to each host's remote_root")
    p.add_argument("--harness-path", default="harness",
                   help="relative to each host's remote_root")
    p.set_defaults(func=_cmd_sweep)


def _cmd_sweep(args: argparse.Namespace) -> int:
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    host = Host(name=args.host_target, ssh_target=args.host_target, remote_root=args.host_root)
    sweep = default_m3_sweep(
        hosts=(host,),
        records_list=tuple(args.records),
        duration_s=args.duration,
        threads=args.threads,
        seed=args.seed,
    )
    sweep = replace(sweep,
                    results_root=args.results_root,
                    coordinator_port=args.coordinator_port,
                    coordinator_host=args.coordinator_host,
                    coordinator_ssh_target=args.coordinator_ssh_target,
                    binary_path=args.binary_path,
                    harness_path=args.harness_path)
    path = run_sweep(sweep)
    print(path)
    return 0
