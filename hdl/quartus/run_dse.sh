#!/usr/bin/env bash
#
# Design Space Explorer II sweep for a built revision.
#
#   ./run_dse.sh [exploration] [num_seeds]
#
# Replaces the hand-rolled seed loop: DSE varies fitter settings as well as the
# placement seed, runs the compiles itself, and reports the distribution rather
# than a single result. Standard-only, so it is one of the reasons the licence
# is worth having.
#
# Exploration flows worth using here:
#   seed                 placement seed only, cheapest, answers "is this seed
#                        lucky or is the design actually closed"
#   timing_aggressive    seed plus fitter effort / physical synthesis settings
#   all_optimization_modes  widest, slowest
#
# The design takes ~10 min per compile on this laptop, so a 6-seed sweep is
# about an hour. Do not start one without meaning to.
set -euo pipefail

EXPLORE="${1:-seed}"
SEEDS="${2:-6}"
WORK="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/work/bladerf-micro-A4-hosted"

if [ ! -f "$WORK/hosted.qsf" ]; then
    echo "No fitted revision in $WORK -- run build_bladerf.sh first." >&2
    exit 1
fi

cd "$WORK"

# nice keeps the machine usable; this is the owner's laptop. quartus_dse in
# 25.1 no longer accepts --lower-priority, which 23.1 had.
exec nice -n 15 "$HOME/soft/q25" quartus_dse bladerf \
    --revision hosted \
    --explore "$EXPLORE" \
    --num-seeds "$SEEDS" \
    --report-file "dse_${EXPLORE}.rpt"
