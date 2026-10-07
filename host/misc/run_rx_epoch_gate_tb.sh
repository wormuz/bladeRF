#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="${repo_root}/host/build/rx_epoch_gate_tb"
mkdir -p "${build_dir}"
cd "${build_dir}"

ghdl -a --std=08 -frelaxed \
    "${repo_root}/hdl/fpga/platforms/common/bladerf/vhdl/fx3_gpif_p.vhd" \
    "${repo_root}/hdl/fpga/ip/nuand/synthesis/fifo_readwrite_p.vhd" \
    "${repo_root}/hdl/fpga/ip/nuand/synthesis/synchronizer.vhd" \
    "${repo_root}/hdl/fpga/ip/nuand/synthesis/handshake.vhd" \
    "${repo_root}/hdl/fpga/ip/nuand/synthesis/fifo_writer.vhd" \
    "${repo_root}/hdl/fpga/ip/nuand/synthesis/rx_epoch_gate.vhd" \
    "${repo_root}/hdl/fpga/ip/nuand/simulation/handshake_rearm_tb.vhd" \
    "${repo_root}/hdl/fpga/ip/nuand/simulation/rx_epoch_gate_tb.vhd" \
    "${repo_root}/hdl/fpga/ip/nuand/simulation/fifo_writer_epoch_fence_tb.vhd" \
    "${repo_root}/hdl/fpga/ip/nuand/simulation/fifo_writer_abort_tb.vhd" \
    "${repo_root}/hdl/fpga/ip/nuand/simulation/fifo_writer_enable_tb.vhd"
ghdl -e --std=08 rx_epoch_gate_tb
ghdl -r --std=08 rx_epoch_gate_tb --assert-level=error

for testbench in \
    handshake_rearm_tb \
    fifo_writer_epoch_fence_tb \
    fifo_writer_abort_tb \
    fifo_writer_enable_tb; do
    ghdl -e --std=08 -frelaxed "${testbench}"
    ghdl -r --std=08 -frelaxed "${testbench}" --assert-level=error
done
