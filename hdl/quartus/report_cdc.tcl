# Metastability / CDC sign-off for a fitted revision.
#
#   ~/soft/q25 quartus_sta -t report_cdc.tcl <project> <revision>
#
# Run from the work directory of a completed build. Writes three reports and
# prints a one-line verdict per check so the output is greppable from a script.
#
# What this answers that the normal timing summary does not:
#   * are the two-FF synchronizers actually recognised as synchronizer chains
#     (if not, the MTBF number is "Unknown" and metastability is unanalysed)
#   * what the design MTBF is, and which chain is the weakest
#   * whether any timing exception matches nothing (an exception that silently
#     applies to an empty collection is worse than no exception: the intent is
#     recorded but not enforced)

set project  [lindex $quartus(args) 0]
set revision [lindex $quartus(args) 1]
if {$project eq ""}  { set project  "bladerf" }
if {$revision eq ""} { set revision "hosted" }

project_open -revision $revision $project

# Metastability is a fast-corner concern: the shorter the data path, the less
# settling time between the two stages.
create_timing_netlist -model fast -temperature 0 -voltage 1100
read_sdc
update_timing_netlist

report_metastability -file metastability.rpt
report_exceptions    -file exceptions.rpt
report_ucp           -file unconstrained.rpt

# --- synchronizer recognition -------------------------------------------------
# get_registers on the known synchronizer instances tells us the cells exist;
# the report tells us whether Quartus treated them as chains.
set sync_regs [get_registers -nowarn {*synchronizer:*|reg*}]
puts "CDC_SYNC_REGISTERS [get_collection_size $sync_regs]"

set fh [open metastability.rpt r]
set body [read $fh]
close $fh

if {[regexp {Number of Synchronizer Chains Found\s*;\s*([0-9]+)} $body -> n]} {
    puts "CDC_CHAINS_FOUND $n"
} else {
    puts "CDC_CHAINS_FOUND unknown"
}

if {[regexp {Design MTBF\s*;\s*([^;]+);} $body -> mtbf]} {
    puts "CDC_DESIGN_MTBF [string trim $mtbf]"
} else {
    # "Unknown" here normally means the chains were not identified, or the
    # clocks were not declared asynchronous, so there is nothing to analyse.
    puts "CDC_DESIGN_MTBF not_reported"
}

# --- exceptions that match nothing -------------------------------------------
set fh [open exceptions.rpt r]
set exc [read $fh]
close $fh
set invalid 0
foreach line [split $exc "\n"] {
    set f [split $line ";"]
    if {[llength $f] > 2} {
        set status [string trim [lindex $f 1]]
        if {$status eq "Invalid"} { incr invalid }
    }
}
puts "CDC_INVALID_EXCEPTIONS $invalid"

project_close
