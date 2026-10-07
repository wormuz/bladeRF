#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/../.." && pwd)"
build_dir="${BLADERF_RX_LOSS_BUILD_DIR:-${TMPDIR:-/tmp}/bladerf-rx-loss-build}"
binary="$(mktemp "${TMPDIR:-/tmp}/rx_runtime_fpga_loss_live.XXXXXX")"
trap 'rm -f "$binary"' EXIT

if (($# > 1)); then
  echo "usage: $0 [RX1|RX2|RX_X2]" >&2
  exit 2
fi

cmake -S "$repo_root/host" -B "$build_dir" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_DOCUMENTATION=OFF \
  -DENABLE_TEST_RX_TRANSITION_STALL_INJECTION=ON
cmake --build "$build_dir" --target libbladerf_shared -j2

cc -std=c11 -Wall -Wextra -Werror -O2 \
  -I"$repo_root/host/libraries/libbladeRF/include" \
  "$script_dir/rx_runtime_fpga_loss_live.c" \
  -L"$build_dir/output" -Wl,-rpath,"$build_dir/output" \
  -lbladeRF -o "$binary"

if (($# == 1)); then
  "$binary" "$1"
else
  for layout in RX1 RX2 RX_X2; do
    "$binary" "$layout"
  done
fi
