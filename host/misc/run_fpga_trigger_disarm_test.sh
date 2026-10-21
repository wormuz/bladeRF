#!/bin/sh
# Trigger state survives the process, so close must clear it: behaviour test of
# fpga_trigger_disarm_all() plus a structural check that bladerf2_close() calls
# it before any other step.
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
host_build_dir=${HOST_BUILD_DIR:-"$repo_root/host/build"}
src="$repo_root/host/libraries/libbladeRF/src"
bin=$(mktemp)
trap 'rm -f "$bin"' EXIT

cc -Wall -Wextra -Werror -Wno-unused-parameter \
    -I"$repo_root/host/libraries/libbladeRF/include" \
    -I"$src" -I"$repo_root/host/common/include" \
    -I"$host_build_dir/common/include" -I"$repo_root/firmware_common" \
    -I"$repo_root/fpga_common/include" \
    "$repo_root/host/misc/test_fpga_trigger_disarm.c" \
    "$src/driver/fpga_trigger.c" \
    -Wl,--unresolved-symbols=ignore-all -o "$bin"
"$bin"

# the call must be inside bladerf2_close, ahead of the sync_deinit loop
awk '/^static void bladerf2_close\(/ {in_fn=1}
     in_fn && /fpga_trigger_disarm_all/ {armed=NR}
     in_fn && /sync_deinit\(/ {if (!armed) {print "bladerf2_close: disarm not before sync_deinit"; exit 1}; ok=1; exit 0}
     END {if (!ok) {print "bladerf2_close: no fpga_trigger_disarm_all"; exit 1}}' \
    "$src/board/bladerf2/bladerf2.c"
echo "bladerf2_close disarms triggers: PASS"
