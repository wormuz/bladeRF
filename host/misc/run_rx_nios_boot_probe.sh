#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/../.." && pwd)"
build_dir="${BLADERF_HOST_BUILD_DIR:-$repo_root/host/build}"
binary="$build_dir/output/rx_nios_boot_probe"
source_file="$script_dir/rx_nios_boot_probe.c"
header_file="/usr/include/libusb-1.0/libusb.h"
serial="${BLADERF_PROBE_SERIAL:-f695006ba84a40daa7b777c6a6eba78}"

mkdir -p "$build_dir/output"
if [[ ! -x "$binary" || "$source_file" -nt "$binary" ||
      "$header_file" -nt "$binary" ]]; then
  cc -D_DEFAULT_SOURCE -std=c11 -Wall -Wextra -Werror -O2 \
    $(pkg-config --cflags libusb-1.0) "$source_file" \
    $(pkg-config --libs libusb-1.0) -o "$binary"
fi

exec "$binary" "$serial"
