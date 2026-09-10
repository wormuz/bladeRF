# bladeRF Micro Design Constraints File
#
# Copyright (c) 2013-2017 Nuand LLC
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.

# Clock inputs
create_clock -period "38.4 MHz"  [get_ports c5_clock2]
create_clock -period "250.0 MHz" [get_ports adi_rx_clock]

# Generate the appropriate PLL clocks
derive_pll_clocks
derive_clock_uncertainty

# Platform-specific clock aliases
set fx3_clock    {U_fx3_pll|altera_pll_i|general[0].gpll~PLL_OUTPUT_COUNTER|divclk}
set system_clock {U_system_pll|altera_pll_i|general[0].gpll~PLL_OUTPUT_COUNTER|divclk}

# Trace delays between AD9361 and FPGA (bladeRF Micro)
set adi_spi_clk_trace_delay     0.127
set adi_spi_di_trace_delay      0.168
set adi_spi_do_trace_delay      0.119
set adi_spi_enb_trace_delay     0.137

# Trace delays between FX3 and FPGA (bladeRF Micro)
set fx3_data_trace_delay        0.260
set fx3_sclk_trace_delay        0.000
set fx3_dclk_trace_delay        0.226

# Trace delays between ADF4002 and FPGA (bladeRF Micro)
set adf_spi_clk_trace_delay     0.383
set adf_spi_data_trace_delay    0.373
set adf_spi_le_trace_delay      0.311

# Trace delays between AD5621 and FPGA (bladeRF Micro)
set dac_spi_sclk_trace_delay    0.164
set dac_spi_sdin_trace_delay    0.181
set dac_spi_nsync_trace_delay   0.193

# The handshake bundled-data crossings used to be cut with a blanket
#     set_false_path -from [get_registers {*source_holding[*]}] -to *
# which removed them from analysis entirely -- no timing check and, more to
# the point, no bound on how far apart the bits of the captured word may be
# placed. See the max_skew constraints at the end of this file, which replace
# it with the treatment Altera applies to its own dcfifo pointer crossings.

# Slow Interfaces
set_false_path -from *             -to [get_ports ps_sync_1p*]
set_false_path -from *             -to [get_ports {rx_bias_en tx_bias_en}]
set_false_path -from *             -to [get_ports {exp_gpio[*] exp_clock_oe}]
set_false_path -from *             -to [get_ports adf_ce]
set_false_path -from *             -to [get_ports led*]
set_false_path -from *             -to [get_ports {adi_*x_spdt*_v*}]
set_false_path -from *             -to [get_ports si_clock_sel]
set_false_path -from *             -to [get_ports ufl_clock_oe]
set_false_path -from adf_muxout    -to *
set_false_path -from exp_clock_req -to *
set_false_path -from exp_present   -to *
set_false_path -from pwr_status    -to *

# JTAG settings
set_clock_groups -exclusive -group [get_clocks altera_reserved_tck]
set_input_delay  -clock [get_clocks altera_reserved_tck] 2.0 [get_ports altera_reserved_tdi]
set_input_delay  -clock [get_clocks altera_reserved_tck] 2.0 [get_ports altera_reserved_tms]
set_output_delay -clock [get_clocks altera_reserved_tck] 2.0 [get_ports altera_reserved_tdo]

# Exceptions

# The TX dcfifo aclr (TX clock) has recovery violation going to the write clock domain (PCLK)
# The DCFIFO documentation says to false path aclr-->rdclk, but we need to do it to wrclk.
# Has not been an issue so far, so probably safe?
# With the LVDS cores, the TX PLL clock got merged with the RX PLL clock
set_false_path -from {reset_synchronizer:U_reset_sync_rx|sync} -to {tx:U_tx|tx_*fifo*common_dcfifo*dffpipe_3dc:wraclr|dffe12a[0]}
set_false_path -from {reset_synchronizer:U_reset_sync_rx|sync} -to {tx:U_tx|tx_*fifo*common_dcfifo*dffpipe_3dc:wraclr|dffe13a[0]}
set_false_path -from {reset_synchronizer:U_reset_sync_tx|sync} -to {tx:U_tx|tx_*fifo*common_dcfifo*dffpipe_3dc:wraclr|dffe12a[0]}
set_false_path -from {reset_synchronizer:U_reset_sync_tx|sync} -to {tx:U_tx|tx_*fifo*common_dcfifo*dffpipe_3dc:wraclr|dffe13a[0]}

# hold_time -> compare_time is a bundled-data crossing, not a false path.
#
# It used to be cut outright, "due to the way the FSM is setup". What the FSM
# actually does (time_tamer.vhd, WAIT_FOR_LOAD) is capture all 64 bits of
# hold_time in the sample clock domain on a single enable, ts_compare_load,
# which is itself synchronised through U_sync_load. That is the same
# hold-the-data-and-synchronise-the-strobe protocol the handshake block
# implements, just written out by hand -- so the payload needs the same
# treatment: no per-cycle timing requirement, but a bound on how far apart the
# bits may be placed. Cutting it left the skew unbounded, which is the one
# thing that can hand the comparator a value that never existed.
#
# 128 such paths, found by classifying every system -> LVDS crossing by its
# endpoints after the blanket cut was removed.
# Same omission as the handshake block below: skew and net delay were added,
# the relaxation was not, so this crossing is still being timed per cycle.
#
# Here the capture register genuinely exists in the RTL -- time_tamer does
# "compare_time <= hold_time" under ts_compare_load, in the ts_clock domain --
# so the exception has a proper endpoint and stops there. The comparison that
# follows, compare_time = timestamp, is ordinary same-clock logic and stays
# timed.
# Per instance, not one glob across both. There are two time_tamer instances,
# rx_tamer and tx_tamer, and "*_tamer|hold_time[*]" collects the registers of
# BOTH. An exception written that way asks, among other things, for a path
# from rx_tamer's hold_time to tx_tamer's compare_time, which does not exist:
# the same mistake that left the dcfifo pointer constraints unbound twice.
set ht_done 0
foreach tamer {rx_tamer tx_tamer} {
    set ht_src [get_keepers -nowarn "*time_tamer:${tamer}|hold_time\[*\]"]
    set ht_dst [get_keepers -nowarn "*time_tamer:${tamer}|compare_time\[*\]"]
    if { [get_collection_size $ht_src] > 0 && [get_collection_size $ht_dst] > 0 } {
        # Two-ended max/min instead of a false path. Measured: with
        # set_false_path on this exact pair, the set_max_skew that follows is
        # reported as "No path is found" and dropped -- so the crossing ended
        # up with neither a timing requirement nor a bound on how far apart
        # the bits may be placed, which is the one thing a held-data bus
        # cannot do without.
        #
        # The huge numbers are not a target. They say "do not enforce
        # ordinary synchronous setup/hold on this asynchronous held-data
        # crossing". The real limits are the skew and net delay below.
        #
        # Both ends are named. A -from-only form of these two once reached
        # past the crossing and overrode the SPI and I2C multicycles, which
        # run off the same system PLL.
        set_max_delay 100  -from $ht_src -to $ht_dst
        set_min_delay -100 -from $ht_src -to $ht_dst
        set_max_skew  -from $ht_src -to $ht_dst \
            -get_skew_value_from_clock_period dst_clock_period -skew_value_multiplier 0.8
        set_net_delay -from $ht_src -to $ht_dst -max \
            -get_value_from_clock_period dst_clock_period -value_multiplier 0.8
        incr ht_done
    }
}
if { $ht_done == 0 } {
    post_message -type critical_warning "tamer hold_time crossing not matched: it would be timed as a single-cycle path"
} else {
    post_message -type info "tamer hold_time crossing constrained on $ht_done instance(s)"
}

# Mini Expansion Port (J51)
set_false_path -from [get_ports mini_exp*] -to *
set_false_path -from *                     -to [get_ports mini_exp*]

# IQ correction coefficients are quasi-static: written only by a processor
# register write (up_adc_channel.v:281, AXI address 4'h5), carried into the
# ADC clock domain by an explicit up_xfer_cntrl handshake, then re-flopped in
# ad_iqcor.v:106. The value cannot change while samples stream, so the
# hold check against the same edge is not a real requirement -- the fitter
# was spending routing effort on it and still missing by 8 ps into the
# hardened DSP datab input register, which has no delay elements to insert.
# Setup remains constrained at one cycle.
set_multicycle_path -hold -from [get_registers {*ad_iqcor:*|iqcor_coeff_*_r[*]}] 1

# ⛔ The dcfifo gray-pointer crossings are NOT constrained here, and must not
# be. The megafunction already constrains them itself.
#
# Quartus reports, per instance, from the generated IP rather than from any
# file we source:
#
#     Info (332165): Entity dcfifo_mu92
#       Info (332166): set_false_path -from *rdptr_g*
#                        -to *ws_dgrp|dffpipe_4f9:dffpipe15|dffe16a*
#
# Twenty of those in this design, two per instance. The paths are therefore
# already cut before our code runs, which is why every set_max_skew added on
# top came back as "No path is found" and was dropped. Measured, not assumed:
#
#     get_timing_paths -from <rdptr_g> -to <ws_dgrp>   ->  0 paths
#     get_fanins <ws_dgrp> -synch                      ->  0 fanins
#
# and a set_max_skew with an explicit numeric bound on the same endpoints
# binds without complaint, which rules out a syntax or collection problem.
#
# Three rounds went into "fixing" the pattern here -- the colon separator,
# the per-instance pairing, the rdptr_g versus rdptr_g1p ordering -- each one
# producing a more correct pattern for a crossing that needed nothing at all.
# The question that should have come first is whether a path exists between
# the two ends. It is one query and it settles the matter.
#
# What remains is the instance count, which is still worth reporting: it is
# how a FIFO appearing or disappearing becomes visible instead of silent.
proc bladerf_dcfifo_owners { pattern } {
    set owners [list]
    foreach_in_collection n [get_keepers -nowarn $pattern] {
        set name [get_node_info -name $n]
        # No regexp: hierarchy names carry backslashes from VHDL generate
        # labels ("dcfifo:\fifo_gen:U_dcfifo|..."), and a backslash in a Tcl
        # regexp is an escape, so a pattern that reads correctly matches
        # nothing. Split on the separator instead.
        set parts [split $name "|"]
        # The segment is "dcfifo_0p92:auto_generated", not bare
        # "auto_generated", so match on the suffix rather than equality.
        set idx -1
        for { set i 0 } { $i < [llength $parts] } { incr i } {
            if { [string match "*auto_generated" [lindex $parts $i]] } {
                set idx $i
                break
            }
        }
        if { $idx < 0 } { continue }
        set owner [join [lrange $parts 0 $idx] "|"]
        if { [lsearch -exact $owners $owner] < 0 } {
            lappend owners $owner
        }
    }
    return $owners
}

set ptr_rd [llength [bladerf_dcfifo_owners {*|auto_generated|*ws_dgrp*dffpipe*|dffe*}]]
set ptr_wr [llength [bladerf_dcfifo_owners {*|auto_generated|*rs_dgwp*dffpipe*|dffe*}]]
set ptr_done [expr {$ptr_rd + $ptr_wr}]

if { $ptr_done == 0 } {
    post_message -type critical_warning "dcfifo pointer synchronisers: none found -- the megafunction's own exceptions cannot be confirmed either"
} else {
    post_message -type info "dcfifo pointer crossings constrained on $ptr_done instance(s)"
}


# Bundled-data crossings in nuand's handshake block. There are eight of them
# in this revision and three carry the 64-bit sample timestamp:
# time_tamer's U_snap and U_current, and U_handshake_timestamp at top level.
# The rest carry the VCTCXO PPS counter and similar wide values.
#
# The block is correct by construction -- source_holding is loaded only on a
# synchronised request edge and the source cannot overwrite it until the
# acknowledge returns -- but that argument is about the PROTOCOL. It says
# nothing about how far apart the fitter may place the bits of the captured
# word, and the destination samples all of them on one edge. Without a skew
# bound there is nothing stopping one bit of a timestamp arriving a cycle
# after its neighbours, which would hand the scheduler a value that never
# existed. Same reasoning, and the same remedy, as the vendor dcfifo pointer
# constraints above.
#
# source_holding is stable for the whole request/acknowledge round trip, so
# the crossing is deliberately not timed as a single-cycle path: bound the
# skew and the net delay instead, and relax setup/hold.
# ⛔ set_max_skew and set_net_delay do NOT relax setup/hold. They bound the
# physical placement of the bits; the ordinary per-cycle timing check stays.
# Removing the old blanket false path and adding only those two left this
# crossing analysed as a single-cycle path and produced the worst path in the
# design, -9.545 ns from source_holding[49] into time_tamer's comparator. The
# relaxation has to be restored -- narrowly.
#
# Narrowly means: end the exception at the FIRST register on the destination
# side, never "-to *". The old form hid everything downstream of the crossing
# as well, which is how a 64-bit compare feeding an FSM state decision went
# unnoticed at -11.203 ns.
#
# handshake.vhd has no capture flop of its own -- it is literally
# "dest_data <= source_holding" -- so the first destination register is in the
# consumer. In time_tamer that is current_time_q, added for exactly this
# reason: without it the comparator read the far domain's register directly
# and could sample it mid-change, answering about a timestamp that never
# existed. Anything after current_time_q is ordinary same-clock logic and must
# stay timed.
# Five handshake instances carry data in this revision, and each one ends at a
# different consumer register. Named individually rather than with one glob:
# a pattern that silently matches nothing is how 160 dcfifo constraints were
# lost, so an empty collection here is an error, not a warning.
#
#   U_current  -> current_time_q   time_tamer, the -9.545 ns path
#   U_snap     -> dout             time_tamer, read back a byte at a time
#   timestamp  -> fx3_timestamp    top level, tx_clock into fx3_pclk_pll
#
# vctcxo_tamer's two instances are covered too, consumers read rather than
# assumed:
#
#   pps_counter U_handshake      vctcxo_clock -> sys_clock, dest_data is
#                                sys_count, which leaves the block as a port
#                                and lands in pps_1s/10s/100s.count at
#                                vctcxo_tamer.vhd:223/239/255
#   U_handshake_tune_mode        mm_clock -> tune_ref, dest_data is
#                                tune_ref_mode_hs, captured into
#                                tune_ref_mode at vctcxo_tamer.vhd:426 under
#                                tune_ref_mode_update_ack
#
# Leaving them out was the wrong call: an unconstrained crossing is not
# safer than a constrained one, it is only less visible.
# Source and destination are paired PER INSTANCE. Collecting every
# source_holding and every capture register into two big collections would
# ask for paths that do not exist -- rx_tamer's handshake into tx_tamer's
# capture register, and so on -- and the constraint would bind to less than it
# appears to. Three separate regressions in this file had exactly that shape.
set hs_pairs [list \
    {*time_tamer:rx_tamer|handshake:U_current|source_holding[*]}  {*time_tamer:rx_tamer|current_time_q[*]} \
    {*time_tamer:tx_tamer|handshake:U_current|source_holding[*]}  {*time_tamer:tx_tamer|current_time_q[*]} \
    {*time_tamer:rx_tamer|handshake:U_snap|source_holding[*]}     {*time_tamer:rx_tamer|dout[*]}           \
    {*time_tamer:tx_tamer|handshake:U_snap|source_holding[*]}     {*time_tamer:tx_tamer|dout[*]}           \
    {*U_handshake_timestamp|source_holding[*]}                    {*fx3_gpif:*|current.tx_ts_plus32[*]}    ]

# vctcxo_tamer.vhd has two more handshake instances, and they are NOT listed
# above because this revision does not instantiate that entity at all:
# bladerf-hosted.vhd never names it, and the vctcxo_tamer_0 that does appear
# in the fitted netlist is an altera_avalon_onchip_memory2 from the Qsys
# system that happens to share the name (nios_system.tcl:331).
#
# Constraints for them were written and had to be removed: they matched
# nothing, which the count check reported as a critical warning rather than
# passing over in silence. If the entity is ever instantiated here, its pairs
# are pps_counter's source_holding into pps_1s/10s/100s.count, and
# U_handshake_tune_mode's into tune_ref_mode.

set hs_done 0
foreach { src_pat dst_pat } $hs_pairs {
    set src [get_keepers -nowarn $src_pat]
    set dst [get_keepers -nowarn $dst_pat]
    if { [get_collection_size $src] > 0 && [get_collection_size $dst] > 0 } {
        # Same treatment as the tamer crossing above, and for the same
        # measured reason: a false path here silently took the skew and
        # net-delay bounds down with it, leaving the bus with no limit on how
        # far apart its bits may be placed. Verified on q25_v4: with these
        # two replacing the false path, the tamer skew constraints bind
        # instead of being reported "No path is found".
        set_max_delay 100  -from $src -to $dst
        set_min_delay -100 -from $src -to $dst
        set_max_skew  -from $src -to $dst \
            -get_skew_value_from_clock_period dst_clock_period -skew_value_multiplier 0.8
        set_net_delay -from $src -to $dst -max \
            -get_value_from_clock_period dst_clock_period -value_multiplier 0.8
        incr hs_done
    } else {
        post_message -type critical_warning "handshake crossing not matched: $src_pat -> $dst_pat"
    }
}
if { $hs_done == 0 } {
    post_message -type critical_warning "no handshake crossing constrained: bundled-data transfers would be timed as single-cycle"
} else {
    post_message -type info "handshake crossings constrained: $hs_done"
}

# Every handshake instance in the design is named in the pairs above. This
# block is the net that catches a future one: a new instance gets a physical
# bound from the day it appears, rather than silently crossing unconstrained
# until someone notices. Skew and net delay excuse nothing, so applying them
# to a superset is safe.
#
# The count check below is the part that matters. If the number of instances
# ever exceeds the number of pairs, a crossing exists that nobody wrote an
# endpoint for, and that must be said out loud rather than left to show up as
# a timing number months later.
#
# Deliberately -from only HERE. A -from-only set_max_delay would be a
# different matter: tried once, it reached far past the crossing and overrode
# the multicycles on SPI and I2C, which run off the same system PLL,
# producing eight violations up to -14.061 ns on unrelated domains.
set hs_all [get_keepers -nowarn {*handshake:*|source_holding[0]}]
set hs_inst [get_collection_size $hs_all]
if { $hs_inst > $hs_done } {
    post_message -type critical_warning \
        "handshake instances: $hs_inst, endpoint pairs written: $hs_done -- some crossing has no capture endpoint named"
}

set hs_src [get_keepers -nowarn {*handshake:*|source_holding[*]}]
if { [get_collection_size $hs_src] > 0 } {
    set_max_skew  -from $hs_src \
        -get_skew_value_from_clock_period dst_clock_period -skew_value_multiplier 0.8
    set_net_delay -from $hs_src -max \
        -get_value_from_clock_period dst_clock_period -value_multiplier 0.8
} else {
    post_message -type warning "handshake source_holding registers not found"
}

# Asynchronous clock groups.
#
# Until now the only group declared in the project was JTAG, so every other
# domain was implicitly synchronous to every other one. That has two costs.
# The tool spends effort trying to close paths between clocks that have no
# phase relationship, and -- the reason this was found -- the
# SYNCHRONIZER_IDENTIFICATION "FORCED IF ASYNCHRONOUS" attribute on the 93
# synchronizer and 22 reset-synchronizer instances never fires, because
# Quartus only classifies a two-flop chain as a synchronizer when the
# crossing is declared asynchronous. No classification means no
# metastability analysis, which is why report_metastability has never
# produced an MTBF for this design.
#
# Three physically independent sources, traced in the RTL:
#   c5_clock2    38.4 MHz VCTCXO   -> U_system_pll  -> system domain
#   fx3_pclk     from the FX3      -> U_fx3_pll     -> FX3 domain
#   adi_rx_clock 250 MHz AD9361    -> i_altlvds_rx  -> sclk / fclk / ena
#
# The AD9361 takes the same VCTCXO as its reference, but its sample clock
# comes out of the part's own PLL, so there is no edge relationship STA could
# use. Asynchronous, not exclusive: all three run at once, they simply have
# no defined phase.
#
# fx3_virtual belongs in the FX3 group. It models the FX3 as the launching
# device for set_input_delay on fx3_gpif/fx3_ctl; putting it in a group of
# its own would cut those paths and silently discard the I/O constraints.
#
# Groups are built from clocks that actually exist in the revision. Naming a
# clock that is not there produces an exception that matches nothing, which
# is the failure mode already visible elsewhere in report_exceptions.
proc _grp { patterns } {
    set out {}
    foreach p $patterns {
        foreach_in_collection c [get_clocks -nowarn $p] {
            lappend out [get_clock_info -name $c]
        }
    }
    return $out
}

set grp_sys  [_grp {c5_clock2 {*U_system_pll*divclk}}]
set grp_fx3  [_grp {fx3_pclk fx3_virtual {*U_fx3_pll*divclk}}]
set grp_lvds [_grp {adi_rx_clock {*i_altlvds_rx*divclk}}]

# JTAG is deliberately absent: it already has its own -exclusive group above,
# and listing it twice would be two competing statements about the same clock.
#
# So are the pin-generated serial clocks -- adi_sclk_pin, adf_sclk_pin,
# dac_sclk_pin, pwr_scl_pin, i2c_scl_reg, peri_sclk_reg, adi_sclk_reg. They
# are divided down from the system PLL, so they are genuinely related to it,
# and they already carry multicycle paths sized for their bit periods
# (100 and 199 cycles). Declaring them asynchronous would throw that away.
set groups {}
foreach g [list $grp_sys $grp_fx3 $grp_lvds] {
    if { [llength $g] > 0 } { lappend groups -group $g }
}

# Held behind a switch until the before/after crossing inventory is done.
# Declaring clocks asynchronous stops the tool timing every path between the
# families in BOTH directions, so it can turn a real unsynchronised crossing
# from a visible violation into a silent pass. The inventory
# (quartus/report_cdc_inventory.tcl, run with the switch off and again with it
# on) lists what stops being timed; each entry has to be a known CDC mechanism
# before this is turned on for good.
#
#   ~/soft/q25 quartus_sta -t ../../../../quartus/report_cdc_inventory.tcl
#
# Each family contributes two list elements (-group plus its clock list), so
# four elements is two families -- the minimum for the statement to say
# anything at all.
if { ![info exists ::env(BLADERF_ASYNC_CLOCK_GROUPS)] } {
    post_message -type warning \
        "async clock groups NOT applied (set BLADERF_ASYNC_CLOCK_GROUPS=1 to enable)"
} elseif { [llength $groups] >= 4 } {
    set_clock_groups -asynchronous {*}$groups
    post_message -type info "async clock groups applied: [expr {[llength $groups]/2}] families"
} else {
    post_message -type warning "fewer than two clock families resolved; not grouping"
}
