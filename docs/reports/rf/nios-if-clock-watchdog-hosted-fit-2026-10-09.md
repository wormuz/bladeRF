# NIOS AD9361 clock preflight and hosted A4 fit — 2026-10-09

## Change

The no-OS AD9361 AXI adapters use raw `IORD_32DIRECT`/`IOWR_32DIRECT` operations without a software timeout. If `if_l_clk` is absent, a NIOS request entering that aperture can wait indefinitely. The NIOS `control` PIO is clocked from the independent FPGA system clock and had an unused GPI[27]. The hosted and sweep FPGA projects now include `if_clock_watchdog`, which monitors `adi_rx_clock` in the `sys_clock` domain, requires eight divided-clock transitions before reporting alive, and withdraws alive after 100,000 system-clock cycles without an edge (1 ms at the current system-clock rate).

After enabling the RFIC and before `ad9361_init()` accesses the AXI core, `_rfic_initialize()` checks GPI[27] for at most 200 × 100 µs. Missing clock returns an initialization failure with stage `IF_CLOCK_TIMEOUT`; host diagnostics map that stage. This guard checks the precondition before entering the unbounded adapter. It does not protect a transaction if the clock stops after AXI access starts, and does not prove the cause of the preserved NIOS mode-switch hang.

## Verification

- `hdl/quartus/qsim if_clock_watchdog_tb`: pass; absent, running, stopped, and restarted clock cases.
- NIOS firmware build for hosted A4: pass.
- `hdl/quartus/qcheck`: clean.
- `cmake --build host/build --target libbladerf_shared -j2`: pass.
- `host/build/output/libbladeRF_test_sync_epoch_traversal`: pass (fixture warning/error output expected).
- Full hosted A4 Quartus compile: pass, 0 errors. Fit report: `hdl/quartus/work/bladerf-micro-A4-hosted/output_files/hosted.fit.rpt`; STA summary: `.../hosted.sta.summary`.
- Latest fitted RBF SHA-256: `e09d3e55162c9c039a9b255bef3ae766b81cbb2d82c8d02a4db9c4bd25e409da`.
- STA has no negative slack. Worst slow-corner setup is +0.563 ns; the minimum summary margin is +0.010 ns on the internal `altera_reserved_tck` hold check, while the minimum AD9361 RX PLL hold is +0.066 ns.
- `qgate` passes the fitted hosted report: expected 12 constrained handshake pairs, 9 ADI control bundles, 10 DCFIFO pointer bundles, no ignored/empty/no-path constraints, and no negative slack. The current DRC reports 669 D101 structures (338 held-handshake); the 30-bit RX fault-cause transfer is present in the fitted handshake logic but is not counted as D101 in this run. `qgate` was corrected to disposition this exact inventory only when the fitted RX fault handshake evidence and all 12 constrained-pair evidence are present. `qcheck` covers rejection when that evidence or a handshake pair is missing.

## Sweep A4 static fit

The same source also passed a full sweep A4 compile and the post-fit max-skew report. Compile log: `/tmp/sweep-a4-if-clock-watchdog-20261009.log`; fitted RBF SHA-256: `e477b0da5802ec8d648e72ee3d58369ac5d91922103dadd7d19c8a5ac31ddb6f`. Full compile had 0 errors. STA has no negative slack; worst slow-corner setup is +0.435 ns, minimum summary hold is +0.025 ns on internal `altera_reserved_tck`, and minimum AD9361 RX PLL hold is +0.082 ns. The four RX `up_xfer` bundles pair 61/61 bits; max-skew report found 240 paths, zero violated, worst slack +3.303 ns. Clock monitor bundles pair 2/2; status bundles pair 6/6. Sweep DRC reports 720 D101 (389 held-handshake) and D103=0. The fault-cause and sweep epoch-status handshakes appear in the fitted netlist and all 15 bundled-data handshakes are constrained; qgate now checks these exact current-source inventories against the generated fit/STA/DRC reports. `hdl/quartus/qgate /tmp/sweep-a4-if-clock-watchdog-20261009.log` and `hdl/quartus/qcheck` pass.

The qgate input was reconstructed from the completed fitter report plus its revision and STA-summary path because the full Quartus stdout was not saved to disk. This is an offline check of the exact generated fit/STA/DRC artifacts; it is not a separately archived full-run log.

## Qualification limits

No image was loaded and no USB reset, power cycle, or RF test was performed. The xA4 remains in its intentionally preserved NIOS nonresponse state. Host/NIOS hard-freeze diagnosis and hardware qualification remain open. The hosted and sweep RBFs are build artifacts, not release candidates.
