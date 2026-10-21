#!/bin/sh
# pkt_retune2 callback rejections and failures against the PC-simulated queue.
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
out_dir=$(mktemp -d)
trap 'rm -rf "$out_dir"' EXIT

cc -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-function \
    -ffunction-sections -fdata-sections \
    -DBLADERF_NIOS_PC_SIMULATION \
    -DBLADERF_NIOS_LIBAD936X \
    -I"$repo_root/fpga_common/include" \
    -I"$repo_root/hdl/fpga/platforms/common/bladerf/software/bladeRF_nios/src" \
    -I"$repo_root/hdl/fpga/platforms/bladerf-micro/software/bladeRF_nios/src" \
    -I"$repo_root/host/libraries/libbladeRF/include" \
    -I"$repo_root/host/libraries/libbladeRF/src" \
    "$repo_root/host/misc/test_nios_retune2_rejected.c" \
    -Wl,--gc-sections -o "$out_dir/rejected"
"$out_dir/rejected"
