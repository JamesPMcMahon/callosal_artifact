"""Tests for ckv.scaling.

The thread-assignment math is the interesting part: every (total, T, N)
triple must produce a split that sums back to ``total`` and never exceeds
``T`` on any node. The cell generator just needs to emit the right count
and shape.
"""

from __future__ import annotations

import pytest

from ckv.config import Distribution, Mix, Probe
from ckv.scaling import build_thread_assignment, scaling_cells


class TestBuildThreadAssignment:
    def test_single_node_exact(self):
        assert build_thread_assignment(1, 16, 4) == (1, 0, 0, 0)
        assert build_thread_assignment(16, 16, 4) == (16, 0, 0, 0)

    def test_two_node_boundary_and_split(self):
        # x=17..32 must fit on 2 nodes.
        assert build_thread_assignment(17, 16, 4) == (9, 8, 0, 0)
        assert build_thread_assignment(20, 16, 4) == (10, 10, 0, 0)
        assert build_thread_assignment(21, 16, 4) == (11, 10, 0, 0)
        assert build_thread_assignment(32, 16, 4) == (16, 16, 0, 0)

    def test_three_node_boundary_and_split(self):
        assert build_thread_assignment(33, 16, 4) == (11, 11, 11, 0)
        assert build_thread_assignment(34, 16, 4) == (12, 11, 11, 0)
        assert build_thread_assignment(48, 16, 4) == (16, 16, 16, 0)

    def test_four_node_boundary_and_full(self):
        assert build_thread_assignment(49, 16, 4) == (13, 12, 12, 12)
        assert build_thread_assignment(64, 16, 4) == (16, 16, 16, 16)

    @pytest.mark.parametrize("total", list(range(1, 65)))
    def test_invariants(self, total: int) -> None:
        tph = build_thread_assignment(total, 16, 4)
        assert sum(tph) == total
        assert all(t <= 16 for t in tph)
        assert all(t >= 0 for t in tph)
        # Nonzero entries must be prefix (no gaps in active hosts).
        nonzero = [t > 0 for t in tph]
        if any(nonzero):
            first_zero = nonzero.index(False) if False in nonzero else len(nonzero)
            assert all(not z for z in nonzero[first_zero:])

    def test_rejects_invalid(self):
        with pytest.raises(ValueError):
            build_thread_assignment(0, 16, 4)
        with pytest.raises(ValueError):
            build_thread_assignment(-1, 16, 4)
        with pytest.raises(ValueError):
            build_thread_assignment(65, 16, 4)

    def test_max_nodes_eq_one(self):
        assert build_thread_assignment(1, 16, 1) == (1,)
        assert build_thread_assignment(16, 16, 1) == (16,)
        with pytest.raises(ValueError):
            build_thread_assignment(17, 16, 1)


class TestScalingCells:
    def test_cell_count(self):
        cells = scaling_cells(
            thread_counts=(1, 4, 16),
            mixes=(Mix.ycsb_a(), Mix.ycsb_c()),
            distributions=(Distribution.UNIFORM, Distribution.ZIPF),
            records=100_000, data_dir="/tmp/x", fs_label="ext4",
            warmup_s=2, duration_s=5,
            cores_per_node=16, max_nodes=1,
        )
        # 3 thread counts × 2 mixes × 2 distributions = 12.
        assert len(cells) == 12

    def test_cell_tags_are_unique(self):
        cells = scaling_cells(
            thread_counts=(1, 2, 4),
            mixes=(Mix.ycsb_a(), Mix.ycsb_b(), Mix.ycsb_c()),
            distributions=(Distribution.UNIFORM, Distribution.ZIPF),
            records=100_000, data_dir="/tmp/x", fs_label="tmpfs",
            warmup_s=0, duration_s=5,
            cores_per_node=16, max_nodes=1,
        )
        tags = [c.tag() for c in cells]
        assert len(set(tags)) == len(tags)
        # All tags end with the fs label.
        for t in tags:
            assert t.endswith("-tmpfs")

    def test_threads_per_host_plumbed_through(self):
        cells = scaling_cells(
            thread_counts=(20,),
            mixes=(Mix.ycsb_c(),),
            distributions=(Distribution.UNIFORM,),
            records=1000, data_dir="/x", fs_label="nfs",
            warmup_s=0, duration_s=1,
            cores_per_node=16, max_nodes=4,
        )
        assert len(cells) == 1
        assert cells[0].threads_per_host == (10, 10, 0, 0)
        assert cells[0].label == "nfs"
        assert cells[0].warmup_s == 0
        assert cells[0].probe == Probe.MONOTONIC
