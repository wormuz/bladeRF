#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="${repo_root}/host/build/rx_epoch_gate_tb"
mkdir -p "${build_dir}"
cd "${build_dir}"

ghdl -a --std=08 \
    "${repo_root}/hdl/fpga/ip/nuand/synthesis/fifo_readwrite_p.vhd" \
    "${repo_root}/hdl/fpga/ip/nuand/synthesis/rx_epoch_gate.vhd" \
    "${repo_root}/hdl/fpga/ip/nuand/simulation/rx_epoch_gate_tb.vhd"
ghdl -e --std=08 rx_epoch_gate_tb
ghdl -r --std=08 rx_epoch_gate_tb --assert-level=error
