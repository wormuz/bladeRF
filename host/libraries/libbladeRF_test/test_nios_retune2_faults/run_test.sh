#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd -- "$script_dir/../../../.." && pwd)"
build_dir="${BLADERF_HOST_BUILD_DIR:-$root/host/build}"
test_bin="$(mktemp -p "${TMPDIR:-/var/tmp}")"
trap 'rm -f "$test_bin"' EXIT

cc -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-variable \
  -Wno-unused-but-set-variable -ffunction-sections -fdata-sections \
  -I"$root/fpga_common/include" \
  -I"$root/host/libraries/libbladeRF/include" \
  -I"$root/host/libraries/libbladeRF/src" \
  -I"$root/host/common/include" \
  -I"$build_dir/common/include" \
  -I"$build_dir/host/common/include" \
  -I"$root/firmware_common" \
  "$root/host/libraries/libbladeRF/src/backend/usb/nios_access.c" \
  "$root/host/libraries/libbladeRF/src/backend/usb/nios_transaction.c" \
  "$root/host/libraries/libbladeRF/src/helpers/timeout.c" \
  "$script_dir/main.c" -Wl,--gc-sections -o "$test_bin"

"$test_bin"
