#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
test_bin=$(mktemp)
trap 'rm -f "$test_bin"' EXIT

cc -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-variable \
    -Wno-unused-but-set-variable -ffunction-sections -fdata-sections \
    -I"$repo_root/fpga_common/include" \
    -I"$repo_root/host/libraries/libbladeRF/include" \
    -I"$repo_root/host/libraries/libbladeRF/src" \
    -I"$repo_root/host/common/include" \
    -I"$repo_root/host/build/common/include" \
    -I"$repo_root/firmware_common" \
    "$repo_root/host/libraries/libbladeRF/src/backend/usb/nios_legacy_access.c" \
    "$repo_root/host/misc/test_nios_legacy_error_marker.c" \
    -Wl,--gc-sections -o "$test_bin"

"$test_bin"
