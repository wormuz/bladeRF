# Post-fit proof that every CDC constraint reached the design it names.
#
#   cd hdl/quartus/work/bladerf-micro-A4-hosted
#   ~/soft/q25 quartus_sta -t ../../qbind.tcl
#
# qcheck reads the SDC as text and catches patterns that are wrong on their
# face. It cannot answer the question that actually cost this project three
# days: does the fitted netlist contain the paths and endpoints the SDC set
# out to constrain?
#
# Every defect in that cycle was in this blind spot. fx3_timestamp was a wire
# taken for a register. delayed_wrptr_g was a vendor name absent from this IP
# version. The dcfifo pointer crossings were already cut by the megafunction,
# so anything layered on them attached to nothing. All three read correctly,
# bound to nothing, and were found only by a build.
#
# Exits non-zero if any check fails, so a build script can gate on it.

set failures 0

proc fail { msg } {
    global failures
    incr failures
    puts "FAIL  $msg"
}

proc ok { msg } {
    puts "ok    $msg"
}

# ---------------------------------------------------------------------------

project_open -revision hosted bladerf
create_timing_netlist -model slow -temperature 85 -voltage 1100
read_sdc
update_timing_netlist

# A crossing is described by its two ends and how many instances of it we
# expect. Keep this list in step with bladerf.sdc: a crossing that appears in
# one and not the other is exactly the drift this file exists to catch.
set crossings [list \
    "tamer hold_time rx"  {*time_tamer:rx_tamer|hold_time[*]} \
                          {*time_tamer:rx_tamer|compare_time[*]} \
    "tamer hold_time tx"  {*time_tamer:tx_tamer|hold_time[*]} \
                          {*time_tamer:tx_tamer|compare_time[*]} \
    "handshake current rx" {*time_tamer:rx_tamer|handshake:U_current|source_holding[*]} \
                           {*time_tamer:rx_tamer|current_time_q[*]} \
    "handshake current tx" {*time_tamer:tx_tamer|handshake:U_current|source_holding[*]} \
                           {*time_tamer:tx_tamer|current_time_q[*]} \
    "handshake snap rx"    {*time_tamer:rx_tamer|handshake:U_snap|source_holding[*]} \
                           {*time_tamer:rx_tamer|dout[*]} \
    "handshake snap tx"    {*time_tamer:tx_tamer|handshake:U_snap|source_holding[*]} \
                           {*time_tamer:tx_tamer|dout[*]} \
    "handshake timestamp"  {*U_handshake_timestamp|source_holding[*]} \
                           {*fx3_gpif:*|current.tx_ts_plus32[*]} ]

foreach { label src_pat dst_pat } $crossings {
    set src [get_keepers -nowarn $src_pat]
    set dst [get_keepers -nowarn $dst_pat]
    set ns [get_collection_size $src]
    set nd [get_collection_size $dst]

    if { $ns == 0 } {
        fail "$label: source matches no keeper -- $src_pat"
        continue
    }
    if { $nd == 0 } {
        # The trap that caught fx3_timestamp: the name exists in the RTL as a
        # signal but never becomes a register, so it can never be an endpoint.
        fail "$label: destination matches no keeper (a wire, not a register?) -- $dst_pat"
        continue
    }

    ok "$label: $ns source, $nd destination keepers"
}

# ---------------------------------------------------------------------------
# Exceptions that were accepted and then discarded. report_exceptions is the
# authority here: an exception listed as invalid or matching nothing is the
# same defect as one never written.

set exc_file "qbind_exceptions.rpt"
report_exceptions -file $exc_file
if { [file exists $exc_file] } {
    set fh [open $exc_file r]
    set body [read $fh]
    close $fh
    set bad 0
    foreach line [split $body "\n"] {
        if { [regexp -nocase {invalid|ignored|no path} $line] } {
            incr bad
        }
    }
    if { $bad > 0 } {
        fail "report_exceptions: $bad line(s) marked invalid, ignored or pathless"
    } else {
        ok "report_exceptions: nothing invalid or ignored"
    }
}

# ---------------------------------------------------------------------------
# Unconstrained paths. A whole crossing can escape analysis without any single
# constraint being wrong, and the only sign is a non-zero count here.

set ucp_file "qbind_ucp.rpt"
report_ucp -file $ucp_file
if { [file exists $ucp_file] } {
    set fh [open $ucp_file r]
    set body [read $fh]
    close $fh
    set total 0
    foreach line [split $body "\n"] {
        if { [regexp {;\s*([0-9]+)\s*;\s*$} $line -> n] } {
            incr total $n
        }
    }
    if { $total > 0 } {
        fail "report_ucp: $total unconstrained path(s)"
    } else {
        ok "report_ucp: none"
    }
}

# ---------------------------------------------------------------------------
# Resource budget. M10K decides whether anything new fits in this design --
# 282 of 308 at last measurement, while ALMs sit near half. Warn before it
# becomes an emergency rather than after.

if { [catch {
    set m10k_used [get_fitter_resource_usage -resource "M10K blocks" -used]
    set m10k_avail [get_fitter_resource_usage -resource "M10K blocks" -available]
    set pct [expr {100.0 * $m10k_used / $m10k_avail}]
    if { $pct > 96.0 } {
        fail [format "M10K %d/%d (%.0f%%) -- over 96%%, needs an explicit waiver" \
                 $m10k_used $m10k_avail $pct]
    } elseif { $pct > 94.0 } {
        puts [format "warn  M10K %d/%d (%.0f%%) -- approaching the limit" \
                 $m10k_used $m10k_avail $pct]
    } else {
        ok [format "M10K %d/%d (%.0f%%)" $m10k_used $m10k_avail $pct]
    }
} err] } {
    puts "warn  M10K usage unavailable: $err"
}

project_close

puts ""
if { $failures > 0 } {
    puts "qbind: $failures failure(s)"
    qexit -error
}
puts "qbind: clean"
