#!/usr/bin/env python3
"""PreToolUse: block SDC exceptions whose two ends span different instances.

Three times in one session the same mistake shipped, each time looking
correct:

    set_false_path -from [get_keepers {*|auto_generated|*rdptr_g*}] \
                   -to   [get_keepers {*|auto_generated|*ws_dgrp*dffe*}]

Measured on hostedxA4-2026-09-10_01.49.45, rdptr_g exists in five dcfifo
instances and ws_dgrp in two, so no pair of endpoints ever belonged to the
same FIFO. Both collections come back non-empty, so a
`get_collection_size > 0` guard passes; Quartus reports 332182 "No path is
found" and the constraint binds to nothing. The same shape hit
`*_tamer|hold_time[*]` -> `*_tamer|compare_time[*]` (two time_tamer
instances) and the handshake block (five instances).

So: an exception naming BOTH -from and -to, where both patterns are
wildcard-led and neither pins an instance, is refused. Ways to satisfy it:

  * name the instance in at least one end
      *time_tamer:rx_tamer|hold_time[*]
  * walk the owning instances and constrain each pair separately
  * assign the collection to a variable first (the hook only sees literal
    patterns, and a variable means the surrounding Tcl is doing the pairing)

set_max_skew and set_net_delay with -from only are NOT blocked: they bound
placement and excuse nothing, so a broad match is safe there. set_false_path
with -from only IS blocked -- that is the blanket form that hid a -11.203 ns
path for weeks.
"""
import json
import re
import sys

# Commands where a mismatched pair silently drops the constraint.
_PAIRED = r"set_false_path|set_max_skew|set_net_delay|set_multicycle_path|set_max_delay|set_min_delay"

# Form 1: -from {...} -to {...} in one command.
_PATTERN = re.compile(
    r"(" + _PAIRED + r")\b[^\n]*?"
    r"-from\s*\[?\s*get_(?:keepers|registers|pins|nodes)[^\{]*\{\s*(\*[^\}]*)\}"
    r"[^\n]*?"
    r"-to\s*\[?\s*get_(?:keepers|registers|pins|nodes)[^\{]*\{\s*(\*[^\}]*)\}",
    re.IGNORECASE)

# Form 2, and the one that actually shipped: the pair is assembled from two
# variables and handed to a procedure, so no single line contains both ends.
#
#     set rd_from [get_keepers -nowarn {*|auto_generated|*rdptr_g*}]
#     set rd_to   [get_keepers -nowarn {*|auto_generated|*ws_dgrp*dffe*}]
#     ...
#     apply_sdc_mw_dcfifo_for_ptrs $rd_from $rd_to
#
# Catch it by collecting every `set <var> [get_keepers {<wildcard pattern>}]`
# that does not pin an instance, then looking for a call or command that uses
# two of those variables together.
_ASSIGN = re.compile(
    r"set\s+(\w+)\s+\[\s*get_(?:keepers|registers|pins|nodes)[^\{]*\{\s*(\*[^\}]*)\}",
    re.IGNORECASE)

_USES_TWO = re.compile(r"\$(\w+)[^\n$]*\$(\w+)")

# An instance qualifier looks like  entity:instance|  -- that pins the pattern
# to one instance rather than every elaboration of the entity.
_INSTANCE = re.compile(r"[A-Za-z_][\w]*:[A-Za-z_][\w]*\|")


def _offending(text: str):
    # Strip comments: the file explains the defect at length, and the
    # explanation must not trip the check.
    code = "\n".join(l.split("#")[0] for l in text.split("\n"))

    for m in _PATTERN.finditer(code):
        cmd, src, dst = m.group(1), m.group(2), m.group(3)
        if _INSTANCE.search(src) or _INSTANCE.search(dst):
            continue
        return cmd, src, dst

    # Variables holding an unpinned wildcard collection.
    loose = {v: p for v, p in _ASSIGN.findall(code) if not _INSTANCE.search(p)}
    if len(loose) < 2:
        return None

    for line in code.split("\n"):
        pair = _USES_TWO.search(line)
        if not pair:
            continue
        a, b = pair.group(1), pair.group(2)
        if a in loose and b in loose and a != b:
            return ("$%s / $%s" % (a, b), loose[a], loose[b])
    return None


def main() -> int:
    try:
        payload = json.load(sys.stdin)
    except ValueError:
        return 0

    if payload.get("tool_name") not in ("Edit", "Write", "MultiEdit"):
        return 0

    ti = payload.get("tool_input", {}) or {}
    path = ti.get("file_path", "")
    if not path.endswith(".sdc"):
        return 0

    text = ti.get("new_string") or ti.get("content") or ""
    hit = _offending(text)
    if not hit:
        return 0

    cmd, src, dst = hit
    print(json.dumps({
        "hookSpecificOutput": {
            "hookEventName": "PreToolUse",
            "permissionDecision": "deny",
            "permissionDecisionReason": (
                f"⛔ {cmd}: both ends are wildcards and neither pins an "
                f"instance.\n\n"
                f"  -from {{{src}}}\n"
                f"  -to   {{{dst}}}\n\n"
                "A pattern like this collects registers from SEVERAL "
                "instances, so the constraint asks for paths that do not "
                "exist -- one FIFO's pointer into another FIFO's "
                "synchroniser, rx_tamer's hold_time into tx_tamer's "
                "compare_time. Both collections are non-empty, so a size "
                "guard passes, and Quartus reports 332182 'No path is found' "
                "while the constraint binds to nothing. This exact shape "
                "shipped three times in one session.\n\n"
                "Fix: pin the instance in at least one end "
                "(*time_tamer:rx_tamer|hold_time[*]), or walk the owning "
                "instances and constrain each pair, or build the collections "
                "in Tcl variables so the pairing is explicit.\n\n"
                "Then count what bound: a block that constrains zero "
                "instances must post_message -type error, not warning."),
        }
    }))
    return 0


if __name__ == "__main__":
    sys.exit(main())
