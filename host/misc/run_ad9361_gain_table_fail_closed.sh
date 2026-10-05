#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
bladerf_root="$(cd -- "$script_dir/../.." && pwd)"
build_dir="${BLADERF_HOST_BUILD_DIR:-$bladerf_root/host/build}"
test_bin="$(mktemp)"
trap 'rm -f "$test_bin"' EXIT

cc -std=gnu11 -DNUAND_MODIFICATIONS=1 -DLOG_INCLUDE_FILE_INFO \
  -ffunction-sections -fdata-sections -O0 -g \
  -I"$bladerf_root/thirdparty/analogdevicesinc/no-OS_local/platform_bladerf2" \
  -I"$build_dir/common/thirdparty/ad936x" \
  -I"$bladerf_root/host/libraries/libbladeRF/src" \
  -I"$bladerf_root/host/libraries/libbladeRF/include" \
  -I"$bladerf_root/host/common/include" \
  -I"$bladerf_root/firmware_common" \
  -I"$build_dir/host/common/include" \
  -I"$build_dir/common/include" \
  "$script_dir/test_ad9361_gain_table_fail_closed.c" \
  -Wl,--gc-sections -o "$test_bin"

"$test_bin"
