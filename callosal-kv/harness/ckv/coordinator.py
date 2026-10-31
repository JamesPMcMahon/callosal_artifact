"""UDP barrier for cluster start synchronization.

Protocol — intentionally tiny, text-framed so it can be driven from anything
with a socket:

    worker → coord :  ``READY <node_id>\\n``
    coord  → worker : ``GO <start_ns>\\n``

The coordinator binds a UDP port, blocks until it has received ``READY`` from
each of the expected node ids, records the sender address for each, then
unicasts ``GO <start_ns>`` back to every worker and exits. ``start_ns`` is a
coordinator-local ``CLOCK_REALTIME`` nanosecond timestamp; workers treat it
as an opaque tag (equal across the barrier, useful for correlating logs) —
the actual measurement clock on each worker is still ``CLOCK_MONOTONIC``.

Why UDP, not TCP or a shared file: stateless, one packet per side, tolerates
workers started in any order, and avoids TCP connection cleanup when
rerunning quickly.

Usable two ways:

1. Library — ``Barrier(port, expected_node_ids).run(timeout=...)`` returns a
   ``BarrierResult`` with the collected addresses and the broadcast ``start_ns``.
2. CLI — ``python -m ckv coordinator --port 47811 --workers 0,1 --timeout 30``.
"""

from __future__ import annotations

import argparse
import logging
import socket
import sys
import time
from collections.abc import Iterable
from dataclasses import dataclass

_LOG = logging.getLogger(__name__)

# Cap received datagrams. The protocol messages are tiny (<64 bytes); anything
# larger is garbage or an attacker and we want to drop it cheaply.
_MAX_DATAGRAM = 512


@dataclass(frozen=True, slots=True)
class BarrierResult:
    """Outcome of a successful barrier run.

    ``workers`` maps node_id → (host, port) of the worker that sent READY.
    ``start_ns`` is the coordinator-local realtime timestamp broadcast as GO.
    """

    workers: dict[int, tuple[str, int]]
    start_ns: int


class BarrierTimeout(RuntimeError):
    """Raised when not all expected workers registered before the deadline."""


class BarrierProtocolError(RuntimeError):
    """Raised on malformed or unexpected datagrams."""


class Barrier:
    """Single-use UDP barrier.

    Construct once per run; call :meth:`run` once. The socket is bound in
    the constructor so callers can learn the actual port (useful when
    ``port=0`` requests an OS-assigned port — e.g. in unit tests).
    """

    def __init__(
        self,
        port: int,
        expected_node_ids: Iterable[int],
        *,
        bind_host: str = "0.0.0.0",
    ) -> None:
        self._expected: set[int] = set(expected_node_ids)
        if not self._expected:
            raise ValueError("Barrier requires at least one expected node_id")
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        # SO_REUSEADDR helps when the harness is restarted quickly against
        # a fixed port; it does not affect correctness within a single run.
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._sock.bind((bind_host, port))
        self._bound_port: int = self._sock.getsockname()[1]

    @property
    def port(self) -> int:
        return self._bound_port

    def close(self) -> None:
        self._sock.close()

    def __enter__(self) -> Barrier:
        return self

    def __exit__(self, *_: object) -> None:
        self.close()

    def run(self, *, timeout_s: float = 60.0) -> BarrierResult:
        """Wait for all expected READYs, then broadcast GO. Blocks."""
        deadline = time.monotonic() + timeout_s
        workers: dict[int, tuple[str, int]] = {}

        while len(workers) < len(self._expected):
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                missing = sorted(self._expected - workers.keys())
                raise BarrierTimeout(
                    f"timed out after {timeout_s:.1f}s "
                    f"waiting for node_ids {missing}; got {sorted(workers)}"
                )
            self._sock.settimeout(remaining)
            try:
                payload, addr = self._sock.recvfrom(_MAX_DATAGRAM)
            except TimeoutError as exc:
                missing = sorted(self._expected - workers.keys())
                raise BarrierTimeout(
                    f"timed out after {timeout_s:.1f}s "
                    f"waiting for node_ids {missing}; got {sorted(workers)}"
                ) from exc

            node_id = _parse_ready(payload)
            if node_id is None:
                _LOG.warning("ignoring non-READY datagram from %s: %r", addr, payload[:64])
                continue
            if node_id not in self._expected:
                _LOG.warning("ignoring READY for unexpected node_id=%d from %s", node_id, addr)
                continue
            if node_id in workers:
                # Duplicate — worker retried; keep the latest address.
                _LOG.debug("duplicate READY from node_id=%d; updating address", node_id)
            workers[node_id] = addr
            _LOG.info("READY node_id=%d from %s:%d (%d/%d)",
                      node_id, addr[0], addr[1], len(workers), len(self._expected))

        start_ns = time.time_ns()
        go_bytes = f"GO {start_ns}\n".encode("ascii")
        for node_id, addr in workers.items():
            self._sock.sendto(go_bytes, addr)
            _LOG.info("GO → node_id=%d at %s:%d (start_ns=%d)",
                      node_id, addr[0], addr[1], start_ns)
        return BarrierResult(workers=workers, start_ns=start_ns)


def _parse_ready(payload: bytes) -> int | None:
    """Return node_id from a ``READY <n>`` datagram, or ``None`` on mismatch."""
    try:
        text = payload.decode("ascii").strip()
    except UnicodeDecodeError:
        return None
    parts = text.split()
    # READY <node_id> → exactly two whitespace-separated tokens.
    _READY_PARTS = 2
    if len(parts) != _READY_PARTS or parts[0] != "READY":
        return None
    try:
        return int(parts[1])
    except ValueError:
        return None


# --- CLI -------------------------------------------------------------------


def _parse_worker_list(spec: str) -> list[int]:
    """Parse a ``--workers`` spec: ``"0,1,2"`` or ``"3"`` (meaning 0..2)."""
    if "," in spec or spec.isdigit() is False:
        return [int(x) for x in spec.split(",") if x.strip()]
    # A single integer N expands to 0..N-1, matching how --num-nodes is used.
    return list(range(int(spec)))


def add_cli(subparsers: argparse._SubParsersAction[argparse.ArgumentParser]) -> None:
    p = subparsers.add_parser(
        "coordinator",
        help="UDP barrier: wait for READY from each worker then broadcast GO",
    )
    p.add_argument("--port", type=int, default=47811, help="UDP port to bind (default 47811)")
    p.add_argument(
        "--workers",
        required=True,
        help="comma list of node_ids (e.g. '0,1,2') or a count (e.g. '3' = 0..2)",
    )
    p.add_argument("--timeout", type=float, default=60.0, help="barrier timeout seconds")
    p.add_argument("--bind", default="0.0.0.0", help="bind host (default 0.0.0.0)")
    p.set_defaults(func=_cmd_coordinator)


def _cmd_coordinator(args: argparse.Namespace) -> int:
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    node_ids = _parse_worker_list(args.workers)
    with Barrier(args.port, node_ids, bind_host=args.bind) as b:
        print(f"coordinator: listening on {args.bind}:{b.port} for {node_ids}", file=sys.stderr)
        try:
            result = b.run(timeout_s=args.timeout)
        except BarrierTimeout as exc:
            print(f"coordinator: {exc}", file=sys.stderr)
            return 1
    print(f"coordinator: released {len(result.workers)} workers at start_ns={result.start_ns}")
    return 0


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="python -m ckv.coordinator")
    p.add_argument("--port", type=int, default=47811, help="UDP port to bind (default 47811)")
    p.add_argument(
        "--workers",
        required=True,
        help="comma list of node_ids (e.g. '0,1,2') or a count (e.g. '3' = 0..2)",
    )
    p.add_argument("--timeout", type=float, default=60.0, help="barrier timeout seconds")
    p.add_argument("--bind", default="0.0.0.0", help="bind host (default 0.0.0.0)")
    return _cmd_coordinator(p.parse_args(argv))


if __name__ == "__main__":
    sys.exit(main())
