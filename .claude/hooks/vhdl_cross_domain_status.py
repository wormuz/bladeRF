#!/usr/bin/env python3
"""PreToolUse: block raw rx_/tx_ signals being packed into a system-clock word.

The RF_LINK_STATUS word was assembled in bladerf-hosted.vhd from signals
living in rx_clock and tx_clock and wired straight into a system-clock input
PIO. Thirty of thirty-two bits crossed a clock domain with nothing in
between. Worst setup path -7.168 ns, rx|fifo_writer|link_active_i into
rf_link_status|readdata[16], measured on hostedxA4-2026-09-10_01.49.45.

It read perfectly well as VHDL. Nothing in the assignment says which domain
either side lives in, which is exactly why it survived review and was found
by a failing build.

So: an assignment into a *_status / *_state word whose right-hand side names
a bare rx_* or tx_* signal is refused. What passes:

    rf_link_status(0) <= tx_link_active_sys;   -- synchronised, _sys suffix
    rf_link_status(3) <= '0';                  -- constant
    rf_link_status(2) <= nios_gpio.o.usb_speed;-- already system domain

What does not:

    rf_link_status(0) <= tx_link_active;               -- raw crossing
    rf_link_status(3) <= tx_mismatch or rx_mismatch;   -- worse: two domains
                                                       -- ORed combinationally,
                                                       -- so the result has no
                                                       -- owning domain at all
                                                       -- and no synchroniser
                                                       -- can be attached

The suffix convention is the check. If a signal is genuinely in the system
domain but named rx_something, rename it or add _sys -- the name is the only
thing a reader (or this hook) has to go on.
"""
import json
import re
import sys

# Left-hand sides that are read by another domain: status/state words that
# feed a PIO or a register file.
_TARGET = re.compile(r"^\s*(\w*(?:status|state)\w*)\s*\(", re.IGNORECASE)

# A bare rx_/tx_ signal: not followed by _sys, and not a record field of
# something already qualified.
_RAW = re.compile(r"\b((?:rx|tx)_\w+)\b")


def _is_synchronised(name: str) -> bool:
    return name.endswith("_sys") or name.endswith("_sync")


def _locally_derived(name: str, code: str) -> bool:
    """True if the signal is computed in this file from already-synchronised
    inputs, rather than arriving raw from another domain.

    rx_epoch_current is the case that forced this: it is named rx_* but is a
    system-domain comparison of rx_epoch_ack_sys against the issued toggle.
    Judging by suffix alone flagged it, which would have taught the reader to
    ignore the hook.
    """
    for line in code.split("\n"):
        if "<=" not in line:
            continue
        lhs, rhs = line.split("<=", 1)
        if lhs.strip() != name:
            continue
        # Every rx_/tx_ name it depends on must itself be synchronised.
        deps = _RAW.findall(rhs)
        return bool(deps) and all(_is_synchronised(d) for d in deps)
    return False


def _offending(text: str):
    for line in text.split("\n"):
        code = line.split("--")[0]
        if "<=" not in code:
            continue
        m = _TARGET.match(code)
        if not m:
            continue
        rhs = code.split("<=", 1)[1]
        raw = [n for n in _RAW.findall(rhs)
               if not _is_synchronised(n) and not _locally_derived(n, text)]
        if raw:
            return m.group(1), code.strip(), raw
    return None


def main() -> int:
    try:
        payload = json.load(sys.stdin)
    except ValueError:
        return 0

    if payload.get("tool_name") not in ("Edit", "Write", "MultiEdit"):
        return 0

    ti = payload.get("tool_input", {}) or {}
    if not ti.get("file_path", "").endswith(".vhd"):
        return 0

    text = ti.get("new_string") or ti.get("content") or ""
    hit = _offending(text)
    if not hit:
        return 0

    word, line, raw = hit
    print(json.dumps({
        "hookSpecificOutput": {
            "hookEventName": "PreToolUse",
            "permissionDecision": "deny",
            "permissionDecisionReason": (
                f"⛔ {word} is read from another clock domain, and this line "
                f"packs raw signals into it:\n\n    {line}\n\n"
                f"Unsynchronised: {', '.join(sorted(set(raw)))}\n\n"
                "Thirty bits of RF_LINK_STATUS crossed a domain this way and "
                "surfaced only as a failing setup path (measured on build "
                "hostedxA4-2026-09-10_01.49.45, Slow 1100mV 85C). The "
                "assignment reads fine; nothing in it says which domain "
                "either side is in.\n\n"
                "Fix: run each bit through the synchronizer entity into the "
                "destination clock and assemble the word from the _sys "
                "copies. If two directions must be combined, combine the "
                "synchronised copies -- an OR of a tx_clock signal and an "
                "rx_clock signal has no owning domain, so no synchroniser can "
                "be attached to it at all.\n\n"
                "If the signal really is in the destination domain already, "
                "give it a name that says so."),
        }
    }))
    return 0


if __name__ == "__main__":
    sys.exit(main())
