#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/../.." && pwd)"
build_dir="${BLADERF_RX_TIMEOUT_BUILD_DIR:-${TMPDIR:-/tmp}/bladerf-positive-timeout-build}"
binary="$(mktemp "${TMPDIR:-/tmp}/rx_epoch_positive_timeout_live.XXXXXX")"
trap 'rm -f "$binary"' EXIT

cmake -S "$repo_root/host" -B "$build_dir" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_DOCUMENTATION=OFF \
  -DENABLE_TEST_RX_TRANSITION_STALL_INJECTION=ON
cmake --build "$build_dir" --target libbladerf_shared -j2

cc -D_DEFAULT_SOURCE -std=c11 -Wall -Wextra -Werror -O2 \
  -I"$repo_root/host/libraries/libbladeRF/include" \
  "$script_dir/rx_epoch_positive_timeout_live.c" \
  -L"$build_dir/output" -Wl,-rpath,"$build_dir/output" \
  -lbladeRF -o "$binary"

"$binary"
