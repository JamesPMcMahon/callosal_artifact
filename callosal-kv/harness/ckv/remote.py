"""Thin ssh / rsync / scp wrapper over ``subprocess``.

All remote access in the harness goes through this module so host config,
ssh options, and timeouts live in one place. Nothing here is clever —
it's just shell plumbing with types.

Rationale for not using paramiko or fabric: the harness runs on a developer
laptop against a small set of CloudLab nodes, ssh keys are already set up,
and OpenSSH's control-master keeps the handshake cost amortized. Adding a
library dependency for features we do not need (interactive auth, tunneling,
SFTP mode switching) is not worth it.
"""

from __future__ import annotations

import logging
import shlex
import subprocess
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

from ckv.config import Host

_LOG = logging.getLogger(__name__)

# Reasonable defaults: do not prompt if a host key is unknown in a fresh
# CloudLab allocation; share a control master so repeated invocations do
# not re-handshake. StrictHostKeyChecking=accept-new is safer than "no".
_SSH_OPTS = [
    "-o", "StrictHostKeyChecking=accept-new",
    "-o", "BatchMode=yes",
    "-o", "ControlMaster=auto",
    "-o", "ControlPath=~/.ssh/cm-%r@%h:%p",
    "-o", "ControlPersist=60s",
]

# Non-interactive ssh doesn't read the user's login profile, so tools
# installed under $HOME (notably ``uv`` at ``~/.local/bin``) aren't on
# PATH. Prepend a PATH extension to every remote command so harness
# callers don't have to think about it.
_REMOTE_ENV_PREAMBLE = 'export PATH="$HOME/.local/bin:$PATH"'


@dataclass(frozen=True, slots=True)
class RemoteResult:
    returncode: int
    stdout: str
    stderr: str

    def ok(self) -> bool:
        return self.returncode == 0


class Remote:
    """One remote host. Stateless from the caller's POV; each method shells out."""

    def __init__(self, host: Host) -> None:
        self.host = host

    # -- helpers -----------------------------------------------------------

    def _ssh_prefix(self) -> list[str]:
        return ["ssh", *_SSH_OPTS, self.host.ssh_target]

    def _remote_path(self, relative: str) -> PurePosixPath:
        """Resolve ``relative`` against ``host.remote_root`` on the remote."""
        return PurePosixPath(self.host.remote_root) / relative

    # -- ops ---------------------------------------------------------------

    def run(self, argv: list[str], *, cwd: str | None = None,
            timeout_s: float | None = None) -> RemoteResult:
        """Execute ``argv`` on the remote. Quoting is handled here."""
        remote_cmd = shlex.join(argv)
        if cwd:
            remote_cmd = f"cd {shlex.quote(cwd)} && {remote_cmd}"
        remote_cmd = f"{_REMOTE_ENV_PREAMBLE}; {remote_cmd}"
        full = [*self._ssh_prefix(), remote_cmd]
        _LOG.info("remote.run %s: %s", self.host.name, remote_cmd)
        cp = subprocess.run(
            full, capture_output=True, text=True, check=False, timeout=timeout_s,
        )
        return RemoteResult(returncode=cp.returncode, stdout=cp.stdout, stderr=cp.stderr)

    def run_background(self, argv: list[str], *, cwd: str | None = None,
                       stdout_path: str | None = None) -> subprocess.Popen[str]:
        """Launch ``argv`` on the remote and return a handle to the ssh process.

        Output is piped unless ``stdout_path`` is set (a remote file path —
        shell redirection applied on the remote side). Use this to kick off
        a long-running worker; terminate or wait on the returned handle.
        """
        remote_cmd = shlex.join(argv)
        if stdout_path:
            remote_cmd += f" > {shlex.quote(stdout_path)} 2>&1"
        if cwd:
            remote_cmd = f"cd {shlex.quote(cwd)} && {remote_cmd}"
        remote_cmd = f"{_REMOTE_ENV_PREAMBLE}; {remote_cmd}"
        full = [*self._ssh_prefix(), remote_cmd]
        _LOG.info("remote.run_background %s: %s", self.host.name, remote_cmd)
        return subprocess.Popen(
            full, stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )

    def rsync_to(self, local_path: Path, remote_relative: str,
                 *, extra_args: list[str] | None = None) -> RemoteResult:
        """Rsync ``local_path`` → ``host:remote_root/remote_relative``."""
        remote_dest = f"{self.host.ssh_target}:{self._remote_path(remote_relative)}"
        ssh_cmd = " ".join(["ssh", *_SSH_OPTS])
        argv = ["rsync", "-az", "--delete", "-e", ssh_cmd]
        if extra_args:
            argv.extend(extra_args)
        argv.extend([str(local_path), remote_dest])
        _LOG.info("rsync_to %s: %s → %s", self.host.name, local_path, remote_dest)
        cp = subprocess.run(argv, capture_output=True, text=True, check=False)
        return RemoteResult(returncode=cp.returncode, stdout=cp.stdout, stderr=cp.stderr)

    def fetch(self, remote_relative: str, local_path: Path) -> RemoteResult:
        """scp ``host:remote_root/remote_relative`` → ``local_path``."""
        local_path.parent.mkdir(parents=True, exist_ok=True)
        src = f"{self.host.ssh_target}:{self._remote_path(remote_relative)}"
        argv = ["scp", *_SSH_OPTS, src, str(local_path)]
        _LOG.info("fetch %s: %s → %s", self.host.name, src, local_path)
        cp = subprocess.run(argv, capture_output=True, text=True, check=False)
        return RemoteResult(returncode=cp.returncode, stdout=cp.stdout, stderr=cp.stderr)
