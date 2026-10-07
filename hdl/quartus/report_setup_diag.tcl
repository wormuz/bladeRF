# Diagnose worst setup paths for the selected Quartus revision.
#
#   quartus_sta --64bit -t report_setup_diag.tcl -projname bladerf -rev sweep
#
# This report is intentionally path-level: aggregate slack changes alone do
# not identify whether a new CDC register, memory placement, or an unrelated
# high-fanout system path caused a regression.

set project_name "bladerf"
set revision "sweep"
foreach {key value} $quartus(args) {
    if {$key eq "-projname"} { set project_name $value }
    if {$key eq "-rev"} { set revision $value }
}

project_open -revision $revision $project_name
create_timing_netlist -model slow -temperature 85 -voltage 1100
read_sdc
update_timing_netlist

set index 0
foreach_in_collection path [get_timing_paths -setup -npaths 20 -detail path_only] {
    incr index
    set from [get_node_info -name [get_path_info $path -from]]
    set to [get_node_info -name [get_path_info $path -to]]
    set from_clock [get_clock_info -name [get_path_info $path -from_clock]]
    set to_clock [get_clock_info -name [get_path_info $path -to_clock]]
    puts [format "SETUP %2d slack=%+8.3f skew=%+8.3f %s -> %s" \
        $index [get_path_info $path -slack] \
        [get_path_info $path -clock_skew] $from_clock $to_clock]
    puts "        from $from"
    puts "        to   $to"
}

project_close
