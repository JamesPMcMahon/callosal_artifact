"""Orchestrate a single measurement run across one or more workers.

A "run" here is one ``RunConfig`` cell executed against a cluster of
``Host`` workers that have already been populated. The steps are:

1. ssh to ``coordinator_ssh_target`` and launch ``python -m ckv coordinator``
   in the background, bound to ``coordinator_port``. The coordinator
   expects one READY per worker.
2. Launch each worker with ``--coordinator COORDINATOR_HOST:PORT``, where
   ``COORDINATOR_HOST`` is an internal DNS name resolvable from the
   experimental network. The worker opens its shards, sends READY, then
   blocks on the barrier.
3. Once all READYs arrive the coordinator broadcasts GO with a start_ns.
   Workers run for ``cfg.duration_s``.
4. The driver waits for each worker to exit, scp's per-node stats JSON
   back, and reaps the coordinator process.

Populate is **not** orchestrated here — it's a per-records-count fixture
handled by ``ckv.sweep``.
"""

from __future__ import annotations

import logging
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path

from ckv import binary, remote
from ckv.config import Host, RunConfig

_LOG = logging.getLogger(__name__)


@dataclass(frozen=True, slots=True)
class WorkerOutcome:
    host: Host
    node_id: int
    returncode: int
    stats_path: Path          # local path to the fetched JSON (may not exist on failure)
    stdout: str
    stderr: str


@dataclass(frozen=True, slots=True)
class RunOutcome:
    cfg: RunConfig
    workers: tuple[WorkerOutcome, ...]
    coordinator_returncode: int

    def ok(self) -> bool:
        return (
            self.coordinator_returncode == 0
            and len(self.workers) > 0
            and all(w.returncode == 0 for w in self.workers)
        )


def _coordinator_argv(
    *, port: int, workers: int,
    timeout_s: float,
) -> list[str]:
    """argv that launches the Python coordinator on a cluster node.

    The coordinator is stdlib-only, so launching it directly via
    ``python3 -m ckv.coordinator`` is both faster and more robust than
    booting the full harness environment via ``uv run`` on each cell.

    The ``workers`` argument is the integer count (the N→0..N-1 shorthand
    the CLI already handles).
    """
    return [
        "python3", "-m", "ckv.coordinator",
        "--port", str(port),
        "--workers", str(workers),
        "--timeout", str(int(timeout_s)),
    ]


def run_single(
    cfg: RunConfig,
    hosts: tuple[Host, ...],
    *,
    coordinator_host: str,
    coordinator_ssh_target: str,
    coordinator_port: int,
    run_dir: Path,
    binary_path: str = "./callosal-kv",
    harness_path: str = "harness",
    barrier_timeout_s: float = 30.0,
) -> RunOutcome:
    """Execute one cell across ``hosts``. Fetches per-node JSON into ``run_dir``.

    If ``cfg.threads_per_host`` is set, hosts with a 0 entry are skipped
    and the remaining hosts are numbered densely (0..n_active-1) for the
    C side's shard routing — the populate step must have used the same
    ``n_active`` or keys will route to the wrong owner.
    """
    run_dir.mkdir(parents=True, exist_ok=True)
    tph = cfg.threads_per_host
    if tph is not None:
        if len(tph) != len(hosts):
            raise ValueError(
                f"threads_per_host length {len(tph)} != hosts length {len(hosts)}"
            )
        active: list[tuple[Host, int]] = [
            (h, t) for h, t in zip(hosts, tph) if t > 0
        ]
    else:
        active = [(h, cfg.threads or 0) for h in hosts]
    if not active:
        raise ValueError("no active hosts for this cell")
    n_nodes = len(active)

    coord_proc: subprocess.Popen[str] | None = None
    coord_spec: str | None = None
    if n_nodes > 1:
        # --- Start coordinator on a cluster node -------------------------
        coord_host_obj = Host(
            name=coordinator_ssh_target,
            ssh_target=coordinator_ssh_target,
            remote_root=hosts[0].remote_root,  # assume shared layout
        )
        coord_argv = _coordinator_argv(
            port=coordinator_port, workers=n_nodes,
            timeout_s=barrier_timeout_s,
        )
        coord_cwd = str(Path(coord_host_obj.remote_root) / harness_path)
        _LOG.info("starting coordinator on %s", coordinator_ssh_target)
        if coord_host_obj.is_local:
            coord_proc = subprocess.Popen(
                coord_argv, stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                cwd=coord_cwd,
            )
        else:
            coord_proc = remote.Remote(coord_host_obj).run_background(
                coord_argv, cwd=coord_cwd,
            )
        # The worker sends a single UDP READY datagram. Give the remote
        # coordinator a brief window to bind its socket before any worker
        # starts or the first READY can be lost.
        time.sleep(1.0)
        coord_spec = f"{coordinator_host}:{coordinator_port}"

    # --- Launch workers ---------------------------------------------------
    remote_out_relative = {i: f"results/.run/{cfg.tag()}-node{i}.json"
                           for i in range(n_nodes)}
    procs: list[tuple[Host, int, subprocess.Popen[str]]] = []
    for node_id, (host, threads) in enumerate(active):
        argv = binary.run_argv(
            binary_path, cfg,
            node_id=node_id, n_nodes=n_nodes,
            out_path=remote_out_relative[node_id],
            coordinator=coord_spec,
            threads_override=threads if threads > 0 else None,
        )
        if host.is_local:
            # Mirror the remote branch: the C binary opens --out relative
            # to its cwd (which is our cwd), so ensure results/.run/ is
            # there before launching or stats_dump_json fails with ENOENT.
            Path("results/.run").mkdir(parents=True, exist_ok=True)
            proc = subprocess.Popen(
                argv, stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            )
        else:
            r = remote.Remote(host)
            r.run(["mkdir", "-p", "results/.run"], cwd=host.remote_root)
            proc = r.run_background(argv, cwd=host.remote_root)
        procs.append((host, node_id, proc))

    # --- Collect worker output -------------------------------------------
    outcomes: list[WorkerOutcome] = []
    worker_timeout = (cfg.duration_s + cfg.warmup_s) * 3 + 60
    for host, node_id, proc in procs:
        try:
            stdout, stderr = proc.communicate(timeout=worker_timeout)
        except subprocess.TimeoutExpired:
            proc.kill()
            stdout, stderr = proc.communicate()
        local_stats = run_dir / f"node{node_id}.json"
        if proc.returncode == 0:
            if host.is_local:
                src = Path(remote_out_relative[node_id])
                if src.exists():
                    local_stats.write_bytes(src.read_bytes())
            else:
                remote.Remote(host).fetch(remote_out_relative[node_id], local_stats)
        outcomes.append(WorkerOutcome(
            host=host, node_id=node_id,
            returncode=proc.returncode,
            stats_path=local_stats,
            stdout=stdout, stderr=stderr,
        ))
        (run_dir / f"node{node_id}.log").write_text(stdout + stderr)

    # --- Reap coordinator -------------------------------------------------
    coord_returncode = 0
    if coord_proc is not None:
        try:
            coord_stdout, coord_stderr = coord_proc.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            coord_proc.kill()
            coord_stdout, coord_stderr = coord_proc.communicate()
        (run_dir / "coordinator.log").write_text(coord_stdout + coord_stderr)
        coord_returncode = coord_proc.returncode
    else:
        (run_dir / "coordinator.log").write_text("")

    return RunOutcome(
        cfg=cfg,
        workers=tuple(outcomes),
        coordinator_returncode=coord_returncode,
    )
