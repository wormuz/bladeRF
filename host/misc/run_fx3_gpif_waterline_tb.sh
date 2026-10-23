#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="${BLADERF_TEST_BUILD_DIR:-${repo_root}/host/build/fx3_gpif_waterline_tb}"
mkdir -p "${build_dir}"
cd "${build_dir}"

ghdl -a --std=08 --work=nuand \
    "${repo_root}/hdl/fpga/platforms/common/bladerf/vhdl/fx3_gpif_p.vhd"
ghdl -a --std=08 \
    "${repo_root}/hdl/fpga/platforms/common/bladerf/vhdl/tb/fx3_gpif_waterline_tb.vhd"
ghdl -e --std=08 fx3_gpif_waterline_tb
ghdl -r --std=08 fx3_gpif_waterline_tb --assert-level=error
