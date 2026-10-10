#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cc="${CC:-cc}"
tmp_dir="$(mktemp -d)"
trap 'rm -rf "$tmp_dir"' EXIT

"$cc" -std=c11 -Wall -Wextra -Werror \
    "$repo_root/host/misc/test_libusb_transfer_policy.c" \
    -o "$tmp_dir/test_libusb_transfer_policy"
"$tmp_dir/test_libusb_transfer_policy"
