#!/bin/sh
# Host packet/status mapping for NIOS fastlock profile upload, then the NIOS
# PC-simulation retune2 queue test that covers write -> retune load -> state.
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
host_build_dir=${HOST_BUILD_DIR:-"$repo_root/host/build"}
host_test_bin=$(mktemp)
queue_test_bin=$(mktemp)
trap 'rm -f "$host_test_bin" "$queue_test_bin"' EXIT

# The existing nios_access.c logging macros compile out some local variables.
cc -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-variable \
    -Wno-unused-but-set-variable -ffunction-sections -fdata-sections \
    -I"$repo_root/fpga_common/include" \
    -I"$repo_root/host/libraries/libbladeRF/src" \
    -I"$repo_root/host/common/include" \
    -I"$host_build_dir/common/include" \
    -I"$repo_root/firmware_common" \
    "$repo_root/host/libraries/libbladeRF/src/backend/usb/nios_access.c" \
    "$repo_root/host/libraries/libbladeRF/src/backend/usb/nios_transaction.c" \
    "$repo_root/host/libraries/libbladeRF/src/helpers/timeout.c" \
    "$repo_root/host/misc/test_nios_fastlock_profile_host.c" \
    -Wl,--gc-sections -o "$host_test_bin"

"$host_test_bin"

cc -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-function \
    -ffunction-sections -fdata-sections \
    -DBLADERF_NIOS_PC_SIMULATION \
    -DBLADERF_NIOS_LIBAD936X \
    -I"$repo_root/fpga_common/include" \
    -I"$repo_root/hdl/fpga/platforms/common/bladerf/software/bladeRF_nios/src" \
    -I"$repo_root/hdl/fpga/platforms/bladerf-micro/software/bladeRF_nios/src" \
    "$repo_root/host/misc/test_nios_retune2_queue.c" \
    -Wl,--gc-sections -o "$queue_test_bin"

"$queue_test_bin"
