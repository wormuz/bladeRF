# quartusq

Local queue for Quartus builds. SQLite job table + one worker process +
Typer CLI. Guarantees builds never overlap: SQLite claim (rowcount-checked
UPDATE) plus an flock at `/var/lock/quartusq.lock`.

## Run

```bash
cd hdl/quartus
python3 -m quartusq.cli submit --revision hostedxA4 --seed 3 --label "my build"
python3 -m quartusq.cli list
python3 -m quartusq.worker    # starts the single worker; polls every 2s
```

Database: `hdl/quartus/quartusq.db` (WAL mode). Per-job build directories
and logs: `hdl/quartus/builds/<id>-<revision>-seed<N>-<sha7>/logs/`.

## qgate is the verdict, not the process exit code

A job is `passed` only if the compile log both contains
`Quartus Prime Fitter was successful` and passes `qgate`. Quartus can
exit 0 with unbound timing constraints; qgate's own header explains why
that's a failure here, not a warning.

## CLI

```
quartusq submit --revision <rev> --seed <n> [--label ...] [--priority N] [--json]
quartusq list [--state ...] [--json]
quartusq status <id> [--json]
quartusq logs <id> [--tail N] [--json]
quartusq cancel <id> [--json]
quartusq artifact <id> [--json]
```

## Testing without real Quartus

Set `QUARTUSQ_FAKE_COMMAND` to any shell command that writes to stdout;
the worker streams it exactly like a real build. Example:

```bash
QUARTUSQ_FAKE_COMMAND="bash -c 'echo Quartus Prime Fitter was successful'" \
  python3 -m quartusq.worker
```
