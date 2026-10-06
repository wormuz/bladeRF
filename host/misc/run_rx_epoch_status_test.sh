#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
bladerf_root="$(cd -- "$script_dir/../.." && pwd)"
test_bin="$(mktemp)"
trap 'rm -f "$test_bin"' EXIT

cc -std=gnu11 -Wall -Wextra -Werror \
  -I"$bladerf_root/fpga_common/include" \
  "$script_dir/test_rx_epoch_status.c" -o "$test_bin"

"$test_bin"
printf 'RX epoch status identity contract: PASS\n'
