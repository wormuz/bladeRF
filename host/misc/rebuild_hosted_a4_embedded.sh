#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/../.." && pwd)"
quartus_dir="$repo_root/hdl/quartus"
workdir_rel="${1:-work/bladerf-micro-A4-hosted}"
workdir_abs="$quartus_dir/$workdir_rel"
nios_make_dir="$repo_root/hdl/fpga/platforms/bladerf-micro/software/bladeRF_nios"

if [[ ! -f "$workdir_abs/hosted.qpf" ]]; then
    printf 'No hosted Quartus project at %s\n' "$workdir_abs" >&2
    exit 2
fi

make -C "$nios_make_dir" WORKDIR="$workdir_rel" app
make -C "$nios_make_dir" WORKDIR="$workdir_rel" mem_init_generate

# The Qsys-generated system consumes this copy, while mem_init_generate writes
# the application-specific image under bladeRF_nios/mem_init.
cp -f "$workdir_abs/bladeRF_nios/mem_init/nios_system_ram.hex" \
    "$workdir_abs/nios_system/synthesis/submodules/nios_system_ram.hex"

(
    cd "$workdir_abs"
    quartus_cdb hosted -c hosted --update_mif
    quartus_asm hosted -c hosted
)

printf 'FPGA version: '
awk '/^#define FPGA_VERSION_MAJOR/{major=$3} \
     /^#define FPGA_VERSION_MINOR/{minor=$3} \
     /^#define FPGA_VERSION_PATCH/{patch=$3} \
     END {printf "%s.%s.%s\n", major, minor, patch}' \
    "$repo_root/hdl/fpga/platforms/bladerf-micro/software/bladeRF_nios/src/fpga_version.h"
sha256sum "$workdir_abs/output_files/hosted.rbf"
