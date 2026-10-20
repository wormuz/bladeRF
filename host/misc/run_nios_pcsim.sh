#!/bin/sh
# Build and run the NIOS PC-simulation test cases for both platforms. The
# simulator aborts on the first response mismatch.
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
out_dir=$(mktemp -d)
trap 'rm -rf "$out_dir"' EXIT

for platform in bladerf-micro bladerf; do
    make -s -C "$repo_root/hdl/fpga/platforms/$platform/software/bladeRF_nios" \
        -f pcsim.mk NIOS_BUILD_OUTDIR="$out_dir/$platform"
    "$out_dir/$platform/bladeRF_nios.sim" > "$out_dir/$platform.log" 2>&1
    cases=$(grep -c '^Test case' "$out_dir/$platform.log")
    passed=$(grep -c '^Pass\.$' "$out_dir/$platform.log")
    # Test case 1 is garbage input and must produce no response.
    if [ "$passed" -ne $((cases - 1)) ]; then
        echo "$platform: $passed of $cases test cases answered correctly" >&2
        exit 1
    fi
    echo "NIOS PC simulation ($platform): $passed/$((cases - 1)) PASS"
done
