#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
work_dir="$(mktemp -d -p "${TMPDIR:-/var/tmp}")"
trap 'rm -rf "$work_dir"' EXIT
cd "$work_dir"

uart_dir="$repo_root/hdl/fpga/ip/nuand/command_uart/vhdl"
ghdl -a --std=08 -frelaxed "$uart_dir/command_uart.vhd" \
    "$uart_dir/tb/command_uart_final_stop_tb.vhd"
ghdl -e --std=08 -frelaxed command_uart_final_stop_tb
ghdl -r --std=08 -frelaxed command_uart_final_stop_tb --assert-level=error
