#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/../.." && pwd)"
build_dir="${BLADERF_HOST_BUILD_DIR:-$repo_root/host/build}"
binary="$build_dir/output/rx_epoch_transition_qualification"
source_file="$script_dir/rx_epoch_transition_qualification.c"
header_file="$repo_root/host/libraries/libbladeRF/include/libbladeRF.h"
library_file="$build_dir/output/libbladeRF.so"

mkdir -p "$build_dir/output"
if [[ ! -x "$binary" || "$source_file" -nt "$binary" ||
      "$header_file" -nt "$binary" || "$library_file" -nt "$binary" ]]; then
  cc -D_DEFAULT_SOURCE -std=c11 -Wall -Wextra -Werror -O2 \
    -I"$repo_root/host/libraries/libbladeRF/include" \
    "$source_file" \
    -L"$build_dir/output" -Wl,-rpath,"$build_dir/output" \
    -lbladeRF -o "$binary"
fi

exec "$binary" "$@"
