"""Seed sweeps as a group of jobs, and build-to-build comparison.

A sweep is the question "does this design close on seeds other than the one
that happened to work". Answering it by hand means N submits, N log reads and
a table typed from memory -- which is where wrong numbers come from. Here the
group is a row, each seed is an ordinary job, and the summary is computed
from what the jobs recorded.

Nothing here starts a build: sweep_submit only queues, the worker runs them
one at a time like any other job.
"""
from __future__ import annotations

import sqlite3
import statistics
from typing import Any, Optional

from . import db


def parse_seeds(text: str) -> list[int]:
    """"1,2,3" or "1 2 3" -> [1, 2, 3]. Duplicates are an error, not a
    silently deduplicated list: asking for the same seed twice means the
    caller believes it is getting two data points."""
    raw = [p for p in text.replace(",", " ").split() if p]
    if not raw:
        raise ValueError("no seeds given")
    seeds = []
    for p in raw:
        try:
            seeds.append(int(p))
        except ValueError:
            raise ValueError(f"seed {p!r} is not an integer") from None
    dupes = {s for s in seeds if seeds.count(s) > 1}
    if dupes:
        raise ValueError(f"duplicate seed(s): {sorted(dupes)}")
    return seeds


def sweep_submit(
    conn: sqlite3.Connection,
    *,
    revision: str,
    seeds: list[int],
    label: Optional[str] = None,
    git_ref: str = "HEAD",
    git_commit: Optional[str] = None,
    device: Optional[str] = None,
    priority: int = 300,
    **job_kwargs: Any,
) -> tuple[int, list[int]]:
    """Create the group, then one queued job per seed.

    Default priority 300 puts a sweep behind interactive work: a 12-seed
    sweep must not block the one build someone is waiting on.
    """
    sweep_id = db.create_sweep(
        conn, revision=revision, seeds=seeds, label=label,
        git_ref=git_ref, git_commit=git_commit,
    )

    job_ids = [
        db.submit_job(
            conn, revision=revision, seed=seed, label=label, priority=priority,
            git_ref=git_ref, git_commit=git_commit, device=device,
            sweep_id=sweep_id, **job_kwargs,
        )
        for seed in seeds
    ]
    return sweep_id, job_ids


def sweep_summary(conn: sqlite3.Connection, sweep_id: int) -> dict[str, Any]:
    """One row per seed plus medians. Medians are taken over the seeds that
    actually reported a number; a seed still queued contributes nothing and
    is not counted as a pass or a failure."""
    sweep = db.get_sweep(conn, sweep_id)
    if sweep is None:
        raise KeyError(f"no sweep {sweep_id}")

    jobs = db.list_jobs_for_sweep(conn, sweep_id)

    rows = [{
        "job_id": j["id"], "seed": j["seed"], "state": j["state"],
        "setup_wns_ns": j["setup_wns_ns"], "hold_wns_ns": j["hold_wns_ns"],
        "qgate_result": j["qgate_result"], "qgate_failures": j["qgate_failures"],
        "m10k_used": j["m10k_used"], "alm_used": j["alm_used"],
    } for j in jobs]

    finished = [r for r in rows if r["state"] in db.TERMINAL_STATES]
    passed = [r for r in finished if r["state"] == "passed"]

    def med(key: str) -> Optional[float]:
        vals = [r[key] for r in rows if r[key] is not None]
        return statistics.median(vals) if vals else None

    return {
        "sweep_id": sweep_id,
        "label": sweep["label"],
        "revision": sweep["revision"],
        "git_commit": sweep["git_commit"],
        "seeds": [r["seed"] for r in rows],
        "rows": rows,
        "median_setup_wns_ns": med("setup_wns_ns"),
        "median_hold_wns_ns": med("hold_wns_ns"),
        "passed": len(passed),
        "finished": len(finished),
        "total": len(rows),
        # Spelled out rather than left to the caller: "4/6" alone hides
        # whether the other two failed or have not run yet, and that is the
        # difference between "this design is marginal" and "wait".
        "pass_summary": (
            f"passed {len(passed)} of {len(finished)} finished"
            f" ({len(rows)} total, {len(rows) - len(finished)} not finished)"
        ),
    }


# The CLI calls this name; sweep_submit is kept as the descriptive alias so
# either spelling works from a script.
submit_sweep = sweep_submit


