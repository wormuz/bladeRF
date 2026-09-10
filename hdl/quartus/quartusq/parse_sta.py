"""Extract timing metrics from a Quartus .sta.rpt (or compile log containing
the same section) — specifically the "Multicorner Timing Analysis Summary"
table's "Worst-case Slack" row, which is the true cross-corner worst number.

Golden fixtures checked against (read-only, not shipped):
  ~/projects/bladerf/hdl/quartus/hostedxA4-2026-09-09_19.33.46/hostedxA4.sta.rpt
and ~15 sibling hostedxA4-* dirs, all with the same table layout:

    ; Multicorner Timing Analysis Summary                                    ;
    +----...--+----------+----------+----------+---------+---------------------+
    ; Clock                                                ; Setup ; Hold ; Recovery ; Removal ; Minimum Pulse Width ;
    +----...--+----------+----------+----------+---------+---------------------+
    ; Worst-case Slack                                     ; 0.690 ; 0.015; 2.536    ; 0.183   ; 0.978               ;
    ;  <per-clock rows, not used here>                     ; ...

Per-clock rows below "Worst-case Slack" use N/A for columns that don't
apply to that clock; only the "Worst-case Slack" row itself is parsed —
it is already the minimum across every clock/corner, which is exactly
setup_wns_ns et al.

Do NOT regex "Setup"/"Hold" globally across the file: those words appear
in dozens of per-corner section titles and per-clock table headers
elsewhere in the report, producing false matches.
"""
from __future__ import annotations

import re
from typing import Any

_SECTION_HEADER = re.compile(r"Multicorner Timing Analysis Summary")
_WORST_CASE_ROW = re.compile(
    r"^\s*;\s*Worst-case Slack\s*;\s*"
    r"(N/A|-?\d+\.\d+)\s*;\s*"
    r"(N/A|-?\d+\.\d+)\s*;\s*"
    r"(N/A|-?\d+\.\d+)\s*;\s*"
    r"(N/A|-?\d+\.\d+)\s*;\s*"
    r"(N/A|-?\d+\.\d+)\s*;",
    re.MULTILINE,
)

_FIELDS = ("setup_wns_ns", "hold_wns_ns", "recovery_wns_ns", "removal_wns_ns", "minpulse_wns_ns")


def _to_float(s: str) -> Any:
    return None if s == "N/A" else float(s)


def parse_sta(text: str) -> dict[str, Any]:
    """Return the five *_wns_ns fields from the Multicorner Timing Analysis
    Summary's "Worst-case Slack" row. Empty dict if the section or row is
    not found (e.g. .sta.rpt from a failed/partial compile).

    "Multicorner Timing Analysis Summary" also appears once as a plain
    entry in the report's table-of-contents, far from any data row — every
    match of the header is tried in order and the first one actually
    followed (within a bounded window) by a Worst-case Slack row wins.
    """
    for header in _SECTION_HEADER.finditer(text):
        m = _WORST_CASE_ROW.search(text, header.end(), header.end() + 2000)
        if m:
            return {field: _to_float(m.group(i + 1)) for i, field in enumerate(_FIELDS)}
    return {}


def parse_sta_file(path) -> dict[str, Any]:
    from pathlib import Path

    return parse_sta(Path(path).read_text(errors="replace"))
