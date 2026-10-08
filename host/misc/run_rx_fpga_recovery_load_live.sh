#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/../.." && pwd)"
build_dir="${BLADERF_HOST_BUILD_DIR:-$repo_root/host/build}"
binary="$build_dir/output/rx_fpga_recovery_load_live"
source_file="$script_dir/rx_fpga_recovery_load_live.c"
header_file="$repo_root/host/libraries/libbladeRF/include/libbladeRF.h"
library_file="$build_dir/output/libbladeRF.so"
device="${BLADERF_RECOVERY_DEVICE:-*:serial=f695006ba84a40daa7b777c6a6eba78}"
image="${BLADERF_RECOVERY_RBF:-$repo_root/hdl/quartus/work/bladerf-micro-A4-sweep/output_files/sweep.rbf}"
expected_sha256="f53cd1c1fbdc8176c40397d1881a70b5081e8d489854928fb7732dc20ab4fa12"

[[ -r "$image" ]] || { echo "RBF not found: $image" >&2; exit 2; }
actual_sha256="$(sha256sum "$image" | cut -d' ' -f1)"
[[ "$actual_sha256" == "$expected_sha256" ]] || {
  echo "unexpected RBF SHA-256: $actual_sha256" >&2
  echo "expected: $expected_sha256" >&2
  exit 2
}

mkdir -p "$build_dir/output"
if [[ ! -x "$binary" || "$source_file" -nt "$binary" ||
      "$header_file" -nt "$binary" || "$library_file" -nt "$binary" ]]; then
  cc -D_DEFAULT_SOURCE -std=c11 -Wall -Wextra -Werror -O2 \
    -I"$repo_root/host/libraries/libbladeRF/include" \
    "$source_file" \
    -L"$build_dir/output" -Wl,-rpath,"$build_dir/output" \
    -lbladeRF -o "$binary"
fi

exec "$binary" "$device" "$image"
