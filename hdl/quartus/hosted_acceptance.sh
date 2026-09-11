#!/usr/bin/env bash
#
# Hosted gateware acceptance on the attached bladeRF xA4. Loads the given
# RBF over USB and runs the checks that caught every defect so far:
#
#   A. in-process RX epoch cycles (enable/rx/disable x6): all deliver,
#      status words show violation=0, rx_fault=0, tx_fault=0 at disable
#   B. separate-process RX captures x3 at 5 dB: full byte count, real
#      samples (I std in a sane band, no clipping)
#   C. TX-01: TX-only tone session, tx_active=1 tx_fault=0 at disable
#   D. TX-05: RX+TX concurrent, RX gets every requested byte
#
#   ./hosted_acceptance.sh <hosted.rbf>
#
# Prints one PASS/FAIL line per check and exits non-zero on any FAIL.
# Needs: host/build/output/{bladeRF-cli,libbladeRF.so.2}, the cycle probe
# built at /tmp/probe2 from host/misc/rx_epoch_cycle_probe.c, and the TX
# tone file (generated here if missing). Runtime about one minute.

set -u
RBF=${1:?usage: hosted_acceptance.sh <hosted.rbf>}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
CLI=$ROOT/host/build/output/bladeRF-cli
export LD_PRELOAD=$ROOT/host/build/output/libbladeRF.so.2
export CAP_RUN_OK=200
fail=0
verdict() { printf '%-6s %s\n' "$1" "$2"; if [ "$1" = FAIL ]; then fail=1; fi; return 0; }

[ -x /tmp/probe2 ] || gcc -o /tmp/probe2 "$ROOT/host/misc/rx_epoch_cycle_probe.c" \
    -I"$ROOT/host/libraries/libbladeRF/include" -L"$ROOT/host/build/output" -lbladeRF
[ -r /var/tmp/tx_tone.bin ] || python3 -c "
import struct, math
with open('/var/tmp/tx_tone.bin', 'wb') as f:
    for n in range(262144):
        a = 2*math.pi*n/64.0
        f.write(struct.pack('<hh', int(1000*math.cos(a)), int(1000*math.sin(a))))"

load_out=$(timeout 40 "$CLI" -l "$RBF" 2>&1)
if echo "$load_out" | grep -q "Successfully loaded FPGA"; then
    verdict PASS "load $RBF"
else
    verdict FAIL "load $RBF: $(echo "$load_out" | grep -iE 'error|fail|unable' | head -1)"
fi
"$CLI" -e version 2>&1 | grep "FPGA version" | sed 's/^ */       /'

# A. in-process cycles with status words
out=$(BLADERF_LOG_LEVEL=verbose timeout 40 /tmp/probe2 6 2>&1)
cycles_ok=$(echo "$out" | grep -c "cycle [0-9]*: ok")
bad=$(echo "$out" | grep -E "status at|status on entry" \
      | grep -cE "violation=1|rx_fault=1|tx_fault=1|rx_abort=1")
[ "$cycles_ok" -eq 6 ] && verdict PASS "A: 6/6 in-process cycles" \
                       || verdict FAIL "A: $cycles_ok/6 in-process cycles"
[ "$bad" -eq 0 ] && verdict PASS "A: no violation/fault/abort in any status word" \
                 || verdict FAIL "A: $bad status words with violation/fault/abort"

# B. separate processes, real samples
for i in 1 2 3; do
    rm -f /var/tmp/acc$i.bin
    timeout 15 "$CLI" -e "set frequency rx1 925M" -e "set samplerate rx1 61.44M" \
        -e "set agc rx1 off" -e "set gain rx1 5" \
        -e "rx config file=/var/tmp/acc$i.bin format=bin n=50000" \
        -e "rx start" -e "rx wait" >/dev/null 2>&1
    bytes=$(stat -c%s /var/tmp/acc$i.bin 2>/dev/null || echo 0)
    if [ "$bytes" -eq 200000 ]; then
        stat_line=$(python3 - "$i" <<'PY'
import struct, math, sys
d = open('/var/tmp/acc%s.bin' % sys.argv[1], 'rb').read()
n = len(d) // 2
v = struct.unpack('<%dh' % n, d)
I = v[0::2]
m = sum(I) / len(I)
s = math.sqrt(sum((x - m) ** 2 for x in I) / len(I))
clip = sum(1 for x in v if abs(x) >= 2044) / n * 100
zeros = sum(1 for x in v if x == 0) / n * 100
ok = 5 < s < 800 and clip < 0.1 and zeros < 5
print(("PASS" if ok else "FAIL") + " B: process %s I_std=%.1f clip=%.2f%% zeros=%.2f%%" % (sys.argv[1], s, clip, zeros))
PY
)
        verdict ${stat_line%% *} "${stat_line#* }"
    else
        verdict FAIL "B: process $i delivered $bytes bytes"
    fi
    rm -f /var/tmp/acc$i.bin
done

# C. TX-01
out=$(timeout 30 "$CLI" -v verbose -e "set frequency tx1 925M" -e "set samplerate tx1 61.44M" \
      -e "tx config file=/var/tmp/tx_tone.bin format=bin repeat=8" \
      -e "tx start" -e "tx wait 10000" -e "tx" 2>&1)
echo "$out" | grep -q "status at TX disable.*tx_active=1.*tx_fault=0" \
    && echo "$out" | grep -q "Last error: None" \
    && verdict PASS "C: TX-01 tone session, tx_active=1 tx_fault=0" \
    || verdict FAIL "C: TX-01 $(echo "$out" | grep -E 'status at TX|Last error' | tail -2 | tr '\n' ' ')"

# D. TX-05 concurrent
rm -f /var/tmp/accrxtx.bin
out=$(timeout 40 "$CLI" -v verbose -e "set frequency rx1 925M" -e "set frequency tx1 2400M" \
      -e "set samplerate rx1 30.72M" -e "set samplerate tx1 30.72M" \
      -e "set agc rx1 off" -e "set gain rx1 5" \
      -e "rx config file=/var/tmp/accrxtx.bin format=bin n=2000000" \
      -e "tx config file=/var/tmp/tx_tone.bin format=bin repeat=40" \
      -e "rx start" -e "tx start" -e "rx wait 10000" -e "tx wait 10000" 2>&1)
bytes=$(stat -c%s /var/tmp/accrxtx.bin 2>/dev/null || echo 0)
faults=$(echo "$out" | grep "status at" | grep -cE "rx_fault=1|tx_fault=1|rx_abort=1")
[ "$bytes" -eq 8000000 ] && [ "$faults" -eq 0 ] \
    && verdict PASS "D: TX-05 RX+TX concurrent, RX $bytes bytes, no faults" \
    || verdict FAIL "D: TX-05 RX $bytes bytes, $faults status words with faults"
rm -f /var/tmp/accrxtx.bin

exit $fail
