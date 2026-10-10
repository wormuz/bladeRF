#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/../.." && pwd)"
build_dir="${BLADERF_TX_FAULT_BUILD_DIR:-${TMPDIR:-/tmp}/bladerf-tx-rfic-fault-build}"
binary="$(mktemp "${TMPDIR:-/tmp}/tx_runtime_rfic_fault_live.XXXXXX")"
trap 'rm -f "$binary"' EXIT

if (($# != 1)); then
  echo "usage: $0 MODE" >&2
  echo "modes: RUNTIME_TX_RFIC_STATUS_READ_FAILURE "\
       "RUNTIME_TX_RFPLL_UNLOCKED RUNTIME_TX_ENSM_NOT_TX "\
       "RUNTIME_TX_BBPLL_UNLOCKED RUNTIME_TX_LINK_FAULT_NO_RFIC_READ "\
       "RUNTIME_TX_RETUNE_FAILURE" >&2
  exit 2
fi

cmake -S "$repo_root/host" -B "$build_dir" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_DOCUMENTATION=OFF \
  -DENABLE_TEST_RX_TRANSITION_STALL_INJECTION=ON
cmake --build "$build_dir" --target libbladerf_shared -j2

cc -std=c11 -Wall -Wextra -Werror -O2 \
  -pthread \
  -I"$repo_root/host/libraries/libbladeRF/include" \
  "$script_dir/tx_runtime_rfic_fault_live.c" \
  -L"$build_dir/output" -Wl,-rpath,"$build_dir/output" \
  -lbladeRF -o "$binary"

BLADERF_TEST_RX_TRANSITION_STALL="$1" "$binary"
