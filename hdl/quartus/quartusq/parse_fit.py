"""Extract utilization metrics from a Quartus .fit.rpt (or compile log
containing the same section) — specifically the "Fitter Summary" table.

Golden fixture (read-only, not shipped):
  ~/projects/bladerf/hdl/quartus/hostedxA4-2026-09-09_19.33.46/hostedxA4.fit.rpt

    ; Fitter Summary                                                                        ;
    +---------------------------------+-----------------------------------------------------+
    ; Fitter Status                   ; Successful - Wed Sep  9 19:32:40 2026               ;
    ...
    ; Logic utilization (in ALMs)     ; 7,560 / 18,480 ( 41 % )                             ;
    ; Total registers                 ; 16085                                               ;
    ...
    ; Total block memory bits         ; 2,245,056 / 3,153,920 ( 71 % )                      ;
    ; Total RAM Blocks                ; 282 / 308 ( 92 % )                                  ;
    ; Total DSP Blocks                ; 12 / 66 ( 18 % )                                    ;
    ...
    +---------------------------------+-----------------------------------------------------+

⛔ M10K count is "Total RAM Blocks" (block count), NOT "Total block memory
bits" (bit capacity, a different number). A later "Fitter DSP Block Usage
Summary" / "Fitter RAM Summary" section repeats "Total DSP Blocks" with a
different column layout (separate trailing "%" column, no "/ N ( %)" in
one cell) — parsing must be scoped to the "Fitter Summary" block only, not
grepped globally, or the wrong occurrence can be picked up depending on
section order.
"""
from __future__ import annotations

import re
from typing import Any

_SECTION_START = re.compile(r";\s*Fitter Summary\s*;")
_SECTION_END = re.compile(r"^\+-+\+-+\+\s*$", re.MULTILINE)

_ROW_PATTERNS = {
    ("alm_used", "alm_total"): re.compile(r";\s*Logic utilization \(in ALMs\)\s*;\s*([\d,]+)\s*/\s*([\d,]+)"),
    ("m10k_used", "m10k_total"): re.compile(r";\s*Total RAM Blocks\s*;\s*([\d,]+)\s*/\s*([\d,]+)"),
    ("dsp_used", "dsp_total"): re.compile(r";\s*Total DSP Blocks\s*;\s*([\d,]+)\s*/\s*([\d,]+)"),
}


def _to_int(s: str) -> int:
    return int(s.replace(",", ""))


def _fitter_summary_block(text: str) -> str:
    start = _SECTION_START.search(text)
    if not start:
        return ""
    # The header line "; Fitter Summary ;" is immediately followed by the
    # divider that OPENS the table; skip that one and take the block up to
    # the divider that CLOSES it (the next one after the data rows).
    opening_divider = _SECTION_END.search(text, start.end())
    if not opening_divider:
        return text[start.end():start.end() + 4000]
    closing_divider = _SECTION_END.search(text, opening_divider.end())
    return (
        text[opening_divider.end():closing_divider.start()]
        if closing_divider
        else text[opening_divider.end():opening_divider.end() + 4000]
    )


def parse_fit(text: str) -> dict[str, Any]:
    """Return alm/m10k/dsp used+total, scoped to the Fitter Summary block.
    Empty dict if the section isn't present.
    """
    block = _fitter_summary_block(text)
    if not block:
        return {}
    result: dict[str, Any] = {}
    for (used_field, total_field), pattern in _ROW_PATTERNS.items():
        m = pattern.search(block)
        if m:
            result[used_field] = _to_int(m.group(1))
            result[total_field] = _to_int(m.group(2))
    return result


def fitter_status_successful(text: str) -> bool:
    block = _fitter_summary_block(text)
    return "Fitter Status" in block and "Successful" in block


def parse_fit_file(path) -> dict[str, Any]:
    from pathlib import Path

    return parse_fit(Path(path).read_text(errors="replace"))
