#!/usr/bin/env python3
"""Draw the benchmark figures in docs/img/ from the raw runs in docs/benchmarks/.

    scripts/plot_benchmarks.py

Requires matplotlib. Re-run scripts/run_benchmarks.sh first to refresh the data.
"""

import collections
import csv
import pathlib
import statistics

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter

ROOT = pathlib.Path(__file__).resolve().parent.parent
RESULTS = ROOT / "docs" / "benchmarks"
IMAGES = ROOT / "docs" / "img"

# Palette: one accent per series, text in ink tones, everything else recedes.
SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK_SECONDARY = "#52514e"
GRID = "#e6e5e1"
BLUE = "#2a78d6"
ORANGE = "#eb6834"
MUTED = "#b9b8b2"

plt.rcParams.update({
    "font.family": "DejaVu Sans",
    "font.size": 10,
    "axes.edgecolor": GRID,
    "axes.labelcolor": INK_SECONDARY,
    "axes.titlesize": 12,
    "axes.titleweight": "bold",
    "axes.titlecolor": INK,
    "axes.titlelocation": "left",
    "axes.titlepad": 14,
    "axes.spines.top": False,
    "axes.spines.right": False,
    "axes.facecolor": SURFACE,
    "figure.facecolor": SURFACE,
    "xtick.color": INK_SECONDARY,
    "ytick.color": INK_SECONDARY,
    "xtick.major.size": 0,
    "ytick.major.size": 0,
    "grid.color": GRID,
    "grid.linewidth": 1,
    "svg.fonttype": "none",
})


def read_runs(name):
    """Parse a file of `key=value ...` lines into a list of dicts."""
    runs = []
    for line in (RESULTS / name).read_text().splitlines():
        if line.strip():
            runs.append(dict(field.split("=", 1) for field in line.split()))
    return runs


def compact(value):
    """12345 -> '12.3K', 742715 -> '743K', 5523 -> '5.5K', 4000000 -> '4M'."""
    if value >= 1e6:
        text, suffix = f"{value / 1e6:.1f}", "M"
    elif value >= 1e5:
        text, suffix = f"{value / 1e3:.0f}", "K"
    elif value >= 1e3:
        text, suffix = f"{value / 1e3:.1f}", "K"
    else:
        text, suffix = f"{value:.0f}", ""
    if text.endswith(".0"):
        text = text[:-2]
    return text + suffix


def save(figure, name):
    figure.savefig(IMAGES / name, format="svg", bbox_inches="tight", pad_inches=0.25)
    plt.close(figure)
    print(f"wrote docs/img/{name}")


def horizontal_bars(axis, labels, values, colours):
    positions = range(len(labels))
    axis.barh(positions, values, height=0.5, color=colours)
    axis.set_yticks(positions, labels, color=INK)
    axis.invert_yaxis()
    axis.xaxis.grid(True)
    axis.set_axisbelow(True)
    axis.spines["left"].set_visible(False)
    axis.xaxis.set_major_formatter(FuncFormatter(lambda value, _: compact(value)))
    for position, value in zip(positions, values):
        axis.annotate(compact(value), (value, position), xytext=(6, 0),
                      textcoords="offset points", va="center", color=INK, fontweight="bold",
                      bbox={"facecolor": SURFACE, "edgecolor": "none", "pad": 1.5})
    axis.set_xlim(0, max(values) * 1.14)


def plot_insert_modes():
    rates = collections.defaultdict(list)
    for run in read_runs("insert_modes.txt"):
        rates[(run["sync"], run["keys"], run["rows"])].append(float(run["rows_per_sec"]))
    median = {key: statistics.median(values) for key, values in rates.items()}

    rows = [
        ("fsync on every commit", median[("full", "ascending", "30000")], MUTED),
        ("group commit, 5 ms", median[("group", "ascending", "2000000")], BLUE),
        ("group commit, random keys\n(500K rows: tree fits the cache)",
         median[("group", "random", "500000")], BLUE),
        ("group commit, random keys\n(2M rows: tree outgrows the cache)",
         median[("group", "random", "2000000")], BLUE),
        ("no fsync (upper bound)", median[("off", "ascending", "2000000")], MUTED),
    ]
    figure, axis = plt.subplots(figsize=(8.2, 3.6))
    horizontal_bars(axis, [row[0] for row in rows], [row[1] for row in rows],
                    [row[2] for row in rows])
    axis.axvline(50000, color=ORANGE, linewidth=1.5, zorder=0.5)
    axis.annotate("50K target", (50000, -0.62), xytext=(4, 0), textcoords="offset points",
                  color=INK_SECONDARY, va="center", annotation_clip=False)
    axis.set_title("Single-row INSERT statements per second, all durable at the end")
    axis.set_xlabel("rows per second")
    save(figure, "insert-throughput.svg")


def plot_delay_sweep():
    rates = collections.defaultdict(list)
    for run in read_runs("insert_delay.txt"):
        rates[int(run["delay_ms"])].append(float(run["rows_per_sec"]))
    delays = sorted(rates)
    medians = [statistics.median(rates[delay]) for delay in delays]
    lows = [min(rates[delay]) for delay in delays]
    highs = [max(rates[delay]) for delay in delays]

    figure, axis = plt.subplots(figsize=(7.4, 3.6))
    positions = range(len(delays))
    axis.vlines(positions, lows, highs, color=BLUE, linewidth=2, alpha=0.3)
    axis.plot(positions, medians, color=BLUE, linewidth=2, marker="o", markersize=7,
              markeredgecolor=SURFACE, markeredgewidth=2)
    axis.set_xticks(positions, [f"{delay} ms" for delay in delays])
    axis.yaxis.grid(True)
    axis.set_axisbelow(True)
    axis.set_ylim(0, max(highs) * 1.15)
    axis.yaxis.set_major_formatter(FuncFormatter(lambda value, _: compact(value)))
    axis.annotate(f"{compact(medians[0])}: one fsync per commit", (0, medians[0]),
                  xytext=(10, 14), textcoords="offset points", color=INK)
    default_index = delays.index(5)
    axis.annotate(f"{compact(medians[default_index])} at the default",
                  (default_index, medians[default_index]), xytext=(0, 14),
                  textcoords="offset points", color=INK, ha="center")
    axis.set_title("Insert rate against the group-commit delay")
    axis.set_xlabel("longest a commit may wait for its fsync")
    axis.set_ylabel("rows per second (median of 3, bars: min-max)")
    save(figure, "group-commit-delay.svg")


def plot_sustained():
    with open(RESULTS / "insert_timeline.csv") as timeline:
        points = [(int(row["rows"]), float(row["seconds"])) for row in csv.DictReader(timeline)]
    seconds, rates = [], []
    previous_rows, previous_seconds = 0, 0.0
    for rows, elapsed in points:
        rates.append((rows - previous_rows) / (elapsed - previous_seconds))
        seconds.append(elapsed)
        previous_rows, previous_seconds = rows, elapsed
    summary = read_runs("insert_sustained.txt")[0]
    average = float(summary["rows_per_sec"])

    figure, axis = plt.subplots(figsize=(8.2, 3.4))
    axis.plot(seconds, rates, color=BLUE, linewidth=2)
    axis.axhline(average, color=INK_SECONDARY, linewidth=1)
    axis.annotate(f"average {compact(average)} rows/s", (seconds[-1], average), xytext=(8, 0),
                  textcoords="offset points", va="center", color=INK, annotation_clip=False)
    axis.axhline(50000, color=ORANGE, linewidth=1.5)
    axis.annotate("50K target", (seconds[-1], 50000), xytext=(8, 0), textcoords="offset points",
                  va="center", color=INK_SECONDARY, annotation_clip=False)
    axis.yaxis.grid(True)
    axis.set_axisbelow(True)
    axis.set_ylim(0, max(rates) * 1.15)
    axis.set_xlim(0, seconds[-1])
    axis.yaxis.set_major_formatter(FuncFormatter(lambda value, _: compact(value)))
    axis.set_title(f"{int(summary['rows']) // 1000000}M inserts in a row: rate per 50,000 rows "
                   f"({summary['checkpoints']} checkpoints along the way)")
    axis.set_xlabel("seconds since start")
    axis.set_ylabel("rows per second")
    save(figure, "insert-sustained.svg")


def plot_lookup():
    runs = read_runs("lookup.txt")
    rows = [int(run["rows"]) for run in runs]
    microseconds = [float(run["ns_per_lookup"]) / 1000 for run in runs]
    pages = [float(run["pages_per_lookup"]) for run in runs]

    figure, (time_axis, page_axis) = plt.subplots(1, 2, figsize=(9, 3.3))
    for axis, values, title, unit in (
            (time_axis, microseconds, "Time per point lookup", "µs"),
            (page_axis, pages, "Pages read per lookup (tree height)", "")):
        axis.plot(rows, values, color=BLUE, linewidth=2, marker="o", markersize=7,
                  markeredgecolor=SURFACE, markeredgewidth=2)
        axis.set_xscale("log")
        axis.set_ylim(0, max(values) * 1.3)
        if not unit:
            axis.set_yticks(range(0, int(max(values)) + 2))
        axis.yaxis.grid(True)
        axis.set_axisbelow(True)
        axis.set_title(title)
        axis.set_ylabel("microseconds" if unit else "pages")
        axis.set_xlabel("rows in the table")
        axis.set_xticks(rows, [compact(count) for count in rows])
        axis.minorticks_off()
        axis.annotate(f"{values[0]:.2g} {unit}".strip(), (rows[0], values[0]), xytext=(0, 9),
                      textcoords="offset points", ha="left", color=INK)
        axis.annotate(f"{values[-1]:.2g} {unit}".strip(), (rows[-1], values[-1]), xytext=(0, 9),
                      textcoords="offset points", ha="right", color=INK)
    save(figure, "lookup-scaling.svg")


def plot_join():
    runs = read_runs("join.txt")
    rows = [int(run["rows"]) for run in runs]
    index_ms = [float(run["index_join_seconds"]) * 1000 for run in runs]
    nested = [(int(run["rows"]), float(run["nested_loop_seconds"]) * 1000) for run in runs
              if float(run["nested_loop_seconds"]) >= 0]

    figure, axis = plt.subplots(figsize=(7.4, 3.8))
    axis.plot([row for row, _ in nested], [ms for _, ms in nested], color=ORANGE, linewidth=2,
              marker="o", markersize=7, markeredgecolor=SURFACE, markeredgewidth=2,
              label="nested-loop join")
    axis.plot(rows, index_ms, color=BLUE, linewidth=2, marker="o", markersize=7,
              markeredgecolor=SURFACE, markeredgewidth=2, label="index join")
    axis.set_xscale("log")
    axis.set_yscale("log")
    axis.yaxis.grid(True)
    axis.set_axisbelow(True)
    axis.minorticks_off()
    axis.set_xticks(rows, [compact(count) for count in rows])
    axis.yaxis.set_major_formatter(FuncFormatter(
        lambda value, _: f"{value:g} ms" if value < 1000 else f"{value / 1000:g} s"))
    last_rows, last_ms = nested[-1]
    axis.annotate(f"{last_ms / 1000:.1f} s", (last_rows, last_ms), xytext=(8, 0),
                  textcoords="offset points", va="center", color=INK)
    matching = index_ms[rows.index(last_rows)]
    axis.annotate(f"{matching:.1f} ms", (last_rows, matching), xytext=(0, -16),
                  textcoords="offset points", ha="center", color=INK)
    axis.legend(frameon=False, loc="upper left", labelcolor=INK)
    axis.set_title("Joining two tables of N rows each (log-log)")
    axis.set_xlabel("rows per table")
    save(figure, "join-scaling.svg")


def plot_optimization_steps():
    steps = [  # commit -> label, in the order the changes were made
        ("dedbf96", "group commit\n(starting point)"),
        ("839922b", "+ slicing-by-8\nCRC-32"),
        ("f615192", "+ larger dirty-\npage budget"),
        ("ce7a558", "+ keyword\npre-filter"),
    ]
    rates = collections.defaultdict(list)
    for run in read_runs("optimization_steps.txt"):
        if run["sync"] == "group":
            rates[(run["commit"], run["keys"])].append(float(run["rows_per_sec"]))

    figure, axes = plt.subplots(1, 2, figsize=(9.6, 3.6))
    for axis, keys, title in ((axes[0], "ascending", "Ascending keys, 2M rows"),
                              (axes[1], "random", "Random keys, 500K rows")):
        medians = [statistics.median(rates[(commit, keys)]) for commit, _ in steps]
        positions = range(len(steps))
        axis.bar(positions, medians, width=0.42, color=BLUE)
        for position, (commit, _) in zip(positions, steps):
            values = rates[(commit, keys)]
            axis.vlines(position, min(values), max(values), color=INK, linewidth=1)
        for position, value in zip(positions, medians):
            axis.annotate(compact(value), (position, value), xytext=(14, 2),
                          textcoords="offset points", color=INK, fontweight="bold")
        axis.set_xticks(positions, [label for _, label in steps], fontsize=8.5)
        axis.yaxis.grid(True)
        axis.set_axisbelow(True)
        axis.set_ylim(0, max(max(rates[(commit, keys)]) for commit, _ in steps) * 1.15)
        axis.yaxis.set_major_formatter(FuncFormatter(lambda value, _: compact(value)))
        axis.set_title(title)
    axes[0].set_ylabel("rows per second (median; line: min-max)")
    save(figure, "optimization-steps.svg")


def main():
    IMAGES.mkdir(parents=True, exist_ok=True)
    plot_insert_modes()
    plot_delay_sweep()
    plot_sustained()
    plot_lookup()
    plot_join()
    plot_optimization_steps()


if __name__ == "__main__":
    main()
