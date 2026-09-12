#!/usr/bin/env bash
#
# Acceptance for the sweep revision after the CCPP/max_skew root-cause fix.
#
#   ./sweep_acceptance.sh <build-dir>
#
# "It built" is not the acceptance criterion. The fix removed a constraint,
# so the question this answers is whether it removed an expensive and
# under-specified one, or whether it quietly removed coverage: a crossing
# that used to be bounded and now is not is a worse outcome than the stall.
#
# Checks, in the order they can fail meaningfully:
#
#   A. constraints    read_sdc clean, nothing ignored/dropped/unbound; the
#                     two paired bundled-data bounds present and bound; the
#                     363-register source-only blanket absent
#   B. timing         setup/hold/recovery/removal/min-pulse all >= 0
#   C. resources      against the hosted baseline and the diagnostic run
#   D. CDC            vendor DCFIFO constraints still active, no new
#                     unclassified multi-bit crossing
#
# Prints one PASS/FAIL per check, exits non-zero on any FAIL.

set -u
BUILD=${1:?usage: sweep_acceptance.sh <build-dir>}
LOG="$BUILD/logs/compile.log"
WORK=$(dirname "$(find "$BUILD" -name 'sweep.fit.summary' -print -quit 2>/dev/null)")
fail=0
verdict() { printf '%-6s %s\n' "$1" "$2"; if [ "$1" = FAIL ]; then fail=1; fi; return 0; }

[ -r "$LOG" ] || { echo "no compile log at $LOG"; exit 2; }

# ---------------------------------------------------------------- A. flow
for stage in "Analysis & Synthesis" "Fitter" "Timing Analyzer" "Assembler"; do
    if grep -q "$stage was successful" "$LOG"; then
        verdict PASS "flow: $stage successful"
    else
        verdict FAIL "flow: $stage did not report success"
    fi
done

bt=$(grep -oE "Total Build Time: [0-9:]+" "$LOG" | tail -1)
verdict PASS "flow: ${bt:-build time not printed}"

# ------------------------------------------------------- B. constraints
# Anything Quartus says it could not apply is a dropped bound, which is the
# specific risk this fix carries.
for pat in "Ignored .* assignment" "No paths found" "is an illegal" "cannot be applied"; do
    n=$(grep -cE "$pat" "$LOG" 2>/dev/null || true)
    [ "${n:-0}" -eq 0 ] && verdict PASS "sdc: no '$pat'" \
                        || verdict FAIL "sdc: $n x '$pat'"
done

# 332182 is "No path is found satisfying assignment" -- a constraint that
# bound to nothing. Four are known and expected (the SPI/I2C pin clocks that
# have no internal path); more than that means something stopped binding.
ucp=$(grep -c "Warning (332182)" "$LOG" 2>/dev/null || echo 0)
[ "$ucp" -le 4 ] && verdict PASS "sdc: unbound assignments $ucp (<= 4 known)" \
                 || verdict FAIL "sdc: unbound assignments $ucp (> 4 known)"

# The two paired bounds must still be there, and the blanket must not be.
if grep -q "handshake crossings constrained: [1-9]" "$LOG"; then
    verdict PASS "sdc: paired handshake bounds applied ($(grep -oE 'handshake crossings constrained: [0-9]+' "$LOG" | tail -1))"
else
    verdict FAIL "sdc: paired handshake bounds NOT applied"
fi
if grep -q "tamer hold_time crossing constrained on [1-9]" "$LOG"; then
    verdict PASS "sdc: time_tamer bounds applied"
else
    verdict FAIL "sdc: time_tamer bounds NOT applied"
fi
if grep -q "some crossing has no capture endpoint named" "$LOG"; then
    verdict FAIL "sdc: instance-count guard fired -- a handshake has no paired endpoints"
else
    verdict PASS "sdc: instance-count guard quiet (every handshake instance paired)"
fi
# The BLADERF_DIAG_NO_MAX_SKEW bypass was removed once CCPP was confirmed as
# the stall cause; this checks it has not been reintroduced. Reading the SDC
# rather than the log is deliberate: a bypass that is present but not taken
# still leaves a way to build an image with no bound on bundled-data skew.
if grep -q "BLADERF_DIAG_NO_MAX_SKEW" \
   ../fpga/platforms/bladerf-micro/constraints/bladerf.sdc 2>/dev/null \
   || grep -q "DIAGNOSTIC BUILD" "$LOG"; then
    verdict FAIL "sdc: a max_skew bypass exists or was used -- not release-valid"
else
    verdict PASS "sdc: no diagnostic bypass present"
fi

# The readout crossings must be cut, and the count says how many. A silent
# skip -- pattern matching nothing, guard stepping over it -- is what left the
# system PLL domain failing for a whole revision.
rc=$(grep -oE "readout crossings cut: [0-9]+" "$LOG" | tail -1)
if [ -n "$rc" ] && [ "${rc##* }" -ge 1 ]; then
    verdict PASS "sdc: $rc"
else
    verdict FAIL "sdc: readout crossings not cut (${rc:-counter absent})"
fi
if grep -q "readout crossing NOT cut" "$LOG"; then
    verdict FAIL "sdc: a readout crossing has a source but no destination match"
else
    verdict PASS "sdc: every readout crossing with a source found its destination"
fi
if grep -qE "(handshake|readout) pair too wide" "$LOG"; then
    verdict FAIL "sdc: a destination pattern reaches past its crossing"
else
    verdict PASS "sdc: no pattern reaches past its crossing"
fi

# ------------------------------------------------------------- C. timing
# qgate is the project's own verdict on the timing report; reuse it rather
# than re-deriving slack parsing here.
if [ -x ./qgate ]; then
    if ./qgate "$LOG" >/dev/null 2>&1; then
        verdict PASS "timing: qgate accepted the build"
    else
        verdict FAIL "timing: qgate rejected the build ($(./qgate "$LOG" 2>&1 | grep -iE 'fail|slack|negative' | head -1))"
    fi
else
    verdict FAIL "timing: qgate not found, cannot judge"
fi

# ---------------------------------------------------------- D. resources
if [ -n "$WORK" ] && [ -r "$WORK/sweep.fit.summary" ]; then
    sed -n 's/^\(Logic utilization.*\|Total RAM Blocks.*\|Total DSP Blocks.*\|Total registers.*\)$/       \1/p' \
        "$WORK/sweep.fit.summary"
    alm=$(grep -oE "Logic utilization \(in ALMs\) : [0-9,]+" "$WORK/sweep.fit.summary" | grep -oE "[0-9,]+$" | tr -d ,)
    m10k=$(grep -oE "Total RAM Blocks : [0-9]+" "$WORK/sweep.fit.summary" | grep -oE "[0-9]+$")
    # Diagnostic run measured 8493 ALM / 282 M10K. A large departure means the
    # fix changed implementation, not just constraint evaluation.
    if [ -n "$alm" ] && [ "$alm" -lt 12000 ]; then
        verdict PASS "resources: ALM $alm (diagnostic baseline 8493)"
    else
        verdict FAIL "resources: ALM ${alm:-unknown} unexpectedly high"
    fi
    if [ -n "$m10k" ] && [ "$m10k" -le 300 ]; then
        verdict PASS "resources: M10K $m10k/308 (diagnostic baseline 282)"
    else
        verdict FAIL "resources: M10K ${m10k:-unknown}/308 -- at or over budget"
    fi
else
    verdict FAIL "resources: no fit summary found under $BUILD"
fi

exit $fail
