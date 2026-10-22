#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/../.." && pwd)"
build_dir="${BLADERF_HOST_BUILD_DIR:-$repo_root/host/build}"
test_bin="$(mktemp "${TMPDIR:-/tmp}/spi_gt_region.XXXXXX")"
trap 'rm -f "$test_bin"' EXIT

cc -std=gnu11 -Wall -Wextra -Werror -O1 \
  -I"$repo_root/fpga_common/include" \
  -I"$repo_root/host/libraries/libbladeRF/include" \
  -I"$repo_root/host/libraries/libbladeRF/src" \
  -I"$repo_root/host/common/include" \
  -I"$build_dir/host/common/include" \
  -I"$build_dir/common/include" \
  -I"$repo_root/firmware_common" \
  -I"$repo_root/thirdparty/analogdevicesinc/no-OS_local/platform_bladerf2" \
  -I"$build_dir/common/thirdparty/ad936x" \
  "$repo_root/thirdparty/analogdevicesinc/no-OS_local/platform_bladerf2/bladerf2_spi.c" \
  "$script_dir/test_bladerf2_spi_gain_table_region.c" \
  -o "$test_bin"

"$test_bin"
