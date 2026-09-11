"""Single-worker loop for quartusq: claims one job at a time, runs it,
streams its log, and never lets two builds run at once.

Two independent locks enforce that: the SQLite claim (UPDATE ... WHERE
state='queued', checked by rowcount) stops two workers from claiming the
same row, and an flock on QUARTUSQ_LOCK_PATH stops two worker processes
from being "running" concurrently even if they never touch the same row
(e.g. one is mid-build on job A while a second worker claims job B).
"""
from __future__ import annotations

import fcntl
import json
import os
import shlex
import subprocess
import threading
import sys
import time
from pathlib import Path
from typing import Optional

from . import archive
from . import db
from . import parse

QUARTUS_ROOT = Path(__file__).resolve().parent.parent  # hdl/quartus
REPO_ROOT = QUARTUS_ROOT.parent.parent               # the git repository
BUILDS_DIR = QUARTUS_ROOT / "builds"
LOCK_PATH = Path(os.environ.get("QUARTUSQ_LOCK_PATH", "/var/lock/quartusq.lock"))
POLL_INTERVAL_S = 2.0

# The real invocation, per contract in hdl/quartus/CLAUDE-facing doc:
# ~/soft/q25 bash ./build_bladerf.sh -b bladeRF-micro -r <rev> -s A4 -n Fast -S <seed>
# cwd must be hdl/quartus. QUARTUSQ_FAKE_COMMAND overrides this for
# selfchecks/tests so no real Quartus run is ever triggered by this module.
REAL_COMMAND_TEMPLATE = [
    "{q25}", "bash", "./build_bladerf.sh",
    "-b", "bladeRF-micro", "-r", "{revision}", "-s", "{size}",
    "-n", "Fast", "-S", "{seed}",
]

# build_bladerf.sh takes revision and size as separate arguments: -r hosted
# -s A4. "hostedxA4" is the name of the *artifact directory*, not a revision,
# and passing it as -r fails after submodule init with "Invalid Quartus
# project revision" -- a minute in, looking like a build failure rather than
# a typo. Checked at submit instead.
VALID_REVISIONS = ("hosted", "sweep", "headless", "adsb", "atsc_tx",
                   "fsk_bridge", "qpsk_tx")
VALID_SIZES = ("A4", "A5", "A9")


def split_revision_size(revision: str) -> tuple[str, str]:
    """Accept "hosted" or the artifact-directory spelling "hostedxA4".

    The second form is what every build directory on disk is called, so it
    is the one that comes to mind first; rejecting it outright would be
    correct and useless. Split it instead, and reject anything else.
    """
    for size in VALID_SIZES:
        suffix = f"x{size}"
        if revision.endswith(suffix):
            return revision[: -len(suffix)], size
    return revision, "A4"


def validate_revision(revision: str) -> tuple[str, str]:
    rev, size = split_revision_size(revision)
    if rev not in VALID_REVISIONS:
        raise ValueError(
            f"revision {rev!r} is not one of {', '.join(VALID_REVISIONS)}"
            f" (from {revision!r})"
        )
    return rev, size


def build_command(revision: str, seed: int, q25: str = "~/soft/q25") -> list[str]:
    # expanduser because this list goes straight to subprocess, with no shell
    # to expand the tilde: an unexpanded "~/soft/q25" is a FileNotFoundError
    # at spawn time, which reads like the wrapper is missing when it is not.
    q25 = os.path.expanduser(q25)
    rev, size = validate_revision(revision)
    return [
        p.format(q25=q25, revision=rev, size=size, seed=seed)
        for p in REAL_COMMAND_TEMPLATE
    ]


def job_build_dir(job: "db.sqlite3.Row") -> Path:
    sha7 = (job["git_commit"] or "nogit")[:7]
    name = f"{job['id']:06d}-{job['revision']}-seed{job['seed']}-{sha7}"
    return BUILDS_DIR / name


class SingleInstanceLock:
    """Advisory flock; raises if another worker already holds it."""

    def __init__(self, path: Path):
        self.path = path
        self._fh = None

    def acquire(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self._fh = open(self.path, "w")
        try:
            fcntl.flock(self._fh.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            self._fh.close()
            self._fh = None
            raise RuntimeError(f"another quartusq worker already holds {self.path}")
        self._fh.write(str(os.getpid()))
        self._fh.flush()

    def release(self) -> None:
        if self._fh is not None:
            fcntl.flock(self._fh.fileno(), fcntl.LOCK_UN)
            self._fh.close()
            self._fh = None


class Worker:
    def __init__(self, conn, queue_name: str = "quartus-local",
                 fake_command: Optional[str] = None,
                 db_path: Optional[Path] = None):
        self.conn = conn
        self.queue_name = queue_name
        # The cancel watcher runs on its own thread and so needs its own
        # connection -- sqlite3 objects belong to the thread that created
        # them. Kept as a path rather than a second connection so a Worker
        # built for a selfcheck, with an in-memory or temporary database,
        # still works.
        self.db_path = db_path or db.DEFAULT_DB_PATH
        # fake_command lets selfchecks/tests exercise the whole pipeline
        # (spawn, stream log, parse, qgate) without invoking real Quartus.
        self.fake_command = fake_command or os.environ.get("QUARTUSQ_FAKE_COMMAND")

    def recover_stale_jobs(self) -> list[int]:
        return db.reap_stale_running_jobs(self.conn)

    def run_forever(self) -> None:
        while True:
            self.run_one_cycle()
            time.sleep(POLL_INTERVAL_S)

    @staticmethod
    def foreign_quartus_running() -> bool:
        """Is a Quartus tool running that this worker did not start?

        The flock only stops a second *worker*. A build started by hand
        before the service existed is invisible to it, and starting a second
        one shares hdl/quartus/work/ -- overwritten reports, a corrupted
        database, two unusable results. Observed: a queued job launched into
        a hand-started build and both were spoiled.
        """
        if os.environ.get("QUARTUSQ_FAKE_COMMAND"):
            return False        # selfchecks never touch Quartus
        try:
            r = subprocess.run(["pgrep", "-f", "quartus_[a-z]"],
                               capture_output=True, text=True, timeout=10)
        except (OSError, subprocess.TimeoutExpired):
            return False        # cannot tell; do not deadlock the queue
        return bool(r.stdout.strip())

    def run_one_cycle(self) -> Optional[int]:
        # Checked before claiming, so a job is not taken out of the queue
        # only to sit blocked: it stays queued and visible in `list`.
        if self.foreign_quartus_running():
            return None
        job = db.claim_next_job(self.conn, self.queue_name)
        if job is None:
            return None
        self._run_job(job["id"])
        return job["id"]

    def _prepare_worktree(self, job_id: int, commit: str, build_dir: Path) -> Path:
        """Check the job's commit out into its own worktree.

        Without this the build runs in the main tree and reads whatever is
        there at the moment each file is opened -- Quartus reads sources
        throughout a run, not as a snapshot at the start. Editing during a
        build then produces a result describing no version of the design,
        silently: on 2026-09-11 that cost 3h25m on a compile that takes 19
        minutes, and the only symptom was the runtime.

        A worktree also makes the build reproducible: it is pinned to a
        commit, so its manifest names something that can be checked out
        again. Development continues in the main tree meanwhile.
        """
        wt = build_dir / "src"
        if wt.exists():
            return wt

        db.log_event(self.conn, job_id, "info",
                     f"worktree at {commit[:12]}", phase="prepare")
        subprocess.run(
            ["git", "worktree", "add", "--detach", str(wt), commit],
            cwd=str(REPO_ROOT), check=True,
            capture_output=True, text=True, timeout=300,
        )
        # Submodules are not carried over by worktree add, and the build
        # needs them: the ADI tree lives in one. build_bladerf.sh runs
        # submodule update itself, but from the worktree that is a no-op
        # unless the modules are initialised there first.
        subprocess.run(
            ["git", "submodule", "update", "--init", "--recursive"],
            cwd=str(wt), check=False,
            capture_output=True, text=True, timeout=1800,
        )
        return wt

    def _run_job(self, job_id: int) -> None:
        conn = self.conn
        job = db.get_job(conn, job_id)
        build_dir = job_build_dir(job)
        logs_dir = build_dir / "logs"
        logs_dir.mkdir(parents=True, exist_ok=True)
        db.set_job_fields(conn, job_id, build_dir=str(build_dir), phase="prepare")
        db.log_event(conn, job_id, "info", "job prepared", phase="prepare")

        command = (
            shlex.split(self.fake_command)
            if self.fake_command
            else build_command(job["revision"], job["seed"])
        )
        db.set_job_fields(conn, job_id, command=json.dumps(command))

        compile_log = logs_dir / "compile.log"
        live_jsonl = logs_dir / "live.jsonl"

        # Build in the job's own worktree, not the shared tree. Falls back
        # to the shared tree only when there is no commit to pin to -- a
        # fake_command selfcheck, where nothing is compiled anyway.
        if self.fake_command or not job["git_commit"]:
            run_cwd = QUARTUS_ROOT
        else:
            wt = self._prepare_worktree(job_id, job["git_commit"], build_dir)
            run_cwd = wt / "hdl" / "quartus"

        proc = subprocess.Popen(
            command,
            cwd=str(run_cwd),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
        db.mark_running(conn, job_id, proc.pid)
        db.log_event(conn, job_id, "info", f"pid={proc.pid} started", phase="analysis_synthesis")

        # Cancellation has to be watched on a timer, not between log lines.
        #
        # The streaming loop below blocks in `for line in proc.stdout` for as
        # long as the process says nothing, and quartus_map can say nothing
        # for hours -- that is the exact condition under which someone wants
        # to cancel. Checking only after a line arrives meant `cancel` was
        # recorded in the database and never acted on: several builds were
        # killed by hand while the queue still believed they were running.
        #
        # Its own connection: sqlite3 objects belong to the thread that made
        # them, and this one is read-only apart from the kill it performs.
        cancel_stop = threading.Event()

        def _watch_cancel() -> None:
            watch_conn = db.connect(self.db_path) if self.db_path else None
            try:
                while not cancel_stop.wait(5.0):
                    if watch_conn is None:
                        return
                    row = db.get_job(watch_conn, job_id)
                    if row is not None and row["state"] == "cancel_requested":
                        # terminate, then kill: Quartus spawns children that
                        # outlive a polite signal to the parent, and a
                        # half-killed compile holds the project database.
                        proc.terminate()
                        try:
                            proc.wait(timeout=20)
                        except subprocess.TimeoutExpired:
                            proc.kill()
                        return
            finally:
                if watch_conn is not None:
                    watch_conn.close()

        cancel_thread = threading.Thread(target=_watch_cancel, daemon=True)
        cancel_thread.start()

        with compile_log.open("a") as clog, live_jsonl.open("a") as jlog:
            for line in proc.stdout:
                clog.write(line)
                clog.flush()
                phase = self._infer_phase(line)
                event = {"at": time.time(), "line": line.rstrip("\n")}
                if phase:
                    event["phase"] = phase
                    db.set_job_fields(conn, job_id, phase=phase)
                jlog.write(json.dumps(event) + "\n")
                jlog.flush()

                # Kept as well as the watcher thread: when output IS flowing
                # this reacts within one line instead of up to five seconds.
                if self._cancel_requested(job_id):
                    proc.terminate()
                    cancel_stop.set()
                    db.finish_job(conn, job_id, state="cancelled", exit_code=None)
                    db.log_event(conn, job_id, "info", "cancelled by request")
                    return

        exit_code = proc.wait()
        cancel_stop.set()

        # The watcher may have killed the process. Recording that as a plain
        # failure would blame the build for something the operator asked
        # for, so the requested state wins over the exit code.
        if self._cancel_requested(job_id):
            db.finish_job(conn, job_id, state="cancelled", exit_code=exit_code)
            db.log_event(conn, job_id, "info", "cancelled while quiet")
            return
        self._finalize(job_id, compile_log, exit_code)

    def _infer_phase(self, line: str) -> Optional[str]:
        markers = [
            ("Analysis & Synthesis", "analysis_synthesis"),
            ("Fitter", "fitter"),
            ("Assembler", "assembler"),
            ("Timing Analyzer", "timequest"),
            ("Quartus Prime Shell", "reports"),
        ]
        for needle, phase in markers:
            if needle in line:
                return phase
        return None

    def _cancel_requested(self, job_id: int) -> bool:
        job = db.get_job(self.conn, job_id)
        return job is not None and job["state"] == "cancel_requested"

    def _finalize(self, job_id: int, compile_log: Path, exit_code: int) -> None:
        conn = self.conn
        db.set_job_fields(conn, job_id, phase="qgate", exit_code=exit_code)

        log_text = compile_log.read_text(errors="replace")
        job = db.get_job(conn, job_id)
        build_dir = Path(job["build_dir"])
        # Prefer standalone .sta.rpt/.fit.rpt over the compile log: the log
        # can truncate or interleave with Nios/software sub-builds, the
        # report files are the canonical per-tool output.
        metrics = parse.parse_job_reports(build_dir, job["revision"])
        if not metrics:
            metrics = parse.parse_log(log_text)
        if metrics:
            db.set_job_fields(conn, job_id, **metrics)

        # qgate is the release gate: its own header explains why the process
        # exit code alone is not proof of a good build (unbound constraints
        # can compile clean). Run it against the same log.
        qgate_path = QUARTUS_ROOT / "qgate"
        try:
            qgate_proc = subprocess.run(
                [str(qgate_path), str(compile_log)],
                cwd=str(QUARTUS_ROOT),
                capture_output=True,
                text=True,
                timeout=60,
            )
            qgate_ok = qgate_proc.returncode == 0
            qgate_output = qgate_proc.stdout + qgate_proc.stderr
        except (OSError, subprocess.TimeoutExpired) as exc:
            qgate_ok = False
            qgate_output = f"qgate invocation failed: {exc}"

        fitter_ok = parse.fitter_succeeded(log_text)

        # qgate prints one "FAIL <label>: n" line per failed check. Counting
        # every occurrence of the substring would also catch the word inside
        # an explanatory sentence, so count the verdict lines themselves.
        qgate_failures = sum(
            1 for line in qgate_output.splitlines()
            if line.startswith("FAIL")
        )

        # The field is a verdict, not a transcript: sweep summaries and
        # compare tables print it next to a seed number. The full output goes
        # beside the build where it can be read in full.
        (build_dir / "logs" / "qgate.txt").write_text(qgate_output)
        db.set_job_fields(
            conn, job_id,
            qgate_result="PASS" if qgate_ok else "FAIL",
            qgate_failures=qgate_failures,
        )

        if fitter_ok and qgate_ok:
            db.set_job_fields(conn, job_id, phase="archive")
            archive_result = archive.archive_job(build_dir, job["revision"])
            if archive_result.get("rbf_sha256"):
                db.set_job_fields(conn, job_id, rbf_sha256=archive_result["rbf_sha256"])
            db.finish_job(conn, job_id, state="passed")
            db.log_event(conn, job_id, "info", "passed qgate", phase="qgate")
        else:
            reasons = []
            if not fitter_ok:
                reasons.append(f"log missing '{parse.FITTER_SUCCESS_LINE}'")
            if not qgate_ok:
                reasons.append("qgate rejected the log")
            summary = "; ".join(reasons)
            db.finish_job(conn, job_id, state="failed", error_summary=summary)
            db.log_event(conn, job_id, "error", summary, phase="qgate")


def main() -> None:
    lock = SingleInstanceLock(LOCK_PATH)
    lock.acquire()
    try:
        conn = db.connect()
        worker = Worker(conn)
        reaped = worker.recover_stale_jobs()
        if reaped:
            print(f"reaped stale jobs: {reaped}", file=sys.stderr)
        worker.run_forever()
    finally:
        lock.release()


if __name__ == "__main__":
    main()
