#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
test_bin=$(mktemp)
trap 'rm -f "$test_bin"' EXIT

cc -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-function \
    -DBLADERF_NIOS_PC_SIMULATION \
    -include "$repo_root/host/misc/nios_peripheral_test_stubs.h" \
    -ffunction-sections -fdata-sections \
    -I"$repo_root/fpga_common/include" \
    -I"$repo_root/hdl/fpga/platforms/common/bladerf/software/bladeRF_nios/src" \
    -I"$repo_root/hdl/fpga/platforms/bladerf-micro/software/bladeRF_nios/src" \
    "$repo_root/hdl/fpga/platforms/common/bladerf/software/bladeRF_nios/src/pkt_8x16.c" \
    "$repo_root/hdl/fpga/platforms/common/bladerf/software/bladeRF_nios/src/pkt_8x8.c" \
    "$repo_root/hdl/fpga/platforms/common/bladerf/software/bladeRF_nios/src/pkt_8x32.c" \
    "$repo_root/host/misc/test_nios_peripheral_packet_status.c" \
    -Wl,--gc-sections -o "$test_bin"

"$test_bin"

test_bin=$(mktemp)
trap 'rm -f "$test_bin"' EXIT
cc -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-function \
    -DBLADERF_NIOS_PC_SIMULATION -DBOARD_BLADERF_MICRO \
    -ffunction-sections -fdata-sections \
    -I"$repo_root/fpga_common/include" \
    -I"$repo_root/hdl/fpga/platforms/common/bladerf/software/bladeRF_nios/src" \
    -I"$repo_root/hdl/fpga/platforms/bladerf-micro/software/bladeRF_nios/src" \
    "$repo_root/hdl/fpga/platforms/common/bladerf/software/bladeRF_nios/src/pkt_8x16.c" \
    "$repo_root/host/misc/test_nios_ina219_packet_status.c" \
    -Wl,--gc-sections -o "$test_bin"
"$test_bin"
