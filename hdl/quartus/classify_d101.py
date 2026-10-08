#!/usr/bin/env python3
"""Inventory every Design Assistant D101 structure by its source endpoint.

This is an audit aid, not a waiver: it fails on malformed, missing, duplicate,
or unknown source rows and prints the exact normalized source families that
must be backed by the post-fit CDC evidence.
"""

from __future__ import annotations

import argparse
import collections
import re
import sys
from pathlib import Path


RULE = re.compile(r"Rule D101:.* - Structure (\d+)")
SUMMARY = re.compile(r";\s+Structure (\d+)\s*;")
KNOWN = (
    ("held-handshake", re.compile(r"(?:^|\|)handshake:")),
    ("tamer-payload", re.compile(r"(?:^|\|)(?:time_tamer:|compare_payload)")),
    ("adi-up-xfer", re.compile(r"(?:^|\|)up_xfer_cntrl:")),
    ("adi-xfer-status", re.compile(r"(?:^|\|)up_xfer_status:")),
    ("adi-clock-monitor", re.compile(r"(?:^|\|)up_clock_mon:")),
)


def field(line: str) -> str:
    """Return the final populated semicolon-delimited report field."""
    return next((part.strip() for part in reversed(line.split(";")) if part.strip()), "")


def inventory(path: Path) -> tuple[collections.Counter[str], int]:
    lines = path.read_text(errors="replace").splitlines()
    structures: dict[int, str] = {}
    rule_ids: list[int] = []

    for index, line in enumerate(lines):
        match = RULE.search(line)
        if not match:
            continue
        number = int(match.group(1))
        rule_ids.append(number)
        source = ""
        if number <= 30:
            for candidate in lines[index + 1 : index + 4]:
                if "Source node(s)" in candidate:
                    source = field(candidate)
                    break
        else:
            for candidate in lines[index + 1 : index + 3]:
                summary = SUMMARY.match(candidate)
                if summary and int(summary.group(1)) == number:
                    source = field(candidate)
                    break
        if not source:
            raise ValueError(f"D101 structure {number} has no source endpoint row")
        if number in structures:
            raise ValueError(f"duplicate D101 structure {number}")
        structures[number] = re.sub(r"\[\d+\]", "[*]", source)

    expected = list(range(1, len(rule_ids) + 1))
    if sorted(rule_ids) != expected:
        raise ValueError("D101 structure IDs are missing or duplicated")
    if set(structures) != set(expected):
        raise ValueError("not every D101 structure has a parsed source")

    counts: collections.Counter[str] = collections.Counter()
    unknown: list[str] = []
    for source in structures.values():
        for category, pattern in KNOWN:
            if pattern.search(source):
                counts[f"{category}: {source}"] += 1
                break
        else:
            unknown.append(source)
    if unknown:
        examples = "\n".join(f"  {source}" for source in unknown[:10])
        raise ValueError(f"{len(unknown)} D101 sources are unclassified:\n{examples}")
    return counts, len(structures)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path, help="Quartus Design Assistant .drc.rpt")
    args = parser.parse_args()
    try:
        counts, total = inventory(args.report)
    except (OSError, ValueError) as error:
        print(f"D101 inventory failed: {error}", file=sys.stderr)
        return 1
    print(f"D101 source structures: {total}")
    for key, count in sorted(counts.items()):
        print(f"{count:4d} {key}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
