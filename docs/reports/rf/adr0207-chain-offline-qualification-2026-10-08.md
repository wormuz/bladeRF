# ADR-0207 chain qualification status, 2026-10-08

## Result

Offline checks for the current firmware/libbladeRF/Python-wrapper source pass.
This is not a stable release: the connected xA4 cannot be opened by either the
local or installed CLI because FPGA-version access times out. The candidate
seed-3 RBF has not been loaded, and live RX1 qualification has not run. RX2
qualification remains sequenced after RX1.

## Verified

- Current-source Quartus A4 seed-3 fit and qgate pass. The fitted RBF SHA-256
  is `f53cd1c1fbdc8176c40397d1881a70b5081e8d489854928fb7732dc20ab4fa12`.
- `hdl/quartus/qcheck` reports `clean`.
- `cmake --build host/build -j2` completes successfully.
- RX epoch metadata, status identity, and transition-policy native tests pass.
  The transition-policy test had a stale five-argument call after the helper
  became a three-argument policy; it is corrected and passes in commit
  `d2219dd4`.
- `run_sync_rx_epoch_traversal_test.sh` passes.
- GHDL `rx_epoch_controller_tb`, `rx_epoch_gate_tb`, and
  `fifo_writer_epoch_fence_tb` each pass.
- The live qualification source and script are persistent under `host/misc`;
  commit `ab117165` also keeps the compiled runner in `host/build/output` and
  rebuilds it only when its source, public header, or library changes. A
  repeated invocation confirmed the binary was reused. Execution reaches the
  device open and then stops at the documented FPGA-version timeout.
- The Python binding builds against the current fork using
  `PYTHON_BLADERF_CFLAGS='-I/home/bonho/projects/bladerf/host/libraries/libbladeRF/include'`
  and `PYTHON_BLADERF_LDFLAGS='-L/home/bonho/projects/bladerf/host/build/output -lbladeRF'`.
  A link-order defect was fixed in wrapper commit `9adf65d`: the extensions now
  retain `DT_NEEDED: libbladeRF.so.2` instead of relying on `LD_PRELOAD`. All
  29 wrapper tests and 31 scanner transition/epoch integration tests pass with
  `LD_LIBRARY_PATH` selecting the current fork. The default `/usr/local`
  libbladeRF on this host is older and does not provide the new RX event API;
  a release install must ship the wrapper with its matching library. A
  `python_bladerf-1.5.0-cp314` wheel was built; its packaged extension retains
  the dependency and imports successfully from the wheel when the current
  fork is selected through `LD_LIBRARY_PATH`.

## Hardware access failure

The bladeRF 2.0 micro enumerates as USB bus 2, address 4, serial
`f695006ba84a40daa7b777c6a6eba78`. Both
`LD_LIBRARY_PATH=host/build/output host/build/output/bladeRF-cli -e 'version'`
and `/usr/local/bin/bladeRF-cli -e 'version'` fail during open with
`get_fpga_version ... Operation timed out`. Resetting USB device `2cf0:5250`
did not restore protocol access. Debug verbosity localizes the timeout to the
first legacy NIOS reply while reading FPGA version byte 0:
`nios_legacy_access.c:128 Failed to receive NIOS II response`; USB enumeration
and FX3 firmware-version access succeed, but there is no reply on the NIOS
peripheral IN transaction. Forcing A4 detection does not change this. No FPGA
image or flash contents were changed.

## Release gate still required

Restore board protocol access, load the candidate RBF volatilely, then run the
existing RX1 transition qualification with stale-epoch leakage checks and
known-cell return validation. Only after RX1 passes, run RX2/paired-channel
qualification. Preserve event traces and timing distributions in the hardware
qualification report before calling this chain stable.
