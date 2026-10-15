#!/usr/bin/env bash
set -euo pipefail
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/../.." && pwd)"
build_dir="${BLADERF_HOST_BUILD_DIR:-$repo_root/host/build}"
test_dir="$(mktemp -d "${TMPDIR:-/tmp}/tx-event-wait.XXXXXX")"
trap 'rm -rf "$test_dir"' EXIT

includes=(
  -I"$repo_root/host/libraries/libbladeRF/include"
  -I"$repo_root/host/libraries/libbladeRF/src"
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
       -O0 -g -ffunction-sections -fdata-sections)
cc "${flags[@]}" "${includes[@]}" -c \
  "$repo_root/host/libraries/libbladeRF/src/board/bladerf2/rf_transition.c" \
  -o "$test_dir/rf_transition.o"
cc "${flags[@]}" "${includes[@]}" \
  "$script_dir/test_tx_event_wait.c" "$test_dir/rf_transition.o" \
  -Wl,--gc-sections -Wl,--wrap=pthread_cond_wait -pthread \
  -o "$test_dir/test_tx_event_wait"
"$test_dir/test_tx_event_wait"
