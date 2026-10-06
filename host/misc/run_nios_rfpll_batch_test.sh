#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
test_bin=$(mktemp)
host_test_bin=$(mktemp)
trap 'rm -f "$test_bin" "$host_test_bin"' EXIT

cc -Wall -Wextra -Werror -Wno-unused-parameter \
    -DBLADERF_NIOS_PC_SIMULATION \
    -I"$repo_root/fpga_common/include" \
    -I"$repo_root/hdl/fpga/platforms/common/bladerf/software/bladeRF_nios/src" \
    -I"$repo_root/hdl/fpga/platforms/bladerf-micro/software/bladeRF_nios/src" \
    "$repo_root/hdl/fpga/platforms/common/bladerf/software/bladeRF_nios/src/pkt_16x64.c" \
    "$repo_root/host/misc/test_nios_rfpll_batch.c" \
    -o "$test_bin"

"$test_bin"

# The existing nios_access.c logging macros compile out some local variables.
cc -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-variable \
    -Wno-unused-but-set-variable -ffunction-sections -fdata-sections \
    -I"$repo_root/fpga_common/include" \
    -I"$repo_root/host/libraries/libbladeRF/src" \
    -I"$repo_root/host/common/include" \
    -I"$repo_root/host/build/common/include" \
    -I"$repo_root/firmware_common" \
    "$repo_root/host/libraries/libbladeRF/src/backend/usb/nios_access.c" \
    "$repo_root/host/misc/test_nios_rfpll_batch_host.c" \
    -Wl,--gc-sections -o "$host_test_bin"

"$host_test_bin"
