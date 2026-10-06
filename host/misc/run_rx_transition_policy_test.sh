#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
bladerf_root="$(cd -- "$script_dir/../.." && pwd)"
test_bin="$(mktemp)"
trap 'rm -f "$test_bin"' EXIT

cc -std=gnu11 -Wall -Wextra -Werror \
  -I"$bladerf_root/host/libraries/libbladeRF/include" \
  -I"$bladerf_root/host/libraries/libbladeRF/src/board/bladerf2" \
  "$script_dir/test_rx_transition_policy.c" -o "$test_bin"

"$test_bin"
printf 'RX transition requirement policy: PASS\n'
