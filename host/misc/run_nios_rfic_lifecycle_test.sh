#!/usr/bin/env bash
# Production NIOS RFIC command worker on the host, AD9361 transaction layer
# faked. Needs the generated no-OS headers from a configured host build.
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd -- "$script_dir/../.." && pwd)"
build_dir="${BLADERF_HOST_BUILD_DIR:-$root/host/build}"
nios="$root/hdl/fpga/platforms/common/bladerf/software/bladeRF_nios/src"
test_bin="$(mktemp -p "${TMPDIR:-/var/tmp}")"
trap 'rm -f "$test_bin"' EXIT

cc -std=gnu11 -Wall -Wextra -Werror=implicit-function-declaration -Wno-unused-parameter \
  -Wno-unused-function -Wno-unused-variable -Wno-format \
  -DNUAND_MODIFICATIONS=1 -DLOG_INCLUDE_FILE_INFO \
  -DBOARD_BLADERF_MICRO -DBLADERF_NIOS_LIBAD936X \
  -DBLADERF_NIOS_PC_SIMULATION -O0 -g \
  -I"$script_dir/nios_stubs" -I"$script_dir" -I"$nios" \
  -I"$root/hdl/fpga/platforms/bladerf-micro/software/bladeRF_nios/src" \
  -I"$root/fpga_common/include" -I"$root/firmware_common" \
  -I"$root/thirdparty/analogdevicesinc/no-OS_local/platform_bladerf2" \
  -I"$root/host/libraries/libbladeRF/src" \
  -I"$root/host/libraries/libbladeRF/include" \
  -I"$root/host/common/include" \
  -I"$build_dir/common/thirdparty/ad936x" \
  -I"$build_dir/host/common/include" -I"$build_dir/common/include" \
  "$script_dir/test_nios_rfic_lifecycle.c" \
  "$root/fpga_common/src/bladerf2_common.c" \
  "$root/host/common/src/range.c" -o "$test_bin"

"$test_bin"
