#!/usr/bin/env python3
"""Static NIOS RFIC step-bound report (no hardware, no Nios II timing).

Parses the ADI AD9361 driver, fpga_common/src/ad936x_helpers.c and the NIOS
RFIC command sources into one call graph, then prints per call root: static
SPI/AXI call sites reachable from ONE call, delay sites, loops that contain
SPI/delay/poll, and CPU-only loops. Call-site counts are per call, not
multiplied by loop trip counts; loops with SPI are listed so the multiplier
is visible. Output is the evidence for doc/development/nios-wcet-static.md.

usage: python3 host/misc/nios_wcet_static.py [repo_root]
"""
import os
import re
import sys

ROOT = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else
                       os.path.join(os.path.dirname(__file__), "..", ".."))
ADI = os.path.join(ROOT, "thirdparty/analogdevicesinc/no-OS/drivers/rf-transceiver/ad9361/")
NIOS = os.path.join(ROOT, "hdl/fpga/platforms/common/bladerf/software/bladeRF_nios/src/")
SOURCES = [(ADI + f, "adi") for f in (
    "ad9361.c", "ad9361_api.c", "ad9361_conv.c", "ad9361_util.c",
    "ad9361_ext_band_ctrl.c")] + [
    (os.path.join(ROOT, "fpga_common/src/ad936x_helpers.c"), "helper"),
    (NIOS + "devices_rfic_cmds.c", "nios"), (NIOS + "devices_rfic.c", "nios")]

DEF = re.compile(r'^(?:static\s+|inline\s+|const\s+|unsigned\s+|struct\s+\w+\s+)*'
                 r'[A-Za-z_][\w \t\*]*?[ \t\*]([A-Za-z_]\w*)\s*\(')
CALL = re.compile(r'\b([A-Za-z_]\w*)\s*\(')
SPI = re.compile(r'\b(ad9361_spi_\w+|adi_spi_\w+|spi_(?:read|write)\w*)\s*\(')
AXI = re.compile(r'\b(axi_(?:adc|dac)_\w+|no_os_axi_io_\w+|adi_axi_\w+)\s*\(')
DELAY = re.compile(r'\bno_os_(?:u|m)delay\(')
IO = re.compile(r'ad9361_spi_|no_os_(?:u|m)delay|_poll\(|ad9361_wait|no_os_axi|'
                r'axi_adc|axi_dac|spi_write|spi_read|adi_spi|adi_axi')
LOOP = re.compile(r'\s*(for|while)\s*\((.*)\)\s*\{?\s*$')


def strip(text):
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    return re.sub(r'//[^\n]*', '', text)


# Macros of the NIOS build of libad936x: -DBLADERF_NIOS_BUILD -DNUAND_MODIFICATIONS
# (hdl/fpga/ip/analogdevicesinc/no_OS/CMakeLists.txt:100), ALTERA_PLATFORM and
# HAVE_SPLIT_GAIN_TABLE=0 from platform_bladerf2_headless/config.h:55,64;
# BLADERF_NIOS_LIBAD936X is defined in the NIOS devices.h.
# Anything else (AXI_ADC_NOT_PRESENT, _DEBUG, DEBUG, WIN32) is undefined.
DEFINED = {"BLADERF_NIOS_BUILD": 1, "NUAND_MODIFICATIONS": 1, "ALTERA_PLATFORM": 1,
           "BLADERF_NIOS_LIBAD936X": 1,
           "HAVE_SPLIT_GAIN_TABLE": 0}


def cond_value(expr):
    expr = re.sub(r'\bdefined\s*\(\s*(\w+)\s*\)|\bdefined\s+(\w+)',
                  lambda m: "1" if (m.group(1) or m.group(2)) in DEFINED else "0", expr)
    expr = re.sub(r'[A-Za-z_]\w*', lambda m: str(DEFINED.get(m.group(0), 0)), expr)
    expr = expr.replace("&&", " and ").replace("||", " or ")
    expr = re.sub(r'!(?!=)', " not ", expr)
    return bool(eval(expr, {"__builtins__": {}}))


def preprocess(lines):
    """Blank out lines excluded by #if/#ifdef/#ifndef/#elif/#else for the NIOS build."""
    out, stack = [], []  # stack entries: [parent_active, taken, active]
    for ln in lines:
        m = re.match(r'\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)', ln)
        if m:
            d, arg = m.group(1), m.group(2).split("/*")[0].split("//")[0].strip()
            parent = stack[-1][2] if stack else True
            if d in ("if", "ifdef", "ifndef"):
                val = (cond_value(arg) if d == "if" else
                       (arg in DEFINED) == (d == "ifdef"))
                stack.append([parent, val, parent and val])
            elif d == "elif":
                s = stack[-1]
                val = (not s[1]) and cond_value(arg)
                s[1], s[2] = s[1] or val, s[0] and val
            elif d == "else":
                s = stack[-1]
                s[2], s[1] = s[0] and not s[1], True
            else:
                stack.pop()
            out.append("")
            continue
        out.append(ln if not stack or stack[-1][2] else "")
    return out


funcs = {}  # name -> (file, kind, first line, body)
for path, kind in SOURCES:
    lines = open(path, encoding="utf-8", errors="replace").read().split("\n")
    if kind != "nios":
        lines = preprocess(lines)
    i = 0
    while i < len(lines):
        ln = lines[i]
        m = DEF.match(ln)
        if (m and not ln.startswith(("\t", " ", "#", "/", "*", "typedef", "return",
                                     "else")) and not ln.rstrip().endswith(";")):
            j = i
            while j < len(lines) and "{" not in lines[j] and not lines[j].rstrip().endswith(";"):
                j += 1
            if j < len(lines) and "{" in lines[j] and lines[j][0] != "\t":
                k = j
                while k < len(lines) and lines[k].rstrip() != "}":
                    k += 1
                funcs[m.group(1)] = (os.path.basename(path), kind, i + 1,
                                     "\n".join(lines[i:k + 1]))
                i = k
        i += 1

body = {n: strip(v[3]) for n, v in funcs.items()}
body = {n: b[b.index("{"):] for n, b in body.items()}
call = {n: {c for c in CALL.findall(b) if c in funcs and c != n} |
        {c for c in re.findall(r'[&=,(]\s*(\w+)\s*[,;)]', b) if c in funcs and c != n}
        for n, b in body.items()}


def closure(roots):
    seen, stack = set(), list(roots)
    while stack:
        f = stack.pop()
        if f in seen or f not in funcs:
            continue
        seen.add(f)
        stack.extend(call[f])
    return seen


does_io = {n: any(IO.search(body[f]) for f in closure([n])) for n in funcs}

loops = []  # (func, file, line, header, has_io)
for n, (f, kind, first, _) in funcs.items():
    src = strip(funcs[n][3]).split("\n")
    for i, ln in enumerate(src):
        is_do = re.match(r'\s*do\s*\{?\s*$', ln) is not None
        if not is_do and not LOOP.match(ln) and not ln.strip().startswith("for (;;)"):
            continue
        depth, started, chunk = 0, False, []
        for j in range(i, len(src)):
            chunk.append(src[j])
            depth += src[j].count("{") - src[j].count("}")
            started = started or "{" in src[j]
            if started and depth <= 0:
                break
        head = chunk[-1].strip() if is_do else ln.strip()
        loops.append((n, f, first + i, ("do..." + head if is_do else head)[:90],
                      bool(IO.search("\n".join(chunk)) or any(
                          does_io.get(c) for c in CALL.findall("\n".join(chunk))))))


def sites(cl, rx, skip_prefix=None):
    return sum(len(rx.findall(body[f])) for f in cl
               if not (skip_prefix and f.startswith(skip_prefix)))


def metrics(roots):
    cl = closure(roots)
    return {
        "cl": cl,
        "spi": sites(cl, SPI, "ad9361_spi_"),
        "axi": sites(cl, AXI, "axi_"),
        "delay": sorted(f for f in cl if DELAY.search(body[f])),
        "io_loops": [l for l in loops if l[0] in cl and l[4]],
        "cpu_loops": [l for l in loops if l[0] in cl and not l[4]],
    }


def nios_calls(fn):
    return sorted(c for c in call[fn] if funcs[c][1] != "nios" or c != fn)


def main():
    print("functions parsed:", len(funcs), " loops:", len(loops),
          " cpu-only:", sum(1 for l in loops if not l[4]))

    print("\n== command callbacks (devices_rfic_cmds.c), one callback invocation ==")
    print("%-34s %4s %4s %5s  %s" % ("callback", "spi", "axi", "dly", "driver/helper roots"))
    for n in sorted(funcs):
        if funcs[n][0] != "devices_rfic_cmds.c":
            continue
        if not (n.startswith("_rfic_cmd_") or n in ("_rfic_initialize", "_rfic_standby",
                                                    "_rfic_deinitialize")):
            continue
        roots = sorted(c for c in call[n] if funcs[c][1] in ("adi", "helper"))
        m = metrics([n])
        print("%-34s %4d %4d %5d  %s" % (n, m["spi"], m["axi"], len(m["delay"]),
                                         ",".join(roots)[:110]))

    print("\n== worker poll roots (one worker turn, devices_rfic.c:rfic_command_work_wq) ==")
    for n in sorted(call["rfic_command_work_wq"]):
        if funcs[n][1] == "adi" and n.endswith(("_poll", "_start")):
            m = metrics([n])
            print("%-46s spi=%-3d axi=%-3d io_loops=%d cpu_loops=%d delay_fns=%s" % (
                n, m["spi"], m["axi"], len(m["io_loops"]),
                len(m["cpu_loops"]), ",".join(m["delay"]) or "-"))

    nios_roots = sorted({c for n, v in funcs.items() if v[1] == "nios" for c in call[n]
                         if funcs[c][1] in ("adi", "helper")})
    allm = metrics(nios_roots)
    print("\n== reachable from NIOS call sites: %d roots, %d functions ==" % (
        len(nios_roots), len(allm["cl"])))
    print("-- loops with SPI/delay/poll inside --")
    for l in sorted(allm["io_loops"], key=lambda x: (x[1], x[2])):
        print("  %s:%d %s: %s" % (l[1], l[2], l[0], l[3]))
    print("-- CPU-only loops (%d) --" % len(allm["cpu_loops"]))
    for l in sorted(allm["cpu_loops"], key=lambda x: (x[1], x[2])):
        print("  %s:%d %s: %s" % (l[1], l[2], l[0], l[3]))
    print("-- delay sites (function names) --")
    print("  " + ", ".join(allm["delay"]))

    print("\n== int-only *_with_cleanup wrappers ==")
    wc = set()
    nullw = []
    wrap = re.compile(r'^int32_t (ad9361_\w+)\([^)]*\)\s*\{\s*return\s+(\w+)\([^;]*?NULL\);',
                      re.M)
    for path, kind in SOURCES:
        if kind != "adi":
            continue
        text = strip(open(path, encoding="utf-8", errors="replace").read())
        wc |= set(re.findall(r'^int32_t (ad9361_\w+_with_cleanup)\(', text, re.M))
        for m in wrap.finditer(text):
            nullw.append((os.path.basename(path), m.group(1), m.group(2)))
    print("with_cleanup definitions:", len(wc), " int-only wrappers passing NULL:",
          len(nullw))
    for f, b, t in sorted(nullw):
        print("  %s %s -> %s" % (f, b, t))
    print("with_cleanup without int-only wrapper:",
          sorted(wc - {t for _, _, t in nullw}))


if __name__ == "__main__":
    main()
