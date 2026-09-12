#!/usr/bin/env bash
#
# Run quartus_map alone on a sweep-revision work directory, with optional
# extra QSF assignments, sampling the process's RSS/CPU every 30 s.
#
#   ./sweep_map_probe.sh <work_dir> <label> [qsf assignment]...
#
#   ./sweep_map_probe.sh builds/000030-*/src/hdl/quartus/work/bladerf-micro-A4-sweep baseline
#   ./sweep_map_probe.sh ... no_sharing 'set_global_assignment -name AUTO_RESOURCE_SHARING OFF'
#
# Exists because six full sweep builds stalled inside quartus_map at the same
# log line with nothing recorded about the process while it stalled. The
# architect's ranked hypotheses need one observation each: memory footprint
# during the stall, and whether a single synthesis setting removes it. A
# full-flow queue job answers neither -- it just waits 76 minutes.
#
# Blocks the queue while it runs: the worker refuses to claim a job while
# any quartus_* process it did not start is alive (foreign_quartus_running,
# by design -- two flows sharing work/ spoil both). Run this only when the
# queue is idle, or accept that queued jobs wait for it.
#
# Output: <work_dir>/probe-<label>.log (quartus_map), probe-<label>.rss
# (timestamp, RSS MiB, %CPU), and a one-line verdict on stdout.
# Bounded by MAP_TIMEOUT (default 1500 s = 25 min; a healthy map is ~5 min).

set -u
WORK=$1; LABEL=$2; shift 2
MAP_TIMEOUT=${MAP_TIMEOUT:-1500}
Q=$HOME/soft/q25

cd "$WORK" || exit 2

# Fresh copy of the revision QSF per probe so probes do not accumulate.
[ -f sweep.qsf.orig ] || cp sweep.qsf sweep.qsf.orig
cp sweep.qsf.orig sweep.qsf
for a in "$@"; do echo "$a" >> sweep.qsf; done

LOG=probe-$LABEL.log
RSS=probe-$LABEL.rss
: > "$RSS"

start=$(date +%s)
timeout "$MAP_TIMEOUT" "$Q" quartus_map --64bit bladerf -c sweep > "$LOG" 2>&1 &
mappid=$!

# The launcher forks quartus_map; sample whichever quartus_map is newest.
while kill -0 "$mappid" 2>/dev/null; do
    sleep 30
    p=$(pgrep -n -x quartus_map || true)
    if [ -n "$p" ]; then
        read -r rss cpu <<<"$(ps -o rss=,pcpu= -p "$p" | tr -s ' ')"
        echo "$(( $(date +%s) - start )) $(( rss / 1024 )) $cpu" >> "$RSS"
    fi
done
wait "$mappid"; rc=$?
elapsed=$(( $(date +%s) - start ))

if grep -q "Analysis & Synthesis was successful" "$LOG"; then
    verdict=PASSED
elif [ "$rc" -eq 124 ]; then
    verdict="STALLED (killed at ${MAP_TIMEOUT}s)"
else
    verdict="FAILED rc=$rc"
fi

peak=$(sort -k2 -n "$RSS" | tail -1 | awk '{print $2}')
echo "$LABEL: $verdict in ${elapsed}s, peak RSS ${peak:-?} MiB, last log: $(tail -1 "$LOG" | cut -c1-100)"
