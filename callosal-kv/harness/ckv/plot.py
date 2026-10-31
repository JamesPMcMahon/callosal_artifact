"""Plot the sweep summary with plotnine + a minimal Tufte theme.

Tufte's argument: remove ink that does not encode data. The theme below
strips the panel background, most gridlines, chart borders, and axis
ticks. Axes are drawn by the data-range tick marks only. Fonts stay at
whatever the user's matplotlib backend provides — no embedded font choice
to fight the rendering environment.

One function per figure: each takes a tidy DataFrame and returns a
``plotnine.ggplot`` object. Callers decide how to save.
"""

from __future__ import annotations

import argparse
import logging
from pathlib import Path

import pandas as pd
import plotnine as p9

_LOG = logging.getLogger(__name__)


def tufte_theme(base_size: int = 11) -> p9.theme:
    """Minimal Tufte-style theme for plotnine.

    Gridlines removed entirely; data-range ticks carry the reading. Fonts
    use Helvetica Neue when the rendering backend has it (falls back to
    Helvetica / Arial / sans-serif otherwise — matplotlib walks the list).
    """
    font_family = ["Helvetica Neue", "Helvetica", "Arial", "sans-serif"]
    return p9.theme_minimal(base_size=base_size) + p9.theme(
        text=p9.element_text(family=font_family),
        panel_grid_major=p9.element_blank(),
        panel_grid_minor=p9.element_blank(),
        panel_background=p9.element_blank(),
        axis_line=p9.element_line(color="black", size=0.3),
        axis_ticks=p9.element_line(color="black", size=0.3),
        strip_background=p9.element_blank(),
        legend_background=p9.element_blank(),
        legend_key=p9.element_blank(),
    )


def plot_throughput_by_mix(df: pd.DataFrame) -> p9.ggplot:
    """Throughput (Mops/s) vs mix, faceted by distribution, coloured by records."""
    return (
        p9.ggplot(df, p9.aes(x="mix", y="mops", color="factor(records)"))
        + p9.geom_point(size=3)
        + p9.geom_line(p9.aes(group="factor(records)"))
        + p9.facet_wrap("~distribution")
        + p9.labs(x="YCSB mix", y="Throughput (Mops/s)",
                  color="records", title="Throughput by mix x distribution")
        + tufte_theme()
    )


def plot_latency_by_mix(df: pd.DataFrame, *, metric: str = "get_p99_ns") -> p9.ggplot:
    """Latency percentile vs mix, faceted by distribution."""
    df = df.copy()
    df["metric_us"] = df[metric] / 1000.0
    return (
        p9.ggplot(df, p9.aes(x="mix", y="metric_us", color="factor(records)"))
        + p9.geom_point(size=3)
        + p9.geom_line(p9.aes(group="factor(records)"))
        + p9.facet_wrap("~distribution")
        + p9.labs(x="YCSB mix", y=f"{metric.removesuffix('_ns')} (µs)",
                  color="records", title=f"{metric} by mix x distribution")
        + tufte_theme()
    )


def plot_scaling(df: pd.DataFrame) -> p9.ggplot:
    """Fig-9 style scaling plot: throughput vs total_threads, by fs.

    Rows = distribution, columns = mix. One line per fs, ordered
    ext4 < tmpfs < nfs so the eye reads CPU-bound → RAM-bound → shared.
    """
    df = df.copy()
    # Force a stable legend order matching the narrative.
    df["fs"] = pd.Categorical(df["fs"], categories=["ext4", "tmpfs", "nfs"],
                              ordered=True)
    # Prefix mix letters with "YCSB-" so facet strips read YCSB-A/B/C.
    df["mix"] = "YCSB-" + df["mix"].astype(str)
    return (
        p9.ggplot(df, p9.aes(x="total_threads", y="mops",
                             color="fs", shape="fs"))
        + p9.geom_line(size=0.5)
        + p9.geom_point(size=1.8)
        + p9.facet_grid("distribution ~ mix")
        + p9.scale_x_continuous(breaks=[0, 8, 16, 24, 32, 40, 48, 56, 64])
        + p9.scale_color_brewer(type="qual", palette="Set1")
        + p9.labs(x="Total client threads", y="Throughput (Mops/s)",
                  color="FS", shape="FS",
                  title="KV throughput vs total client threads (per fs × mix × dist)")
        + tufte_theme()
        # Per-panel 4:3 (height/width = 0.75) — wider than tall.
        + p9.theme(aspect_ratio=0.75)
    )


def render_all(df: pd.DataFrame, out_dir: Path) -> list[Path]:
    """Emit the default figure set into ``out_dir``; return paths written."""
    out_dir.mkdir(parents=True, exist_ok=True)
    targets: dict[str, p9.ggplot] = {}
    if {"fs", "total_threads"}.issubset(df.columns):
        targets["scaling.png"] = plot_scaling(df)
    else:
        targets["throughput_by_mix.png"] = plot_throughput_by_mix(df)
        targets["get_p99_by_mix.png"] = plot_latency_by_mix(df, metric="get_p99_ns")
        targets["put_p99_by_mix.png"] = plot_latency_by_mix(df, metric="put_p99_ns")
    written: list[Path] = []
    for name, plot in targets.items():
        path = out_dir / name
        # plot_scaling wants a wider canvas than the mix/dist charts.
        if name == "scaling.png":
            plot.save(path, width=10, height=6, dpi=160, verbose=False)
            _LOG.info("wrote %s", path)
            written.append(path)
            continue
        # Plotnine's save() spews a deprecation warning on each call with
        # recent matplotlib. Swallow at the logging level — these are not
        # our warnings and we can't action them.
        plot.save(path, width=8, height=4, dpi=160, verbose=False)
        _LOG.info("wrote %s", path)
        written.append(path)
    return written


# --- CLI -------------------------------------------------------------------


def add_cli(subparsers: argparse._SubParsersAction[argparse.ArgumentParser]) -> None:
    p = subparsers.add_parser("plot", help="render summary.csv to figures/")
    p.add_argument("summary_csv", type=Path)
    p.add_argument("--out-dir", type=Path, default=None,
                   help="defaults to <summary_csv parent>/figures/")
    p.set_defaults(func=_cmd_plot)


def _cmd_plot(args: argparse.Namespace) -> int:
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    df = pd.read_csv(args.summary_csv)
    out_dir = args.out_dir if args.out_dir is not None else args.summary_csv.parent / "figures"
    paths = render_all(df, out_dir)
    for p_ in paths:
        print(p_)
    return 0
