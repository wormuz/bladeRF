#!/usr/bin/env bash
#
# Collect locally built artefacts into release-staging/<version>/ with the
# checksums the release workflow verifies.
#
#   tools/stage_release.sh 0.16.1 [build-dir]
#
# Nuand publishes four files per board variant -- the .rbf, an .md5sum, a
# .sha256sum and the .fit.summary -- and this matches that, because anyone
# who has used their images already knows what to do with it. The
# .fit.summary is not decoration: it carries the resource counts and lets
# someone check the claim without trusting it.
#
# Refuses to stage a bitstream whose build did not close timing. A released
# image that fails STA is a defect shipped with a version number on it.

set -euo pipefail

VERSION=${1:?usage: stage_release.sh <version> [build-dir]}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT="$ROOT/release-staging/$VERSION"

die() { printf 'error: %s\n' "$*" >&2; exit 1; }

# --------------------------------------------------------------- bitstreams
# Second argument narrows to one build; otherwise take the newest passing
# build of each revision under hdl/quartus/builds.
stage_rbf() {
    local rbf=$1 name=$2
    [ -r "$rbf" ] || die "no bitstream at $rbf"

    local dir summary sta
    dir=$(dirname "$rbf")
    summary="$dir/$(basename "$rbf" .rbf).fit.summary"
    sta="$dir/$(basename "$rbf" .rbf).sta.summary"

    # Timing gate. Reading the summary rather than trusting the build log:
    # the log says what the tool did, the summary says what it produced.
    if [ -r "$sta" ]; then
        if grep -qE '^Slack\s*:\s*-' "$sta"; then
            die "$name has negative slack in $(basename "$sta") -- not releasable"
        fi
    else
        printf 'warning: %s has no .sta.summary; timing unverified\n' "$name" >&2
    fi

    install -D -m 0644 "$rbf" "$OUT/$name.rbf"
    [ -r "$summary" ] && install -m 0644 "$summary" "$OUT/$name.fit.summary"
    [ -r "$sta" ] && install -m 0644 "$sta" "$OUT/$name.sta.summary"

    ( cd "$OUT" \
      && sha256sum "$name.rbf" > "$name.rbf.sha256sum" \
      && md5sum    "$name.rbf" > "$name.rbf.md5sum" )

    printf '  %-24s %8s bytes\n' "$name.rbf" "$(stat -c%s "$OUT/$name.rbf")"
}

# ------------------------------------------------------------------- firmware
stage_fw() {
    local img=$1
    [ -r "$img" ] || { printf 'warning: no FX3 image at %s, skipping\n' "$img" >&2; return; }
    install -D -m 0644 "$img" "$OUT/bladeRF_fw.img"
    ( cd "$OUT" \
      && sha256sum bladeRF_fw.img > bladeRF_fw.img.sha256sum \
      && md5sum    bladeRF_fw.img > bladeRF_fw.img.md5sum )
    printf '  %-24s %8s bytes\n' "bladeRF_fw.img" "$(stat -c%s "$OUT/bladeRF_fw.img")"
}

mkdir -p "$OUT"
printf 'Staging %s into %s\n' "$VERSION" "${OUT#"$ROOT"/}"

if [ $# -ge 2 ]; then
    for rbf in "$2"/*.rbf; do
        [ -e "$rbf" ] || continue
        stage_rbf "$rbf" "$(basename "$rbf" .rbf)xA4"
    done
else
    for rev in hosted sweep; do
        rbf=$(ls -t "$ROOT"/hdl/quartus/builds/*/src/hdl/quartus/work/bladerf-micro-A4-$rev/output_files/$rev.rbf 2>/dev/null | head -1)
        [ -n "$rbf" ] || { printf 'warning: no %s build found\n' "$rev" >&2; continue; }
        stage_rbf "$rbf" "${rev}xA4"
    done
fi

stage_fw "$ROOT/fx3_firmware/build/bladeRF_fw.img"

printf '\nStaged files:\n'
( cd "$OUT" && ls -1 )
printf '\nVerify before tagging:\n  cd %s && sha256sum -c *.sha256sum\n' "${OUT#"$ROOT"/}"
