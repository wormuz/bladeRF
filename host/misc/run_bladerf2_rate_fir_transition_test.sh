#!/usr/bin/env bash
# Extra compiler flags in RATE_FIR_TEST_CFLAGS, e.g. -DRATE_FIR_MASTER_LOGIC.
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd -- "$script_dir/../.." && pwd)"
build_dir="${BLADERF_HOST_BUILD_DIR:-$root/host/build}"
test_bin="$(mktemp -p "${TMPDIR:-/var/tmp}")"
trap 'rm -f "$test_bin"' EXIT

cc -std=gnu11 -DNUAND_MODIFICATIONS=1 -DLOG_INCLUDE_FILE_INFO \
  -ffunction-sections -fdata-sections -O0 -g -w \
  ${RATE_FIR_TEST_CFLAGS:-} \
  -I"$root/thirdparty/analogdevicesinc/no-OS_local/platform_bladerf2" \
  -I"$build_dir/common/thirdparty/ad936x" \
  -I"$root/host/libraries/libbladeRF/src" \
  -I"$root/host/libraries/libbladeRF/include" \
  -I"$root/host/common/include" \
  -I"$root/firmware_common" \
  -I"$build_dir/host/common/include" \
  -I"$build_dir/common/include" \
  "$script_dir/test_bladerf2_rate_fir_transition.c" -Wl,--gc-sections -o "$test_bin"

"$test_bin" 2>&1 | grep -E "^(FAIL|bladerf2 rate FIR transition)"
exit "${PIPESTATUS[0]}"
