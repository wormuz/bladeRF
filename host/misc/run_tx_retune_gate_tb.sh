#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="${repo_root}/host/build/tx_retune_gate_tb"
mkdir -p "${build_dir}"
cd "${build_dir}"

ghdl -a --std=08 \
    "${repo_root}/hdl/fpga/ip/nuand/synthesis/fifo_readwrite_p.vhd" \
    "${repo_root}/hdl/fpga/ip/nuand/synthesis/handshake.vhd" \
    "${repo_root}/hdl/fpga/ip/nuand/synthesis/tx_retune_gate.vhd" \
    "${repo_root}/hdl/fpga/ip/nuand/synthesis/tx_retune_gate_controller.vhd" \
    "${repo_root}/hdl/fpga/ip/nuand/simulation/tx_retune_gate_tb.vhd" \
    "${repo_root}/hdl/fpga/ip/nuand/synthesis/tb/tx_retune_gate_controller_tb.vhd"

ghdl -e --std=08 tx_retune_gate_tb
ghdl -r --std=08 tx_retune_gate_tb --assert-level=error

ghdl -e --std=08 tx_retune_gate_controller_tb
ghdl -r --std=08 tx_retune_gate_controller_tb --assert-level=error \
    --stop-time=10us

ghdl -a --std=08 \
    "${repo_root}/hdl/fpga/platforms/common/bladerf/vhdl/fx3_gpif_p.vhd" \
    "${repo_root}/hdl/fpga/ip/nuand/synthesis/fifo_reader.vhd" \
    "${repo_root}/hdl/fpga/ip/nuand/simulation/fifo_reader_tb.vhd"

ghdl -e --std=08 fifo_reader_tb
ghdl -r --std=08 fifo_reader_tb --assert-level=error --stop-time=70us
ghdl -r --std=08 fifo_reader_tb -gTGT=0 --assert-level=error \
    --stop-time=70us
