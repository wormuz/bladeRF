#!/usr/bin/env python3
"""PostToolUse: run qcheck after any edit to constrained sources.

qcheck holds one rule per defect class that has already cost a build here.
Running it after an edit turns a lost afternoon into a line of output: the
whole point is that none of these should ever be found by Quartus again.

Reports rather than blocks. The edit is already written by this point, and a
hard block on a PostToolUse hook would leave the file in place with no way to
fix it in the same step. What matters is that nobody starts a fourteen-minute
build on a tree that is already known to be broken.
"""
import json
import subprocess
import sys
from pathlib import Path

WATCHED = (".sdc", ".vhd", ".h")


def main() -> int:
    try:
        payload = json.load(sys.stdin)
    except ValueError:
        return 0

    if payload.get("tool_name") not in ("Edit", "Write", "MultiEdit"):
        return 0

    path = (payload.get("tool_input", {}) or {}).get("file_path", "")
    if not path.endswith(WATCHED):
        return 0

    root = Path(payload.get("cwd") or ".").resolve()
    qcheck = root / "hdl/quartus/qcheck"
    if not qcheck.exists():
        return 0

    r = subprocess.run([str(qcheck)], capture_output=True, text=True, cwd=root)
    if r.returncode == 0:
        return 0

    print(json.dumps({
        "hookSpecificOutput": {
            "hookEventName": "PostToolUse",
            "additionalContext": (
                "qcheck found a defect class that has already cost a build "
                "here. Fix it before compiling -- a build on this tree will "
                "either fail or, worse, succeed while doing something other "
                "than what the constraints say.\n\n" + r.stdout),
        }
    }))
    return 0


if __name__ == "__main__":
    sys.exit(main())
