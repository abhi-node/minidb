#!/usr/bin/env python3
"""Turn stack samples from `minidb_bench --profile` into a flamegraph SVG.

    scripts/flamegraph.py SAMPLES BINARY OUTPUT.svg [--title TEXT] [--top N]

SAMPLES holds one sample per line: a weight (how many timer periods the
sample stands for), then return addresses, innermost frame first.
BINARY must be the statically linked minidb_bench that produced them
(-DMINIDB_STATIC_BENCH=ON), so that addr2line can name every address.

Frames are coloured by the part of the engine they belong to. With --top N
the N hottest functions (by self time) are also printed as a table.
"""

import argparse
import collections
import html
import subprocess
import sys

# Category -> fill colour, in fixed order (also the legend order).
CATEGORIES = [
    ("SQL: tokenize + parse", "#2a78d6"),
    ("SQL: plan + execute", "#eb6834"),
    ("B+-tree", "#1baf7a"),
    ("Write-ahead log", "#eda100"),
    ("Pager + checkpoint", "#e87ba4"),
    ("System calls", "#008300"),
    ("C library", "#6250d6"),
    ("Benchmark driver", "#e34948"),
]
COLOUR = dict(CATEGORIES)
# Fills light enough to need dark text on top of them.
DARK_TEXT_FILLS = {"#1baf7a", "#eda100", "#e87ba4"}

SOURCE_DIRECTORY_CATEGORY = {
    "lexer": "SQL: tokenize + parse",
    "parser": "SQL: tokenize + parse",
    "expression": "SQL: plan + execute",
    "planner": "SQL: plan + execute",
    "executor": "SQL: plan + execute",
    "operator": "SQL: plan + execute",
    "db": "SQL: plan + execute",
    "row": "SQL: plan + execute",
    "value": "SQL: plan + execute",
    "schema": "SQL: plan + execute",
    "common": "SQL: plan + execute",
    "btree": "B+-tree",
    "wal": "Write-ahead log",
    "pager": "Pager + checkpoint",
    "storage": "Pager + checkpoint",
    "catalog": "Pager + checkpoint",
}
SYSTEM_CALL_NAMES = ("fdatasync", "fsync", "pwrite", "pread", "ftruncate", "write", "read",
                     "open", "close", "clock_gettime", "syscall")
SIGNAL_TRAMPOLINE = "__restore_rt"


def categorize(function, source_file):
    if "/src/" in source_file:
        directory = source_file.split("/src/")[1].split("/")[0]
        if source_file.endswith("checksum.c"):
            return "Write-ahead log"
        return SOURCE_DIRECTORY_CATEGORY.get(directory, "SQL: plan + execute")
    if "/include/minidb/" in source_file:
        return "SQL: plan + execute"  # inlined value/operator helpers
    if "/bench/" in source_file:
        return "Benchmark driver"
    bare = function.lstrip("_").replace("libc_", "").replace("GI_", "")
    if any(bare.startswith(name) for name in SYSTEM_CALL_NAMES):
        return "System calls"
    return "C library"


def symbolize(binary, addresses):
    """Map each address to its frames, outermost last: [(function, file), ...]."""
    result = subprocess.run(["addr2line", "-a", "-f", "-i", "-e", binary],
                            input="\n".join(hex(address) for address in addresses),
                            capture_output=True, text=True, check=True)
    frames_by_address = {}
    current = None
    lines = result.stdout.splitlines()
    index = 0
    while index < len(lines):
        if lines[index].startswith("0x"):
            current = frames_by_address.setdefault(int(lines[index], 16), [])
            index += 1
        else:
            function, location = lines[index], lines[index + 1]
            current.append((function, location.split(":")[0]))
            index += 2
    return frames_by_address


def load_stacks(samples_path, binary):
    """Return a Counter of call stacks (outermost first) -> sample count."""
    raw_samples = []
    with open(samples_path) as samples:
        for line in samples:
            fields = line.split()
            if len(fields) > 1:
                raw_samples.append((int(fields[0]), [int(text, 16) for text in fields[1:]]))

    # The profiler's own frames come first. Find them by name: everything up
    # to and including the signal trampoline.
    probe = symbolize(binary, sorted({a for _, sample in raw_samples for a in sample}))

    lookups = set()
    trimmed_samples = []
    for weight, sample in raw_samples:
        start = 0
        for position, address in enumerate(sample):
            if any(function == SIGNAL_TRAMPOLINE for function, _ in probe.get(address, [])):
                start = position + 1
                break
        frames = sample[start:]
        if not frames:
            continue
        # frames[0] is the interrupted instruction itself; the rest are return
        # addresses, which point just past the call -- step back into it.
        adjusted = [frames[0]] + [address - 1 for address in frames[1:]]
        lookups.update(adjusted)
        trimmed_samples.append((weight, adjusted))

    symbols = symbolize(binary, sorted(lookups))
    stacks = collections.Counter()
    for weight, sample in trimmed_samples:
        stack = []
        for address in sample:  # innermost first; inlined frames are innermost first too
            for function, source_file in symbols.get(address, [("??", "??")]):
                stack.append((function, categorize(function, source_file)))
        stack.reverse()
        # Drop the C runtime's start-up frames below main.
        for position, (function, _) in enumerate(stack):
            if function == "main":
                stack = stack[position:]
                break
        stacks[tuple(stack)] += weight
    return stacks


class Node:
    def __init__(self, function, category):
        self.function = function
        self.category = category
        self.samples = 0
        self.children = {}


def build_tree(stacks):
    root = Node("all", "Benchmark driver")
    for stack, count in stacks.items():
        node = root
        node.samples += count
        for function, category in stack:
            node = node.children.setdefault((function, category), Node(function, category))
            node.samples += count
    return root


def tree_depth(node, minimum_samples):
    depths = [tree_depth(child, minimum_samples) for child in node.children.values()
              if child.samples >= minimum_samples]
    return 1 + max(depths, default=0)


def render(root, title, output_path):
    width, margin, row_height, text_size = 1200, 12, 18, 11
    character_width = 6.7  # monospace at 11px
    plot_width = width - 2 * margin
    minimum_samples = max(1, root.samples * 0.0015)  # hide frames under ~2px
    levels = tree_depth(root, minimum_samples) - 1
    legend_rows = 2
    header_height = 56 + legend_rows * 18
    height = header_height + levels * row_height + margin
    parts = []

    def draw(node, x, depth):
        child_x = x
        for child in sorted(node.children.values(), key=lambda c: c.function):
            child_width = plot_width * child.samples / root.samples
            if child.samples >= minimum_samples:
                y = height - margin - depth * row_height
                fill = COLOUR[child.category]
                share = 100.0 * child.samples / root.samples
                parts.append(
                    f'<g><title>{html.escape(child.function)} - {share:.1f}% '
                    f'({child.samples} samples)</title>'
                    f'<rect x="{child_x:.2f}" y="{y - row_height + 1}" '
                    f'width="{max(child_width - 1, 0.5):.2f}" height="{row_height - 1}" '
                    f'rx="2" fill="{fill}"/>')
                fitting = int((child_width - 8) / character_width)
                if fitting >= 4:
                    label = child.function
                    if len(label) > fitting:
                        label = label[:fitting - 1] + "…"
                    ink = "#0b0b0b" if fill in DARK_TEXT_FILLS else "#ffffff"
                    parts.append(f'<text x="{child_x + 4:.2f}" y="{y - 5}" fill="{ink}">'
                                 f'{html.escape(label)}</text>')
                parts.append('</g>')
                draw(child, child_x, depth + 1)
            child_x += child_width

    draw(root, margin, 1)

    legend = []
    for index, (name, fill) in enumerate(CATEGORIES):
        x = margin + (index % 4) * 290
        y = 44 + (index // 4) * 18
        legend.append(f'<rect x="{x}" y="{y}" width="11" height="11" rx="2" fill="{fill}"/>'
                      f'<text x="{x + 17}" y="{y + 10}" fill="#52514e">{html.escape(name)}</text>')

    svg = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
           f'viewBox="0 0 {width} {height}" font-family="ui-monospace, Menlo, Consolas, monospace" '
           f'font-size="{text_size}">'
           f'<rect width="{width}" height="{height}" fill="#fcfcfb"/>'
           f'<text x="{margin}" y="24" font-size="15" font-weight="600" fill="#0b0b0b" '
           f'font-family="system-ui, sans-serif">{html.escape(title)}</text>'
           + "".join(legend) + "".join(parts) + '</svg>')
    with open(output_path, "w") as output:
        output.write(svg)


def print_summary(stacks, top):
    total = sum(stacks.values())
    self_time = collections.Counter()
    by_category = collections.Counter()
    for stack, count in stacks.items():
        if stack:
            self_time[stack[-1]] += count
            by_category[stack[-1][1]] += count
    print(f"{total} samples")
    print("\nself time by area:")
    for name, _ in CATEGORIES:
        print(f"  {100.0 * by_category[name] / total:5.1f}%  {name}")
    print(f"\ntop {top} functions by self time:")
    for (function, category), count in self_time.most_common(top):
        print(f"  {100.0 * count / total:5.1f}%  {function}  [{category}]")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("samples")
    parser.add_argument("binary")
    parser.add_argument("output")
    parser.add_argument("--title", default="minidb flamegraph")
    parser.add_argument("--top", type=int, default=0)
    arguments = parser.parse_args()

    stacks = load_stacks(arguments.samples, arguments.binary)
    if not stacks:
        sys.exit("no samples")
    render(build_tree(stacks), arguments.title, arguments.output)
    if arguments.top:
        print_summary(stacks, arguments.top)


if __name__ == "__main__":
    main()
