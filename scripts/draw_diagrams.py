#!/usr/bin/env python3
"""Draw the explanatory diagrams in docs/img/ as plain SVG.

    scripts/draw_diagrams.py

No dependencies. Each diagram is a function that places boxes, arrows and
text on a canvas; edit the coordinates and re-run.
"""

import html
import pathlib

IMAGES = pathlib.Path(__file__).resolve().parent.parent / "docs" / "img"

SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK_SECONDARY = "#52514e"
LINE = "#8a8983"

# One hue per subsystem (the same hues the flamegraphs use): (fill tint, accent).
SQL = ("#dbe9fb", "#2a78d6")
EXEC = ("#fbe0d5", "#eb6834")
TREE = ("#d3f1e6", "#1baf7a")
LOG = ("#fbeccb", "#eda100")
PAGER = ("#fae3ec", "#e87ba4")
PLAIN = ("#efeeea", "#8a8983")


class Canvas:
    def __init__(self, width, height):
        self.width = width
        self.height = height
        self.parts = []

    def box(self, x, y, width, height, hue, title=None, lines=(), bold=True, radius=6):
        fill, accent = hue
        self.parts.append(f'<rect x="{x}" y="{y}" width="{width}" height="{height}" rx="{radius}" '
                          f'fill="{fill}" stroke="{accent}" stroke-width="1.2"/>')
        rows = ([title] if title else []) + list(lines)
        line_height = 16
        first_baseline = y + height / 2 - (len(rows) - 1) * line_height / 2 + 4.5
        for index, row in enumerate(rows):
            is_title = title is not None and index == 0
            self.text(x + width / 2, first_baseline + index * line_height, row, anchor="middle",
                      weight="600" if (is_title and bold) else "400",
                      colour=INK if is_title else INK_SECONDARY, size=13 if is_title else 12)

    def text(self, x, y, content, anchor="start", weight="400", colour=INK, size=13, mono=False):
        family = ' font-family="ui-monospace, Menlo, Consolas, monospace"' if mono else ""
        self.parts.append(f'<text x="{x}" y="{y}" text-anchor="{anchor}" font-weight="{weight}" '
                          f'fill="{colour}" font-size="{size}"{family}>{html.escape(content)}</text>')

    def arrow(self, x1, y1, x2, y2, colour=LINE, dashed=False, label=None, label_dx=6, label_dy=-6):
        dash = ' stroke-dasharray="5 4"' if dashed else ""
        self.parts.append(f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" stroke="{colour}" '
                          f'stroke-width="1.6"{dash} marker-end="url(#arrowhead)"/>')
        if label:
            self.text((x1 + x2) / 2 + label_dx, (y1 + y2) / 2 + label_dy, label,
                      colour=INK_SECONDARY, size=11.5)

    def line(self, x1, y1, x2, y2, colour=LINE, width=1.2, dashed=False):
        dash = ' stroke-dasharray="4 4"' if dashed else ""
        self.parts.append(f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" stroke="{colour}" '
                          f'stroke-width="{width}"{dash}/>')

    def rect(self, x, y, width, height, fill, stroke="none", radius=0):
        self.parts.append(f'<rect x="{x}" y="{y}" width="{width}" height="{height}" rx="{radius}" '
                          f'fill="{fill}" stroke="{stroke}"/>')

    def save(self, name):
        svg = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{self.width}" '
               f'height="{self.height}" viewBox="0 0 {self.width} {self.height}" '
               f'font-family="system-ui, -apple-system, Segoe UI, Helvetica, Arial, sans-serif">'
               f'<defs><marker id="arrowhead" viewBox="0 0 10 10" refX="9" refY="5" '
               f'markerWidth="7" markerHeight="7" orient="auto-start-reverse">'
               f'<path d="M0,0 L10,5 L0,10 z" fill="{LINE}"/></marker></defs>'
               f'<rect width="{self.width}" height="{self.height}" fill="{SURFACE}"/>'
               + "".join(self.parts) + "</svg>")
        (IMAGES / name).write_text(svg)
        print(f"wrote docs/img/{name}")


def architecture():
    canvas = Canvas(860, 470)
    canvas.text(20, 28, "One statement, top to bottom", weight="600", size=15)

    # Row labels.
    for y, label in ((78, "SQL front end"), (188, "Query execution"), (298, "Storage engine"),
                     (408, "Files on disk")):
        canvas.text(20, y + 5, label, colour=INK_SECONDARY, size=12)

    # SQL front end.
    canvas.box(150, 52, 150, 52, PLAIN, "SQL text", ["SELECT ... WHERE id = 7"], bold=False)
    canvas.box(340, 52, 130, 52, SQL, "Tokenizer", ["text → tokens"])
    canvas.box(510, 52, 130, 52, SQL, "Parser", ["recursive descent"])
    canvas.box(680, 52, 150, 52, SQL, "AST", ["arena-allocated"])
    canvas.arrow(300, 78, 338, 78)
    canvas.arrow(470, 78, 508, 78)
    canvas.arrow(640, 78, 678, 78)

    # Execution.
    canvas.box(150, 162, 190, 52, EXEC, "Binder", ["resolve names, check types"])
    canvas.box(395, 162, 190, 52, EXEC, "Planner", ["key ranges, join method"])
    canvas.box(640, 162, 190, 52, EXEC, "Volcano operators", ["open / next / close"])
    canvas.arrow(755, 104, 755, 132)
    canvas.line(755, 132, 245, 132)
    canvas.arrow(245, 132, 245, 160)
    canvas.arrow(340, 188, 393, 188)
    canvas.arrow(585, 188, 638, 188)

    # Storage engine.
    canvas.box(150, 272, 190, 52, TREE, "B+-tree", ["one per table: key → row"])
    canvas.box(395, 272, 190, 52, PAGER, "Pager", ["4 KB page cache, no-steal"])
    canvas.box(640, 272, 190, 52, LOG, "Write-ahead log", ["group commit, checksums"])
    canvas.arrow(700, 214, 300, 270, label="read / change rows", label_dx=-160, label_dy=-4)
    canvas.arrow(340, 298, 393, 298, label="pages", label_dx=-18, label_dy=-8)
    canvas.arrow(790, 214, 790, 270, label="log each change", label_dx=-100, label_dy=4)

    # Files.
    canvas.box(395, 382, 190, 52, PLAIN, "shop.db", ["B+-tree pages"], bold=False)
    canvas.box(640, 382, 190, 52, PLAIN, "shop.db.wal", ["records since last checkpoint"],
               bold=False)
    canvas.arrow(490, 324, 490, 380, label="checkpoint only", label_dx=-100, label_dy=4)
    canvas.arrow(735, 324, 735, 380, label="every commit", label_dx=8, label_dy=4)
    canvas.arrow(640, 316, 587, 316, dashed=True)
    canvas.text(613, 344, "page images", anchor="middle", colour=INK_SECONDARY, size=11.5)
    canvas.save("architecture.svg")


def operator_tree():
    canvas = Canvas(860, 430)
    canvas.text(20, 28, "A query is a tree of iterators; rows are pulled from the top",
                weight="600", size=15)
    canvas.text(20, 54, "SELECT u.name, o.total FROM orders o JOIN users u ON u.id = o.user_id",
                mono=True, size=12, colour=INK_SECONDARY)
    canvas.text(20, 72, "WHERE o.total > 10 ORDER BY o.total DESC LIMIT 5", mono=True, size=12,
                colour=INK_SECONDARY)

    column = 300
    width = 250
    rows = [
        (96, "Limit 5", "stops pulling after five rows"),
        (160, "Project  u.name, o.total", "evaluates the select list"),
        (224, "Sort  o.total DESC", "the only operator that buffers rows"),
        (288, "Filter  o.total > 10", "type-checked at plan time"),
        (352, "IndexJoin  u.id = o.user_id", "probes users by primary key"),
    ]
    for y, title, note in rows:
        canvas.box(column, y, width, 40, EXEC, title, bold=True)
        canvas.text(column + width + 16, y + 25, note, colour=INK_SECONDARY, size=12)
    for (y, _, _), (next_y, _, _) in zip(rows, rows[1:]):
        canvas.arrow(column + width / 2 - 30, y + 40, column + width / 2 - 30, next_y - 2)
        canvas.arrow(column + width / 2 + 30, next_y, column + width / 2 + 30, y + 42, dashed=True)

    canvas.box(60, 352, 180, 40, TREE, "Scan  orders")
    canvas.arrow(298, 372, 242, 372)
    canvas.text(60, 412, "walks the leaf chain in key order", colour=INK_SECONDARY, size=12)

    canvas.text(20, 120, "solid arrows: next() calls go down", colour=INK_SECONDARY, size=12)
    canvas.text(20, 138, "dashed arrows: rows come back up", colour=INK_SECONDARY, size=12)
    canvas.save("operator-tree.svg")


def page_layout():
    canvas = Canvas(860, 472)
    canvas.text(20, 28, "The database file is an array of 4 KB pages", weight="600", size=15)

    # The file as pages.
    labels = ["0  file header", "1  catalog root", "2  leaf", "3  leaf", "4  internal", "5  leaf", "…"]
    hues = [PLAIN, TREE, TREE, TREE, TREE, TREE, PLAIN]
    x = 20
    for label, hue in zip(labels, hues):
        width = 60 if label == "…" else 122
        canvas.box(x, 46, width, 36, hue, None, [label])
        x += width + 6
    canvas.text(20, 100, "page count and free-list head live in page 0; every table is a B+-tree "
                "whose root page never moves", colour=INK_SECONDARY, size=12)

    canvas.text(20, 142, "A leaf page", weight="600")
    y = 150
    segments = [
        (20, 120, PLAIN, "header", "12 bytes"),
        (140, 150, TREE, "slot array →", "2 bytes per cell, sorted"),
        (290, 250, (SURFACE, LINE), "free space", "grows from both ends"),
        (540, 100, TREE, "cell", "key 30"),
        (640, 100, TREE, "cell", "key 10"),
        (740, 100, TREE, "cell", "key 20"),
    ]
    for x, width, hue, title, note in segments:
        canvas.box(x, y, width, 56, hue, title, [note], radius=0)
    # Slot -> cell pointers: slots are in key order, cells in arrival order.
    for slot_x, cell_x in ((170, 690), (215, 790), (260, 590)):
        canvas.parts.append(f'<path d="M{slot_x},206 C{slot_x},256 {cell_x},256 {cell_x},210" '
                            f'fill="none" stroke="{TREE[1]}" stroke-width="1.3" '
                            f'marker-end="url(#arrowhead)"/>')
    canvas.text(20, 272, "Slots are sorted by key and point at the cells, which stay where they "
                "landed: an insert moves 2-byte slots, not rows.", colour=INK_SECONDARY, size=12)

    # One cell.
    canvas.text(20, 312, "A cell", weight="600")
    cell = [
        (20, 130, TREE, "key", "8 bytes"),
        (150, 110, TREE, "row length", "2 bytes"),
        (260, 580, EXEC, "row", "column values back to back, no padding"),
    ]
    for x, width, hue, title, note in cell:
        canvas.box(x, 320, width, 50, hue, title, [note], radius=0)

    # Row serialization.
    canvas.text(20, 408, "The row", weight="600")
    canvas.text(84, 408, "(7, 'Ada', 36.5, TRUE)", mono=True, size=12.5, colour=INK_SECONDARY)
    fields = [
        (260, 190, "INT", "07 00 00 00 00 00 00 00"),
        (450, 60, "length", "03 00"),
        (510, 80, "TEXT", "41 64 61"),
        (590, 190, "DECIMAL", "00 00 00 00 00 40 42 40"),
        (780, 60, "BOOL", "01"),
    ]
    for x, width, title, encoded in fields:
        canvas.box(x, 390, width, 26, EXEC, None, [title], radius=0)
        canvas.text(x + width / 2, 434, encoded, anchor="middle", mono=True, size=10.5,
                    colour=INK_SECONDARY)
    canvas.text(840, 458, "22 bytes; TEXT is read in place, never copied", anchor="end",
                colour=INK_SECONDARY, size=12)
    canvas.save("page-layout.svg")


def btree():
    canvas = Canvas(860, 330)
    canvas.text(20, 28, "B+-tree: internal pages route, leaves hold the rows",
                weight="600", size=15)

    highlight = ("#d3f1e6", "#0b0b0b")
    canvas.box(340, 56, 180, 44, highlight, "40   |   90", ["root (page id fixed)"])
    leaves = [
        (40, "10  20  30", TREE, "keys < 40"),
        (340, "40  57  61  80", highlight, "40 ≤ keys < 90"),
        (640, "90  95  99", TREE, "keys ≥ 90"),
    ]
    for x, keys, hue, note in leaves:
        canvas.box(x, 180, 180, 44, hue, keys, [note])
    canvas.arrow(370, 100, 150, 178)
    canvas.arrow(430, 100, 430, 178, colour=INK)
    canvas.arrow(490, 100, 710, 178)
    canvas.arrow(220, 202, 338, 202, label="next leaf", label_dx=-28, label_dy=-8)
    canvas.arrow(520, 202, 638, 202, label="next leaf", label_dx=-28, label_dy=-8)

    canvas.text(438, 146, "lookup of id = 57", colour=INK, size=12)
    canvas.text(20, 262, "Point lookup", weight="600")
    canvas.text(120, 262, "binary-search the root, follow one child, binary-search the leaf's "
                "slots: O(log n) pages.", colour=INK_SECONDARY, size=12.5)
    canvas.text(20, 284, "Range scan", weight="600")
    canvas.text(120, 284, "descend once to the first key, then walk right along the leaf links; "
                "never back up the tree.", colour=INK_SECONDARY, size=12.5)
    canvas.text(20, 306, "Append", weight="600")
    canvas.text(120, 306, "a full last leaf splits at the new key, not in half, so ascending "
                "inserts leave every leaf full.", colour=INK_SECONDARY, size=12.5)
    canvas.save("btree.svg")


def group_commit():
    canvas = Canvas(860, 330)
    canvas.text(20, 28, "Group commit: many commits share one fsync", weight="600", size=15)

    def timeline(y, title, note):
        canvas.text(20, y - 30, title, weight="600")
        canvas.text(20, y - 13, note, colour=INK_SECONDARY, size=12)
        canvas.line(20, y + 40, 840, y + 40)

    # Row 1: fsync per commit. Each commit (thin) is followed by a long fsync.
    timeline(92, "fsync on every commit",
             "each INSERT waits ~180 µs for the disk: about 5,500 commits per second")
    x = 20
    for _ in range(9):
        canvas.rect(x, 96, 6, 30, SQL[1])
        canvas.rect(x + 8, 96, 78, 30, LOG[1], radius=2)
        x += 91
    canvas.text(67, 116, "fsync", anchor="middle", size=11)

    # Row 2: group commit. Many thin commits, then one fsync per window.
    timeline(222, "group commit, 5 ms window",
             "commits are appended to an in-memory log buffer; one write + fsync covers the "
             "whole group")
    x = 20
    for window in range(3):
        for _ in range(38):
            canvas.rect(x, 226, 4, 30, SQL[1])
            x += 5.6
        canvas.rect(x + 2, 226, 54, 30, LOG[1], radius=2)
        if window == 0:
            canvas.text(x + 29, 246, "fsync", anchor="middle", size=11)
        x += 62
    canvas.line(20, 280, 232, 280, colour=INK)
    canvas.line(20, 275, 20, 285, colour=INK)
    canvas.line(232, 275, 232, 285, colour=INK)
    canvas.text(126, 298, "5 ms ≈ 3,700 commits at 740K/s", anchor="middle", size=12,
                colour=INK_SECONDARY)
    canvas.text(840, 298, "a crash loses at most the commits still in the buffer",
                anchor="end", size=12, colour=INK_SECONDARY)

    canvas.rect(620, 16, 10, 12, SQL[1])
    canvas.text(636, 27, "commit", size=12, colour=INK_SECONDARY)
    canvas.rect(700, 16, 22, 12, LOG[1], radius=2)
    canvas.text(728, 27, "waiting on fsync", size=12, colour=INK_SECONDARY)
    canvas.save("group-commit.svg")


def checkpoint_recovery():
    canvas = Canvas(860, 400)
    canvas.text(20, 28, "Checkpoint: pages are logged before the file is overwritten",
                weight="600", size=15)

    steps = [
        (20, PAGER, "Dirty pages", ["in the cache only"]),
        (235, LOG, "1. Log page images", ["append to WAL + end marker", "fsync the WAL"]),
        (450, PAGER, "2. Overwrite pages", ["write into the database file", "fsync the file"]),
        (665, LOG, "3. Empty the WAL", ["truncate to its header", "fsync"]),
    ]
    for x, hue, title, lines in steps:
        canvas.box(x, 56, 175, 70, hue, title, lines)
    for x in (195, 410, 625):
        canvas.arrow(x, 91, x + 38, 91)

    canvas.text(20, 170, "If the process dies…", weight="600")
    canvas.text(165, 170, "recovery at the next open reads the WAL and does exactly one of these:",
                colour=INK_SECONDARY, size=12.5)

    outcomes = [
        (235, "…during step 1", ["No end marker, so the file was", "never touched. Ignore the",
                                 "images; replay the logged", "statements onto the old file."]),
        (450, "…during step 2", ["End marker is durable. Copy", "every image into the file",
                                 "again (idempotent), then", "replay anything logged after."]),
        (665, "…during step 3", ["File is already complete.", "Copying the images again",
                                 "changes nothing; the WAL", "is emptied."]),
    ]
    for x, title, lines in outcomes:
        canvas.line(x + 87, 126, x + 87, 150, dashed=True)
        canvas.line(x + 87, 180, x + 87, 194, dashed=True)
        canvas.box(x, 196, 175, 110, PLAIN, title, lines, bold=True)

    canvas.box(20, 196, 175, 110, PLAIN, "…between checkpoints",
               ["Replay every committed", "statement in the WAL.", "A torn or uncommitted",
                "tail fails its checksum", "and is dropped."])
    canvas.line(107, 126, 107, 150, dashed=True)
    canvas.line(107, 180, 107, 194, dashed=True)

    canvas.text(20, 344, "Why the detour through the log?", weight="600")
    canvas.text(20, 366, "Overwriting 4 KB pages in place is not atomic. A crash half-way leaves "
                "a mix of old and new pages that replaying", colour=INK_SECONDARY, size=12.5)
    canvas.text(20, 384, "logical records cannot repair. With the images safely in the WAL "
                "first, there is always a complete copy to finish from.",
                colour=INK_SECONDARY, size=12.5)
    canvas.save("checkpoint-recovery.svg")


def main():
    IMAGES.mkdir(parents=True, exist_ok=True)
    architecture()
    operator_tree()
    page_layout()
    btree()
    group_commit()
    checkpoint_recovery()


if __name__ == "__main__":
    main()
