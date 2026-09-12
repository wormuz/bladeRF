# Diagnose the worst hold paths of a fitted revision.
#
#   ~/soft/q25 quartus_sta -t report_hold_diag.tcl
#
# Hold is a fast-corner check: the shorter the data path, the less time the
# destination has to see a stable value. Run this before reaching for any
# fitter option, because the right response depends entirely on what the path
# is:
#
#   plain FF -> FF, same clock     a real short path; fitter hold optimisation
#                                  or placement, not RTL
#   FF -> M10K / DSP input reg     hard-block entry, no delay elements to
#                                  insert; needs placement, not a constraint
#   synchronizer stage 1 -> 2      check recognition and MTBF first; a broad
#                                  multicycle here would defeat the settling
#                                  time the chain exists to provide
#   dcfifo gray pointer            the vendor constraint IS the intended
#                                  check; do not "fix" by deleting it
#   reset removal                  analysed separately from hold; look at the
#                                  recovery/removal numbers instead
#
# Prints, per path: slack, the multicycle values in force, clock skew, both
# endpoints and a guess at the class.

project_open -revision hosted bladerf
create_timing_netlist -model fast -temperature 0 -voltage 1100
read_sdc
update_timing_netlist

proc classify {from to} {
    foreach {pat name} {
        dcfifo        dcfifo
        altsyncram    memory
        MULT_MACRO    dsp
        lpm_mult      dsp
        synchronizer  synchronizer
        handshake     handshake
        up_xfer       up_xfer
        reserved_tck  jtag
    } {
        if {[string match "*$pat*" $from] || [string match "*$pat*" $to]} { return $name }
    }
    return "plain"
}

set n 0
foreach_in_collection p [get_timing_paths -hold -npaths 20 -detail path_only] {
    incr n
    set from [get_node_info -name [get_path_info $p -from]]
    set to   [get_node_info -name [get_path_info $p -to]]
    puts [format "HOLD %2d %+7.3f  mcp %s/%s  skew %+6.3f  %s" \
        $n \
        [get_path_info $p -slack] \
        [get_path_info $p -hold_start_multicycle] \
        [get_path_info $p -hold_end_multicycle] \
        [get_path_info $p -clock_skew] \
        [classify $from $to]]
    puts "        from $from"
    puts "        to   $to"
}

project_close
