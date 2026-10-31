"""Thread-scaling sweep for the Fig-9-style baseline plot.

Generates one cell per (fs × total_threads × mix × distribution), populates
once per (fs × num_nodes) group, and writes a combined ``summary.csv``
with an added ``fs`` column and a ``total_threads`` column.

The node-count phase rule (for cluster size N, per-node cap T):

    x in [1, T]      → 1 node,  [x]
    x in (T,  2T]    → 2 nodes, base ⌈(x - T)/2⌉ per node + round-robin extras
    x in (2T, 3T]    → 3 nodes, base ⌈(x-2T)/3⌉ per node + round-robin extras
    x in (3T, 4T]    → 4 nodes, base ⌈(x-3T)/4⌉ per node + round-robin extras

Extras are distributed left → right → left → ... matching the user's
"alternate sides" intuition. Single-FS sweeps (ext4, tmpfs) cap x at T
since they're node-local.
"""

from __future__ import annotations

import logging
import time
from dataclasses import dataclass, replace
from pathlib import Path

from ckv import remote
from ckv.binary import populate_argv, invoke_local
from ckv.config import Distribution, Host, Mix, Probe, RunConfig, ycsb_mixes
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
from ckv.sweep import clear_data_dir

_LOG = logging.getLogger(__name__)


@dataclass(frozen=True, slots=True)
class FsSpec:
    """One filesystem variant (ext4 / tmpfs / nfs).

    ``max_nodes`` caps cluster width: ext4/tmpfs can only see their own
    node's disk/RAM, so they're capped at 1. NFS sees ``len(hosts)``.
    """

    label: str
    data_dir: str
    max_nodes: int


def build_thread_assignment(
    total_threads: int, cores_per_node: int, max_nodes: int,
) -> tuple[int, ...]:
    """Split ``total_threads`` across [1, max_nodes] nodes per the phase rule.

    Returns a tuple of length ``max_nodes`` with per-node thread counts;
    trailing zeros indicate idle nodes. Raises ``ValueError`` if total is
    out of range (≤0, or > ``cores_per_node * max_nodes``).

    Examples with T=16, max_nodes=4::

        x=1   → (1, 0, 0, 0)
        x=16  → (16, 0, 0, 0)
        x=20  → (10, 10, 0, 0)
        x=21  → (11, 10, 0, 0)
        x=33  → (11, 11, 11, 0)
        x=34  → (12, 11, 11, 0)
        x=64  → (16, 16, 16, 16)
    """
    if total_threads <= 0:
        raise ValueError(f"total_threads must be positive, got {total_threads}")
    if total_threads > cores_per_node * max_nodes:
        raise ValueError(
            f"total_threads={total_threads} exceeds capacity "
            f"{cores_per_node}×{max_nodes}"
        )

    # Pick the smallest N such that total_threads ≤ N·T.
    n_active = (total_threads + cores_per_node - 1) // cores_per_node
    if n_active < 1:
        n_active = 1
    if n_active > max_nodes:
        raise ValueError(f"total_threads={total_threads} cannot fit max_nodes={max_nodes}")

    base = total_threads // n_active
    extras = total_threads - base * n_active

    # Distribute extras "left → right → left …" = round-robin starting at 0.
    per_node = [base] * n_active
    for i in range(extras):
        per_node[i % n_active] += 1

    # Pad with zeros out to max_nodes for a uniform return shape.
    per_node = per_node + [0] * (max_nodes - n_active)
    return tuple(per_node)


def scaling_cells(
    *,
    thread_counts: tuple[int, ...],
    mixes: tuple[Mix, ...],
    distributions: tuple[Distribution, ...],
    records: int,
    data_dir: str,
    fs_label: str,
    warmup_s: int,
    duration_s: int,
    cores_per_node: int,
    max_nodes: int,
    seed: int = 1,
    zipf_theta: float = 0.99,
    probe: Probe = Probe.MONOTONIC,
) -> tuple[RunConfig, ...]:
    """Emit one cell per (thread_count × mix × distribution) for this fs."""
    cells: list[RunConfig] = []
    for total in thread_counts:
        tph = build_thread_assignment(total, cores_per_node, max_nodes)
        for dist in distributions:
            for mix in mixes:
                cells.append(RunConfig(
                    records=records,
                    threads=None,
                    duration_s=duration_s,
                    warmup_s=warmup_s,
                    mix=mix,
                    distribution=dist,
                    zipf_theta=zipf_theta,
                    probe=probe,
                    seed=seed,
                    data_dir=data_dir,
                    pin=False,
                    threads_per_host=tph,
                    label=fs_label,
                ))
    return tuple(cells)


def _populate_on(
    host: Host, cfg: RunConfig, node_id: int, n_nodes: int, *, binary_path: str,
) -> None:
    """Populate this node's shards — small copy of sweep._populate_on
    that we keep local to avoid a cross-import just for this call."""
    argv = populate_argv(binary_path, cfg, node_id=node_id, n_nodes=n_nodes)
    if host.is_local:
        cp = invoke_local(argv)
        if cp.returncode != 0:
            raise RuntimeError(f"populate failed on {host.name}: {cp.stderr}")
    else:
        r = remote.Remote(host)
        res = r.run(argv, cwd=host.remote_root)
        if not res.ok():
            raise RuntimeError(f"populate failed on {host.name}: {res.stderr}")


@dataclass(frozen=True, slots=True)
class ScalingRow:
    """Summary row with the extra ``fs`` / ``total_threads`` columns."""

    base: Row
    fs: str
    total_threads: int


def _aggregate(cfg: RunConfig, outcome: RunOutcome, fs: str) -> ScalingRow:
    total_ops = 0
    duration_ns = 0
    get_p50 = get_p95 = get_p99 = 0
    put_p50 = put_p95 = put_p99 = 0
    retries = errors = 0
    probe_obs = probe_viol = 0
    threads_used = 0

    for w in outcome.workers:
        if not w.stats_path.exists():
            raise RuntimeError(f"missing stats for {w.host.name} node{w.node_id}")
        stats = read_stats_json(w.stats_path)
        total_ops += int(stats.get("total_ops", 0))
        duration_ns = max(duration_ns, int(stats.get("duration_ns", 0)))
        get_p50 = max(get_p50, int(stats.get("get_p50_ns", 0)))
        get_p95 = max(get_p95, int(stats.get("get_p95_ns", 0)))
        get_p99 = max(get_p99, int(stats.get("get_p99_ns", 0)))
        put_p50 = max(put_p50, int(stats.get("put_p50_ns", 0)))
        put_p95 = max(put_p95, int(stats.get("put_p95_ns", 0)))
        put_p99 = max(put_p99, int(stats.get("put_p99_ns", 0)))
        retries += int(stats.get("get_retries", 0))
        errors  += int(stats.get("errors", 0))
        v, o = parse_probe_log(w.stdout)
        probe_viol += v
        probe_obs  += o
        t = parse_threads_log(w.stdout)
        if t is not None:
            threads_used = max(threads_used, t)

    synth = {
        "duration_ns": duration_ns,
        "total_ops": total_ops,
        "gets": 0, "puts": 0,
        "get_p50_ns": get_p50, "get_p95_ns": get_p95, "get_p99_ns": get_p99,
        "put_p50_ns": put_p50, "put_p95_ns": put_p95, "put_p99_ns": put_p99,
        "get_retries": retries, "errors": errors,
    }
    total_threads = sum(cfg.threads_per_host or ())
    row = row_from_stats(
        cfg, synth, threads_used=threads_used or total_threads,
        probe_observations=probe_obs, probe_violations=probe_viol,
    )
    return ScalingRow(base=row, fs=fs, total_threads=total_threads)


def run_scaling_experiment(
    *,
    hosts: tuple[Host, ...],
    cores_per_node: int,
    thread_counts: tuple[int, ...],
    records: int,
    fs_specs: tuple[FsSpec, ...],
    warmup_s: int,
    duration_s: int,
    mixes: tuple[Mix, ...] = (),
    distributions: tuple[Distribution, ...] = (Distribution.UNIFORM, Distribution.ZIPF),
    results_root: Path = Path("results"),
    coordinator_host: str | None = None,
    coordinator_ssh_target: str | None = None,
    coordinator_port: int = 47811,
    binary_path: str = "./callosal-kv",
    harness_path: str = "harness",
    seed: int = 1,
) -> Path:
    """Drive the full scaling sweep; return path to summary.csv."""
    if not mixes:
        mixes = ycsb_mixes()

    run_id = time.strftime("%Y%m%d-%H%M%S", time.gmtime())
    out_root = results_root / f"scaling-{run_id}"
    out_root.mkdir(parents=True, exist_ok=True)
    coord_host = coordinator_host or hosts[0].ssh_target
    coord_ssh = coordinator_ssh_target or hosts[0].ssh_target
    _LOG.info("scaling sweep → %s (hosts=%d, cores/node=%d, fs=%d, coord=%s)",
              out_root, len(hosts), cores_per_node, len(fs_specs), coord_host)

    rows: list[ScalingRow] = []
    total_cells = 0
    for spec in fs_specs:
        allowed = tuple(t for t in thread_counts if t <= cores_per_node * spec.max_nodes)
        total_cells += len(scaling_cells(
            thread_counts=allowed,
            mixes=mixes,
            distributions=distributions,
            records=records,
            data_dir=spec.data_dir,
            fs_label=spec.label,
            warmup_s=warmup_s,
            duration_s=duration_s,
            cores_per_node=cores_per_node,
            max_nodes=spec.max_nodes,
            seed=seed,
        ))
    completed_cells = 0
    for spec in fs_specs:
        _LOG.info("=== fs=%s data_dir=%s max_nodes=%d ===",
                  spec.label, spec.data_dir, spec.max_nodes)
        # Only these thread counts fit within this fs's max_nodes.
        allowed = tuple(t for t in thread_counts
                        if t <= cores_per_node * spec.max_nodes)
        cells = scaling_cells(
            thread_counts=allowed, mixes=mixes, distributions=distributions,
            records=records, data_dir=spec.data_dir, fs_label=spec.label,
            warmup_s=warmup_s, duration_s=duration_s,
            cores_per_node=cores_per_node, max_nodes=spec.max_nodes,
            seed=seed,
        )

        # Group by active-node count so populate runs once per (fs, N).
        by_n: dict[int, list[RunConfig]] = {}
        for c in cells:
            n_active = sum(1 for t in (c.threads_per_host or ()) if t > 0)
            by_n.setdefault(n_active, []).append(c)

        for n_active in sorted(by_n):
            group = by_n[n_active]
            active_hosts = hosts[:n_active]
            _LOG.info("populate fs=%s n_active=%d (%d cells)",
                      spec.label, n_active, len(group))
            # NFS is shared — one wipe + one populate across all nodes.
            # ext4/tmpfs are node-local, so the wipe runs on each host
            # (there's only 1 in practice for those fs's).
            if spec.label == "nfs":
                clear_data_dir(active_hosts[0], spec.data_dir)
            else:
                for h in active_hosts:
                    clear_data_dir(h, spec.data_dir)
            for node_id, h in enumerate(active_hosts):
                _populate_on(h, group[0], node_id, n_active,
                             binary_path=binary_path)

            for cell in group:
                cell_dir = out_root / cell.tag()
                completed_cells += 1
                _LOG.info("run [%d/%d] %s", completed_cells, total_cells, cell.tag())
                # hosts=hosts (full length) because threads_per_host aligns
                # to the full hosts list; run_single prunes inactive entries.
                # But we only have threads_per_host of length max_nodes,
                # not len(hosts). Re-project to hosts-length by padding
                # with zeros for any hosts beyond max_nodes.
                tph_full = tuple(cell.threads_per_host or ()) + \
                           (0,) * (len(hosts) - len(cell.threads_per_host or ()))
                cell_runtime = replace(cell, threads_per_host=tph_full)
                outcome = run_single(
                    cell_runtime, hosts,
                    coordinator_host=coord_host,
                    coordinator_ssh_target=coord_ssh,
                    coordinator_port=coordinator_port,
                    run_dir=cell_dir,
                    binary_path=binary_path,
                    harness_path=harness_path,
                )
                if not outcome.ok():
                    _LOG.warning("cell %s failed; continuing", cell.tag())
                    continue
                rows.append(_aggregate(cell_runtime, outcome, spec.label))

    # Fold into a DataFrame with fs + total_threads added.
    base_df = rows_to_dataframe([r.base for r in rows])
    if len(rows) > 0:
        base_df["fs"] = [r.fs for r in rows]
        base_df["total_threads"] = [r.total_threads for r in rows]
    summary_path = out_root / "summary.csv"
    write_summary_csv(base_df, summary_path)
    _LOG.info("scaling sweep done: %s", summary_path)
    return summary_path


# --- CLI -------------------------------------------------------------------


def add_cli(subparsers) -> None:  # type: ignore[no-untyped-def]
    import argparse  # local, keep module import cheap
    p: argparse.ArgumentParser = subparsers.add_parser(
        "scaling", help="run the thread-scaling baseline sweep (ext4/tmpfs/nfs)")
    p.add_argument("--hosts", required=True,
                   help="comma-separated ssh targets, node0 first")
    p.add_argument("--host-root", default="callosal-kv",
                   help="remote working dir on each host")
    p.add_argument("--cores-per-node", type=int, default=16)
    p.add_argument("--records", type=int, default=10_000_000)
    p.add_argument("--warmup", type=int, default=10)
    p.add_argument("--duration", type=int, default=30)
    p.add_argument("--thread-counts", default="1,2,4,8,12,16,20,24,28,32,36,40,44,48,52,56,60,64")
    p.add_argument("--ext4-dir", default="/var/lib/callosal-kv-ext4/kvdata")
    p.add_argument("--tmpfs-dir", default="/mnt/tmpfs-kvdata")
    p.add_argument("--nfs-dir", default="/mnt/kvshared")
    p.add_argument("--coordinator-host", default=None)
    p.add_argument("--coordinator-ssh-target", default=None)
    p.add_argument("--coordinator-port", type=int, default=47811)
    p.add_argument("--binary-path", default="./callosal-kv")
    p.add_argument("--harness-path", default="harness")
    p.add_argument("--results-root", type=Path, default=Path("results"))
    p.add_argument("--fs", default="ext4,tmpfs,nfs",
                   help="comma-separated subset of {ext4,tmpfs,nfs} to run")
    p.set_defaults(func=_cmd_scaling)


def _cmd_scaling(args) -> int:  # type: ignore[no-untyped-def]
    logging.basicConfig(level=logging.INFO,
                        format="%(asctime)s %(levelname)s %(message)s")
    targets = [t.strip() for t in args.hosts.split(",") if t.strip()]
    hosts = tuple(Host(name=t, ssh_target=t, remote_root=args.host_root)
                  for t in targets)
    thread_counts = tuple(int(t) for t in args.thread_counts.split(",") if t.strip())

    fs_available = {
        "ext4": FsSpec("ext4", args.ext4_dir, max_nodes=1),
        "tmpfs": FsSpec("tmpfs", args.tmpfs_dir, max_nodes=1),
        "nfs": FsSpec("nfs", args.nfs_dir, max_nodes=len(hosts)),
    }
    wanted = [s.strip() for s in args.fs.split(",") if s.strip()]
    fs_specs = tuple(fs_available[k] for k in wanted)

    path = run_scaling_experiment(
        hosts=hosts,
        cores_per_node=args.cores_per_node,
        thread_counts=thread_counts,
        records=args.records,
        fs_specs=fs_specs,
        warmup_s=args.warmup,
        duration_s=args.duration,
        results_root=args.results_root,
        coordinator_host=args.coordinator_host,
        coordinator_ssh_target=args.coordinator_ssh_target,
        coordinator_port=args.coordinator_port,
        binary_path=args.binary_path,
        harness_path=args.harness_path,
    )
    print(path)
    return 0
