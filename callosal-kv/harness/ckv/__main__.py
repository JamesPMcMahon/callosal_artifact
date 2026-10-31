"""``python -m ckv <subcommand>`` dispatcher.

Each submodule that exposes a CLI registers its subparser via ``add_cli``.
This file stays boring on purpose: argparse boilerplate only.
"""

from __future__ import annotations

import argparse
import sys

from ckv import coordinator, plot, scaling, sweep


def _build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="ckv", description="callosal-kv experiment harness")
    subparsers = p.add_subparsers(dest="command", required=True)
    coordinator.add_cli(subparsers)
    sweep.add_cli(subparsers)
    scaling.add_cli(subparsers)
    plot.add_cli(subparsers)
    return p


def main(argv: list[str] | None = None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)
    return int(args.func(args))


if __name__ == "__main__":
    sys.exit(main())
