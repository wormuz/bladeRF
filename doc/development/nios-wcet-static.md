# NIOS RFIC: static step bounds, validity trace, int-only wrappers

Static analysis of the bladeRF xA4 NIOS RFIC command path at commit
`33fead4b` plus the changes in this branch. Nothing here was run on hardware
and nothing is a Nios II time. Everything that needs time on the device is
left to HW-NIOS-WCET-001 (cold INIT, per-command max idle gap, host watchdog
lifetime, stack/heap high-water).

Regenerate the raw numbers with `python3 host/misc/nios_wcet_static.py`.

## 1. Method and limits

`host/misc/nios_wcet_static.py` parses the ADI AD9361 driver
(`thirdparty/analogdevicesinc/no-OS/drivers/rf-transceiver/ad9361/`),
`fpga_common/src/ad936x_helpers.c` and the NIOS command files
(`devices_rfic_cmds.c`, `devices_rfic.c`) into one call graph.

- Preprocessing: `#if`/`#ifdef` are evaluated for the NIOS build
  (`BLADERF_NIOS_BUILD`, `NUAND_MODIFICATIONS`, `ALTERA_PLATFORM`,
  `HAVE_SPLIT_GAIN_TABLE=0`; `hdl/fpga/ip/analogdevicesinc/no_OS/CMakeLists.txt:100`,
  `platform_bladerf2_headless/config.h:55,64`). `AXI_ADC_NOT_PRESENT`, `_DEBUG`,
  `DEBUG` are undefined.
- The closure is path-insensitive. Runtime guards are not evaluated, so a
  closure can contain code that the NIOS configuration never executes
  (section 4 lists the guards that exclude it).
- A loop counts as "with I/O" when its body, or any function called from it,
  contains SPI/AXI/delay/poll calls (earlier text-only classification
  mislabelled loops that call a step function as CPU-only).
- Call-site counts are per single call and are not multiplied by loop trip
  counts. They are an upper bound on distinct sites, not a per-turn count.

Result of the run: 555 functions, 130 loops, 62 without I/O; 57 driver/helper
roots are called from the NIOS sources, 301 functions are reachable from them,
41 of the CPU-only loops are reachable.

## 2. Step definition and safe points

A *safe point* is a call of `rfic_service_safe_point()`
(`devices.c:52-55`), which calls `bladerf_platform_service()` and so the hook
registered at `bladeRF_nios.c:816` (`service_rfic_status_request`,
`bladeRF_nios.c:709`; non-reentrant through the `servicing` flag, `:711,718`).
The hook only serves status reads and duplicate ACKs.

| Source of a safe point | Where | Spacing |
| --- | --- | --- |
| End of every SPI transaction (success) | `devices.c:931` | one per `bladerf_spi_command_bounded` |
| SPI timeout path | `devices.c:942` | same |
| SPI arbiter lock / unlock failure | `devices.c:765`, `:793` | failure only |
| AXI read / write | `devices.c:1165` (`adi_axi_read`, `:1144`), `:1196` (`adi_axi_write`, `:1176`) | one per access |
| `no_os_udelay` / `no_os_mdelay` | `platform_bladerf2_headless/platform.c:63-92` (`BLADERF_PLATFORM_SERVICE_SLICE_US` = 1000, `:61`) | one per slice of at most 1000 us; `usecs == 0` also services |

The NIOS build uses `platform_bladerf2_headless` (`CMakeLists.txt:42,86`), whose
SPI ops table has only `write_and_read` (`bladerf2_headless_spi.c:90,97` ->
`adi_spi_write` `devices.c:1027` / `adi_spi_read_checked` `:948`), so every
AD9361 register access is one `bladerf_spi_command_bounded`, including
read-modify-write (two transactions, two safe points).

Software deadlines per SPI access (compile-time constants, not measured):
SPI poll loop 10 ms or 1,000,000 polls (`devices.c:835,837`), arbiter acquire
and release 10 ms or 100,000 reads each (`devices.c:712-713`,
`spi_arbiter_policy.h:23-100`). Between two safe points on the success path
the code can run unlock + lock + one SPI command, so the software bound is
3 x 10 ms. The deadline is checked every 64 (arbiter) or 256 (SPI) polls, so
the overshoot is not bounded by this analysis.

A worker *turn* is one call of `rfic_command_work_wq`
(`devices_rfic.c:583`, pumped from `pkt_16x64.c:278`): either one command
callback invocation or one `*_transaction_poll` (`devices_rfic.c:625-900`).
A turn can contain synchronous sections that wait; those still reach a
safe point on every SPI/AXI access and every delay slice.

Correction to the earlier note: `devices.c:1165,1196` are AXI safe points; the
SPI ones are `:931,942`.

### Maximum gap between safe points

- In SPI/AXI operations: 1 for every path found. No NIOS or driver code path
  issues a second SPI/AXI access without passing `devices.c:931/942/1165/1196`.
  The write-register batch (`adi_spi_write_register_batch`, `devices.c:1076`,
  `count <= 3`) and gain-table row (`:1111`) hold the arbiter across several
  accesses but call `adi_spi_write_unlocked`, which ends in
  `bladerf_spi_command_bounded`, so each access still has its own safe point.
- In delays: at most 1000 us per slice.
- In CPU-only loops: see section 5. All reachable CPU-only loops have a
  literal, array, enum or static-table bound.

## 3. Command table (`devices_rfic_cmds.c`, dispatch table `devices_rfic.c:368`)

"Sites" = static SPI call sites in the closure of the callback (not loop
multiplied; includes code reached only through guarded branches, see
section 4). "Async" = work that continues as worker polls after the callback
returns `true` with a pending job. Every row: gap in SPI/AXI ops = 1.

| Command | Callback | Callback work | Async continuation | Sites |
| --- | --- | --- | --- | --- |
| STATUS, TRANSACTION_STATUS, PROGRESS, RESULT_DETAIL | `_rfic_cmd_rd_status` `:904`, `_rd_transaction_status` `:950`, `_rd_progress` `:968`, `_rd_result_detail` `:1035` | state snapshot, no SPI/AXI; allowed while faulted (`devices_rfic.c:1279-1286`) | none | 0 |
| INIT | `_rfic_cmd_wr_init` `:1063` -> `_rfic_initialize` `:461`; `_rfic_cmd_rd_init` `:1169` | first turn: RFFE clear, reset release + `no_os_mdelay(1)` (`:564`), IF-clock wait at most 200 x 100 us (`:49-65`), `ad9361_init_transaction_start`; later turns: tail phases (per-module FIR pair, sample rate, final LO; per-channel TX gain, mute) | `ad9361_init_transaction_poll`, then FIR load/enable, sample-rate, RFPLL, RFDC, TXQ polls | 123 (rd: 0) |
| ENABLE | `_rfic_cmd_wr_enable` `:1186`, `_rd_enable` `:1408` | RFFE read/write, LO read (SPI), RF port select (SPI) | RFDC poll or TXQ poll, then a resume callback that only commits the RFFE | 17 (rd: 0) |
| SAMPLERATE | `_rfic_cmd_wr_samplerate` `:1431`, `_rd_samplerate` `:1525` | `ad9361_sample_rate_transaction_start`; BIST tune only if `rx_adc != NULL` (`:1486-1507`, absent on NIOS) | `ad9361_sample_rate_transaction_poll`, resume callback for the interface check | 99 (rd: 19) |
| FREQUENCY | `_rfic_cmd_wr_frequency` `:1698` -> `_internal` `:1555`, `_rd_frequency` `:1713` | `ad9361_fastlock_exit_foreign`, RFFE, RF port select, `ad9361_rfpll_clock_transaction_start`; synchronous `ad9361_set_{rx,tx}_lo_freq` only on `-ENOTSUP` (external LO, `:1651-1659`) | RFPLL poll, then RFDC (RX) or TXQ (TX) poll | 77 (rd: 4) |
| BANDWIDTH | `_rfic_cmd_wr_bandwidth` `:1742`, `_rd_bandwidth` `:1805` | `ad9361_validate_rf_bw`, `ad9361_rf_bandwidth_transaction_start` | `ad9361_rf_bandwidth_transaction_poll` | 10 (rd: 0) |
| GAINMODE | `_rfic_cmd_wr_gainmode` `:1835`, `_rd_gainmode` `:1891` | synchronous register writes | none | 11 (rd: 0) |
| GAIN | `_rfic_cmd_wr_gain` `:1937`, `_rd_gain` `:1975` | synchronous register accesses | none | 9 (rd: 15) |
| RSSI | `_rfic_cmd_rd_rssi` `:2010` | synchronous register reads | none | 5 |
| FILTER | `_rfic_cmd_wr_filter` `:2357` -> `_internal` `:2092`, `_rd_filter` `:2379` | init-pair path: `ad9361_set_{rx,tx}_fir_config_transaction_start` (async). Non-init path: synchronous `ad9361_set_{tx,rx}_fir_config_with_cleanup` (`:2190`, `:2317`) = coefficient load + ENSM transitions with delay waits, then `ad9361_fir_enable_transaction_start` | FIR load poll (init pair), FIR enable poll | 33 (rd: 0) |
| TXMUTE | `_rfic_cmd_wr_txmute` `:2408`, `_rd_txmute` `:2427` | synchronous | none | 5 (rd: 0) |
| FASTLOCK | `_rfic_cmd_wr_fastlock` `:2448` | `adi_fastlock_save` loop of 16 x (write + read) (`devices.c:1278-1290`) | none | 25 |

Worker poll roots (one turn each, `devices_rfic.c:625-900`). Static counts as
above:

| Poll root | SPI sites | AXI sites | I/O loops | CPU-only loops |
| --- | --- | --- | --- | --- |
| `ad9361_init_transaction_poll` | 348 | 34 | 38 | 36 |
| `ad9361_sample_rate_transaction_poll` | 155 | 32 | 28 | 33 |
| `ad9361_fir_enable_transaction_poll` | 155 | 32 | 28 | 33 |
| `ad9361_rf_bandwidth_transaction_poll` | 81 | 0 | 2 | 8 |
| `ad9361_tx_quad_calibration_transaction_poll` | 81 | 0 | 2 | 8 |
| `ad9361_rfdc_calibration_poll` | 31 | 0 | 1 | 5 |
| `ad9361_rfpll_clock_transaction_poll` | 27 | 0 | 2 | 10 |
| `ad9361_fir_load_transaction_poll` | 18 | 0 | 0 | 5 |

A poll is a stage state machine; one call executes one stage, so the real
per-turn count is much smaller than the closure total. Per-stage and per-turn
timing is what `rfic_init_timing_*` (`devices_rfic.h`, `max_worker_ticks`,
`max_service_gap_ticks`) measures on hardware (HW-NIOS-WCET-001).

Synchronous sections inside one turn that wait on the chip (each iteration
passes a safe point):

- INIT reset: `ad9361_reset` two `no_os_mdelay(1)` (`ad9361.c:1110,1114`).
- INIT post-calibration stage: `ad9361_ensm_set_state` (`ad9361.c:7497,7546,7548`,
  `ad9361_wait_status_timeout` `:1691`), `ad9361_rssi_gain_step_calib_with_cleanup`
  (`:16149-16215`, `ad9361_run_calibration_timeout` `:4773`),
  `ad9361_rssi_program_lna_gain` (`:16314`).
- Non-init FILTER: `ad9361_ensm_transition_sync` (`ad9361.c:3129`), coefficient
  load loop (`ad9361.c:13222`, one SPI per iteration).

## 4. Where the closure is wider than the NIOS configuration

The closure of `init`/`sample_rate`/`fir_enable` polls also contains
`ad9361_dig_tune_*`, `ad9361_check_pn`, `ad9361_bbpll_clock_set_rate`,
`ad9361_rfpll_int_set_rate`, `ad9361_load_gt`,
`ad9361_ext_band_transaction_run_sync`. These are excluded at run time:

- Digital interface tuning (BIST delay search with `no_os_udelay`): INIT refuses
  to create the PHY unless `skip_mode == 2` and `fir_disable != 0`
  (`rfic_init_policy.h:43-47`, `devices_rfic_cmds.c:503-510`);
  `ad9361_dig_tune_with_cleanup` takes the skip path at `ad9361_conv.c:936`;
  `ad9361.c:7998` and `:13700` need `!dig_interface_tune_fir_disable`; `:8070`
  needs `pdata->bb_clk_change_dig_tune_en`, which no bladeRF source assigns
  (grep over `fpga_common`, `host/libraries`, `hdl`, the driver);
  `ad9361_api.c:833` excludes the converter block under `BLADERF_NIOS_BUILD`.
- `clk_set_rate` from the clock-chain DIVIDERS stage (`ad9361.c:7943,7960`) is
  called for divider clocks only; the BBPLL and RFPLL cases of its `switch`
  (`ad9361_util.c:~520+`) are not taken. The BBPLL and RFPLL changes use the
  retained `*_transaction_poll` roots.
- External band control: `ad9361_ext_band_transaction_start` returns at once
  when `phy->ext_band_ctl == NULL` (`ad9361_ext_band_ctrl.c:569-573`).
  `ad9361_register_ext_band_control` is called only from
  `host/misc/test_ad9361_calibration_read_error.c`. Its `for (;;)` poll loop
  (`:650`) is bounded by `AD9361_EXT_BAND_MAX_SEQ_DEPTH` = 8 and the
  configured GPIO count, and has no safe point between GPIO writes.
- `adi_fastlock_recall` (`devices.c:1688`, unbounded do/while with a delay per
  iteration) is compiled out of the xA4 build: `pkt_retune2.c:1082-1105`
  always uses `adi_fastlock_recall_start` under `BLADERF_NIOS_LIBAD936X`.

## 5. Loops

I/O loops (each iteration performs an SPI/AXI access or a delay, so each has a
safe point): `for (;;)` waits `ad9361_wait_status_timeout` (`ad9361.c:1691`),
`ad9361_ensm_transition_sync` (`:3129`), `ad9361_run_calibration_timeout`
(`:4773`), `ad9361_set_trx_clock_chain_with_cleanup` (`:8120`),
`ad9361_rfpll_int_set_rate` (`:15521`), `ad9361_bbpll_clock_set_rate`
(`ad9361_util.c:326`), step cursors `ad9361_gc_update` (`:3562`),
`ad9361_pp_port_setup` (`:6719`), `ad9361_rssi_setup` (`:7449`), constant-bound
register loops (`:2611`, `:6158` x2, `:7160`, `:7420` x9, `:8540`, `:16149` x4,
`:16188` x15, `:16376` x16); the `ad9361_conv.c` loops (`check_pn`, `hdl_loopback`,
`midscale_iodelay`, `dig_tune_*`) belong to digital interface tuning, excluded by
the guards in section 4. Every `for (;;)` wait either performs SPI in each
`poll` or calls `no_os_udelay` (serviced in 1 ms slices); the clock-chain
non-BBPLL stages are finite stage steps.

CPU-only loops reachable from the NIOS roots (41, full list in the script
output). Bounds, all checked in source:

| Loop | Bound |
| --- | --- |
| `ad9361_find_opt` (`ad9361.c:1031`) | 64 (`search->field`, `ad9361.h:3879`); 32/16 from `ad9361_conv.c:346,669` |
| `find_table_index` (`:1886`), `ad9361_gain_table_find_index` (`:1923`) | `max_index` 77 (`SIZE_FULL_TABLE`, `:408`) or 41 (`SIZE_SPLIT_TABLE`, `:533`) |
| `ad9361_gt_tableindex` (`:1791`) | `ad9361_adi_gt_info[]` sentinel entry `tab == NULL` (`:612-660`) |
| `ad9361_rfpll_vco_init` (`:2536`) | `SYNTH_LUT_SIZE` 53 (`:72`) |
| `ad9361_calc_rfpll_int_divder` (`:15048`) | at most 7: frequency validated to at least 70 MHz by `ad9361_validate_rfpll` (`:996-1015`, `ad9361.h:2849`) against `MIN_VCO_FREQ_HZ` 6 GHz (`ad9361.h:2847`) |
| `ad9361_calculate_rf_clock_chain` (`:8257`, `:8304`) | `i < 7`; do/while halves `div` from `MAX_BBPLL_DIV` to `MIN_BBPLL_DIV`, at most 6 |
| `ad9361_validate_trx_clock_chain` (`:7729`, `:7785`, `:7790`), `ad9361_validate_clock_chain_path` (`:7673`) | array sizes, 3, 4 |
| `ad9361_rssi_setup_step` (`:7365-7381`) | 4, 15, 4 |
| `ad9361_tx_bb_second_filter_prepare` (`:4284`) | 4 |
| `ad9361_mark_calibration_state_unknown` (`:4406-4421`), `ad9361_unregister_clocks` (`:16063`), `clk_set_rate` (`ad9361_util.c:500,657`) | enum counts (`NUM_AD9361_CLKS` = 20, `ad9361.h:3299`) |
| `ad9361_invalidate_clock_rate_subtree` (`ad9361_util.c:143-158`) | fixpoint over `NUM_AD9361_CLKS`, at most 20 passes of 20 |
| `int_sqrt` (`ad9361_util.c:711`) | 16 (`m` starts at `1 << 30`, shifts by 2) |
| `ad9361_init_transaction_start` (`ad9361_api.c:263,266`), `ad9361_fir_enable_transaction_start_mask` (`ad9361.c:13876,13878`) | 6, array sizes |

The largest chain between two consecutive I/O calls is therefore a few
hundred iterations of integer bodies (the 20 x 20 fixpoint is the largest
single product). Converting that to time needs Nios II measurement.

NIOS-side loops outside the driver: `adi_spi_write_register_batch`
(`devices.c:1093`, `count <= 3`), `adi_fastlock_save` (`:1278`, 16),
`adi_fastlock_load` (`:1490`, `fl_prog_bytes`), `adi_rfpll_synth_read`
(`:1670`), RETUNE2/RFIC queue scans (`pkt_retune2.c:661-1699`,
`devices_rfic.c:1078`, bounded by queue capacity), `dwell_summary_read`
(`devices_inline.h:347`, 4 x 14), `rx_sfir_wait_status`
(`devices_inline.h:618`, 10000 MMIO reads), `command_uart_write_response`
(`devices_inline.h:911`, 100000 MMIO reads). The last two have no safe point;
they are MMIO-only and counted. I2C TIP waits (`devices.c:615-671`) use a 10 ms
deadline (`:590`) or 100000 tries and are not on the RFIC command path.

### Waits that are unbounded in elapsed time by design

- `ad9361_status_wait_poll` (`ad9361.c:1610-1678`): "Only the observed state or
  an observation error can retire this wait" (`:1673`). The poll returns 1
  until the chip reports the state. In the NIOS configuration this runs inside
  the 1 ms-sliced delays of the callers above (`ensm`, `calibration`) or as
  worker turns.
- `adi_fastlock_recall_poll` (`devices.c:1597`): "has no elapsed-time or
  poll-count completion cutoff" (`:1594-1596`); polled once per worker turn
  from `pkt_retune2.c:1029,1793,1809` (one SPI read per turn).

No loop reachable from the NIOS RFIC command path was found that waits on
hardware without a safe point in each iteration, so no code change was made.
The total duration of any command that contains one of the waits above is not
statically bounded; that is the content of HW-NIOS-WCET-001.

## 6. Validity of AD9361 state after a partial mutation

One flag describes the whole chip. There is no per-channel validity in the
driver or in NIOS, so RX1, RX2, RX_X2 and TX share the same latch and differ
only in how the host reports it.

AD9361 driver:

- `phy->calibration_state_unknown` (`ad9361.h:3386`), set by
  `ad9361_mark_calibration_state_unknown` (`ad9361.c:4359-4365`), which also
  drops every cached LO, rate, bandwidth, FIR, port, TX-quad and gain-table
  record (`:4366-4428`). 231 call sites in `ad9361.c`.
- Effect: `ad9361_reg_write` returns `-EIO` (`ad9361.c:864-865`), `clk_set_rate`
  and every `*_start` I read refuse (`clk_set_rate`: `ad9361_util.c:494-495`).

NIOS (`devices_rfic.c`):

1. `rfic_checked_poll_status` marks the flag for any poll status above 1
   (`:50-55`); a terminal failure of a mutating job marks it inside the driver.
2. `rfic_latch_calibration_fault` (`:557-572`): if the flag is set, set
   `state.calibration_faulted` (`devices_rfic.h:159`), set `init_state` to OFF
   (so STATUS reports not initialized, `_rfic_is_initialized`
   `devices_rfic.h:384`), and force RFFE off (`rfic_force_rf_path_off`
   `devices_rfic_cmds.c:208` -> `_clear_rffe_ctrl` `:161`).
   Call sites: `:221` (TXQ failure), `:892` (TXQ poll during INIT), `:1009`
   (after every worker step), `:1242` (after an immediate write).
3. Gating while faulted: worker `:977` (`rfic_calibration_command_allowed`,
   `rfic_calibration_policy.h:118`), `rfic_command_write` `:1166`,
   `rfic_command_write_immed` `:1231`; reads are refused except STATUS,
   TRANSACTION_STATUS, PROGRESS, RESULT_DETAIL, INIT (`:1279-1286`); raw
   AD9361 SPI writes and gain-table rows are refused
   (`pkt_16x64.c:43-48,53`, `rfic_direct_ad9361_write_allowed`). Raw AD9361 SPI
   reads stay allowed (`pkt_16x64.c:96-107`).
4. RETUNE2 (`pkt_retune2.c`): a new request is rejected with
   `NIOS_PKT_RETUNE2_FAILURE_RFIC_FAULTED` (`:1011,1049-1054`, also `:1935`);
   `fail_queue_for_rfic_fault` (`:931-947`) records that failure for every
   queued entry; the scheduler calls it for both queues at `:1777-1779`
   (`rx_queue` and `tx_queue`). A recall already started is drained first
   (`:1027-1036`).

libbladeRF NIOS-owned path (`rfic_fpga.c`):

- A failed command: the wait loop reads STATUS (`rfic_fpga.c:97-113`,
  `:205-420`), maps the retired result code through
  `_rfic_fpga_result_to_status` (`:503`), reads RESULT_DETAIL when supported
  (`:317-345`, `:423-440`) and returns a negative status to the public setter.
- A failed RETUNE2: `nios_access.c:2934-2992` extracts the failure stage;
  RX: `bladerf2.c:5098-5160` publishes `BLADERF_RF_EVT_RX_RETUNE_FAILED` with
  `BLADERF_RF_NIOS_FAILURE_RFIC_FAULTED` (`libbladeRF.h:5360`);
  TX: `bladerf2_tx_retune_failure_detail_event`
  (`rx_event_history.c:500-530`, called at `bladerf2.c:4793`,
  `rf_transition.c:1086`) clears `tx_async_rfic_state_certified` and bumps
  `tx_async_admission_generation`.
- RX setters call `bladerf2_rx_data_invalidate` before the mutation
  (`rf_transition.c:3221`), which revokes the RX epoch certificate for
  RX1 and RX2 together ("shared by RX1/RX2", `:3272`); the certificate
  is set again only by an explicit epoch-valid event
  (`rx_event_history.c:302`).
- TX recovery stays blocked while the host or driver flag is set
  (`tx_fault_recovery_board.c:106` returns `BLADERF_ERR_WOULD_BLOCK`).

libbladeRF host-owned path (`rfic_host.c`):

- `_rfic_host_calibration_faulted` (`:54-68`) copies the driver flag into
  `board_data->calibration_faulted` and drops the FIR knowledge.
- `_rfic_host_fail_closed` (`:71-117`): zero the RFDC calibration mask,
  `bladerf2_rx_rfic_state_invalid` (`:85`, `rfic_fault.c:37-67`), then
  `bladerf2_tx_stream_set_faulted` (`:91`, `rf_transition.c:157-178`), TX event
  `BLADERF_RF_EVT_TX_RFIC_STATE_INVALID` (`:98`), RFFE isolation and TX gate
  abort. Every `CHECK_AD936X` calls it (`rfic_host.c:256-272`).

Per configuration:

| Configuration | What invalidates it | What is reported |
| --- | --- | --- |
| RX1, RX2 | driver flag -> NIOS latch (or host latch); RX epoch revoked at setter start (`rf_transition.c:3221`) and by `bladerf2_rx_rfic_state_invalid` (host-owned) | `BLADERF_RF_EVT_RX_DATA_INVALIDATED`, flags `BLADERF_RF_INVALIDATE_RFIC_REG`, `channel_flags = 0` (`rfic_fault.c:60`), so no RX1/RX2 attribution; a channel companion event exists only when a transition carries the channel (`rx_event_history.c:589-609`, `rf_transition.c:649,885`) |
| RX_X2 | same revocation; there is a single RX sync stream and epoch (`sync_rx_epoch_revoke_delivery_with_callback(&board_data->sync[BLADERF_RX], ...)`, `rfic_fault.c:66`) | same event; the layout does not change the revocation, `rfic_host.c:803,817` only changes MIMO enable handling |
| TX | `bladerf2_tx_stream_set_faulted`, `tx_async_rfic_state_certified = false` (`rf_transition.c:170-175`, `rx_event_history.c:531`), RFFE disconnect at the NIOS latch (`devices_rfic.c:566`) | `BLADERF_RF_EVT_TX_RFIC_STATE_INVALID` (host-owned), `BLADERF_RF_EVT_TX_RETUNE_FAILED` with failure stage (NIOS-owned RETUNE2) |

Monitor gap (found while tracing, fixed): on the NIOS-owned path the RX/TX
monitor (`rf_transition.c:3066-3125`) judged health only from raw AD9361 SPI
reads (`_read_rfic_reg`), which NIOS serves while faulted
(`pkt_16x64.c:96-107`). A NIOS latch that set no failed command result or
RETUNE2 failure record (for example a fault latched by a poll after the
setter returned) therefore left healthy-looking PLL/ENSM/BBPLL values and no
host signal. The single source of truth is the NIOS latch, which clears
`init_state` to OFF (`devices_rfic.c:566`); the INIT read is served while
faulted (`devices_rfic.c:1279-1286`). In FPGA tuning mode `_read_rfic_reg`
now reads NIOS INIT first and returns an error unless it is ON
(`bladerf2_rx_monitor_nios_state_readable`, `rf_transition_policy.h`), so the
existing status-read-failure paths revoke the RX epoch
(`BLADERF_RF_INVALIDATE_RFIC_STATUS_UNAVAILABLE`) and mark TX invalid. Test:
`test_rx_monitor_nios_state` (policy only; not run against a device).
HOST tuning with NIOS RETUNE2 sweep sessions (checked, no gap): NIOS can
latch `calibration_faulted` only through its own RFIC commands, and those are
issued only by `rfic_fpga_control`. `rfic_latch_calibration_fault` returns at
once without a NIOS PHY (`devices_rfic.c:557-560`), and in HOST mode NIOS has
none: `bladerf2_set_tuning_mode` runs `rfic_other->deinitialize` before
setting `tuning_mode = HOST` and returns on failure (`bladerf2.c`, the
`CHECK_STATUS(rfic_other->deinitialize(dev))` step), so a failed NIOS deinit
(the paths that set the flag without a PHY, `devices_rfic_cmds.c:192,306,479,
1146`) keeps the board in FPGA mode, where the monitor now reads NIOS INIT.
In HOST mode NIOS RETUNE2 failures (lock read failure, timeout, rejected
request) are not a latch: each one goes through `record_retune2_failure`
(`pkt_retune2.c`), which the host reads as a failure count plus exact
records. The sweep session turns them into `BLADERF_RX_SWEEP_FAILED` or, if
records are lost or the counts disagree, `BLADERF_RX_SWEEP_LOST`
(`rx_sweep.c:160-300`), and the monitor's `should_poll_rx_retune` path
consumes the same counters (`bladerf2.c:5098-5160`). Not exercised on a
device.


## 7. Int-only wrappers

`ad9361_*_with_cleanup` functions report a primary status and a separate
cleanup status. The script finds 32 definitions and 32 wrappers that call them
with `NULL` (two, `ad9361_set_rx_sampling_freq` / `_tx_`, through
`ad9361_sample_rate_transaction_run_sync`):

- `ad9361.c`: `ad9361_clk_mux_set_parent`, `ad9361_do_calib_run`,
  `ad9361_do_calib_timeout`, `ad9361_load_fir_filter_coef`, `ad9361_parse_fir`,
  `ad9361_rssi_gain_step_calib`, `ad9361_set_trx_clock_chain`,
  `ad9361_set_trx_clock_chain_freq`, `ad9361_setup`,
  `ad9361_update_rf_bandwidth`, `ad9361_validate_enable_fir`.
- `ad9361_api.c`: `ad9361_do_calib`, `ad9361_get_rx_fir_config`,
  `ad9361_get_tx_fir_config`, `ad9361_init`, `ad9361_set_no_ch_mode`,
  `ad9361_set_rx_fir_config`, `ad9361_set_rx_fir_en_dis`,
  `ad9361_set_rx_lo_int_ext`, `ad9361_set_rx_rf_bandwidth`,
  `ad9361_set_rx_sampling_freq`, `ad9361_set_trx_fir_en_dis`,
  `ad9361_set_trx_path_clks`, `ad9361_set_tx_fir_config`,
  `ad9361_set_tx_fir_en_dis`, `ad9361_set_tx_lo_int_ext`,
  `ad9361_set_tx_rf_bandwidth`, `ad9361_set_tx_sampling_freq`,
  `ad9361_trx_load_enable_fir`.
- `ad9361_conv.c`: `ad9361_dig_interface_timing_analysis`, `ad9361_dig_tune`,
  `ad9361_post_setup`.

Callers outside the wrappers (grep over `hdl`, `host`, `fpga_common`,
`thirdparty/analogdevicesinc/no-OS/drivers`, excluding tests and `host/misc`):

- NIOS: none. It calls the cleanup-aware forms
  (`devices_rfic_cmds.c:1495,2190,2317`) or the `*_transaction_start/poll` API.
- libbladeRF: none. `rfic_host.c:464,986,989,1244,1247,1702,1724,1785,1807` and
  `rf_transition.c:4783` call the cleanup-aware forms.
- Driver internals: `ad9361.c:12003` calls `ad9361_do_calib_timeout`.
- `iio_ad9361.c:1205,1207,2619` (`ad9361_set_{tx,rx}_fir_en_dis`,
  `ad9361_setup`); `iio_ad9361.c` is not part of the NIOS or host library
  build.

One int-only public function with no `_with_cleanup` variant:
`ad9361_ensure_tx_quad_calib` (`ad9361.c:11671`) owns a local
`ad9361_tx_quad_calibration_transaction` and discards its `cleanup_status`.
Its only library caller is `rfic_host.c:357`, which compensates by testing
`ad9361_calibration_state_unknown(phy)` after a failure (`:362`) and then
calling `_rfic_host_fail_closed`. NIOS uses
`ad9361_ensure_tx_quad_calib_start` directly (`devices_rfic.c`, TXQ jobs) and
reads `tx_quad_job.cleanup_status` (`devices_rfic.c:212,862`).

## 8. Not established here

- Elapsed time of any section on the Nios II (HW-NIOS-WCET-001).
- Overshoot of the 10 ms deadlines (checked every 64/256 polls).
- Per-stage SPI counts of the poll state machines.
- Hardware behaviour of the monitor INIT read (USB cost per poll, behaviour during a legitimate INIT in flight).
