"""Loopback tests for the UDP barrier."""

from __future__ import annotations

import socket
import threading
import time

import pytest

from ckv.coordinator import Barrier, BarrierTimeout, _parse_ready


def _send_ready(port: int, node_id: int, *, bind_port: int = 0) -> socket.socket:
    """Send READY from a new ephemeral UDP socket; return the socket so the
    caller can recvfrom to observe GO."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", bind_port))
    s.sendto(f"READY {node_id}\n".encode("ascii"), ("127.0.0.1", port))
    return s


def test_parse_ready_ok() -> None:
    assert _parse_ready(b"READY 0\n") == 0
    assert _parse_ready(b"READY 7") == 7


def test_parse_ready_rejects_garbage() -> None:
    assert _parse_ready(b"GO 42") is None
    assert _parse_ready(b"ready 0") is None       # case-sensitive
    assert _parse_ready(b"READY foo") is None
    assert _parse_ready(b"\xff\xfe") is None      # non-ascii


def test_barrier_releases_after_all_ready() -> None:
    # Use port=0 to let the OS pick a free port, avoiding flakes under
    # parallel pytest runs.
    with Barrier(port=0, expected_node_ids=[0, 1]) as b:
        port = b.port
        workers = [_send_ready(port, 0), _send_ready(port, 1)]
        result = b.run(timeout_s=2.0)

    assert set(result.workers.keys()) == {0, 1}
    assert result.start_ns > 0

    # Both workers should now have a GO message waiting.
    for w in workers:
        w.settimeout(1.0)
        payload, _ = w.recvfrom(512)
        text = payload.decode("ascii").strip()
        assert text.startswith("GO ")
        assert int(text.split()[1]) == result.start_ns
        w.close()


def test_barrier_times_out_when_worker_missing() -> None:
    with Barrier(port=0, expected_node_ids=[0, 1]) as b:
        _send_ready(b.port, 0).close()
        with pytest.raises(BarrierTimeout):
            b.run(timeout_s=0.2)


def test_barrier_ignores_unexpected_node_id() -> None:
    with Barrier(port=0, expected_node_ids=[0]) as b:
        # Send a stray READY for node 99, then the real one for node 0.
        _send_ready(b.port, 99).close()
        _send_ready(b.port, 0).close()
        result = b.run(timeout_s=2.0)
    assert set(result.workers.keys()) == {0}


def test_barrier_ignores_malformed_datagram() -> None:
    with Barrier(port=0, expected_node_ids=[0]) as b:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.sendto(b"not a protocol message", ("127.0.0.1", b.port))
        s.close()
        _send_ready(b.port, 0).close()
        result = b.run(timeout_s=2.0)
    assert set(result.workers.keys()) == {0}


def test_barrier_concurrent_readys() -> None:
    """Workers starting in parallel from different threads — the common case."""

    def worker(port: int, node_id: int, done: threading.Event) -> None:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind(("127.0.0.1", 0))
        # Stagger slightly to force coordinator to block between receives.
        time.sleep(0.01 * node_id)
        s.sendto(f"READY {node_id}".encode("ascii"), ("127.0.0.1", port))
        s.settimeout(2.0)
        s.recvfrom(512)
        done.set()
        s.close()

    with Barrier(port=0, expected_node_ids=range(4)) as b:
        events = [threading.Event() for _ in range(4)]
        threads = [
            threading.Thread(target=worker, args=(b.port, i, events[i]))
            for i in range(4)
        ]
        for t in threads:
            t.start()
        result = b.run(timeout_s=5.0)
        for t in threads:
            t.join(timeout=2.0)

    assert set(result.workers.keys()) == {0, 1, 2, 3}
    assert all(e.is_set() for e in events)
