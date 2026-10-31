"""Push source to each worker and build the binary there.

Called once per sweep, before populate. Centralizes the "get the same
code onto every node" step so callers don't replicate shell invocations.

Build is per-node rather than build-once-copy because we compile with
``-march=native``; a binary cooked on one generation of CPU will not
always run (and certainly not optimally) on another. The build cost is
small (<30 s) and amortized across the whole sweep.
"""

from __future__ import annotations

import logging
import subprocess
from pathlib import Path

from ckv import remote
from ckv.config import Host

_LOG = logging.getLogger(__name__)

# Exclude patterns that mirror ``scripts/sync.sh`` — build artifacts,
# venv caches, stale binaries, git metadata. Keep the local source of
# truth; if sync.sh changes, update this list too (or vice versa).
_RSYNC_EXCLUDES = (
    "/build/",
    "/.git/",
    "/config.env",
    "/runs/",
    "/callosal-kv",             # host-local built binary, rebuilt per node
    "/harness/.venv/",
    "/harness/.ruff_cache/",
    "/harness/.mypy_cache/",
    "/harness/__pycache__/",
    "/results/",
)


def rsync_tree(local_root: Path, host: Host) -> None:
    """rsync ``local_root`` → ``host:~/host.remote_root``. Raises on failure."""
    if host.is_local:
        return
    excludes: list[str] = []
    for pat in _RSYNC_EXCLUDES:
        excludes.extend(["--exclude", pat])
    # Mirror sync.sh: -az --delete, ssh ControlMaster baked into remote.py.
    src = str(local_root).rstrip("/") + "/"
    dest = f"{host.ssh_target}:{host.remote_root}/"
    ssh_cmd = "ssh " + " ".join([
        "-o", "StrictHostKeyChecking=accept-new",
        "-o", "BatchMode=yes",
        "-o", "ControlMaster=auto",
        "-o", "ControlPath=~/.ssh/cm-%r@%h:%p",
        "-o", "ControlPersist=60s",
    ])
    argv = ["rsync", "-az", "--delete", "-e", ssh_cmd, *excludes, src, dest]
    _LOG.info("rsync %s: %s → %s", host.name, local_root, dest)
    # mkdir the remote root first so rsync's dest exists.
    remote.Remote(host).run(["mkdir", "-p", host.remote_root])
    cp = subprocess.run(argv, capture_output=True, text=True, check=False)
    if cp.returncode != 0:
        raise RuntimeError(f"rsync to {host.name} failed: {cp.stderr}")


def build_binary(host: Host) -> None:
    """Run ``make`` on ``host`` to produce a native ``callosal-kv`` binary."""
    if host.is_local:
        cp = subprocess.run(["make"], capture_output=True, text=True, check=False)
        if cp.returncode != 0:
            raise RuntimeError(f"local build failed: {cp.stderr}")
        return
    r = remote.Remote(host)
    res = r.run(["make"], cwd=host.remote_root, timeout_s=300.0)
    if not res.ok():
        raise RuntimeError(f"build on {host.name} failed: {res.stderr}")


def setup_baseline_storage(
    hosts: tuple[Host, ...],
    *,
    ext4_dir: str = "/var/lib/callosal-kv-ext4/kvdata",
    tmpfs_dir: str = "/mnt/tmpfs-kvdata",
    tmpfs_size: str = "16G",
    nfs_dir: str = "/mnt/kvshared",
) -> None:
    """Idempotent storage setup for the three-baseline scaling sweep.

    - ext4: make a directory on node0's root disk and chown it.
    - tmpfs: mount a tmpfs on node0 at ``tmpfs_dir`` if it isn't already.
    - NFS: verify ``nfs_dir`` is mounted on every host beyond node0.
      This helper validates an existing NFS setup; it does not create one.

    Raises on any failure. Safe to re-run across sweeps.
    """
    if not hosts:
        return
    node0 = hosts[0]
    rest = hosts[1:]

    def _sh(h: Host, cmd: str) -> None:
        r = remote.Remote(h)
        res = r.run(["sh", "-c", cmd], cwd=h.remote_root)
        if not res.ok():
            raise RuntimeError(f"setup on {h.name}: `{cmd}` failed: {res.stderr}")

    # --- ext4: plain directory on node0's root disk -----------------------
    gid = "$(id -g)"
    uid = "$(id -u)"
    _sh(node0, f"sudo mkdir -p {ext4_dir} && "
               f"sudo chown {uid}:{gid} {ext4_dir} && "
               f"sudo chmod 775 {ext4_dir}")

    # --- tmpfs: mount if not already a tmpfs ------------------------------
    tmpfs_cmd = (
        f"sudo mkdir -p {tmpfs_dir} && "
        f"if ! mountpoint -q {tmpfs_dir} || "
        f"   [ \"$(findmnt -no FSTYPE {tmpfs_dir})\" != tmpfs ]; then "
        f"  sudo mount -t tmpfs -o size={tmpfs_size},mode=775,"
        f"uid={uid},gid={gid} tmpfs {tmpfs_dir}; "
        f"fi"
    )
    _sh(node0, tmpfs_cmd)

    # --- NFS: verify live mount on every non-head node --------------------
    for h in rest:
        _sh(h, f"mountpoint -q {nfs_dir} || "
               f"{{ echo NFS not mounted on $(hostname):{nfs_dir}; exit 1; }}")
    # Touch-test one round-trip to catch stale handles after a node reboot.
    _sh(node0, f"test -d {nfs_dir} && touch {nfs_dir}/.ckv-ping && "
               f"rm -f {nfs_dir}/.ckv-ping")


def provision_hosts(local_root: Path, hosts: tuple[Host, ...]) -> None:
    """rsync the tree and build the binary on every host.

    Serial today — good enough for ≤8 nodes. If this ever needs to be
    parallel we can wrap each host in a thread; the tricky bit is just
    surfacing exceptions cleanly.
    """
    for h in hosts:
        _LOG.info("provision: %s", h.name)
        rsync_tree(local_root, h)
        build_binary(h)
