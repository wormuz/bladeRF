#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/../.." && pwd)"
build_dir="${BLADERF_HOST_BUILD_DIR:-$repo_root/host/build}"
binary="$(mktemp "${TMPDIR:-/tmp}/rfic_init_timing_live.XXXXXX")"
trap 'rm -f "$binary"' EXIT

cc -D_DEFAULT_SOURCE -std=c11 -Wall -Wextra -Werror -O2 \
  -I"$repo_root/host/libraries/libbladeRF/include" \
  -I"$repo_root/host/libraries/libbladeRF/src" \
  -I"$repo_root/host/libraries/libbladeRF/src/backend/usb" \
  -I"$build_dir/common/include" \
  -I"$repo_root/host/common/include" \
  -I"$repo_root/firmware_common" \
  -I"$repo_root/fpga_common/include" \
  "$script_dir/rfic_init_timing_live.c" \
  -L"$build_dir/output" -Wl,-rpath,"$build_dir/output" \
  -lbladeRF -o "$binary"

"$binary" "$@"
