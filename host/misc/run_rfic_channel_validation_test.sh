#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
bladerf_root="$(cd -- "$script_dir/../.." && pwd)"
test_bin="$(mktemp)"
trap 'rm -f "$test_bin"' EXIT

cc -std=c99 -Wall -Wextra -Werror \
  -I"$bladerf_root/host/libraries/libbladeRF/include" \
  -I"$bladerf_root/hdl/fpga/platforms/common/bladerf/software/bladeRF_nios/src" \
  "$script_dir/test_rfic_channel_validation.c" -o "$test_bin"

"$test_bin"
