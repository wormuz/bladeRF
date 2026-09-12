"""Compare two finished jobs: metric deltas with an explicit better/worse
direction, not just a bare number difference.

Direction conventions:
  slack (setup/hold/...)  higher is better (more margin)
  utilization (alm/m10k/dsp)  lower is better (more headroom left)
  qgate_failures            lower is better
"""
from __future__ import annotations

from typing import Any

_HIGHER_IS_BETTER = {
    "setup_wns_ns", "hold_wns_ns", "recovery_wns_ns", "removal_wns_ns", "minpulse_wns_ns",
}
_LOWER_IS_BETTER = {
    "alm_used", "m10k_used", "dsp_used", "qgate_failures",
}

METRIC_FIELDS = sorted(_HIGHER_IS_BETTER | _LOWER_IS_BETTER)


def _direction(field: str, delta: float) -> str:
    if delta == 0:
        return "same"
    if field in _HIGHER_IS_BETTER:
        return "better" if delta > 0 else "worse"
    if field in _LOWER_IS_BETTER:
        return "better" if delta < 0 else "worse"
    return "n/a"


def compare_jobs(job_a, job_b) -> dict[str, Any]:
    """job_a/job_b are sqlite3.Row (or dict-like) from db.get_job. Returns
    per-field {a, b, delta, direction} for every metric field present on
    both, plus a top-level qgate_result comparison.
    """
    fields: dict[str, Any] = {}
    for field in METRIC_FIELDS:
        a_val = job_a[field] if field in job_a.keys() else None
        b_val = job_b[field] if field in job_b.keys() else None
        if a_val is None or b_val is None:
            fields[field] = {"a": a_val, "b": b_val, "delta": None, "direction": "not_evaluated"}
            continue
        delta = b_val - a_val
        fields[field] = {"a": a_val, "b": b_val, "delta": delta, "direction": _direction(field, delta)}

    qgate_a = job_a["qgate_result"] if "qgate_result" in job_a.keys() else None
    qgate_b = job_b["qgate_result"] if "qgate_result" in job_b.keys() else None

    return {
        "job_a": job_a["id"],
        "job_b": job_b["id"],
        "state_a": job_a["state"],
        "state_b": job_b["state"],
        "fields": fields,
        "qgate_same": qgate_a == qgate_b,
    }
