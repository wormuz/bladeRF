#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/../.." && pwd)"
build_dir="${BLADERF_HOST_BUILD_DIR:-$repo_root/host/build}"
binary="$(mktemp "${TMPDIR:-/tmp}/rx_transition_validity_live.XXXXXX")"
trap 'rm -f "$binary"' EXIT

mode="${1:-RX1}"
case "$mode" in
  RX1|RX2|BOTH) ;;
  *) echo "usage: $0 [RX1|RX2|BOTH]" >&2; exit 2 ;;
esac

cc -std=c11 -Wall -Wextra -Werror -O2 \
  -I"$repo_root/host/libraries/libbladeRF/include" \
  "$script_dir/rx_transition_validity_live.c" \
  -L"$build_dir/output" -Wl,-rpath,"$build_dir/output" \
  -lbladeRF -o "$binary"

"$binary" "$mode"
