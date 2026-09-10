"""Typer CLI for quartusq: submit/list/status/logs/cancel/artifact."""
from __future__ import annotations

import json
import subprocess
from pathlib import Path
from typing import Optional

import typer

from . import compare as compare_mod
from . import db
from . import sweep as sweep_mod
from . import worker as worker_mod

app = typer.Typer(help="Local queue for Quartus builds.")
sweep_app = typer.Typer(help="Sweep jobs over a set of seeds.")
app.add_typer(sweep_app, name="sweep")


def _git_info() -> tuple[Optional[str], Optional[str]]:
    try:
        ref = subprocess.run(
            ["git", "rev-parse", "--abbrev-ref", "HEAD"],
            cwd=str(worker_mod.QUARTUS_ROOT), capture_output=True, text=True, check=True,
        ).stdout.strip()
        commit = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=str(worker_mod.QUARTUS_ROOT), capture_output=True, text=True, check=True,
        ).stdout.strip()
        return ref, commit
    except (OSError, subprocess.CalledProcessError):
        return None, None


def _row_to_dict(row) -> dict:
    return {k: row[k] for k in row.keys()}


@app.command()
def submit(
    revision: str = typer.Option(..., "--revision"),
    seed: int = typer.Option(1, "--seed"),
    label: Optional[str] = typer.Option(None, "--label"),
    priority: int = typer.Option(100, "--priority"),
    device: Optional[str] = typer.Option(None, "--device"),
    as_json: bool = typer.Option(False, "--json"),
):
    conn = db.connect()
    git_ref, git_commit = _git_info()
    job_id = db.submit_job(
        conn, revision=revision, seed=seed, label=label, priority=priority,
        device=device, git_ref=git_ref, git_commit=git_commit,
    )
    if as_json:
        typer.echo(json.dumps({"id": job_id}))
    else:
        typer.echo(f"submitted job {job_id}")


@app.command(name="list")
def list_cmd(
    state: Optional[str] = typer.Option(None, "--state"),
    limit: int = typer.Option(100, "--limit"),
    as_json: bool = typer.Option(False, "--json"),
):
    conn = db.connect()
    rows = db.list_jobs(conn, state=state, limit=limit)
    if as_json:
        typer.echo(json.dumps([_row_to_dict(r) for r in rows]))
        return
    for r in rows:
        typer.echo(f"{r['id']:>6}  {r['state']:<16} {r['revision']:<12} seed={r['seed']} {r['label'] or ''}")


@app.command()
def status(job_id: int, as_json: bool = typer.Option(False, "--json")):
    conn = db.connect()
    row = db.get_job(conn, job_id)
    if row is None:
        typer.echo(f"no such job: {job_id}", err=True)
        raise typer.Exit(1)
    if as_json:
        typer.echo(json.dumps(_row_to_dict(row)))
    else:
        for k in row.keys():
            typer.echo(f"{k}: {row[k]}")


@app.command()
def logs(
    job_id: int,
    tail: int = typer.Option(50, "--tail"),
    as_json: bool = typer.Option(False, "--json"),
):
    conn = db.connect()
    row = db.get_job(conn, job_id)
    if row is None or not row["build_dir"]:
        typer.echo(f"no log for job {job_id}", err=True)
        raise typer.Exit(1)
    log_path = Path(row["build_dir"]) / "logs" / "compile.log"
    if not log_path.exists():
        typer.echo(f"log file not found: {log_path}", err=True)
        raise typer.Exit(1)
    lines = log_path.read_text(errors="replace").splitlines()[-tail:]
    if as_json:
        typer.echo(json.dumps(lines))
    else:
        for line in lines:
            typer.echo(line)


@app.command()
def cancel(job_id: int, as_json: bool = typer.Option(False, "--json")):
    conn = db.connect()
    ok = db.request_cancel(conn, job_id)
    if as_json:
        typer.echo(json.dumps({"ok": ok}))
    else:
        typer.echo("cancel requested" if ok else "job not cancellable (already terminal or missing)")


@app.command()
def artifact(job_id: int, as_json: bool = typer.Option(False, "--json")):
    conn = db.connect()
    row = db.get_job(conn, job_id)
    if row is None or not row["build_dir"]:
        typer.echo(f"no artifact for job {job_id}", err=True)
        raise typer.Exit(1)
    build_dir = Path(row["build_dir"])
    rbf_candidates = list(build_dir.glob("*.rbf")) + list((build_dir / "output_files").glob("*.rbf") if (build_dir / "output_files").exists() else [])
    result = {
        "job_id": job_id,
        "build_dir": str(build_dir),
        "rbf": str(rbf_candidates[0]) if rbf_candidates else None,
        "rbf_sha256": row["rbf_sha256"],
    }
    if as_json:
        typer.echo(json.dumps(result))
    else:
        for k, v in result.items():
            typer.echo(f"{k}: {v}")


@app.command()
def compare(job_a: int, job_b: int, as_json: bool = typer.Option(False, "--json")):
    conn = db.connect()
    row_a, row_b = db.get_job(conn, job_a), db.get_job(conn, job_b)
    if row_a is None or row_b is None:
        typer.echo("one or both jobs not found", err=True)
        raise typer.Exit(1)
    result = compare_mod.compare_jobs(row_a, row_b)
    if as_json:
        typer.echo(json.dumps(result))
        return
    typer.echo(f"job {job_a} ({result['state_a']})  vs  job {job_b} ({result['state_b']})")
    for field, d in result["fields"].items():
        typer.echo(f"  {field:<16} a={d['a']}  b={d['b']}  delta={d['delta']}  [{d['direction']}]")


@sweep_app.command(name="submit")
def sweep_submit(
    revision: str = typer.Option(..., "--revision"),
    seeds: str = typer.Option(..., "--seeds", help="comma-separated, e.g. 1,2,3,5,7"),
    label: Optional[str] = typer.Option(None, "--label"),
    priority: int = typer.Option(100, "--priority"),
    git_ref_opt: Optional[str] = typer.Option(None, "--git-ref"),
    as_json: bool = typer.Option(False, "--json"),
):
    conn = db.connect()
    seed_list = [int(s.strip()) for s in seeds.split(",") if s.strip()]
    detected_ref, detected_commit = _git_info()
    git_ref = git_ref_opt or detected_ref
    sweep_id, job_ids = sweep_mod.submit_sweep(
        conn, revision=revision, seeds=seed_list, label=label, priority=priority,
        git_ref=git_ref, git_commit=detected_commit,
    )
    if as_json:
        typer.echo(json.dumps({"sweep_id": sweep_id, "job_ids": job_ids}))
    else:
        typer.echo(f"sweep {sweep_id}: submitted jobs {job_ids}")


@sweep_app.command(name="list")
def sweep_list(as_json: bool = typer.Option(False, "--json")):
    conn = db.connect()
    rows = db.list_sweeps(conn)
    if as_json:
        typer.echo(json.dumps([_row_to_dict(r) for r in rows]))
        return
    for r in rows:
        typer.echo(f"{r['id']:>6}  {r['revision']:<12} seeds={r['seeds']} {r['label'] or ''}")


@sweep_app.command(name="show")
def sweep_show(sweep_id: int, as_json: bool = typer.Option(False, "--json")):
    conn = db.connect()
    summary = sweep_mod.sweep_summary(conn, sweep_id)
    if as_json:
        typer.echo(json.dumps(summary))
        return
    typer.echo(f"sweep {sweep_id}  revision={summary['revision']}  {summary['label'] or ''}")
    for row in summary["rows"]:
        typer.echo(
            f"  seed={row['seed']:<4} state={row['state']:<10} "
            f"setup={row['setup_wns_ns']} hold={row['hold_wns_ns']} qgate={row['qgate_result']}"
        )
    typer.echo(f"median setup={summary['median_setup_wns_ns']}  median hold={summary['median_hold_wns_ns']}")
    typer.echo(summary["pass_summary"])


if __name__ == "__main__":
    app()
