"""SQLite schema, migration and job claiming for quartusq.

One database file, WAL mode, two tables: jobs and job_events. All access
goes through this module so the schema is defined exactly once.
"""
from __future__ import annotations

import sqlite3
import subprocess
import time
from pathlib import Path
from typing import Any, Optional

DEFAULT_DB_PATH = Path(__file__).resolve().parent.parent / "quartusq.db"

SCHEMA = """
CREATE TABLE IF NOT EXISTS jobs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    created_at REAL NOT NULL,
    started_at REAL,
    finished_at REAL,
    state TEXT NOT NULL DEFAULT 'queued',
    phase TEXT,
    priority INTEGER NOT NULL DEFAULT 100,
    queue_name TEXT NOT NULL DEFAULT 'quartus-local',
    revision TEXT NOT NULL,
    device TEXT,
    seed INTEGER,
    git_ref TEXT,
    git_commit TEXT,
    dirty_patch TEXT,
    label TEXT,
    command TEXT,
    build_dir TEXT,
    pid INTEGER,
    exit_code INTEGER,
    setup_wns_ns REAL,
    hold_wns_ns REAL,
    recovery_wns_ns REAL,
    removal_wns_ns REAL,
    minpulse_wns_ns REAL,
    m10k_used INTEGER,
    m10k_total INTEGER,
    alm_used INTEGER,
    alm_total INTEGER,
    dsp_used INTEGER,
    dsp_total INTEGER,
    qgate_result TEXT,
    qgate_failures INTEGER,
    rbf_sha256 TEXT,
    error_summary TEXT,
    sweep_id INTEGER
);

CREATE TABLE IF NOT EXISTS job_events (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    job_id INTEGER NOT NULL,
    at REAL NOT NULL,
    level TEXT NOT NULL,
    phase TEXT,
    message TEXT,
    FOREIGN KEY (job_id) REFERENCES jobs(id)
);

CREATE TABLE IF NOT EXISTS sweeps (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    created_at REAL NOT NULL,
    revision TEXT NOT NULL,
    label TEXT,
    git_ref TEXT,
    git_commit TEXT,
    seeds TEXT NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_jobs_state ON jobs(state);
CREATE INDEX IF NOT EXISTS idx_job_events_job_id ON job_events(job_id);
"""


def _migrate(conn: sqlite3.Connection) -> None:
    """Add columns/tables that didn't exist in the Day 1 schema. Every step
    checks PRAGMA table_info first: a DB already on the new schema (or a
    brand-new DB created straight from SCHEMA above, which already has the
    column) must not hit 'duplicate column name'. The sweep_id index is
    created here, AFTER the column is guaranteed to exist, rather than in
    the static SCHEMA script above — a Day 1 database already has a `jobs`
    table (so `CREATE TABLE IF NOT EXISTS` is a no-op) but not the column,
    and an index on a column that isn't there yet fails the whole
    executescript before this function ever runs.
    """
    cols = {row["name"] for row in conn.execute("PRAGMA table_info(jobs)")}
    if "sweep_id" not in cols:
        conn.execute("ALTER TABLE jobs ADD COLUMN sweep_id INTEGER")
    conn.execute("CREATE INDEX IF NOT EXISTS idx_jobs_sweep_id ON jobs(sweep_id)")
    conn.commit()

# States and phases are enforced in application code, not SQL CHECK
# constraints, so a future state can be added without a migration.
STATES = (
    "queued", "preparing", "running", "timing_analysis", "qgate",
    "passed", "failed", "cancel_requested", "cancelled", "aborted", "stale",
)
PHASES = (
    "prepare", "analysis_synthesis", "fitter", "assembler", "timequest",
    "reports", "qlog", "qgate", "archive",
)

TERMINAL_STATES = {"passed", "failed", "cancelled", "aborted", "stale"}


def connect(db_path: Path | str = DEFAULT_DB_PATH) -> sqlite3.Connection:
    db_path = Path(db_path)
    db_path.parent.mkdir(parents=True, exist_ok=True)
    conn = sqlite3.connect(str(db_path), timeout=30.0)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("PRAGMA foreign_keys=ON")
    conn.executescript(SCHEMA)
    conn.commit()
    _migrate(conn)
    return conn


def submit_job(
    conn: sqlite3.Connection,
    *,
    revision: str,
    device: Optional[str] = None,
    seed: Optional[int] = None,
    label: Optional[str] = None,
    priority: int = 100,
    queue_name: str = "quartus-local",
    git_ref: Optional[str] = None,
    git_commit: Optional[str] = None,
    dirty_patch: Optional[str] = None,
    command: Optional[str] = None,
    build_dir: Optional[str] = None,
    sweep_id: Optional[int] = None,
) -> int:
    cur = conn.execute(
        """
        INSERT INTO jobs (
            created_at, state, priority, queue_name, revision, device, seed,
            git_ref, git_commit, dirty_patch, label, command, build_dir, sweep_id
        ) VALUES (?, 'queued', ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        """,
        (
            time.time(), priority, queue_name, revision, device, seed,
            git_ref, git_commit, dirty_patch, label, command, build_dir, sweep_id,
        ),
    )
    conn.commit()
    return cur.lastrowid


def create_sweep(
    conn: sqlite3.Connection, *, revision: str, seeds: list[int],
    label: Optional[str] = None, git_ref: Optional[str] = None,
    git_commit: Optional[str] = None,
) -> int:
    cur = conn.execute(
        "INSERT INTO sweeps (created_at, revision, label, git_ref, git_commit, seeds) "
        "VALUES (?, ?, ?, ?, ?, ?)",
        (time.time(), revision, label, git_ref, git_commit, ",".join(str(s) for s in seeds)),
    )
    conn.commit()
    return cur.lastrowid


def get_sweep(conn: sqlite3.Connection, sweep_id: int) -> Optional[sqlite3.Row]:
    return conn.execute("SELECT * FROM sweeps WHERE id=?", (sweep_id,)).fetchone()


def list_sweeps(conn: sqlite3.Connection, limit: int = 100) -> list[sqlite3.Row]:
    return conn.execute("SELECT * FROM sweeps ORDER BY id DESC LIMIT ?", (limit,)).fetchall()


def list_jobs_for_sweep(conn: sqlite3.Connection, sweep_id: int) -> list[sqlite3.Row]:
    return conn.execute("SELECT * FROM jobs WHERE sweep_id=? ORDER BY seed ASC", (sweep_id,)).fetchall()


def claim_next_job(conn: sqlite3.Connection, queue_name: str = "quartus-local") -> Optional[sqlite3.Row]:
    """Atomically claim the oldest queued job with the highest priority.

    Priority: lower number = runs first (matches 'priority 100' default,
    a 'priority 10' job jumps the queue). Returns the claimed row, or None
    if nothing is queued. Uses a single UPDATE...RETURNING-style pattern
    via rowid selection + conditional UPDATE, checked by rowcount, so two
    workers racing on claim never both win.
    """
    cur = conn.execute(
        """
        SELECT id FROM jobs
        WHERE state = 'queued' AND queue_name = ?
        ORDER BY priority ASC, created_at ASC
        LIMIT 1
        """,
        (queue_name,),
    )
    row = cur.fetchone()
    if row is None:
        return None
    job_id = row["id"]
    upd = conn.execute(
        "UPDATE jobs SET state='preparing', started_at=? WHERE id=? AND state='queued'",
        (time.time(), job_id),
    )
    conn.commit()
    if upd.rowcount == 0:
        # Lost the race (or another claimant beat us); caller retries.
        return None
    return conn.execute("SELECT * FROM jobs WHERE id=?", (job_id,)).fetchone()


def set_job_fields(conn: sqlite3.Connection, job_id: int, **fields: Any) -> None:
    if not fields:
        return
    cols = ", ".join(f"{k}=?" for k in fields)
    conn.execute(f"UPDATE jobs SET {cols} WHERE id=?", (*fields.values(), job_id))
    conn.commit()


def mark_running(conn: sqlite3.Connection, job_id: int, pid: int) -> bool:
    """Move preparing -> running. Returns False if the job is no longer
    'preparing' -- in practice, a cancel arrived while the worktree was
    being prepared. An unconditional write here overwrote that
    cancel_requested with 'running', and the compile went on for its full
    length with the CLI insisting it had been cancelled (job 32)."""
    upd = conn.execute(
        "UPDATE jobs SET state='running', pid=? WHERE id=? AND state='preparing'",
        (pid, job_id),
    )
    conn.commit()
    return upd.rowcount > 0


def finish_job(conn: sqlite3.Connection, job_id: int, *, state: str, **fields: Any) -> None:
    assert state in TERMINAL_STATES, f"finish_job needs a terminal state, got {state!r}"
    set_job_fields(conn, job_id, state=state, finished_at=time.time(), **fields)
    _announce(conn, job_id, state)


# Where a finished job is announced. One line per completion, appended, never
# rewritten -- a watcher can tail it, and a reader who was away still sees
# every build that finished while they were gone.
NOTIFY_LOG = DEFAULT_DB_PATH.parent / "quartusq-finished.log"


def _announce(conn: sqlite3.Connection, job_id: int, state: str) -> None:
    """Record and broadcast that a job reached a terminal state.

    This belongs in finish_job rather than in the worker because the worker
    has three separate completion paths -- passed, failed, cancelled -- and
    one of them will eventually be added without a notification beside it.
    Announcing where the state is written means that cannot happen.

    A build that finishes with nobody watching is the failure this fixes: a
    sweep build completed after four hours of investigation and nothing said
    so, because the notification lived in a shell loop that had exited.
    """
    try:
        job = get_job(conn, job_id)
        label = (job["label"] or "") if job else ""
        rev = (job["revision"] or "?") if job else "?"
        line = (f"{time.strftime('%Y-%m-%d %H:%M:%S')} job {job_id} "
                f"{state} {rev} {label}")

        with open(NOTIFY_LOG, "a") as fh:
            fh.write(line + "\n")

        # Desktop notification is best-effort: no display, no notify-send,
        # or a headless session must not turn a finished build into a
        # crashed worker.
        subprocess.run(
            ["notify-send", "-u", "normal", f"quartusq: {state}", line],
            capture_output=True, timeout=10, check=False,
        )
    except Exception:                       # noqa: BLE001
        pass


def get_job(conn: sqlite3.Connection, job_id: int) -> Optional[sqlite3.Row]:
    return conn.execute("SELECT * FROM jobs WHERE id=?", (job_id,)).fetchone()


def list_jobs(conn: sqlite3.Connection, *, state: Optional[str] = None, limit: int = 100) -> list[sqlite3.Row]:
    if state:
        return conn.execute(
            "SELECT * FROM jobs WHERE state=? ORDER BY id DESC LIMIT ?", (state, limit)
        ).fetchall()
    return conn.execute("SELECT * FROM jobs ORDER BY id DESC LIMIT ?", (limit,)).fetchall()


def request_cancel(conn: sqlite3.Connection, job_id: int) -> bool:
    upd = conn.execute(
        "UPDATE jobs SET state='cancel_requested' WHERE id=? AND state IN ('queued','preparing','running','timing_analysis','qgate')",
        (job_id,),
    )
    conn.commit()
    return upd.rowcount > 0


def log_event(conn: sqlite3.Connection, job_id: int, level: str, message: str, phase: Optional[str] = None) -> None:
    conn.execute(
        "INSERT INTO job_events (job_id, at, level, phase, message) VALUES (?, ?, ?, ?, ?)",
        (job_id, time.time(), level, phase, message),
    )
    conn.commit()


def reap_stale_running_jobs(conn: sqlite3.Connection) -> list[int]:
    """Recovery on worker start: any job left 'running'/'preparing'/etc with
    no live process from a previous worker is dead, not actually running.
    Called once at worker startup before claiming new work.
    """
    import os

    reaped = []
    # cancel_requested is in this list on purpose. A job cancelled while the
    # worker was restarting keeps that state with no process behind it, and
    # nothing else ever resolves it -- claim_next_job only takes 'queued'.
    # Observed: a cancelled job reappeared as running on the next worker
    # start, and the build it was meant to replace waited behind it.
    rows = conn.execute(
        "SELECT id, pid, state FROM jobs WHERE state IN "
        "('preparing','running','timing_analysis','qgate','cancel_requested')"
    ).fetchall()
    for row in rows:
        pid = row["pid"]
        alive = False
        if pid:
            try:
                os.kill(pid, 0)
                alive = True
            except (OSError, ProcessLookupError):
                alive = False
        if not alive:
            # A cancel that outlived its process was honoured, not lost:
            # recording it as stale would blame the restart for something
            # the operator asked for.
            if row["state"] == "cancel_requested":
                finish_job(conn, row["id"], state="cancelled",
                           error_summary="cancelled; process already gone")
            else:
                finish_job(conn, row["id"], state="stale",
                           error_summary="worker restarted, no live process for this job")
            reaped.append(row["id"])
    return reaped
