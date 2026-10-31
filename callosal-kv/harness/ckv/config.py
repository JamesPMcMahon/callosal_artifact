"""Typed configuration objects for experiment runs and sweeps.

These are the harness's source of truth for what to run. Every sweep is an
iteration over a list of ``RunConfig`` cells; every cell becomes one
invocation of ``ckv.run.run_single``.

The dataclasses are intentionally closed (``frozen=True, slots=True``) —
experiment config should be constructed once and then treated as data.
"""

from __future__ import annotations

from dataclasses import dataclass, field, replace
from enum import StrEnum
from pathlib import Path


class Distribution(StrEnum):
    """Key-selection distribution for the op stream."""

    UNIFORM = "uniform"
    ZIPF = "zipf"


class Probe(StrEnum):
    """Correctness probe mode."""

    NONE = "none"
    MONOTONIC = "monotonic"


@dataclass(frozen=True, slots=True)
class Mix:
    """YCSB-style read/write mix, named (``A``/``B``/``C``) and parameterized."""

    name: str
    write_ratio: float

    @staticmethod
    def ycsb_a() -> Mix:
        return Mix("A", 0.50)

    @staticmethod
    def ycsb_b() -> Mix:
        return Mix("B", 0.05)

    @staticmethod
    def ycsb_c() -> Mix:
        return Mix("C", 0.00)


@dataclass(frozen=True, slots=True)
class Host:
    """One worker host.

    ``ssh_target`` is either ``user@hostname`` for remote hosts or the
    literal string ``"localhost"`` for a driver-local worker (no ssh; the
    binary is invoked as a subprocess directly).
    """

    name: str
    ssh_target: str
    remote_root: str  # working directory on the host, e.g. "callosal-kv"

    @property
    def is_local(self) -> bool:
        return self.ssh_target in ("localhost", "127.0.0.1", "")


@dataclass(frozen=True, slots=True)
class RunConfig:
    """One cell of a sweep — enough to parameterize a single measurement run."""

    records: int
    threads: int | None        # None → let the binary pick nproc
    duration_s: int
    mix: Mix
    distribution: Distribution
    zipf_theta: float = 0.99
    load_factor_pct: int = 75
    probe: Probe = Probe.MONOTONIC
    seed: int = 1
    data_dir: str = "/tmp/callosal-kv-data"
    pin: bool = False          # --no-pin by default for harness-driven runs
    max_file_pages: int = 0
    warmup_s: int = 0
    # Per-host thread override. When set, its length must equal len(hosts)
    # on the sweep. A 0 entry means "this host is idle for this cell" —
    # ``run_single`` skips launching on idle hosts and renumbers node-ids
    # densely across active hosts. ``None`` (the default) means "use
    # ``threads`` on every host".
    threads_per_host: tuple[int, ...] | None = None
    # Optional label appended to the cell tag so per-cell output directories
    # don't collide across fs/storage variants. Use e.g. "ext4" / "tmpfs"
    # / "nfs" in the scaling sweep; leave ``None`` for the default sweep.
    label: str | None = None

    def tag(self) -> str:
        """Short filename-safe id for this cell's output files."""
        base = f"{self.records}-{self.distribution.value}-{self.mix.name}"
        if self.threads_per_host is not None:
            total = sum(self.threads_per_host)
            base = f"{base}-t{total}"
        elif self.threads is not None:
            base = f"{base}-t{self.threads}"
        if self.label:
            base = f"{base}-{self.label}"
        return base


@dataclass(frozen=True, slots=True)
class SweepConfig:
    """A named sweep: a cartesian-ish product of cells over a set of hosts."""

    name: str
    hosts: tuple[Host, ...]
    cells: tuple[RunConfig, ...]
    binary_path: str = "./callosal-kv"
    # The UDP barrier runs *inside* the cluster, not on the driver laptop —
    # the driver can't necessarily reach every worker and every worker can't
    # necessarily reach the driver. ``coordinator_host`` names the cluster
    # node that the driver ssh's into to launch ``python -m ckv coordinator``;
    # the same host:port string is handed to each worker as ``--coordinator``.
    # ``coordinator_ssh_target`` is the ssh target for that same machine (may
    # differ from the internal DNS name; e.g. ``node0.ourexp.emulab.net`` vs
    # ``node0``). Defaults to ``hosts[0]`` if left unset.
    coordinator_host: str | None = None
    coordinator_ssh_target: str | None = None
    coordinator_port: int = 47811
    # Where on each worker the Python harness package lives (for invoking
    # the coordinator remotely). Relative to ``host.remote_root``.
    harness_path: str = "harness"
    results_root: Path = field(default_factory=lambda: Path("results"))


# --- Factory helpers -------------------------------------------------------


def ycsb_mixes() -> tuple[Mix, Mix, Mix]:
    return (Mix.ycsb_a(), Mix.ycsb_b(), Mix.ycsb_c())


def default_m3_sweep(
    hosts: tuple[Host, ...],
    *,
    records_list: tuple[int, ...] = (100_000, 10_000_000),
    duration_s: int = 10,
    threads: int | None = None,
    seed: int = 1,
) -> SweepConfig:
    """Build the default 12-cell local/remote sweep.

    Six combinations (3 mixes x 2 distributions) per record count.
    """
    cells: list[RunConfig] = []
    for records in records_list:
        for distribution in (Distribution.UNIFORM, Distribution.ZIPF):
            for mix in ycsb_mixes():
                cells.append(
                    RunConfig(
                        records=records,
                        threads=threads,
                        duration_s=duration_s,
                        mix=mix,
                        distribution=distribution,
                        seed=seed,
                    )
                )
    return SweepConfig(name="m3", hosts=hosts, cells=tuple(cells))


def with_threads(cfg: RunConfig, threads: int) -> RunConfig:
    """Return a copy of ``cfg`` with ``threads`` overridden. Frozen-dataclass idiom."""
    return replace(cfg, threads=threads)
