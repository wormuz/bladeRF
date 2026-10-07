#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
bladerf_root="$(cd -- "$script_dir/../.." && pwd)"
build_dir="${BLADERF_HOST_BUILD_DIR:-$bladerf_root/host/build}"
test_bin="$(mktemp)"
trap 'rm -f "$test_bin"' EXIT

cc -std=gnu11 -Wall -Wextra -Werror -pthread \
  -I"$bladerf_root/host/libraries/libbladeRF/include" \
  -I"$bladerf_root/host/libraries/libbladeRF/src" \
  -I"$bladerf_root/host/common/include" \
  -I"$build_dir/host/common/include" \
  -I"$build_dir/common/include" \
  "$script_dir/test_rx_epoch_metadata.c" -o "$test_bin"

"$test_bin"
printf 'RX epoch metadata contract: PASS\n'
