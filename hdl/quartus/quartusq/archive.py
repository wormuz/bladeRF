"""Archive a passed job's outputs: copy the bitstream/reports out of the
raw Quartus build directory into a stable artifacts/ + reports/ layout,
and write manifest.json (constraint file hashes + git info) and
result.json (metric + qgate summary) next to them.

Called only after a job has already passed qgate (worker._finalize) — this
module doesn't gate anything itself, it just preserves what qgate already
approved.
"""
from __future__ import annotations

import json
import time
from pathlib import Path
from typing import Any

from . import parse

# Constraint files read by every hosted-revision build (see qcheck.SDCS in
# this same directory) — hashed into the manifest so a passed build's
# provenance includes exactly which constraints produced it.
QUARTUS_ROOT = Path(__file__).resolve().parent.parent
REPO_ROOT = QUARTUS_ROOT.parent.parent
CONSTRAINT_FILES = [
    REPO_ROOT / "hdl/fpga/platforms/bladerf-micro/constraints/bladerf.sdc",
    REPO_ROOT / "hdl/fpga/platforms/bladerf-micro/constraints/spi.sdc",
    REPO_ROOT / "hdl/fpga/platforms/bladerf-micro/constraints/i2c.sdc",
    REPO_ROOT / "hdl/fpga/platforms/common/bladerf/constraints/ad9361.sdc",
    REPO_ROOT / "hdl/fpga/platforms/common/bladerf/constraints/fx3.sdc",
]

ARTIFACT_GLOBS = ("*.rbf", "*.sof")
REPORT_GLOBS = ("*.sta.rpt", "*.fit.rpt", "*.map.rpt")


def _copy_matches(build_dir: Path, globs: tuple, dest: Path) -> list[str]:
    dest.mkdir(parents=True, exist_ok=True)
    copied = []
    search_dirs = [build_dir, build_dir / "output_files"]
    for src_dir in search_dirs:
        if not src_dir.exists():
            continue
        for pattern in globs:
            for src in src_dir.glob(pattern):
                target = dest / src.name
                target.write_bytes(src.read_bytes())
                copied.append(str(target))
    return copied


def _git_info() -> dict[str, Any]:
    import subprocess

    try:
        commit = subprocess.run(
            ["git", "rev-parse", "HEAD"], cwd=str(QUARTUS_ROOT),
            capture_output=True, text=True, check=True,
        ).stdout.strip()
        ref = subprocess.run(
            ["git", "rev-parse", "--abbrev-ref", "HEAD"], cwd=str(QUARTUS_ROOT),
            capture_output=True, text=True, check=True,
        ).stdout.strip()
        return {"commit": commit, "ref": ref}
    except Exception:
        return {"commit": None, "ref": None}


def archive_job(build_dir: Path, revision: str) -> dict[str, Any]:
    """Copy artifacts/reports, write manifest.json + result.json. Returns a
    dict with at least 'rbf_sha256' (None if no .rbf was found).
    """
    artifacts_dir = build_dir / "artifacts"
    reports_dir = build_dir / "reports"

    copied_artifacts = _copy_matches(build_dir, ARTIFACT_GLOBS, artifacts_dir)
    copied_reports = _copy_matches(build_dir, REPORT_GLOBS, reports_dir)

    rbf_path = next((Path(p) for p in copied_artifacts if p.endswith(".rbf")), None)
    rbf_sha256 = parse.sha256_file(rbf_path) if rbf_path else None

    constraint_hashes = {
        f.name: parse.sha256_file(f) for f in CONSTRAINT_FILES if f.exists()
    }

    manifest = {
        "revision": revision,
        "build_dir": str(build_dir),
        "constraint_hashes": constraint_hashes,
        "git": _git_info(),
        "archived_at": time.time(),
    }
    (build_dir / "manifest.json").write_text(json.dumps(manifest, indent=2))

    metrics = parse.parse_job_reports(build_dir, revision)
    result = {
        "revision": revision,
        "metrics": metrics,
        "artifacts": copied_artifacts,
        "reports": copied_reports,
        "rbf_sha256": rbf_sha256,
    }
    (build_dir / "result.json").write_text(json.dumps(result, indent=2))

    return {"rbf_sha256": rbf_sha256, "manifest": manifest, "result": result}
