#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/../.." && pwd)"
build_dir="${BLADERF_USB_FAULT_BUILD_DIR:-${TMPDIR:-/tmp}/bladerf-usb-fault-build}"
binary="$(mktemp "${TMPDIR:-/tmp}/rx_async_usb_fault_live.XXXXXX")"
trap 'rm -f "$binary"' EXIT

if (($# > 1)); then
  echo "usage: $0 [RX1|RX2|BOTH]" >&2
  exit 2
fi

cmake -S "$repo_root/host" -B "$build_dir" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_DOCUMENTATION=OFF \
  -DENABLE_TEST_LIBUSB_RX_FAULT_INJECTION=ON
cmake --build "$build_dir" --target libbladerf_shared -j2

cc -std=c11 -Wall -Wextra -Werror -O2 -D_DEFAULT_SOURCE \
  -I"$repo_root/host/libraries/libbladeRF/include" \
  -I"$repo_root/host/libraries/libbladeRF/src" \
  -I"$repo_root/host/common/include" \
  -I"$repo_root/host/build/common/include" \
  "$script_dir/rx_async_usb_fault_live.c" \
  -L"$build_dir/output" -Wl,-rpath,"$build_dir/output" \
  -lbladeRF -pthread -o "$binary"

if (($# == 1)); then
  layouts=("$1")
else
  layouts=(RX1 RX2 BOTH)
fi
faults=(ERROR STALL TIMEOUT NO_DEVICE UNKNOWN)

for layout in "${layouts[@]}"; do
  case "$layout" in
    RX1|RX2|BOTH) ;;
    *) echo "invalid layout: $layout (expected RX1, RX2, or BOTH)" >&2; exit 2 ;;
  esac
  for fault in "${faults[@]}"; do
    echo "CASE $layout $fault"
    BLADERF_TEST_RX_LAYOUT="$layout" \
      BLADERF_TEST_LIBUSB_RX_STATUS="$fault" "$binary"
  done
done
