#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd -- "$script_dir/../../../.." && pwd)"
build_dir="${BLADERF_HOST_BUILD_DIR:-$root/host/build}"
work="$(mktemp -d -p "${TMPDIR:-/var/tmp}")"
trap 'rm -rf "$work"' EXIT
src="$root/host/libraries/libbladeRF/src"

flags=(-std=gnu11 -DNUAND_MODIFICATIONS=1 -DLOG_INCLUDE_FILE_INFO -O0 -g
  -ffunction-sections -fdata-sections
  -I"$build_dir/libraries/libbladeRF/src"
  -I"$root/thirdparty/analogdevicesinc/no-OS_local/platform_bladerf2"
  -I"$build_dir/common/thirdparty/ad936x"
  -I"$src" -I"$src/board/bladerf2" -I"$root/host/libraries/libbladeRF/include"
  -I"$root/host/common/include" -I"$root/fpga_common/include"
  -I"$root/firmware_common" -I"$build_dir/host/common/include"
  -I"$build_dir/common/include")
real=("$script_dir/main.c" "$src/board/bladerf2/capabilities.c"
  "$src/board/bladerf2/compatibility.c" "$src/helpers/version.c"
  "$root/host/common/src/range.c" "$src/helpers/timeout.c")

# Symbols the initialization path never reaches stay unresolved; give each a
# trap so an unexpected call fails loudly instead of linking silently.
printf "#include <unistd.h>\n" > "$work/stubs.c"
for attempt in 1 2 3; do
  if LC_ALL=C cc "${flags[@]}" "${real[@]}" "$work/stubs.c" -Wl,--gc-sections \
      -o "$work/t" 2> "$work/link.log"; then
    break
  fi
  grep -o "undefined reference to \`[A-Za-z0-9_]*'" "$work/link.log" |
    sed "s/.*\`//; s/'//" | sort -u |
    awk '{print "void " $1 "(void) { static const char m[] = \"unexpected call to " $1 "\\n\"; (void)!write(2, m, sizeof m - 1); __builtin_trap(); }"}' >> "$work/stubs.c" || true
done
[ -x "$work/t" ] || { grep -v "^$" "$work/link.log" | head -40 >&2; exit 1; }
"$work/t"
