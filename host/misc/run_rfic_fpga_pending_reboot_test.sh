#!/usr/bin/env bash
set -euo pipefail
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/../.." && pwd)"
build_dir="${BLADERF_HOST_BUILD_DIR:-$repo_root/host/build}"
test_dir="$(mktemp -d "${TMPDIR:-/tmp}/rfic-fpga-pending.XXXXXX")"
trap 'rm -rf "$test_dir"' EXIT

includes=(
  -I"$repo_root/host/libraries/libbladeRF/include"
  -I"$repo_root/host/libraries/libbladeRF/src"
  -I"$repo_root/host/libraries/libbladeRF/src/board/bladerf2"
  -I"$repo_root/host/common/include"
  -I"$build_dir/common/include"
  -I"$repo_root/firmware_common"
  -I"$repo_root/fpga_common/include"
  -I"$repo_root/thirdparty/lzma"
  -I"$build_dir/libraries/libbladeRF/src"
  -I"$build_dir/common/thirdparty/ad936x"
  -I"$repo_root/thirdparty/analogdevicesinc/no-OS_local/platform_bladerf2"
)
flags=(-D_DEFAULT_SOURCE -DNUAND_MODIFICATIONS=1 -DLOGGING_ENABLED
       -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter
       -Wno-unused-function -O0 -g -ffunction-sections -fdata-sections)
cc "${flags[@]}" "${includes[@]}" \
  "$script_dir/test_rfic_fpga_pending_reboot.c" \
  -Wl,--gc-sections -pthread -o "$test_dir/test_rfic_fpga_pending_reboot"
"$test_dir/test_rfic_fpga_pending_reboot"
