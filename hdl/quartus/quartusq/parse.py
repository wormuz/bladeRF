"""Top-level metric extraction for a finished job: combines parse_sta
(timing) and parse_fit (utilization) against the report files a Quartus
run leaves in its build directory, plus the plain compile-log fitter
success check the worker uses as its primary gate.

The old single-file regex set that lived here (grep for "Setup...Worst-
case Slack" anywhere in the text) did not match the real .sta.rpt format
and mapped m10k to the wrong column ("Total block memory bits" instead of
"Total RAM Blocks"). Replaced by parse_sta.py/parse_fit.py, which are
scoped to the actual report sections and verified against real Quartus
output in ~/projects/bladerf/hdl/quartus/hostedxA4-*/.
"""
from __future__ import annotations

import hashlib
from pathlib import Path
from typing import Any, Optional

from . import parse_fit
from . import parse_sta

FITTER_SUCCESS_LINE = "Quartus Prime Fitter was successful"


def parse_log(log_text: str) -> dict[str, Any]:
    """Best-effort metrics straight out of the streamed compile log, in
    case no separate .sta.rpt/.fit.rpt exist yet (log usually embeds the
    same report sections inline).
    """
    result: dict[str, Any] = {}
    result.update(parse_sta.parse_sta(log_text))
    result.update(parse_fit.parse_fit(log_text))
    return result


def parse_job_reports(build_dir: Path, revision: str) -> dict[str, Any]:
    """Read <revision>.sta.rpt and <revision>.fit.rpt from build_dir (or its
    output_files/ subdirectory) and merge their metrics. Missing files
    contribute nothing rather than raising.
    """
    result: dict[str, Any] = {}
    for candidate_dir in (build_dir, build_dir / "output_files"):
        sta = candidate_dir / f"{revision}.sta.rpt"
        if sta.exists():
            result.update(parse_sta.parse_sta_file(sta))
        fit = candidate_dir / f"{revision}.fit.rpt"
        if fit.exists():
            result.update(parse_fit.parse_fit_file(fit))
    return result


def fitter_succeeded(log_text: str) -> bool:
    """The only trustworthy success signal per qgate's own doc comment:
    the process exit code is not proof, this literal line is.
    """
    return FITTER_SUCCESS_LINE in log_text


def parse_log_file(log_path: Path) -> dict[str, Any]:
    return parse_log(log_path.read_text(errors="replace"))


def sha256_file(path: Path) -> Optional[str]:
    if not path.exists():
        return None
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()
