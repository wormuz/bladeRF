# 2x2 complex MIMO FIR spatial whitening in the xA4 RX path: implementation design

Status: design only, nothing built or run. Fork: `~/projects/bladerf` @ `5d833830`.
Input decision: OC `sdr-scanner/rx-precalibration-plan-2026-10-18.md`, last three sections
(5 taps reproduce the per-10-RB oracle in float emulation `tmp/wh_fir.py`; 9 taps also OK).

## 0. Facts this design rests on (read in the tree, not assumed)

| fact | where |
|---|---|
| `rx_clock` is an alias of `ad9361.clock` (= axi_ad9361 `l_clk`, also `tx_clock`) | `bladerf_core.vhd:464-466` |
| `adc_streams(i)` is combinational from axi_ad9361 (after its IQ/DC correction); `data_v` is a rising-edge detect of `adc.i.valid or adc.q.valid` | `bladerf_core.vhd:1966-1987` |
| axi_ad9361 asserts `adc_valid` every other `l_clk` | `analogdevicesinc/.../axi_ad9361_dev_if_alt.v:288-294` |
| RX timestamp increments once per 2 `rx_clock` cycles (one count per sample) | `time_tamer.vhd:333-345` |
| so `rx_clock` = 2 x fs: 122.88 MHz at 61.44 MS/s; STA constrains it at 125 MHz (8 ns) | `hosted.sta.rpt` (pll_sclk divclk 8.000 ns) |
| RX chain in `rx.vhd`: `adc_streams` -> `rx_mux` (1 register, NORMAL = `adc_streams`) -> `rx_epoch_gate` (samples live `rx_timestamp` at its input) -> `fifo_writer` | `rx.vhd:499-534`, `287-314`, `318-378` |
| loopback / counter sources enter at the mux, not before it | `rx.vhd:513-529` |
| 8-bit mode packs `data(11 downto 4)`, packed SC12 mode takes 12 bits: samples must stay inside the 12-bit range | `fifo_writer.vhd:1040-1058` |
| epoch gate: COMPLETE moves PENDING->WAIT_FIRST_SAMPLE, the next valid sample is admitted and its timestamp latched as `first_valid_timestamp` | `rx_epoch_gate.vhd:238-266` |
| `rx_reset` comes from system reset only (not per stream), so fabric state survives stream stop/start and host close/reopen | `bladerf_core.vhd:2027-2036` |
| last hosted fit: DSP 12/66, ALM 9,539/18,480, RAM blocks 304/308 (99 %), regs 21,100 | `quartus/work/...qualification-final-2a879d50/output_files/hosted.fit.summary` |
| last sweep fit: DSP 13/66, RAM blocks 282/308 | `quartus/work/bladerf-micro-A4-sweep/output_files/sweep.fit.summary` |
| same build: worst setup slack on pll_sclk (= rx_clock/tx_clock) is -0.261 ns, TNS -9.439, path `tx:U_tx|fifo_reader|underflow_detected -> underflow_count[*]`; Fmax 121.05 MHz | `hosted.sta.summary`, `hosted.slow_85C.setup_paths.rpt:52-58` |
| NIOS RAM 175,104 B; `bladeRF_nios.elf` text+data+bss = 164,304 B (10.8 KB left for stack/heap/growth) | `nios_system.qsys` memorySize, `size bladeRF_nios.elf` |

## 1. Datapath block `rx_spatial_fir` (new file `hdl/fpga/ip/nuand/synthesis/rx_spatial_fir.vhd`)

### 1.1 Placement in `rx.vhd`

- New instance between the `adc_streams` port (`rx.vhd:132`) and `rx_mux`. Inputs: `adc_controls`,
  `adc_streams`, `rx_timestamp`, `rx_epoch_arm/complete/abort`, coefficient bank and control from
  the controller (section 2). Outputs: `fir_streams : sample_streams_t(adc_streams'range)`,
  `fir_timestamp : unsigned(63 downto 0)`, `fir_epoch_complete : std_logic`, status bits.
- `rx_mux` NORMAL branch (`rx.vhd:506-507`): `mux_streams <= fir_streams` (trigger gating at
  `rx.vhd:508-512` unchanged, now applied to FIR output). In the same process add two registered
  sidebands: `mux_timestamp <= fir_timestamp` and `mux_epoch_complete <= fir_epoch_complete` in
  NORMAL; `mux_timestamp <= rx_timestamp`, `mux_epoch_complete <= rx_epoch_complete` in every
  other mode (test/loopback modes keep stock timing).
- `U_rx_epoch_gate` (`rx.vhd:297`, `:300`): `rx_timestamp => mux_timestamp`,
  `epoch_complete => mux_epoch_complete`. ARM and ABORT stay direct (fencing earlier is safe).
- New generic `ENABLE_SPATIAL_FIR : boolean := false` on `rx` (`rx.vhd:30-32`) and on
  `bladerf_core` next to `ENABLE_SWEEP_ANALYZER` (`bladerf_core.vhd:41`); `bladerf-hosted.vhd:40-43`
  and `bladerf-sweep.vhd:42-` set it true. With false, a `generate` wires `fir_* <= adc_*/rx_*`
  so other revisions (adsb/wlan/foxhunt use their own `rx-adsb.vhd`/top files) are unaffected.
- Not moved: the sweep analyser tap (`bladerf_core.vhd:996-1000`) and the pre-trigger ring
  (`:1542`, `:1561`) keep reading raw `adc_streams(0)`. They measure the AD9361 lane, not the
  whitened mix; this is stated in their comments when the block lands.

### 1.2 Arithmetic contract

    y_i[n] = sum_{j=0..1} sum_{k=0..4} h_ij[k] * x_j[n-k]     (complex, i = output lane)

- Data in: `signed(15 downto 0)` I/Q per lane (SC16 Q11; real ADC content is 12-bit).
- Coefficients: Q2.14 signed 16-bit, range [-2.0, +2.0 - 2^-14]. Q1.15 is rejected because 1.0
  is not representable, so identity could not be bit-exact; Q2.14 makes 1.0 = 16384 exact and
  leaves headroom for whitening gains up to 2 (host normalises W into that range, section 5).
- Center tap c = 2 is the reference. Identity = `h00[2] = h11[2] = 16384 + 0j`, all else 0. This
  matches `tmp/wh_fir.py:fit_fir` (`n = arange(taps) - taps//2`, i.e. taps -2..+2) and makes the
  block's group delay a constant 2 samples whether W is identity or fitted.
- Products: 16x16 -> 32 bit; per (i, j, k) two DSP "sum of two 18x19" results
  `Re: hr*xr + (-hi)*xi`, `Im: hr*xi + hi*xr`. `-hi` is precomputed at commit as 17-bit signed
  (so -(-32768) needs no special case; 17 bits fit the 18/19-bit DSP port).
- Accumulate: 10 DSP results per output component -> 38-bit signed (32 + 1 + 4 growth + sign).
- Round: add 2^13, arithmetic shift right 14 (round half up).
  Identity proof: `x*16384 + 8192 >> 14 = x` for every integer x.
- Saturate to [-2048, +2047] (not to int16): keeps the 12-bit contract the 8-bit and packed modes
  rely on (`fifo_writer.vhd:1054-1055`) and keeps host clip detection meaningful (2044 threshold).
  A saturation sticky counter per lane goes into status (section 2.3).

### 1.3 Pipeline (direct form, all products of one output in the same cycle)

Direct form is chosen over systolic/transposed because a coefficient swap then lands on an output
boundary by construction: every product of output n is formed in one cycle from one bank.

| stage | content |
|---|---|
| S0 | on input `data_v` (lane 0; both lanes strobe together in 2R2T): shift per-lane delay line x[0..4]; shift timestamp line `ts[0..2]`; `valid0 <= data_v` |
| S1 | DSP input registers: data from delay line, coefficients from ACTIVE bank, plus a sampled `mode` bit (FIR vs bypass) and `support_clean` marker (section 3) that travel with the sample from here on |
| S2 | DSP multiply + sum-of-two, output register inside the DSP block |
| S3-S6 | registered adder tree 10 -> 5 -> 3 -> 2 -> 1 per component (4 levels) |
| S7 | round + saturate, registered |
| S8 | output select (FIR result vs bypass tap) by the carried `mode` bit, registered; `data_v` is the carried valid |

Latency L = 9 `rx_clock` cycles, constant in every mode. Bypass path = `x_own[2]` (center tap of
the lane's own delay line) carried through a 7-register shift so both paths align exactly.

`mode = BYPASS` when any of: `adc_controls(0).enable = '0'`, `adc_controls(1).enable = '0'`
(RX_X1 on either channel), active bank flagged identity (`active_is_identity`), or the
`force_identity` level is set. Bypass is a pure pass-through (no rounding/saturation), so identity
is bit-exact even for an input outside 12 bits.

### 1.4 Timestamp alignment (answers "does tagging need the latency accounted": yes)

The gate stamps a sample with the live counter at its input (`rx_epoch_gate.vhd:155`, `:266`).
Inserting L = 9 cycles and the 2-sample center delay would shift META timestamps by 4.5 + 2
samples against the stock gateware and against the TX/tamer clock. The block therefore carries
the timestamp with the data instead of adding constants:

- `ts_in(m)` = value of `rx_timestamp` one cycle after x[m]'s `data_v` (this is exactly what the
  stock gate sees: mux registers data one cycle, gate samples the counter then).
- `ts[0..2]` shifts with the data delay line; the output y[n] carries `ts[2]` = `ts_in(n-2)`
  through S1-S8.

Result: with identity, the (IQ, timestamp) pair of every sample at the gate is identical to stock
gateware (only the first 2 samples after RX enable differ: the delay line is primed with zero and
`ts_in` of the prime). Non-identity W keeps the same mapping (center-tap convention). No change
is needed in `fifo_writer`, the tamer, scheduled TX or `tools/clock_mapping_measure.py`
calibration; this is verified, not assumed (section 6.1 T2).

## 2. Control path

### 2.1 Mechanism chosen: new 8x32 NIOS target + three Avalon PIOs + two `work.handshake` crossings

Comparable host-writable fabric settings today all use this route: host `nios_8x32_write` ->
NIOS `pkt_8x32.c:perform_write` -> `IOWR_ALTERA_AVALON_PIO_DATA` into a sys_clock PIO ->
`work.handshake` into `rx_clock` -> capture register (`dwell_cfg` at `bladerf_core.vhd:1182-1257`,
`rx_epoch_ctrl` via `rx_epoch_controller` at `:1302-1318`, TX retune gate). A custom Avalon-MM
slave with its own `_hw.tcl` would be the only new-kind component in the Qsys system and still
needs the same CDC; it buys nothing for 20 words written every ~10 s. PIOs use no M10K (RAM
blocks are at 304/308).

New PIOs in `hdl/fpga/platforms/bladerf-micro/build/nios_system.tcl` (copy the `rx_epoch_ctrl`
block `:471-498`, export `:821`, data_master connection `:1000-1002`, clock `:1133`, reset `:1193`):

| PIO | dir | width | base (free slots between 0x95e0 and 0x9680) |
|---|---|---|---|
| `rx_sfir_data` | Output | 32 | 0x9600 |
| `rx_sfir_ctrl` | Output | 32 | 0x9620 |
| `rx_sfir_status` | Input | 32 | 0x9640 |

Bases must be confirmed by qsys-generate (no overlap error); the listed addresses are unused in the
current tcl. Add the three `*_export` ports to the `nios_system` component in `bladerf_p.vhd:61-`
(next to `rx_epoch_ctrl_export`, `:142`) and map them in `bladerf_core.vhd:799-` (next to `:872`).
`bladerf-foxhunt.vhd:160` declares its own `nios_system` component; it must get the three ports or
that revision stops elaborating (adsb/wlan use `bladerf_p`, their instances leave the new outputs
unconnected and tie the input to zero).

### 2.2 Word formats

`rx_sfir_data`: one complex coefficient, `[15:0] = I (real)`, `[31:16] = Q (imag)` (same packing
as `pretrig_data`). Index `idx = (i*2 + j)*5 + k`, 0..19.

`rx_sfir_ctrl` (NIOS owned, host never sees raw toggles, same rule as RX_EPOCH_CTRL):

| bits | meaning |
|---|---|
| 4:0 | write index 0..19 |
| 5 | write toggle: on change, `shadow[idx] <= data` |
| 6 | commit toggle: on change, `active <= shadow`, `active_is_identity <= shadow_is_identity`, `gen <= ctrl[23:16]` |
| 7 | `force_identity` level: 1 = bypass now, independent of banks |
| 23:16 | 8-bit commit generation tag from the host |

`rx_sfir_status` (rx -> sys snapshot): `[4:0]` last written index, `[5]` write-toggle echo,
`[6]` commit-toggle echo, `[7]` force_identity as applied, `[15:8]` active generation tag,
`[16]` bypass active now, `[17]` active bank is identity, `[18]` both lanes enabled,
`[20:19]` per-lane saturation sticky (cleared by commit), `[27:21]` 0, `[31:28]` block version = 1.
Version 0 (or a NIOS error on the unknown target) = gateware without the block.

### 2.3 Fabric controller (new `hdl/fpga/ip/nuand/synthesis/rx_spatial_fir_controller.vhd`)

Modelled on `rx_epoch_controller.vhd` (144 lines): one `work.handshake` named `transfer` carries
`{data, ctrl}` (64 bits) sys -> rx with the continuous req/ack cycle and capture-on-ack register
`control_rx` (the `dwell_cfg` comment at `bladerf_core.vhd:1201-1257` explains why both are
required). Toggle edges are decoded against the previous captured word. After `rx_reset` the first
capture only adopts the toggle levels (as `rx_epoch_controller` does with `control_initialized`) and
the active bank is identity, so a toggle state left by an earlier session never fires.
`shadow_is_identity` is a registered compare of the 40 shadow halves, updated the cycle after each
write. Commit is one `rx_clock` assignment of all 20 coefficients, so it is atomic at an output
boundary because the bank is sampled at S1 only.

Status goes back through a second handshake `U_rx_sfir_status_handshake` (32 bits, rx -> sys) into
`rx_sfir_status_sys`, same pattern as `U_rx_epoch_snapshot_handshake`.

Order rule for NIOS: write `rx_sfir_data` first, then `rx_sfir_ctrl`. A capture between the two
writes sees new data with old toggles and does nothing; the action fires only when ctrl changes,
by which time data is already stable (same argument as TX retune target lo/hi then ctrl).

### 2.4 NIOS firmware (`hdl/fpga/platforms/common/bladerf/software/bladeRF_nios/src/`)

- `fpga_common/include/nios_pkt_8x32.h`: `NIOS_PKT_8x32_TARGET_RX_SPATIAL_FIR 0x92` (0x91 is the
  last used, `:388`); addr 0..19 = coefficient word; `..._SFIR_CMD_COMMIT 0x80` (data = 8-bit tag);
  `..._SFIR_CMD_IDENTITY 0x81` (data 0); read addr 0x80 = status word, 0..19 = NIOS mirror.
- `fpga_common/include/nios_pkt_8x32_diag.h:32`: allow the new read addresses.
- `devices_inline.h` (next to `dwell_cfg_write`, `:301-311`, and RX_EPOCH bits `:488`):
  `rx_sfir_write_word(idx, w)`, `rx_sfir_commit(tag)`, `rx_sfir_identity()`, `rx_sfir_status()`,
  all `#ifdef RX_SFIR_CTRL_BASE` so older Qsys builds compile to `return false`.
  Write and commit flip the toggle in a NIOS shadow of the ctrl word, then poll `rx_sfir_status`
  for the echo with a bounded loop (fixed iteration cap, returns false; no unbounded wait). Before
  polling, check GPI[27] `ad9361_if_clock_alive` (`bladerf_core.vhd:590-600`) and fail at once if
  `l_clk` is absent. `rx_sfir_identity()` only sets the level bit and does not wait (section 4).
- `pkt_8x32.c`: new `case` in `perform_write` (`:287`, pattern of RX_EPOCH_CTRL `:436-466`) and in
  `perform_read` (`:48`). Mirror arrays: 20 + 20 words (160 B) of RAM.
- `devices_sim.c`: stubs for the simulator build (as `rx_epoch_status_snapshot_read`, `:107`).
- Not touched: `pkt_retune*.c`, tamer, `set_tuning_mode` paths, fastlock, RFIC queue.

### 2.5 libbladeRF (`host/libraries/libbladeRF`)

- `include/bladeRF2.h` (next to `bladerf_set_dwell_cfg`, `:369`):

      #define BLADERF_RX_SPATIAL_FIR_TAPS   5
      #define BLADERF_RX_SPATIAL_FIR_CENTER 2
      #define BLADERF_RX_SPATIAL_FIR_ONE    16384   /* Q2.14 */
      struct bladerf_rx_spatial_fir_status {
          uint32_t host_generation;  /* library counter, +1 per commit/identity */
          uint8_t  fpga_tag;         /* status[15:8] */
          bool bypass_active, identity, both_lanes, sat_rx1, sat_rx2;
          uint8_t  rx_epoch_id;      /* epoch opened by the last commit, if contract on */
      };
      int bladerf_set_rx_spatial_fir(struct bladerf *dev, const int16_t coef[2][2][5][2]);
      int bladerf_get_rx_spatial_fir(struct bladerf *dev, int16_t coef[2][2][5][2],
                                     struct bladerf_rx_spatial_fir_status *st);
      int bladerf_reset_rx_spatial_fir(struct bladerf *dev);   /* identity */

  `coef[i][j][k][0]` = real, `[1]` = imag, output lane i, input lane j, tap k (k=2 center).
- `include/libbladeRF.h:5107`: `BLADERF_RF_INVALIDATE_SPATIAL_FIR (1U << 19)` (bits 0-18 used).
- `src/backend/usb/nios_access.{c,h}` (pattern `nios_rx_epoch_ctrl_cmd`, `.c:1588`, `.h:452`):
  `nios_rx_sfir_write(dev, idx, word)`, `nios_rx_sfir_commit(dev, tag)`,
  `nios_rx_sfir_identity(dev)`, `nios_rx_sfir_status_read(dev, &w)`, `nios_rx_sfir_mirror_read`.
- New pure header `src/board/bladerf2/rx_spatial_fir_policy.h` (style of `rfic_fir_policy.h`):
  packing/index, identity detection, reset-reason mask, status decode. Unit-tested without hardware.
- `src/board/bladerf2/bladerf2.c`: public functions (pattern `bladerf_set_dwell_cfg` `:7714` for
  locking/lifecycle checks, `bladerf_set_rx_fpga_iqcor_mask` `:7438-7482` for invalidation);
  capability probe from status version at open next to the epoch contract probe (`:682`), stored in
  `board_data`; identity written at open, because fabric state survives host restarts.
- `src/board/bladerf2/rf_transition.c`: the commit sequence of section 3, and the identity hooks of
  section 4 inside `bladerf2_rx_data_invalidate` (`:3171`) and `_bladerf_rx_transition_begin`
  (after the ARM fence `:3947-3975`, before the RFIC retune).

### 2.6 python_bladerf (`~/projects/python_bladerf/python_bladerf/pylibbladerf/`)

- `cbladerf.pxd` (next to `:803-805`): the struct and three functions.
- `pybladerf.pyx` (next to `pybladerf_set_rx_fpga_iqcor_mask`, `:4586-4594`):
  `pybladerf_set_rx_spatial_fir(coef)` taking a numpy `int16` array of shape (2, 2, 5, 2)
  (C-contiguous check, shape check, no dtype coercion: a float array is rejected, not cast),
  `pybladerf_get_rx_spatial_fir() -> (ndarray, dict)`, `pybladerf_reset_rx_spatial_fir()`.
- `pybladerf.pyi` (`:2013-2017` area): stubs. `tests/`: shape/dtype rejection, round trip on a
  device-less mock where the suite has one.

### 2.7 sdr-scanner

- `rfcore/spatial_fir.py` (no sdrscanner/radio import, per rfcore rule): `fit_fir` moved from
  `tmp/wh_fir.py:37-45`, `quantize_q214(h) -> int16[2,2,5,2]` with normalisation (section 5), and a
  bit-true reference `apply_fixed(h_q, x1_ci16, x2_ci16)` that reproduces 1.2 exactly (round, 12-bit
  saturation, center delay). This is the golden model for the VHDL testbench and the host tests.
- `sdrscanner/driver/spatial_fir.py`: apply/reset wrapper on the opened device, capability check
  (UNSUPPORTED -> feature off, logged once), records `host_generation`, `fpga_tag`, `rx_epoch_id`
  and a hash of the coefficients.
- Capture provenance: the SigMF capture segment gets `sdrscanner:spatial_fir` = {generation, hash,
  identity flag}. With the epoch contract on, the RX epoch id already identifies the W in force for
  every buffer (section 3), so corpus rows join on epoch id. IQ after a non-identity W is a
  different representation from raw ADC IQ; consumers that assume raw lanes (lane SNR A/B,
  `tools/rx_lane_snr_ab.py`) must check the flag.
- Estimation of W (pilot-based or the still-open generic in-band estimator) is outside this block.

## 3. Coefficient change and the RX epoch contract

Decision: a non-identity commit is an RX data-plane transition. With the epoch contract enabled
it opens a new epoch; without it (stock client) it is a plain write, same as stock `set_gain`.

Reasons: (a) the sample statistics change at the commit instant, exactly the kind of boundary the
contract exists to mark; (b) epoch id then names the W for every buffer, which is the provenance
link the corpus needs; (c) the commit is atomic at an output boundary (1.3), so the fence costs only
ARM + commit + COMPLETE, not 20 writes.

Sequence in `bladerf_set_rx_spatial_fir` (contract enabled):

1. Validate; reject while any RX channel is in an AGC gain mode (`BLADERF_ERR_UNSUPPORTED`):
   W is estimated for a fixed gain state and AGC changes it without host involvement.
2. Write the 20 words into the shadow bank (no effect on output).
3. Reuse the transition machinery without the RFIC step: `_prepare_rx_epoch_id` (`:435`),
   `sync_rx_epoch_expect_id`, ARM (`nios_rx_epoch_ctrl_cmd`), `_wait_rx_epoch_fenced` (`:339`),
   then `nios_rx_sfir_commit(tag)` (NIOS waits for the fabric echo), read status and check
   `fpga_tag`/identity bit, then COMPLETE and the existing EPOCH_VALID wait. Events carry flag
   `BLADERF_RF_INVALIDATE_SPATIAL_FIR`. Factor this as an internal
   `_rx_epoch_reconfigure(dev, apply_fn, ctx)` in `rf_transition.c` rather than copying the
   begin/wait bodies.
4. Any failure: ABORT (fail closed, as `bladerf2_rx_data_invalidate` does), then identity.

FIR support and COMPLETE: an output y[n] depends on x[n-4..n]. Without care, the first sample the
gate admits after COMPLETE could include inputs taken while the RFIC was still changing (ARM
fenced period), and `first_valid_timestamp` would then name a sample whose support predates the
boundary. The block therefore delays COMPLETE: on `rx_epoch_complete` it starts counting input
`data_v`; the 5th input after the pulse (the first whose whole 5-sample window is newer than
COMPLETE) is tagged `support_clean`, and the tag travels with that sample through S1-S8. The block
asserts `fir_epoch_complete` together with the output sample immediately before the tagged one;
through the mux register the gate sees COMPLETE, enters WAIT_FIRST_SAMPLE, and admits the tagged
sample as the first valid one, timestamped with `ts_in(n-2)`. ARM or ABORT arriving during the
count cancels the pending COMPLETE. The same delay applies in bypass (one rule, 2 extra samples
withheld; no special case). Cost: 4 samples + 9 cycles more before EPOCH_VALID.

## 4. Reset to identity

- Fabric: `rx_reset` -> active = identity, `force_identity` cleared, toggles adopted.
- Library open: identity (fabric state survives close/reopen, `rx_reset` is system reset only).
- Library, inside `bladerf2_rx_data_invalidate` (`rf_transition.c:3171`, the single choke point
  that `set_gain` `bladerf.c:784`, `set_frequency` `:1340`, `schedule_retune` `:1640`, gain mode,
  bias tee and the FPGA correction setters already go through): after the ABORT/revoke succeeded,
  if `reason & SFIR_RESET_MASK` call `nios_rx_sfir_identity`. Mask: FREQUENCY, SAMPLE_RATE,
  BANDWIDTH, GAIN, GAIN_MODE, RF_PORT, CORRECTION, RX_FIR, CLOCK, DEVICE_RESET, FPGA_RELOAD.
  Taps are in samples and W is per gain/LO, so sample rate, bandwidth and AD9361 FIR changes
  invalidate W as much as gain and frequency do.
- `_bladerf_rx_transition_begin` (event-driven retune and quick-tune): identity after the ARM fence,
  before the RFIC write.
- `force_identity` is a level, not a toggle with ack: NIOS returns immediately, so these setters get
  no new dependency on `l_clk` being alive (sample-rate changes stop it while the BBPLL relocks).
  When the clock is stopped no samples flow; when it restarts the handshake delivers the level a few
  cycles later. With the contract on those samples are fenced; without it they are part of the
  setter's transient, as stock.
- A scheduled retune (`bladerf_schedule_retune`) applies identity at schedule time, earlier than the
  retune itself. Conservative; documented in the API comment.
- The next `bladerf_set_rx_spatial_fir` writes all 20 words and clears the level in the same ctrl
  write as the commit toggle, so leaving bypass and loading the new bank happen in one capture.

## 5. Host coefficient normalisation (needed before quantisation)

SINR is invariant to a common scalar on W, so the host chooses it: scale h so that the mean
output noise power per lane equals the mean input noise power (estimated from the same R_n), then
check max |Re|,|Im| of all taps < 2.0 - 2^-14; if not, scale down to fit and record the factor.
Without this, R_n^-1/2 in ADC units maps noise of a few LSB to ~1 LSB and loses precision, or
overflows 12 bits. The bit-true model (2.7) reports the saturation fraction; `clip_fraction`
> 0.01 % after W is a failed apply. `tmp/wh_fir.py` is float and unnormalised; its CRS-SNR results
(16.6 / 16.5 dB with 5 taps) have not been reproduced with Q2.14 + 12-bit saturation yet.

## 6. Verification plan

### 6.1 VHDL (GHDL, new `host/misc/run_rx_spatial_fir_tb.sh` modelled on `run_rx_epoch_gate_tb.sh`; testbenches in `hdl/fpga/ip/nuand/simulation/`)

| id | test | pass criterion |
|---|---|---|
| T1 | identity, random int16 IQ incl. -32768/32767, both lanes, valid every 2nd cycle and irregular | output = input delayed 2 samples, bit-exact, every sample |
| T2 | identity, `rx_spatial_fir` + `rx_mux` + `rx_epoch_gate` vs stock chain fed the same stream | (IQ, timestamp) at gate output identical to stock for every sample after the first 2 |
| T3 | known 2x2 mix, random coefficients incl. +/-2.0 limits, vectors from `rfcore/spatial_fir.apply_fixed` | bit-exact vs golden model |
| T4 | saturation: coefficients 1.99 on both inputs with full-scale x | outputs clamp to -2048/+2047, sticky bits set, no wrap |
| T5 | commit atomicity: commit while streaming | each output equals all-old or all-new golden result, switch at one sample |
| T6 | RX_X1: lane 0 only, lane 1 only, enable toggled mid-stream | bypass bit-exact on the enabled lane, constant latency |
| T7 | epoch: COMPLETE, then COMPLETE + ARM inside the 4-sample window, ABORT inside it | first admitted sample is the first `support_clean` one; `first_valid_timestamp` = its ts; cancel cases never open |
| T8 | controller CDC: real `handshake` with unrelated sys/rx clocks, back-to-back NIOS writes with echo wait, reset in the middle | every write lands; reset adopts toggles without firing; force_identity level applied |

Plus `ghdl --synth --std=08` on both new entities.

### 6.2 Host unit tests (no hardware)

- `libbladeRF_test/test_rx_spatial_fir_policy/`: index packing, identity detection, reset mask,
  status decode, rejection of malformed input.
- Fault injection in the existing epoch test style (`BLADERF_ENABLE_TEST_RX_ABORT_FAULT_INJECTION`):
  commit failure after ARM leaves the epoch aborted and the block in identity.
- python_bladerf shape/dtype tests; sdr-scanner tests for `quantize_q214`, normalisation and
  `apply_fixed` against float `fftconvolve` within quantisation error.

### 6.3 On hardware (owner's run; sdr-monitor stopped for it)

1. Gateware with block, identity: compare a capture with the previous release image, same
   settings, terminators on both RX ports: floor/stats per lane unchanged; counter mux mode
   (`RX_MUX_32BIT_COUNTER`) bit-exact sequence (FIR not in that path, proves mux sideband).
2. Identity bit-exactness on live data cannot be compared against another gateware sample by
   sample; instead load a non-identity W that is exactly representable (swap: h01=h10=delta) and
   check RX1/RX2 swapped bit-exact vs a capture with the same W emulated on the host. Then identity
   again: status identity bit set, bypass active.
3. Live LTE (1855 MHz epochs as in the OC tables): host estimates W from CRS on the first half,
   quantises, applies in FPGA; CRS-SNR on FPGA output vs `apply_fixed` on a raw RX_X2 capture of the
   same moment and vs the float `tmp/wh_fir.py` result. Acceptance from OC: within 0.5 dB of the
   per-group oracle (1855 #0: > 16.1 dB).
4. Epoch: commit with contract on during a stream: one RX_DATA_INVALIDATED / EPOCH_VALID pair,
   new epoch id, no buffer with mixed W (check the first buffer after VALID against `apply_fixed`).
5. `set_gain`/`set_frequency` with non-identity W: status identity bit set afterwards.
6. RX_X1 stream with a W loaded: bypass bit set, stream identical to identity.

## 7. Resources (Cyclone V 5CEBA4F23C8)

| item | estimate |
|---|---|
| DSP blocks, 5 taps | 80 products of 16x16 = 40 blocks in sum-of-two 18x19 mode. Hosted 12 -> 52/66; sweep 13 -> 53/66 |
| DSP blocks, 9 taps full rate | 144 products = 72 blocks: does not fit (54 free) |
| 9 taps, 2:1 time-multiplexed | 36 blocks; possible because a sample arrives at most every 2nd cycle (`adc_valid` alternation, timestamp tick). Not chosen: depends on that cadence and doubles control complexity; the measured 5-tap result does not need it |
| ALMs | adder trees 4 x 9 adders of up to 38 bits, round/sat, delay lines, 64-bit x (3 + 9) timestamp carry, banks (shadow 640 + active ~980 FF), controller: estimate 1,500-2,500 ALMs on 8,941 free. Estimate, not a fit result |
| M10K | 0 (all registers); RAM blocks are at 304/308 |
| NIOS | code + 160 B mirrors; 10.8 KB headroom today |

Acceptance on the first fit: DSP <= 52 (hosted), no new failing setup path inside the block, and the
existing pll_sclk worst slack not worse (виміряно STA, quartus/work/bladerf-micro-A4-hosted-release-qualification-final-2a879d50/output_files/hosted.sta.summary: -0.261 ns, TX fifo_reader).

## 8. CDC and `bladerf.sdc`

Two new crossings, both `work.handshake` bundled data, nothing else:

| crossing | source_holding | capture endpoint |
|---|---|---|
| sys -> rx, `{data, ctrl}` 64 bit | `*rx_spatial_fir_controller:U_rx_spatial_fir_controller|handshake:transfer|source_holding[*]` | `*rx_spatial_fir_controller:U_rx_spatial_fir_controller|control_rx[*]` |
| rx -> sys, status 32 bit | `*U_rx_sfir_status_handshake|source_holding[*]` | `*bladerf_core:*|rx_sfir_status_sys[*]` |

Add both pairs to `hs_pairs` (`bladerf.sdc:326-343`). The existing loop then applies
`set_max_delay 100 / set_min_delay -100 / set_max_skew 6.4 / set_net_delay 6.4`, the width check
(destination not wider than source, `:426-431`) and the completeness guard (`:627-655`) reports any
handshake instance without a named endpoint as a critical warning. Not optional: both exist in
hosted and sweep, so do not add them to `hs_optional_absent_patterns`. `adc_controls` enables, the
epoch toggles and the timestamp are already rx_clock; the block itself adds no crossing. Run the
`vhdl-sdc-binding-verifier` reviewer on the diff before synthesis, and after the fit check the
TimeQuest messages "handshake instances: N covered" with N = previous + 2 and no
"crossing not matched".

## 9. Risks

| risk | handling |
|---|---|
| Timing in pll_sclk (rx = tx clock), already -0.261 ns in the release build | every arithmetic stage registered, DSP input+output registers, no logic between adder levels; adder tree in 2 levels per cycle only if the fit shows slack; first build hosted only |
| quartus_map stall seen when a live consumer was added to `adc_streams(0)` (`bladerf_core.vhd:985-1003`, experiments E1/E3) | FIR is a new consumer of both lanes in that cone. S0 registers the lanes with no combinational fan-in; build hosted first; if map stalls, the comparison point is the existing E3c experiment |
| NIOS hang | only bounded polls; GPI[27] clock-alive check before any poll; identity path does not poll; no change to tuning, retune2, fastlock, `set_tuning_mode` code |
| NIOS RAM (10.8 KB headroom, RAM blocks 99 % so it cannot grow) | code limited to one packet case + 5 small helpers; check `size` of the elf in the build |
| PIO base clash in Qsys | qsys-generate fails loudly; pick other free 0x20 slots |
| foxhunt top declares its own `nios_system` component | add the ports there too or that revision stops elaborating |
| sample consumers that assume raw lanes | provenance flag (2.7); analyser/pretrig taps stay raw |
| build time | full Quartus flow per revision (hosted + sweep); not measured here |

## 10. Files to change (summary)

VHDL: `ip/nuand/synthesis/rx_spatial_fir.vhd` (new), `ip/nuand/synthesis/rx_spatial_fir_controller.vhd`
(new), `platforms/bladerf-micro/vhdl/rx.vhd`, `bladerf_core.vhd`, `bladerf_p.vhd`, `bladerf-hosted.vhd`,
`bladerf-sweep.vhd`, `bladerf-foxhunt.vhd` (component ports); Quartus file lists for the two new
sources (the `.qip`/qsf list that already names `rx_epoch_controller.vhd`).
Qsys: `platforms/bladerf-micro/build/nios_system.tcl`. SDC: `constraints/bladerf.sdc`.
Testbenches: `ip/nuand/simulation/rx_spatial_fir_tb.vhd`, `rx_spatial_fir_controller_tb.vhd`,
`rx_spatial_fir_gate_equiv_tb.vhd`; `host/misc/run_rx_spatial_fir_tb.sh`.
NIOS: `fpga_common/include/nios_pkt_8x32.h`, `nios_pkt_8x32_diag.h`; `bladeRF_nios/src/pkt_8x32.c`,
`devices_inline.h`, `devices.h`, `devices_sim.c`.
libbladeRF: `include/bladeRF2.h`, `include/libbladeRF.h`, `src/backend/usb/nios_access.{c,h}`,
`src/board/bladerf2/{bladerf2.c, rf_transition.c, common.h, rx_spatial_fir_policy.h}`,
`libbladeRF_test/test_rx_spatial_fir_policy/`.
python_bladerf: `cbladerf.pxd`, `pybladerf.pyx`, `pybladerf.pyi`, tests.
sdr-scanner: `rfcore/spatial_fir.py` (new), `sdrscanner/driver/spatial_fir.py` (new), capture
metadata writer, tests.
