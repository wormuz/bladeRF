#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="${BLADERF_TEST_BUILD_DIR:-${repo_root}/host/build/tx_retune_gate_tb}"
mkdir -p "${build_dir}"
cd "${build_dir}"

ghdl -a --std=08 \
    "${repo_root}/hdl/fpga/ip/nuand/synthesis/fifo_readwrite_p.vhd" \
    "${repo_root}/hdl/fpga/ip/nuand/simulation/synchronizer_model.vhd" \
    "${repo_root}/hdl/fpga/ip/nuand/synthesis/reset_synchronizer.vhd" \
    "${repo_root}/hdl/fpga/ip/nuand/synthesis/handshake.vhd" \
    "${repo_root}/hdl/fpga/ip/nuand/synthesis/tx_retune_gate.vhd" \
    "${repo_root}/hdl/fpga/ip/nuand/synthesis/tx_dac_output.vhd" \
    "${repo_root}/hdl/fpga/ip/nuand/synthesis/tx_retune_gate_controller.vhd" \
    "${repo_root}/hdl/fpga/ip/nuand/synthesis/tx_fifo_fault_flush.vhd" \
    "${repo_root}/hdl/fpga/ip/nuand/simulation/tx_retune_gate_tb.vhd" \
    "${repo_root}/hdl/fpga/ip/nuand/simulation/tx_dac_output_tb.vhd" \
    "${repo_root}/hdl/fpga/ip/nuand/synthesis/tb/tx_retune_gate_controller_tb.vhd" \
    "${repo_root}/hdl/fpga/ip/nuand/synthesis/tb/tx_fifo_fault_flush_tb.vhd"

ghdl -e --std=08 tx_retune_gate_tb
ghdl -r --std=08 tx_retune_gate_tb --assert-level=error

ghdl -e --std=08 tx_retune_gate_controller_tb
ghdl -r --std=08 tx_retune_gate_controller_tb --assert-level=error \
    --stop-time=10us

ghdl -e --std=08 tx_fifo_fault_flush_tb
ghdl -r --std=08 tx_fifo_fault_flush_tb --assert-level=error \
    --stop-time=10us

ghdl -a --std=08 \
    "${repo_root}/hdl/fpga/platforms/common/bladerf/vhdl/fx3_gpif_p.vhd" \
    "${repo_root}/hdl/fpga/ip/nuand/synthesis/fifo_reader.vhd" \
    "${repo_root}/hdl/fpga/ip/nuand/simulation/fifo_reader_tb.vhd" \
    "${repo_root}/hdl/fpga/ip/nuand/simulation/fifo_reader_edge_tb.vhd"

ghdl -e --std=08 fifo_reader_tb
ghdl -r --std=08 fifo_reader_tb --assert-level=error --stop-time=70us
ghdl -r --std=08 fifo_reader_tb -gTGT=0 --assert-level=error \
    --stop-time=70us
ghdl -r --std=08 fifo_reader_tb -gTGT=4000 -gNSTREAMS=2 -gPACKED=true \
    --assert-level=error --stop-time=70us
ghdl -r --std=08 fifo_reader_tb -gTGT=0 -gNSTREAMS=2 -gPACKED=true \
    --assert-level=error --stop-time=70us
ghdl -r --std=08 fifo_reader_tb -gTGT=4000 -gPROGRESS_TIMEOUT_LOG2=4 \
    --assert-level=error --stop-time=70us
ghdl -r --std=08 fifo_reader_tb -gTGT=4000 -gNSTREAMS=2 -gPACKED=true \
    -gPROGRESS_TIMEOUT_LOG2=4 --assert-level=error --stop-time=70us

ghdl -e --std=08 fifo_reader_edge_tb
ghdl -r --std=08 fifo_reader_edge_tb --assert-level=error \
    -gCASE_ID=1 \
    -gTS_START=1111111111111111111111111111111111111111111111111111111111110000 \
    -gHDR_TIME=0000000000000000000000000000000000000000000000000000000000010000 \
    -gEXPECT_RELEASE=false -gSTOP_AFTER_CYCLES=8
ghdl -r --std=08 fifo_reader_edge_tb --assert-level=error \
    -gCASE_ID=2 \
    -gTS_START=1111111111111111111111111111111111111111111111111111111111110000 \
    -gHDR_TIME=0000000000000000000000000000000000000000000000000000000000010000 \
    -gEXPECT_RELEASE=true -gSTOP_AFTER_CYCLES=64
ghdl -r --std=08 fifo_reader_edge_tb --assert-level=error \
    -gCASE_ID=3 \
    -gHDR_TIME=0000000000000000000000000000000000000000000000000000000000000000 \
    -gEXPECT_RELEASE=true -gSTOP_AFTER_CYCLES=30
ghdl -r --std=08 fifo_reader_edge_tb --assert-level=error \
    -gCASE_ID=4 \
    -gHDR_TIME=1111111111111111111111111111111111111111111111111111111111111111 \
    -gEXPECT_RELEASE=true -gSTOP_AFTER_CYCLES=64
ghdl -r --std=08 fifo_reader_edge_tb --assert-level=error \
    -gCASE_ID=5 \
    -gTS_START=0000000000000000000000000000000000000000000000000000000000000000 \
    -gHDR_TIME=0000000000000000000000000000000000000000000000000000000001100111 \
    -gEXPECT_RELEASE=true -gSTOP_AFTER_CYCLES=160
ghdl -r --std=08 fifo_reader_edge_tb --assert-level=error \
    -gCASE_ID=6 \
    -gTS_START=0000000000000000000000000000000000000000000000000000000000000000 \
    -gHDR_TIME=0000000000000000000000000000000000000000000000000000000001101000 \
    -gEXPECT_RELEASE=true -gSTOP_AFTER_CYCLES=160
ghdl -r --std=08 fifo_reader_edge_tb --assert-level=error \
    -gCASE_ID=7 -gHDR_TIME=0000000000000000000000000000000000000000000000000000000000000000 \
    -gEXPECT_RELEASE=true -gSTART_EMPTY=true -gEMPTY_FIFO_CYCLES=30 \
    -gPROGRESS_TIMEOUT_LOG2=4 -gSTOP_AFTER_CYCLES=80
# Low-word carry into the high word (high word 0 -> 1), not the 64-bit wrap.
ghdl -r --std=08 fifo_reader_edge_tb --assert-level=error \
    -gCASE_ID=8 \
    -gTS_START=0000000000000000000000000000000011111111111111111111111111110000 \
    -gHDR_TIME=0000000000000000000000000000000100000000000000000000000000010000 \
    -gEXPECT_RELEASE=false -gSTOP_AFTER_CYCLES=8
ghdl -r --std=08 fifo_reader_edge_tb --assert-level=error \
    -gCASE_ID=9 \
    -gTS_START=0000000000000000000000000000000011111111111111111111111111110000 \
    -gHDR_TIME=0000000000000000000000000000000100000000000000000000000000010000 \
    -gEXPECT_RELEASE=true -gSTOP_AFTER_CYCLES=64
# Target in the four ticks where the lead timestamp carries into the high
# word: the first read must land on the header tick, on its first clock.
# Case 11 steps the timestamp every other clock as time_tamer does, case 12
# every fourth clock.
ghdl -r --std=08 fifo_reader_edge_tb --assert-level=error \
    -gCASE_ID=10 \
    -gTS_START=0000000000000000000000000000000011111111111111111111111110000000 \
    -gHDR_TIME=0000000000000000000000000000000100000000000000000000000000000000 \
    -gEXPECT_RELEASE=true -gEXPECT_LATE=0 -gSTOP_AFTER_CYCLES=200
ghdl -r --std=08 fifo_reader_edge_tb --assert-level=error \
    -gCASE_ID=11 -gTS_DIV=2 \
    -gTS_START=0000000000000000000000000000000011111111111111111111111110000000 \
    -gHDR_TIME=0000000000000000000000000000000100000000000000000000000000000001 \
    -gEXPECT_RELEASE=true -gEXPECT_LATE=0 -gSTOP_AFTER_CYCLES=400
ghdl -r --std=08 fifo_reader_edge_tb --assert-level=error \
    -gCASE_ID=12 -gTS_DIV=4 \
    -gTS_START=0000000000000000000000000000000011111111111111111111111110000000 \
    -gHDR_TIME=0000000000000000000000000000000100000000000000000000000000000001 \
    -gEXPECT_RELEASE=true -gEXPECT_LATE=0 -gSTOP_AFTER_CYCLES=800

ghdl -e --std=08 tx_dac_output_tb
ghdl -r --std=08 tx_dac_output_tb --assert-level=error
