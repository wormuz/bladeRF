#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
test_bin=$(mktemp)
trap 'rm -f "$test_bin"' EXIT

cc -std=c99 -Wall -Wextra -Werror \
    -I"$repo_root/hdl/fpga/platforms/common/bladerf/software/bladeRF_nios/src" \
    "$repo_root/host/misc/test_nios_i2c_status_policy.c" \
    -o "$test_bin"

"$test_bin"
