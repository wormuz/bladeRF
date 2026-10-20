#!/bin/sh
# RX sweep: execution data in NIOS success records and the record-read
# packet (PC simulation queue test), host record reads, the sample
# certification rule, and the host session ledger.
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
host_build_dir=${HOST_BUILD_DIR:-"$repo_root/host/build"}
out_dir=$(mktemp -d)
trap 'rm -rf "$out_dir"' EXIT

cc -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-function \
    -ffunction-sections -fdata-sections \
    -DBLADERF_NIOS_PC_SIMULATION \
    -DBLADERF_NIOS_LIBAD936X \
    -I"$repo_root/fpga_common/include" \
    -I"$repo_root/hdl/fpga/platforms/common/bladerf/software/bladeRF_nios/src" \
    -I"$repo_root/hdl/fpga/platforms/bladerf-micro/software/bladeRF_nios/src" \
    -I"$repo_root/host/libraries/libbladeRF/include" \
    -I"$repo_root/host/libraries/libbladeRF/src" \
    "$repo_root/host/misc/test_nios_retune2_queue.c" \
    -Wl,--gc-sections -o "$out_dir/queue"
"$out_dir/queue"

# The existing nios_access.c logging macros compile out some local variables.
cc -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-variable \
    -Wno-unused-but-set-variable -ffunction-sections -fdata-sections \
    -I"$repo_root/fpga_common/include" \
    -I"$repo_root/host/libraries/libbladeRF/include" \
    -I"$repo_root/host/libraries/libbladeRF/src" \
    -I"$repo_root/host/common/include" \
    -I"$host_build_dir/common/include" \
    -I"$repo_root/firmware_common" \
    "$repo_root/host/libraries/libbladeRF/src/backend/usb/nios_access.c" \
    "$repo_root/host/libraries/libbladeRF/src/backend/usb/nios_transaction.c" \
    "$repo_root/host/libraries/libbladeRF/src/helpers/timeout.c" \
    "$repo_root/host/misc/test_nios_retune2_record_host.c" \
    -Wl,--gc-sections -o "$out_dir/record"
"$out_dir/record"

cc -Wall -Wextra -Werror \
    -I"$repo_root/fpga_common/include" \
    -I"$repo_root/host/libraries/libbladeRF/include" \
    -I"$repo_root/host/libraries/libbladeRF/src" \
    -I"$repo_root/host/common/include" \
    -I"$host_build_dir/common/include" \
    "$repo_root/host/misc/test_rx_sweep_policy.c" -o "$out_dir/policy"
"$out_dir/policy"

cc -Wall -Wextra -Werror -Wno-unused-parameter \
    -ffunction-sections -fdata-sections \
    -I"$repo_root/host/libraries/libbladeRF/include" \
    -I"$repo_root/host/libraries/libbladeRF/src" \
    -I"$repo_root/host/common/include" \
    -I"$host_build_dir/common/include" \
    -I"$repo_root/fpga_common/include" \
    -I"$repo_root/firmware_common" \
    -I"$host_build_dir/libraries/libbladeRF/src" \
    -I"$host_build_dir/common/thirdparty/ad936x" \
    -I"$repo_root/thirdparty/analogdevicesinc/no-OS_local/platform_bladerf2" \
    "$repo_root/host/misc/test_rx_sweep_session.c" \
    -Wl,--gc-sections -lpthread -o "$out_dir/session"
"$out_dir/session"

cc -Wall -Wextra -Werror -I"$repo_root/fpga_common/include" \
    "$repo_root/host/misc/test_nios_rfic_owner_admission.c" \
    -o "$out_dir/admission"
"$out_dir/admission"
