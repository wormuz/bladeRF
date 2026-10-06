#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/../.." && pwd)"
build_dir="${BLADERF_HOST_BUILD_DIR:-$repo_root/host/build}"

cmake --build "$build_dir" --target libbladeRF_test_sync_epoch_traversal -j4
"$build_dir/output/libbladeRF_test_sync_epoch_traversal"
printf 'sync_rx sample-META epoch traversal: PASS\n'
