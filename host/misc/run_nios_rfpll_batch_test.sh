#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
test_bin=$(mktemp)
trap 'rm -f "$test_bin"' EXIT

cc -Wall -Wextra -Werror -Wno-unused-parameter \
    -DBLADERF_NIOS_PC_SIMULATION \
    -I"$repo_root/fpga_common/include" \
    -I"$repo_root/hdl/fpga/platforms/common/bladerf/software/bladeRF_nios/src" \
    -I"$repo_root/hdl/fpga/platforms/bladerf-micro/software/bladeRF_nios/src" \
    "$repo_root/hdl/fpga/platforms/common/bladerf/software/bladeRF_nios/src/pkt_16x64.c" \
    "$repo_root/host/misc/test_nios_rfpll_batch.c" \
    -o "$test_bin"

"$test_bin"
