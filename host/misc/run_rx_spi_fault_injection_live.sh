#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/../.." && pwd)"
build_dir="${BLADERF_SPI_FAULT_BUILD_DIR:-$repo_root/host/build-spi-fault}"
binary="$(mktemp "${TMPDIR:-/tmp}/rx_spi_fault_live.XXXXXX")"
trap 'rm -f "$binary"' EXIT

cmake -S "$repo_root/host" -B "$build_dir" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_DOCUMENTATION=OFF \
  -DENABLE_TEST_SPI_FAULT_INJECTION=ON \
  -DENABLE_TEST_RX_ABORT_FAULT_INJECTION=ON
cmake --build "$build_dir" --target libbladerf_shared -j2

cc -D_POSIX_C_SOURCE=200809L \
  -DBLADERF_ENABLE_TEST_SPI_FAULT_INJECTION=1 \
  -DBLADERF_ENABLE_TEST_RX_ABORT_FAULT_INJECTION=1 \
  -std=c11 -Wall -Wextra -Werror -O2 \
  -I"$repo_root/host/libraries/libbladeRF/include" \
  "$script_dir/rx_epoch_abort_live.c" \
  -L"$build_dir/output" -Wl,-rpath,"$build_dir/output" \
  -lbladeRF -o "$binary"

"$binary"
