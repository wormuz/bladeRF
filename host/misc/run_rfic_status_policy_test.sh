#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
test_bin=$(mktemp)
trap 'rm -f "$test_bin"' EXIT

cc -std=c99 -Wall -Wextra -Werror \
    -I"$repo_root/host/libraries/libbladeRF/src" \
    "$repo_root/host/misc/test_rfic_status_policy.c" \
    -o "$test_bin"

"$test_bin"
