#!/usr/bin/env bash
# OpenCores I2C soft reset and diagnostic register (offset 7), iverilog.
set -euo pipefail

repo_root=$(cd "$(dirname "$0")/../.." && pwd)
i2c="$repo_root/hdl/fpga/ip/opencores/i2c"
out=$(mktemp)
trap 'rm -f "$out"' EXIT

iverilog -g2005 -I "$i2c/rtl/verilog" -I "$i2c/bench/verilog" -o "$out" \
    "$i2c/rtl/verilog/i2c_master_bit_ctrl.v" \
    "$i2c/rtl/verilog/i2c_master_byte_ctrl.v" \
    "$i2c/rtl/verilog/i2c_master_top.v" \
    "$i2c/bench/verilog/i2c_slave_model.v" \
    "$i2c/bench/verilog/wb_master_model.v" \
    "$i2c/bench/verilog/tst_soft_reset.v"
result=$(vvp -n "$out")
echo "$result" | grep -E '^(PASS|FAIL)'
echo "$result" | grep -q '^PASS tst_soft_reset$'
