#!/usr/bin/env python3
"""Worst-case NIOS stack depth from the linked ELF.

Works on the final disassembly, not on per-object .su files, so the frame
sizes are those of the code that actually runs (LTO, constprop and isra
clones included, BSP/newlib archives included).

  frame(f)  sum of every negative `addi sp,sp,-N` in f (conservative when a
            function adjusts sp more than once)
  edges     `call`, plus `jmpi`/`br` leaving the function (tail calls,
            counted as if the caller frame were still live: overestimate)
  indirect  `callr`/`jmp rX` sites; targets come from INDIRECT below, keyed
            by the calling function. An unlisted indirect site is an error,
            not a guess.

The ISR path is HAL alt_irq_entry -> alt_irq_handler -> registered ISR, and
it lands on the interrupted stack (no separate interrupt stack in this BSP),
so the budget is max(main path) + max(irq path). HAL IIC does not re-enable
interrupts inside a handler, so ISRs do not nest.
"""
import argparse
import re
import subprocess
import sys

# Indirect call sites -> possible targets, from source review of each
# function-pointer use. Key: function containing the callr/jmp; value: regex
# over the address-taken functions of the ELF (every possible target of any
# pointer call is in that set). An empty regex list means the pointer is a
# hook no linked function is ever stored into (NULL in this build).
INDIRECT = {
    # HAL: registered ISRs (devices.c, pkt_retune2.c alt_ic_isr_register)
    "alt_irq_handler": [r"command_uart_isr|vctcxo_tamer_isr|retune_(rx|tx)"],
    # bladeRF_nios.c pkt_handlers[].init / .exec / .do_work
    "main": [r"pkt_(8x8|8x16|8x32|8x64|16x64|32x32|legacy|retune2)"
             r"(_init)?"],
    "run_deferred_packet_work": [r"pkt_(16x64|retune2)_work"],
    "run_do_work": [r"pkt_(16x64|retune2)_work"],
    # devices_rfic.c funcs[]: pkt_16x64 calls .read32/.read64 (line 1286/90),
    # pkt_16x64_work calls .write32/.write64 (line 980/2)
    "pkt_16x64": [r"_rfic_cmd_rd_.*"],
    "pkt_16x64_work": [r"_rfic_cmd_wr_.*"],
    # spi_arbiter_policy.h expired(deadline_context)
    "bladerf_spi_arbiter_release_until.constprop.0":
        [r"bladerf_spi_arbiter_deadline_expired"],
    # no-OS gpio/spi platform_ops (bladerf2_gpio_ops / bladerf2_spi_ops)
    "_rfic_cmd_wr_init": [r"bladerf2_headless_(gpio_get|spi_init)"],
    "no_os_gpio_get_optional": [r"bladerf2_headless_gpio_get_optional"],
    "no_os_gpio_remove": [r"bladerf2_headless_gpio_remove"],
    "no_os_gpio_direction_output":
        [r"bladerf2_headless_gpio_direction_output"],
    "no_os_gpio_set_value": [r"bladerf2_headless_gpio_set_value"],
    "ad9361_ext_band_transaction_poll":
        [r"bladerf2_headless_gpio_direction_input"],
    "no_os_spi_write_and_read": [r"bladerf2_headless_spi_write_and_read"],
    "ad9361_remove_ex.constprop.0": [r"bladerf2_headless_spi_remove"],
    # platform.c service_hook (bladeRF_nios.c main sets it)
    "bladerf_platform_service": [r"service_rfic_status_request"],
    # phy->read_cal_done = _rfic_read_cal_done (devices_rfic_cmds.c)
    "ad9361_calibration_wait_start": [r"_rfic_read_cal_done"],
    "ad9361_calibration_wait_poll": [r"_rfic_read_cal_done"],
    # spi platform_ops update_register_bits /
    # write_register_batch: not set in bladerf2_spi_ops
    "__ad9361_spi_writef": [],
    # write_gain_table_row: bladerf2_headless_spi.c (RETUNE2 gain-table load)
    "ad9361_gain_table_load_poll":
        [r"bladerf2_headless_spi_write_gain_table_row"],
    "ad9361_spi_write_register_run.constprop.0": [],
    # phy->ad9361_rfpll_ext_*: NULL in ad936x_params.c (external LO unused)
    "ad9361_rfpll_recalc_rate_checked": [],
    "ad9361_rfpll_round_rate": [],
    "ad9361_get_rx_lo_freq": [],
    "ad9361_get_tx_lo_freq": [],
    "clk_set_rate": [],
    # crt0 jump to _start
    "__reset": [],
}

# Context cuts: (ctx, func, callees): while ctx is on the stack, func cannot
# reach those callees (None: any callee). Each one is a source-proven bound
# on a static-graph cycle.
#  service_rfic_status_request  static `servicing` flag tested before the
#      prologue (bladeRF_nios.c); it is also the SPI safe-point hook, so a
#      nested call returns at once.
#  ad9361_dig_tune_delay        retunes the clock chain only `if (max_freq)`;
#      every dig_tune call reachable from the clock-chain poll passes
#      max_freq = 0 (ad9361.c DIG_TUNE stages, FIR validate). Checked by
#      check_context_cuts(): the nonzero caller is not reachable from there.
CONTEXT_CUTS = [
    # service_rfic_status_request_impl forwards only STATUS, INIT,
    # TRANSACTION_STATUS and PROGRESS reads (0x0f00/01/0c/0d) to the RFIC
    # read table; other RFIC requests are answered busy without dispatch.
    ("service_rfic_status_request", "pkt_16x64",
     {"_rfic_cmd_rd_" + c for c in (
         "bandwidth", "enable", "filter", "frequency", "gain", "gainmode",
         "result_detail", "rssi", "samplerate", "txmute")}),
    ("service_rfic_status_request", "service_rfic_status_request", None),
    ("ad9361_clock_chain_transaction_poll",
     "ad9361_dig_tune_delay.constprop.0",
     {"ad9361_clock_chain_transaction_start",
      "ad9361_clock_chain_transaction_poll",
      "ad9361_calculate_rf_clock_chain",
      "ad9361_set_trx_clock_chain_freq_with_cleanup.constprop.0"}),
    # The clock-chain DIVIDERS stage is the only clk_set_rate caller under
    # the chain poll; it passes ref_clk_scale[rx/tx_clock_ids[i]], divider
    # clocks only (ad9361.c). The BBPLL, RFPLL_INT, RFPLL_DUMMY and RFPLL
    # cases of clk_set_rate (ad9361_util.c), where these callees live, are
    # never taken there. Checked by check_context_cuts().
    ("ad9361_clock_chain_transaction_poll", "clk_set_rate",
     {"ad9361_ext_band_transaction_run_sync",
      "ad9361_gain_table_transaction_start.constprop.0",
      "ad9361_gain_table_transaction_poll",
      "ad9361_gain_table_transaction_poll_delay_us",
      "ad9361_rfpll_round_rate",
      "ad9361_rfpll_int_round_rate.constprop.0",
      "ad9361_rfpll_int_set_rate",
      "ad9361_rfpll_dummy_set_rate",
      "ad9361_bbpll_clock_set_rate"}),
]
# Linked dig_tune callers with nonzero max_freq (post-setup, inlined there).
NONZERO_DIG_TUNE = {"_rfic_cmd_wr_samplerate"}
# enum ad9361_clocks ADC_CLK..TX_SAMPL_CLK (ad9361.h).
DIVIDER_CLOCKS = (4, 13)

# Address-taken only as ops-table members whose callers were not linked
# (no_os_gpio_get_value/get_direction removed by --gc-sections), plus the
# reset entry points.
UNCALLED = {"_start", "__reset", "bladerf2_headless_gpio_get_direction",
            "bladerf2_headless_gpio_get_value"}


def run(tool, *args):
    return subprocess.run([tool, *args], check=True, capture_output=True,
                          text=True).stdout


def load(elf, prefix):
    syms = {}
    for line in run(prefix + "readelf", "-sW", elf).splitlines():
        p = line.split()
        if len(p) >= 8 and p[3] == "FUNC" and p[6] != "UND":
            addr, size = int(p[1], 16), int(p[2])
            if size:
                syms.setdefault(addr, (p[7], size))
    funcs = sorted((a, n, s) for a, (n, s) in syms.items())
    starts = {a: n for a, n, _ in funcs}
    ins_re = re.compile(r"^\s*([0-9a-f]+):\s+([0-9a-f]{8})\s+(\S+)\s*(.*)$")
    frame = {n: 0 for _, n, _ in funcs}
    edges = {n: set() for _, n, _ in funcs}
    indirect = {n: 0 for _, n, _ in funcs}
    hi = {}
    taken = set()
    idx = 0
    pending_jmp = None
    in_table = False
    r5 = {}
    last_r5, last_fn = None, None
    for line in run(prefix + "objdump", "-d", elf).splitlines():
        m = ins_re.match(line)
        if not m:
            continue
        addr, word = int(m.group(1), 16), int(m.group(2), 16)
        op, args = m.group(3), m.group(4)
        while idx + 1 < len(funcs) and funcs[idx + 1][0] <= addr:
            idx += 1
        fa, fn, fs = funcs[idx]
        if not (fa <= addr < fa + fs):
            continue
        # A switch is `jmp rX` followed inline by a table of addresses
        # inside the same function; those words are data, not code.
        if pending_jmp is not None or in_table:
            if fa <= word < fa + fs and word != fa:
                pending_jmp, in_table = None, True
                continue
            if pending_jmp is not None:
                indirect[pending_jmp] += 1
            pending_jmp, in_table = None, False
        a = [x.strip() for x in args.split("<")[0].split(",")]
        if fn != last_fn:
            last_r5, last_fn = None, fn
        if op == "addi" and a[:2] == ["sp", "sp"] and int(a[2]) < 0:
            frame[fn] += -int(a[2])
        elif op in ("call", "jmpi", "br"):
            tgt = int(a[-1], 16)
            if op == "call" or not (fa <= tgt < fa + fs):
                if tgt not in starts:
                    sys.exit(f"{fn}@{addr:x}: {op} into non-start {tgt:x}")
                edges[fn].add(starts[tgt])
                r5.setdefault((fn, starts[tgt]), set()).add(last_r5)
        elif op == "callr":
            indirect[fn] += 1
        elif op == "jmp" and a[0] != "ra":
            pending_jmp = fn
        elif op == "movhi":
            hi[a[0]] = int(a[1], 0) << 16
        elif op in ("addi", "ori") and a[0] == a[1] and a[0] in hi:
            lo = int(a[2], 0)
            v = (hi.pop(a[0]) + lo) & 0xFFFFFFFF if op == "addi" \
                else hi.pop(a[0]) | lo
            if v in starts:
                taken.add(starts[v])
        if a[0] == "r5" and not op.startswith(("st", "b", "call")):
            last_r5 = f"{op} {args.split('<')[0].strip()}"
    return funcs, starts, frame, edges, indirect, taken, r5


def data_taken(elf, prefix, starts):
    taken = set()
    for sec in (".rodata", ".rwdata"):
        raw = subprocess.run([prefix + "objcopy", "-O", "binary", "-j", sec,
                              elf, "/dev/stdout"], check=True,
                             capture_output=True).stdout
        for i in range(0, len(raw) - 3, 4):
            v = int.from_bytes(raw[i:i + 4], "little")
            if v in starts:
                taken.add(starts[v])
    return taken


def reachable(root, edges):
    seen, todo = set(), [root]
    while todo:
        f = todo.pop()
        if f not in seen:
            seen.add(f)
            todo.extend(edges[f])
    return seen


def check_context_cuts(edges, r5, clock_ids):
    ctx = "ad9361_clock_chain_transaction_poll"
    under = reachable(ctx, edges)
    bad = under & NONZERO_DIG_TUNE
    if bad:
        sys.exit(f"{ctx} reaches nonzero dig_tune caller {sorted(bad)}")
    for (caller, callee), loads in r5.items():
        if callee == "ad9361_dig_tune_with_cleanup" and caller in under \
                and caller not in NONZERO_DIG_TUNE \
                and loads != {"mov r5,zero"}:
            sys.exit(f"{caller} -> dig_tune with r5 from {sorted(loads)}")
    setters = {f for f, e in edges.items() if "clk_set_rate" in e}
    if setters & under != {ctx}:
        sys.exit(f"clk_set_rate callers under {ctx}: "
                 + ", ".join(sorted(setters & under)))
    for name, ids in clock_ids.items():
        if not ids or not all(DIVIDER_CLOCKS[0] <= i <= DIVIDER_CLOCKS[1]
                              for i in ids):
            sys.exit(f"{name} holds non-divider clocks {list(ids)}")
    if not clock_ids:
        sys.exit("rx_clock_ids/tx_clock_ids not found in the ELF")


def clock_ids(elf, prefix):
    # Bytes of every rx_clock_ids.N / tx_clock_ids.N array in the image.
    secs = []
    for line in run(prefix + "readelf", "-SW", elf).splitlines():
        m = re.match(r"\s*\[\s*\d+\]\s+(\S+)\s+PROGBITS\s+([0-9a-f]+)\s+"
                     r"([0-9a-f]+)\s+([0-9a-f]+)", line)
        if m and int(m.group(2), 16):  # skip non-loaded (debug) sections
            secs.append((int(m.group(2), 16), int(m.group(3), 16),
                         int(m.group(4), 16)))
    with open(elf, "rb") as fh:
        image = fh.read()
    out = {}
    for line in run(prefix + "readelf", "-sW", elf).splitlines():
        p = line.split()
        if len(p) >= 8 and re.fullmatch(r"[rt]x_clock_ids\.\d+", p[7]):
            addr, size = int(p[1], 16), int(p[2])
            for base, off, length in secs:
                if base <= addr and addr + size <= base + length:
                    start = off + addr - base
                    out[p[7]] = image[start:start + size]
    return out


def check_guard_before_prologue(elf, prefix, starts):
    # A None cut charges the nested call no frame: its first instructions
    # must be the flag load and the early-return branch, before any sp move.
    addr = {n: a for a, n in starts.items()}
    for _, f, cut in CONTEXT_CUTS:
        if cut is not None:
            continue
        out = run(prefix + "objdump", "-d", "--no-show-raw-insn",
                  f"--start-address={addr[f]}",
                  f"--stop-address={addr[f] + 8}", elf)
        ops = re.findall(r"^\s*[0-9a-f]+:\s+(\S+)", out, re.M)
        if len(ops) != 2 or not ops[0].startswith("ld") \
                or ops[1] not in ("bne", "beq"):
            sys.exit(f"{f}: re-entry guard is not ahead of the prologue")


def deepest(root, frame, edges):
    # Memo key carries the CONTEXT_CUTS contexts on the stack.
    ctxs = {c for c, _, _ in CONTEXT_CUTS}
    memo, onpath, stack = {}, set(), []

    def go(f, active):
        key = (f, active)
        if key in memo:
            return memo[key]
        if key in onpath:
            raise RecursionError(" > ".join(stack[stack.index(f):] + [f]))
        callees, label, own = set(edges[f]), f, frame[f]
        for c, g, cut in CONTEXT_CUTS:
            if g == f and c in active:
                callees = set() if cut is None else callees - cut
                own = 0 if cut is None else own
                label = f + " (cut)"
        onpath.add(key)
        stack.append(f)
        inner = active | {f} if f in ctxs else active
        best = (0, [])
        for g in callees:
            d, p = go(g, inner)
            if d > best[0]:
                best = (d, p)
        onpath.discard(key)
        stack.pop()
        memo[key] = (own + best[0], [(own, label)] + best[1])
        return memo[key]

    return go(root, frozenset())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("--prefix", default="nios2-elf-")
    ap.add_argument("--list-indirect", action="store_true")
    ap.add_argument("--root", default="alt_main")
    ap.add_argument("--budget", type=int,
                    help="bytes available to stack (fail if exceeded)")
    o = ap.parse_args()

    funcs, starts, frame, edges, indirect, taken, r5 = load(o.elf, o.prefix)
    taken |= data_taken(o.elf, o.prefix, starts)
    if o.list_indirect:
        for f, n in sorted(indirect.items()):
            if n:
                print(f"indirect {f} x{n}")
        for f in sorted(taken):
            print(f"taken {f} frame={frame[f]}")
        return
    missing = [f for f, n in indirect.items() if n and f not in INDIRECT]
    if missing:
        sys.exit("unresolved indirect call sites in: " + ", ".join(missing))
    for f, pats in INDIRECT.items():
        if f not in frame:
            sys.exit(f"INDIRECT caller {f} not in ELF")
        for p in pats:
            hit = {t for t in taken if re.fullmatch(p, t)}
            if not hit:
                sys.exit(f"INDIRECT {f}: {p} matches no address-taken func")
            edges[f] |= hit
    covered = {t for pats in INDIRECT.values() for p in pats
               for t in taken if re.fullmatch(p, t)}
    stray = taken - covered - UNCALLED
    if stray:
        sys.exit("address-taken but never an indirect target: "
                 + ", ".join(sorted(stray)))

    check_context_cuts(edges, r5, clock_ids(o.elf, o.prefix))
    check_guard_before_prologue(o.elf, o.prefix, starts)
    d, path = deepest(o.root, frame, edges)
    print(f"main {o.root}: {d} B")
    for fr, f in path:
        print(f"  {fr:5d}  {f}")
    # HAL alt_exception (asm, no FUNC size) pushes its register frame on the
    # interrupted stack, then calls alt_irq_handler -> registered ISR.
    exc = exception_frame(o.elf, o.prefix)
    di, pi = deepest("alt_irq_handler", frame, edges)
    print(f"irq: {exc + di} B  alt_exception {exc} > "
          + " > ".join(f"{f} {fr}" for fr, f in pi))
    total = d + exc + di
    print(f"total {total} B")
    if o.budget is not None:
        print(f"budget {o.budget} B, margin {o.budget - total} B")
        if total > o.budget:
            sys.exit(1)


def exception_frame(elf, prefix):
    addr = None
    for line in run(prefix + "readelf", "-sW", elf).splitlines():
        p = line.split()
        if len(p) >= 8 and p[7] == "alt_exception":
            addr = int(p[1], 16)
    out = run(prefix + "objdump", "-d", "--no-show-raw-insn",
              f"--start-address={addr}", f"--stop-address={addr + 8}", elf)
    m = re.search(r"addi\s+sp,sp,-(\d+)", out)
    if not m:
        sys.exit("alt_exception does not open with addi sp,sp,-N")
    return int(m.group(1))


if __name__ == "__main__":
    main()
