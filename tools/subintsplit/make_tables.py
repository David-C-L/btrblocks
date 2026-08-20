#!/usr/bin/env python3
"""Turn subintsplit_bench CSVs into per-dataset comparison tables.

The benchmark emits one row per (codec, dataset, block size) and one row per
section of each SubIntSplit plan. Useful, but not readable: the comparisons
anyone actually wants are spread across a hundred rows and have to be pivoted by
hand. This produces the pivot.

Standard library only, deliberately, so it runs under whatever python3 is on
PATH -- unlike parquet_to_i64.py, which needs pyarrow. Regenerating the tables
from committed CSVs should never require a specific interpreter or a build.

  python3 make_tables.py --results docs/subintsplit-results.csv \\
                         --sections docs/subintsplit-sections.csv \\
                         --out-dir benchmark-results
"""

from __future__ import annotations

import argparse
import csv
import re
import shutil
from collections import defaultdict
from pathlib import Path

# ---------------------------------------------------------------------------
# Row bands.
#
# The uncompressed reference and the end-to-end (automatic selection) results
# are context for the per-codec comparison rather than entries in it, so they
# sit above and below it separated by a rule.
BASELINE, CODECS, END_TO_END = "baseline", "codecs", "end_to_end"

# codec -> (band, display name). Order within a band follows this mapping.
CODEC_INFO = {
    "UNCOMPRESSED": (BASELINE, "Uncompressed *(reference)*"),
    "RAW64": (BASELINE, "Uncompressed *(reference)*"),
    "UNCOMPRESSED64": (BASELINE, "Uncompressed *(reference)*"),
    "BP": (CODECS, "BP"),
    "BP64": (CODECS, "BP"),
    "PFOR": (CODECS, "PFOR"),
    "PFOR64": (CODECS, "PFOR"),
    "FOR": (CODECS, "FOR"),
    "FOR64": (CODECS, "FOR"),
    "DICT": (CODECS, "DICT"),
    "DICT64": (CODECS, "DICT"),
    "RLE": (CODECS, "RLE"),
    "RLE64": (CODECS, "RLE"),
    "FREQUENCY": (CODECS, "Frequency"),
    "FREQUENCY64": (CODECS, "Frequency"),
    "SIS_HALVES": (CODECS, "SubIntSplit (fixed split)"),
    "SIS64_HALVES": (CODECS, "SubIntSplit (fixed split)"),
    "SIS_PLANNED": (CODECS, "SubIntSplit (planned)"),
    "SIS64_PLANNED": (CODECS, "SubIntSplit (planned)"),
    "AUTO_BASELINE": (END_TO_END, "BtrBlocks (auto, without SIS)"),
    "AUTO_BASELINE64": (END_TO_END, "BtrBlocks (auto, without SIS)"),
    "AUTO_WITH_SIS": (END_TO_END, "BtrBlocks (auto, with SIS)"),
    "AUTO_WITH_SIS64": (END_TO_END, "BtrBlocks (auto, with SIS)"),
}
BAND_ORDER = [BASELINE, CODECS, END_TO_END]

# Codecs seen in the CSV but absent from CODEC_INFO, reported at the end.
UNKNOWN_CODECS: set[str] = set()

# Datasets in narrative order: real data first, then the generated ones, with
# the control last.
DATASET_ORDER = ["tweet_ids", "snowflake", "increasing", "uniform"]
DATASET_TITLE = {
    "tweet_ids": "tweet_ids — real Twitter identifiers",
    "snowflake": "snowflake — generated, dense burst",
    "increasing": "increasing — monotone, small steps",
    "uniform": "uniform — random *(control)*",
}

# (header, csv field, higher_is_better, formatter)
METRICS = [
    ("Ratio", "ratio", True, lambda v: f"{v:.2f}"),
    ("Encode ms", "encode_ms", False, lambda v: f"{v:,.0f}"),
    ("Decode ms", "decode_ms", False, lambda v: f"{v:.2f}"),
    ("Gather ms", "gather_clustered_ms", False, lambda v: f"{v:.2f}"),
    ("Point ms", "point_ms", False, lambda v: f"{v:.2f}"),
]


def parse_sub_encodings(plan: str) -> str:
    """Sub-schemes of a non-SubIntSplit codec, from its fullDescription.

    Cascading schemes describe themselves as e.g.
        RLE  -> ([valueType] values) BP  -> ([int] counts) PFOR
    so the label/scheme pairs can be lifted directly. A scheme with no children
    describes itself as just its own name.
    """
    pairs = re.findall(r"\(\[[^\]]*\]\s*(\w+)\)\s*([A-Z0-9_]+)", plan)
    if not pairs:
        return "—"
    return ", ".join(f"{label}:{scheme}" for label, scheme in pairs)


def format_sections(sections: list[dict]) -> str:
    """Sub-encodings of a SubIntSplit plan, from the structured section rows.

    Preferred over parsing the plan string: these carry the bit ranges the
    planner chose, which is the part worth seeing.
    """
    return " · ".join(f"[{s['bit_start']}-{s['bit_end']}] {s['actual']}" for s in sections)


def load(results_path: Path, sections_path: Path | None):
    with results_path.open() as fh:
        results = list(csv.DictReader(fh))

    sections = defaultdict(list)
    if sections_path and sections_path.exists():
        with sections_path.open() as fh:
            for row in csv.DictReader(fh):
                key = (row["width"], row["dataset"], row["codec"], row["block_size"])
                sections[key].append(row)
    return results, sections


def build_rows(rows: list[dict], sections, trivial_ratio: float):
    """Order rows into bands and decide which may win a column.

    A row is barred from winning anything when it is the uncompressed reference
    or when it compressed essentially nothing. Both cases are the same mistake:
    on snowflake, DICT falls back to uncompressed, reports ratio 1.00, and then
    posts the fastest decode, gather and point times in the table. Letting it
    win three columns for not compressing would be worse than useless, and the
    raw reference would sweep every speed column on every dataset.
    """
    banded = defaultdict(list)
    for row in rows:
        info = CODEC_INFO.get(row["codec"])
        if info is None:
            # A codec added to the benchmark but not to CODEC_INFO would
            # otherwise vanish from the tables without a trace.
            UNKNOWN_CODECS.add(row["codec"])
            continue
        band, display = info
        ratio = float(row["ratio"])
        key = (row["width"], row["dataset"], row["codec"], row["block_size"])
        section_rows = sections.get(key, [])

        entry = {
            "display": display,
            "values": {field: float(row[field]) for _, field, _, _ in METRICS},
            "eligible": band != BASELINE and ratio > trivial_ratio,
            "sub": format_sections(section_rows) if section_rows
                   else parse_sub_encodings(row["plan"]),
        }
        banded[band].append((list(CODEC_INFO).index(row["codec"]), entry))

    ordered = {}
    for band in BAND_ORDER:
        ordered[band] = [entry for _, entry in sorted(banded[band], key=lambda p: p[0])]
    return ordered


def pick_winners(ordered):
    """Best eligible value per metric, or None if nothing qualifies."""
    everything = [e for band in BAND_ORDER for e in ordered[band]]
    eligible = [e for e in everything if e["eligible"]]
    winners = {}
    for _, field, higher_better, _ in METRICS:
        if not eligible:
            winners[field] = None
            continue
        chooser = max if higher_better else min
        winners[field] = chooser(e["values"][field] for e in eligible)
    return winners


def render_table(ordered, winners) -> list[str]:
    headers = ["Codec"] + [h for h, _, _, _ in METRICS] + ["Sub-encodings"]
    lines = [
        "| " + " | ".join(headers) + " |",
        "|" + "---|" * len(headers),
    ]
    # Markdown has no in-table horizontal rule, so a row of box-drawing runs
    # stands in for one.
    divider = "| " + " | ".join(["───────────────"] + ["───"] * (len(headers) - 1)) + " |"

    populated = [band for band in BAND_ORDER if ordered[band]]
    for index, band in enumerate(populated):
        if index > 0:
            lines.append(divider)
        for entry in ordered[band]:
            cells = [entry["display"]]
            for _, field, _, fmt in METRICS:
                value = entry["values"][field]
                text = fmt(value)
                if entry["eligible"] and winners[field] is not None and value == winners[field]:
                    text = f"**{text}**"
                cells.append(text)
            cells.append(entry["sub"])
            lines.append("| " + " | ".join(cells) + " |")
    return lines


def render_block_size(results, sections, block_size: str, trivial_ratio: float) -> str:
    rows_here = [r for r in results if r["block_size"] == block_size]
    datasets = sorted(
        {r["dataset"] for r in rows_here},
        key=lambda d: DATASET_ORDER.index(d) if d in DATASET_ORDER else len(DATASET_ORDER),
    )

    out = [
        f"# Results at block_size = {int(block_size):,}",
        "",
        "Ratio is raw bytes over encoded bytes, so higher is better; every time is milliseconds,",
        "lower better. **Bold marks the winner of a column.**",
        "",
        "The uncompressed reference and any codec that compressed essentially nothing"
        f" (ratio ≤ {trivial_ratio}) are shown but never marked as winners — otherwise a codec that"
        " fell back to storing the data verbatim would win the speed columns for doing no work.",
        "",
        "Gather uses the *clustered* trace. Uniform gather touches every chunk once the request count"
        " exceeds the chunk count, so it degenerates into a full column decode for every codec and"
        " separates nothing.",
        "",
    ]

    for dataset in datasets:
        out.append(f"## {DATASET_TITLE.get(dataset, dataset)}")
        out.append("")
        for width in sorted({r["width"] for r in rows_here if r["dataset"] == dataset}, key=int):
            subset = [r for r in rows_here if r["dataset"] == dataset and r["width"] == width]
            ordered = build_rows(subset, sections, trivial_ratio)
            winners = pick_winners(ordered)

            out.append(f"### {width}-bit")
            out.append("")
            out.extend(render_table(ordered, winners))
            out.append("")
            if all(w is None for w in winners.values()):
                out.append(
                    "*Nothing is marked: no codec achieved compression on this dataset, so no"
                    " result here is a win.*"
                )
                out.append("")
    return "\n".join(out)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--results", type=Path, required=True)
    parser.add_argument("--sections", type=Path)
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument(
        "--trivial-ratio",
        type=float,
        default=1.01,
        help="at or below this ratio a row cannot win a column (default: 1.01)",
    )
    parser.add_argument(
        "--regenerate-cmd",
        default="tools/subintsplit/run_benchmarks.sh",
        help="command shown in the generated README as how to regenerate these tables"
        " (default: tools/subintsplit/run_benchmarks.sh)",
    )
    args = parser.parse_args()

    results, sections = load(args.results, args.sections)
    if not results:
        raise SystemExit(f"no rows in {args.results}")

    args.out_dir.mkdir(parents=True, exist_ok=True)
    block_sizes = sorted({r["block_size"] for r in results}, key=int)

    written = []
    for block_size in block_sizes:
        path = args.out_dir / f"tables-bs{block_size}.md"
        path.write_text(render_block_size(results, sections, block_size, args.trivial_ratio))
        written.append(path)

    # Keep the inputs beside the tables, so a copied directory stays self-contained.
    for source in (args.results, args.sections):
        if source and source.exists():
            shutil.copy(source, args.out_dir / source.name)

    headline = "65536" if "65536" in block_sizes else block_sizes[-1]
    index = [
        "# Benchmark results",
        "",
        "Generated from `subintsplit_bench` output by `tools/subintsplit/make_tables.py`."
        " Do not edit by hand — regenerate with:",
        "",
        "```",
        args.regenerate_cmd,
        "```",
        "",
        "Each file compares every integer codec on every dataset at one block size. Start with"
        f" [block size {int(headline):,}](tables-bs{headline}.md), the BtrBlocks default.",
        "",
        "| Block size | Tables |",
        "|---|---|",
    ]
    for block_size in block_sizes:
        index.append(f"| {int(block_size):,} | [tables-bs{block_size}.md](tables-bs{block_size}.md) |")
    index += [
        "",
        "Block size is swept because it is the dominant lever on both decode and gather:"
        " sub-schemes have no range decode, so a plan with N sections makes N full passes per chunk,"
        " and a gather pays for whole chunks it barely touches.",
        "",
        "Raw inputs, copied here so this directory stands alone:"
        f" `{args.results.name}`" + (f", `{args.sections.name}`" if args.sections else "") + ".",
        "",
        "Narrative discussion of these numbers, with the dataset properties that explain them,"
        " is in [docs/subintsplit.md](../docs/subintsplit.md).",
        "",
    ]
    (args.out_dir / "README.md").write_text("\n".join(index))

    print(f"wrote {len(written)} table file(s) plus README.md to {args.out_dir}")
    if UNKNOWN_CODECS:
        print(
            "warning: omitted from the tables because they have no entry in CODEC_INFO: "
            + ", ".join(sorted(UNKNOWN_CODECS))
        )


if __name__ == "__main__":
    main()
