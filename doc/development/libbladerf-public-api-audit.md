# libbladeRF public API audit: channel validation and NIOS deadlines

Scope: entry points of `libbladeRF.h` / `bladeRF2.h` that take a channel
(RX0/RX1/TX0/TX1) and lead into `bladerf2.c`, `rf_transition.c`, `rx_sweep.c`.
Rule checked: an out-of-range channel returns `BLADERF_ERR_INVAL` before any
RFIC/NIOS/GPIO access, array index or board-state change.

Test: `host/libraries/libbladeRF_test/test_host_api_audit`. It compiles the
library sources into the test (the board tables are static and hidden in the
shared object), runs every row below with 11 invalid channels (4..8, 100, -1,
-2, INT_MAX, INT_MIN, `BLADERF_CHANNEL_INVALID`) against a fixture with no
backend and no RFIC controller, so any side effect faults and is reported by
entry-point name. The same row with each valid channel must get past
validation (otherwise the fixture would be vacuous). Both the board-table
member (`board.*`) and the public wrapper (`public.*`) are exercised.

## Channel validation

Layer 1 is `bladerf.c` (`valid_public_channel`, `valid_gain_cal_channel`,
`valid_trigger_channel_signal`). Layer 2 is the board-table member; before this
audit layer 2 trusted layer 1, so a direct `dev->board->fn()` call or an
internal caller reached side effects with an invalid channel.

| Entry point (public / board member) | Layer 1 | Layer 2 | Test |
|---|---|---|---|
| `bladerf_set_gain`, `_get_gain`, `_set/get_gain_mode`, `_get_gain_modes`, `_get_gain_range`, `_set/get_gain_stage`, `_get_gain_stage_range`, `_get_gain_stages` | yes | added (`CHECK` at entry); was: faulted on NULL RFIC for get/set, `-EWOULDBLOCK` after `get_frequency` GPIO read for ranges/set | `public.*`, `board.*` |
| `bladerf_set/get_sample_rate`, `_set/get_rational_sample_rate`, `_get_sample_rate_range` | yes | added | same |
| `bladerf_set/get_bandwidth`, `_get_bandwidth_range`, `_select_band` | yes | added | same |
| `bladerf_set/get_frequency`, `_get_frequency_range` | yes | added; `set/get_frequency` read the reference-clock GPIO and cleared `rfic_reference_clock_known` on failure before any channel check | same |
| `bladerf_enable_module` | yes | added; was: board state changed for an invalid channel (test: state_dirty) | same |
| `bladerf_set/get_rf_port`, `_get_rf_ports` | yes | added; was: faulted (no RFIC/phy) for an invalid channel | same |
| `bladerf_get_quick_tune`, `_get_quick_tune_profile`, `_load_quick_tune_profile`, `bladerf2_quick_tune_readback` | yes | added; `get_quick_tune` checked only after the GPIO reference-clock read | same |
| `bladerf_schedule_retune` (`bladerf2_schedule_retune_sequenced`), `_cancel_scheduled_retunes` | yes | added | same |
| `bladerf_set/get_correction` | yes | check moved to function entry (was after `board_data->phy` use, faulted) | same |
| `bladerf_trigger_init`, `_read_trigger`, `_write_trigger` | yes (`valid_trigger_channel_signal`) | `fpga_trigger_*` already accept only RX0/TX0; explicit check added at the board member, no defect found | same |
| `invalidate_rx_data` (all RX/TX setters call it) | yes | added; was: set `rf_transition_setter_active`, revoked sync admission and wrote an event for any channel value | `board.invalidate_rx_data` |
| `tx_frequency_reconfigure_begin/complete` | `IS_TX` only | added (TX0/TX1 only) | `board.tx_frequency_reconfigure_begin` |
| `rx_reconfigure_complete` callback | yes | added (void) | covered by public setters |
| `bladerf_rx_transition_begin`, `_begin_quick_tune` | checked inside `_bladerf_rx_transition_begin`, after the lifecycle gate | channel check moved before `bladerf2_rx_transition_call_enter` | `public.rx_transition_begin` |
| `bladerf_rx_capture_close` | `bladerf2_rx_channel_valid` first | n/a | `public.rx_capture_close` (fixture certified) |
| `bladerf_rx_sweep_begin/_schedule/_completions/_end` | yes (+ RX only) | `sweep_channel_valid`, session channel compare | `*.rx_sweep_*` (session active for schedule/end) |
| `bladerf_enable/print/get_gain_calibration`, `_get_gain_calibration_copy`, `_load_gain_calibration`, `_get_gain_target` | `valid_gain_cal_channel` first | n/a | `public.*_gain_*` |
| `bladerf_get/set_bias_tee`, `_set/get_rx_qec_tracking`, `_get_rx_bbdc_tracking_words`, `_get_rfic_rssi` | n/a (bladeRF2 API) | channel check first | `public.*` |
| `rfic_host_control.set_sample_rate` | n/a | added (non-TX used to fall into the RX branch) | `board.rfic_host_set_sample_rate` |
| `bladerf_get_rx_fpga_adc_chan_regs` (raw bank 0..3, not a `bladerf_channel`) | n/a | `ch > 3` first | `test_adc_channel_index` |

Not channel-indexed (mask/array arguments checked at entry, no change):
`bladerf_set_rx_fpga_{iqcor,dcfilt,adc_enable}_mask` (`mask & 0xf0`),
`bladerf_set_rx_tracking_config` (`qec_mask & ~0x03`).

## NIOS request/response deadlines

| Path | Deadline | Result |
|---|---|---|
| `nios_usb_transaction` (every NIOS packet) | per-request response timeout, late ACK handled by `pending_read_response` | covered by `test_nios_transaction` |
| `nios_8x16_write` BUSY retry (INA219, DAC, IQ-corr writes) | none (`while (true)` on BUSY) | fixed: 1 s monotonic deadline, `BLADERF_ERR_TIMEOUT`; test BUSY x2 then ACK = 3 reads, BUSY forever = timeout in 0.9..3 s |
| `nios_retune_clear_wait` | 1 s | ok |
| TX gate abort / flush / release polls | `timeout_ms` argument | ok |
| `nios_retune2_wait_now_terminal` | none: `for (;;)` with `usleep(120)` until a completion record matches | open, reported (NIOS RETUNE2 owner) |
| `bladerf2_tx_wait_for_validity` | none: `COND_WAIT` until the monitor thread certifies | fixed: 2 s monotonic deadline, `BLADERF_ERR_TIMEOUT` (`test_tx_validity_deadline`) |
| `rf_transition.c` calibration and first-host-data waits | `_deadline_expired`, failure detection only | ok |
| `_rfic_cmd_write` / `_rfic_fpga_*` (`rfic_fpga.c`) | not audited | channel check absent at layer 2, reported (NIOS RFIC owner) |

## Open items outside this change

- `nios_retune2_wait_now_terminal`: add a deadline; terminal cleanup must keep
  the fence when the ACK arrives late.
- `rfic_fpga.c` `_rfic_cmd_write`, `_rfic_fpga_{get,set}_{sample_rate,frequency,bandwidth}`,
  `_rfic_fpga_select_band`, `_rfic_fpga_get_rssi`, `_rfic_fpga_store_fastlock_profile`:
  no channel check; reachable only through the validated board table today.
