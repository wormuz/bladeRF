#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/../.." && pwd)"
build_dir="${BLADERF_HOST_BUILD_DIR:-$repo_root/host/build}"
binary="$(mktemp "${TMPDIR:-/tmp}/rx_trigger_live.XXXXXX")"
trap 'rm -f "$binary"' EXIT

cmake --build "$build_dir" --target libbladerf_shared -j2
cc -std=gnu11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -O2 -pthread \
  -I"$repo_root/host/libraries/libbladeRF/include" \
  "$script_dir/rx_trigger_live.c" \
  -L"$build_dir/output" -Wl,-rpath,"$build_dir/output" \
  -lbladeRF -o "$binary"

"$binary" "$@"
