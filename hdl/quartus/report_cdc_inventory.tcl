# Inventory every clock-to-clock crossing that carries real paths.
#
#   ~/soft/q25 quartus_sta -t report_cdc_inventory.tcl
#
# Run this BEFORE and AFTER changing clock groups and diff the two. Every
# crossing that stops being timed must be a known CDC mechanism -- a two-flop
# synchronizer, a reset synchronizer, a dcfifo, a bundled-data handshake or
# ADI's up_xfer_cntrl. A direct register-to-register path between domains that
# is none of those is an RTL defect, and declaring the clocks asynchronous
# would hide it rather than fix it.
#
# Prints one line per crossing:
#   XFER <worst slack> <paths> <from clock> -> <to clock>

project_open -revision hosted bladerf
create_timing_netlist -model slow -temperature 85 -voltage 1100
read_sdc
update_timing_netlist

# Classifying a crossing by its DESTINATION name alone is not enough. The
# handshake block assigns dest_data <= source_holding combinationally, with no
# flop on the destination side -- that is what bundled-data means: the payload
# is held stable and only req/ack are synchronised. So STA traces the path back
# into the source clock domain and any consumer of the payload looks like an
# unprotected crossing, when in fact it is protected by the protocol plus the
# max_skew/net_delay bounds on source_holding.
#
# Judge such a path by its SOURCE: if it starts at a handshake's source_holding
# the crossing is accounted for. Only a path that neither starts at a known CDC
# structure nor ends at one is a genuine finding.
proc short {name} {
    # The generated PLL clock names are unreadable at full length.
    set n $name
    regsub {\|altera_pll_i\|general\[0\]\.gpll~PLL_OUTPUT_COUNTER\|divclk} $n "" n
    regsub {U_nios_system\|axi_ad9361_0\|i_dev_if\|i_rx\|i_altlvds_rx\|auto_generated\|} $n "LVDS_" n
    regsub {~PLL_OUTPUT_COUNTER\|divclk} $n "" n
    return $n
}

set worst [dict create]
set count [dict create]

foreach_in_collection p [get_timing_paths -setup -npaths 5000 -detail path_only] {
    set fc [get_clock_info -name [get_path_info $p -from_clock]]
    set tc [get_clock_info -name [get_path_info $p -to_clock]]
    if {$fc eq $tc} { continue }
    set k "[short $fc] -> [short $tc]"
    set s [get_path_info $p -slack]
    dict incr count $k
    if {![dict exists $worst $k] || $s < [dict get $worst $k]} {
        dict set worst $k $s
    }
}

set keys [lsort [dict keys $worst]]
foreach k $keys {
    puts [format "XFER %8.3f %6d  %s" [dict get $worst $k] [dict get $count $k] $k]
}
puts "XFER_TOTAL [llength $keys] distinct crossings"

project_close
