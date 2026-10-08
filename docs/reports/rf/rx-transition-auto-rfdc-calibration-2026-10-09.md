# RX transition: remove implicit RFDC calibration from LO hops

Date: 2026-10-09  
Device: bladeRF 2.0 micro xA4, serial `f695006ba84a40daa7b777c6a6eba78`  
FPGA image: sweep A4 SHA-256 `fe68fd403bf6000277aa690575c0346c1dbaef2faeabf4116f0121d160ce79dd`

## Finding

The RX transition path automatically added `BLADERF_RF_REQUIRE_RX_RFDC_CAL_DONE` for the first epoch and whenever the requested LO differed by more than 100 MHz from the last calibrated frequency. This called ADI `ad9361_do_calib_timeout(RFDC_CAL)` during an ordinary LO hop, although the ADR specifies explicit RFDC calibration and does not make an LO-frequency change alone a calibration trigger.

A cross-band RX1 transition measured the calibration at 1,310,592 µs. The sync stream was configured with a 1,000 ms transfer timeout. During RFDC calibration the AD9361 did not produce valid RX samples, so the FPGA epoch gate had no ADC `data_v` edges from which to form its zero-IQ keepalives. The USB stream timed out before calibration completed. The transition then reported a valid FPGA epoch, but the sync stream had already failed and could not deliver valid host data. RX2 reproduced the same pattern. These failures had `rfic_status=0`: they were not the FPGA GPIF fault captured by the diagnostic snapshot.

The first post-fix debug run observed PLL lock in 120 µs, ENSM already in FDD (`REG_STATE=0x1a`) in 246 µs, and FPGA epoch-valid at 630 µs. The measured cross-band transition latency returned to about 21 ms. This isolates the regression to implicit RFDC calibration, rather than ENSM settling or a changed RF channel.

## Change

Ordinary RX LO transitions no longer infer an RFDC calibration from the LO delta or first epoch. They still require the requested PLL, ENSM, FPGA epoch, and host-data events. `BLADERF_RF_REQUIRE_RX_RFDC_CAL_DONE` remains available as an explicit request; when requested, the calibration result is still deadline-bounded and failures remain fail-closed. The now-unused cached calibration-frequency state and frequency-threshold helper were removed.

The native policy test verifies that PLL/ENSM/epoch requirements do not add RFDC calibration, and that explicit `RX_RFDC_CAL_DONE` still normalizes to a data-valid epoch contract.

## Hardware qualification

All runs used the volatile-loaded diagnostic image above. No SPI flash write was issued.

| Layout | Transitions | Stream buffers / samples / USB transfers | Unrecovered | First-read faults | Retry overruns | Stream overrun events | Withheld events | P50 / P95 / P99 / max |
|---|---:|---|---:|---:|---:|---:|---:|---|
| RX1 | 1,000 | 512 / 131,072 / 24 | 0 | 0 | 0 | 0 | 1,000 | 21.226 / 25.167 / 29.737 / 31.667 ms |
| RX2 | 1,000 | 512 / 131,072 / 24 | 0 | 0 | 0 | 0 | 1,000 | 21.349 / 27.466 / 30.085 / 39.976 ms |
| RX_X2 | 1,000 | 64 / 32,768 / 32 | 0 | 0 | 0 | 0 | 1,000 | 24.516 / 24.621 / 24.654 / 32.643 ms |

Each transition alternated 947.5 MHz and 1.835 GHz. Each run validated event order, exact LO readback, epoch and first-host-data metadata. All three runs recorded complete event history, disabled both RX channels as applicable, and closed the device cleanly. Aggregate: 3,000 transitions, zero unrecovered transitions and zero stream-overrun events.

Logs:

- `docs/reports/rf/rx-epoch-rx1-crossband-1000-post-rfdc-policy-20261009.log`
- `docs/reports/rf/rx-epoch-rx2-crossband-1000-post-rfdc-policy-20261009.log`
- `docs/reports/rf/rx-epoch-rxx2-crossband-1000-post-rfdc-policy-20261009.log`
- `docs/reports/rf/gpif-diagnostic-rx1-calibration-20261009.log` (calibration timing and failing pre-fix repro)

## Build and remaining gates

The diagnostic A4 sweep image completed Analysis & Synthesis, Fitter, timing analysis, and Assembler. Fitter resources: 9,060 ALMs, 282/308 RAM blocks. Setup and hold slack were positive in the reported slow/fast corners. The post-fit ADC_XFER report paired all four RX bundles; qgate and sweep acceptance passed against the retained Quartus `.qmsg`/`.rpt` artifacts. `hdl/quartus/sweep_acceptance.sh` was corrected in commit `d59532d0`: it now counts distinct known pin-clock warnings and checks the current `tamer compare payload` message.

The FPGA GPIF diagnostic bits were not triggered in the 3,000 passing transitions. The original failing events were caused by the implicit RFDC calibration and host transfer timeout; the earlier reduced-buffer stress still independently produces host queue overruns and is not a production geometry.

The explicit RFDC-calibration path was subsequently exercised with a live RX META stream. The stream's configured 1,000 ms timeout had previously been clamped only to the 1,000 ms USB bulk default, shorter than calibration. `sync_worker_transfer_timeout_ms()` now gives RX `SC16_Q11_META` a 2,500 ms minimum transfer watchdog while preserving longer caller timeouts and leaving TX/raw RX unchanged. This is only transport liveness: the RF transition retains its 2,000 ms deadline and reports timeout as failure. The qualification runner supports `--require-rfdc-cal` and validates both RFPLL observations around calibration and the `RX_RFDC_CAL_DONE` event in order.

At the runner's default 8,192-sample buffer geometry, explicit cross-band RFDC calibration passed ten consecutive transitions each on RX1, RX2, and paired RX_X2, with ~1.33 s latency, zero read faults/overruns, complete event histories, and clean close. P99/max were 1,341.572/1,342.335 ms (RX1), 1,338.974/1,343.928 ms (RX2), and 1,340.241/1,344.379 ms (RX_X2). The earlier large-buffer failures were run at 4 Msps with 1 s stream timeout and 2 s transition deadline, so they did not isolate geometry. The runner now accepts `BLADERF_QUAL_STREAM_TIMEOUT_MS` and `BLADERF_QUAL_TRANSITION_TIMEOUT_MS`. At matched production settings (23.04 Msps, 3 s stream and transition deadlines), explicit calibration passed 5/5 on RX1 and RX2 with 131,072 samples and 5/5 paired RX_X2 with 32,768 samples; no overruns/faults, clean event history and close. P50/max were 256.301/258.236 ms (RX1), 257.170/270.320 ms (RX2), and 256.631/257.160 ms (RX_X2). At 4 Msps, RX1 and RX_X2 passed 5/5 at 8,192 samples. For paired RX_X2 at 32,768 samples, 64 buffers/8 active USB transfers passed 20/20 with zero faults/overruns (P99 1,343.816 ms); 32 buffers/8 transfers and 64/8 each passed 5/5; 64/16 failed 3/5, and the original 64/32 run failed 3/5 with one stream overrun and aggregate FPGA fault `0x10314900`. This narrows the failure to the low-rate / active-transfer-depth interaction during explicit calibration; sample rate or buffer size alone does not explain it. Keep explicit calibration opt-in; do not report a failed attempt as a valid epoch. The runner's environment parsing was also corrected so a lower buffer count can be paired with an explicitly lower transfer count. Logs: `docs/reports/rf/rx-epoch-rx1-explicit-rfdc-cal-10-default-20261009.log`, `docs/reports/rf/rx-epoch-rx2-explicit-rfdc-cal-10-default-20261009.log`, `docs/reports/rf/rx-epoch-rxx2-explicit-rfdc-cal-10-default-20261009.log`, `docs/reports/rf/rx-epoch-rxx2-explicit-rfdc-cal-32768-20261009.log`, `docs/reports/rf/rx-epoch-rx1-explicit-rfdc-cal-20261009.log`, `docs/reports/rf/rx-epoch-rx1-explicit-rfdc-cal-5-20261009.log`, `docs/reports/rf/rx-epoch-rx1-explicit-rfdc-cal-32768-20261009.log`, `docs/reports/rf/rx-epoch-rx1-explicit-rfdc-cal-131072-matched-20261009.log`, `docs/reports/rf/rx-epoch-rx2-explicit-rfdc-cal-131072-matched-20261009.log`, `docs/reports/rf/rx-epoch-rxx2-explicit-rfdc-cal-32768-matched-20261009.log`, `docs/reports/rf/rx-epoch-rxx2-explicit-rfdc-cal-4msps-3s-20261009.log`, `docs/reports/rf/rx-epoch-rx1-explicit-rfdc-cal-4msps-8k-3s-20261009.log`, `docs/reports/rf/rx-epoch-rxx2-explicit-rfdc-cal-4msps-8k-3s-20261009.log`, `docs/reports/rf/rx-epoch-rxx2-explicit-rfdc-cal-4msps-32k-32x8-3s-20261009.log`, `docs/reports/rf/rx-epoch-rxx2-explicit-rfdc-cal-4msps-32k-64x8-3s-20261009.log`, `docs/reports/rf/rx-epoch-rxx2-explicit-rfdc-cal-4msps-32k-64x8-20-20261009.log`, and `docs/reports/rf/rx-epoch-rxx2-explicit-rfdc-cal-4msps-32k-64x16-3s-20261009.log`, `docs/reports/rf/rx-epoch-rxx2-explicit-rfdc-cal-4msps-32k-24x8-3s-20261009.log`. LTE PSS/MIB reacquisition and full install/release qualification remain open.
