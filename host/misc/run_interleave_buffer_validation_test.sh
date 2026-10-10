#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
bladerf_root="$(cd -- "$script_dir/../.." && pwd)"
test_bin="$(mktemp)"
trap 'rm -f "$test_bin"' EXIT

cc -std=c99 -Wall -Wextra -Werror \
  -I"$bladerf_root/host/libraries/libbladeRF/include" \
  -I"$bladerf_root/host/libraries/libbladeRF/src" \
  "$script_dir/test_interleave_buffer_validation.c" \
  "$bladerf_root/host/libraries/libbladeRF/src/helpers/interleave.c" \
  -o "$test_bin"

"$test_bin"
