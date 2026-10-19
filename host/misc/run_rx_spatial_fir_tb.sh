#!/usr/bin/env bash
# rx_spatial_fir GHDL regression (design doc/development/rx-spatial-mimo-fir-design.md
# section 6.1, T1-T8). Exits non-zero on any mismatch.
#
# Vectors are committed under hdl/fpga/ip/nuand/simulation/rx_spatial_fir_vectors.
# To regenerate them from the golden model first, point SDR_SCANNER at an
# sdr-scanner checkout (rfcore/spatial_fir.py) with its .venv:
#     SDR_SCANNER=~/projects/sdr-scanner host/misc/run_rx_spatial_fir_tb.sh
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
sim_dir="${repo_root}/hdl/fpga/ip/nuand/simulation"
syn_dir="${repo_root}/hdl/fpga/ip/nuand/synthesis"
vec_dir="${sim_dir}/rx_spatial_fir_vectors"
build_dir="${repo_root}/host/build/rx_spatial_fir_tb"

if [[ -n "${SDR_SCANNER:-}" ]]; then
    "${SDR_SCANNER}/.venv/bin/python" "${sim_dir}/rx_spatial_fir_vectors.py" \
        --sdr-scanner "${SDR_SCANNER}" --out "${vec_dir}"
fi

rm -rf "${build_dir}"
mkdir -p "${build_dir}"
cd "${build_dir}"

ghdl -a --std=08 -frelaxed \
    "${repo_root}/hdl/fpga/platforms/common/bladerf/vhdl/fx3_gpif_p.vhd" \
    "${syn_dir}/fifo_readwrite_p.vhd" \
    "${syn_dir}/synchronizer.vhd" \
    "${syn_dir}/handshake.vhd" \
    "${syn_dir}/rx_spatial_fir.vhd" \
    "${syn_dir}/rx_spatial_fir_controller.vhd" \
    "${syn_dir}/rx_epoch_gate.vhd" \
    "${sim_dir}/rx_spatial_fir_tb.vhd" \
    "${sim_dir}/rx_spatial_fir_gate_equiv_tb.vhd" \
    "${sim_dir}/rx_spatial_fir_controller_tb.vhd"

# Synthesisability check of the datapath entity.
ghdl --synth --std=08 -frelaxed --out=none rx_spatial_fir
ghdl --synth --std=08 -frelaxed --out=none rx_spatial_fir_controller

run() {
    ghdl -r --std=08 -frelaxed "$@" --assert-level=error --ieee-asserts=disable
}

ghdl -e --std=08 -frelaxed rx_spatial_fir_tb
# STROBE 0: data_v every 2nd rx_clock (the hardware cadence), all tests.
run rx_spatial_fir_tb -gVEC_DIR="${vec_dir}" -gSTROBE=0
# STROBE 1 (every clock) and 2 (irregular): control-path tests 0..5.
run rx_spatial_fir_tb -gVEC_DIR="${vec_dir}" -gSTROBE=1 -gFIRST=0 -gLAST=5
run rx_spatial_fir_tb -gVEC_DIR="${vec_dir}" -gSTROBE=2 -gFIRST=0 -gLAST=5

ghdl -e --std=08 -frelaxed rx_spatial_fir_gate_equiv_tb
run rx_spatial_fir_gate_equiv_tb

# T8: controller CDC, rx_clock faster (122.88 MHz) and slower than sys_clock.
ghdl -e --std=08 -frelaxed rx_spatial_fir_controller_tb
run rx_spatial_fir_controller_tb -gRX_HALF_PS=4069
run rx_spatial_fir_controller_tb -gRX_HALF_PS=11000

echo "run_rx_spatial_fir_tb: PASS"
