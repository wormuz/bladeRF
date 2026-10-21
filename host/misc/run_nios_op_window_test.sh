#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
bin=$(mktemp)
trap 'rm -f "$bin"' EXIT

cc -std=c11 -Wall -Wextra -Werror \
    -I"$repo_root/fpga_common/include" \
    -I"$repo_root/hdl/fpga/platforms/common/bladerf/software/bladeRF_nios/src" \
    "$repo_root/host/misc/test_nios_op_window.c" -o "$bin"

"$bin"
