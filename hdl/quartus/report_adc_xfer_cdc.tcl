# Inspect the bundled-data control crossings in ADI's up_xfer_cntrl.
# Run from a completed hosted sweep Quartus work directory with:
#   quartus_sta -t /path/to/report_adc_xfer_cdc.tcl

project_open bladerf -revision sweep
create_timing_netlist
read_sdc
update_timing_netlist

set rx_channels {0 1 2 3}
set failed 0
foreach channel $rx_channels {
    set instance "*axi_ad9361_rx_channel:i_rx_channel_${channel}|up_adc_channel:i_up_adc_channel|up_xfer_cntrl:i_xfer_cntrl"
    set src [get_keepers -nowarn "${instance}|up_xfer_data\[*\]"]
    set dst [get_keepers -nowarn "${instance}|d_data_cntrl\[*\]"]
    set src_count [get_collection_size $src]
    set dst_count [get_collection_size $dst]
    puts "ADC_XFER channel=$channel source_bits=$src_count destination_bits=$dst_count"
    if {$src_count == 0 || $src_count != $dst_count} {
        set failed 1
    }
}

# The RX loop above covers four channel bundles. Inventory the four TX channel
# bundles plus the TX common control bundle independently, then gate the
# complete set of nine elaborated AD9361 control-bus pairs.
set remaining_pairs [list]
foreach channel {0 1 2 3} {
    set instance "*axi_ad9361_tx_channel:i_tx_channel_${channel}|up_dac_channel:i_up_dac_channel|up_xfer_cntrl:i_xfer_cntrl"
    lappend remaining_pairs "${instance}|up_xfer_data\[*\]" \
                            "${instance}|d_data_cntrl\[*\]"
}
lappend remaining_pairs \
    {*axi_ad9361_tx:i_tx|up_dac_common:i_up_dac_common|up_xfer_cntrl:i_xfer_cntrl|up_xfer_data[*]} \
    {*axi_ad9361_tx:i_tx|up_dac_common:i_up_dac_common|up_xfer_cntrl:i_xfer_cntrl|d_data_cntrl[*]}
set additional_ok 0
foreach {src_pat dst_pat} $remaining_pairs {
    set src [get_keepers -nowarn $src_pat]
    set dst [get_keepers -nowarn $dst_pat]
    set src_count [get_collection_size $src]
    set dst_count [get_collection_size $dst]
    puts "ADI_XFER additional source_bits=$src_count destination_bits=$dst_count"
    if {$src_count == 0 || $src_count != $dst_count} {
        set failed 1
    } else {
        incr additional_ok
    }
}
puts "ADI_XFER bundles paired: [expr {4 + $additional_ok}]/9"
if {$additional_ok != 5} { set failed 1 }

# Clock monitor count snapshots cross back through a three-stage toggle
# synchronizer. Confirm both per-direction bundles bind with equal widths.
set clock_mon_pairs [list \
    {*axi_ad9361_rx:i_rx|up_adc_common:i_up_adc_common|up_clock_mon:i_clock_mon|d_count_hold[*]} \
    {*axi_ad9361_rx:i_rx|up_adc_common:i_up_adc_common|up_clock_mon:i_clock_mon|up_d_count[*]} \
    {*axi_ad9361_tx:i_tx|up_dac_common:i_up_dac_common|up_clock_mon:i_clock_mon|d_count_hold[*]} \
    {*axi_ad9361_tx:i_tx|up_dac_common:i_up_dac_common|up_clock_mon:i_clock_mon|up_d_count[*]}]
set clock_mon_ok 0
foreach {src_pat dst_pat} $clock_mon_pairs {
    set src [get_keepers -nowarn $src_pat]
    set dst [get_keepers -nowarn $dst_pat]
    set ns [get_collection_size $src]
    set nd [get_collection_size $dst]
    if {$ns != 32 || $nd != 32} {
        puts "CLOCK_MON source_bits=$ns destination_bits=$nd expected=32"
        set failed 1
    } else {
        incr clock_mon_ok
    }
}
puts "CLOCK_MON bundles paired: $clock_mon_ok/2"
if {$clock_mon_ok != 2} { set failed 1 }

# up_xfer_status has four channel-level RX bundles and RX/TX common bundles.
# Its data snapshot is held until the synchronized return toggle, so each
# source/destination vector is a bundled-data crossing and must be bounded.
set status_pairs [list]
foreach channel {0 1 2 3} {
    set instance "*axi_ad9361_rx_channel:i_rx_channel_${channel}|up_adc_channel:i_up_adc_channel|up_xfer_status:i_xfer_status"
    lappend status_pairs "${instance}|d_xfer_data\[*\]" \
                         "${instance}|up_data_status\[*\]"
}
lappend status_pairs \
    {*axi_ad9361_rx:i_rx|up_adc_common:i_up_adc_common|up_xfer_status:i_xfer_status|d_xfer_data[*]} \
    {*axi_ad9361_rx:i_rx|up_adc_common:i_up_adc_common|up_xfer_status:i_xfer_status|up_data_status[*]} \
    {*axi_ad9361_tx:i_tx|up_dac_common:i_up_dac_common|up_xfer_status:i_xfer_status|d_xfer_data[*]} \
    {*axi_ad9361_tx:i_tx|up_dac_common:i_up_dac_common|up_xfer_status:i_xfer_status|up_data_status[*]}
set status_ok 0
foreach {src_pat dst_pat} $status_pairs {
    set src [get_keepers -nowarn $src_pat]
    set dst [get_keepers -nowarn $dst_pat]
    set ns [get_collection_size $src]
    set nd [get_collection_size $dst]
    puts "XFER_STATUS source_bits=$ns destination_bits=$nd"
    if {$ns == 0 || $ns != $nd} {
        set failed 1
    } else {
        incr status_ok
    }
}
puts "XFER_STATUS bundles paired: $status_ok/6"
if {$status_ok != 6} { set failed 1 }

# Quartus Standard's report_max_skew reports every active set_max_skew
# assignment; it does not take -from/-to filters. Keep this alongside the
# per-channel structural inventory so a matching collection cannot masquerade
# as an enforced skew limit.
report_max_skew -npaths 4 -detail path_only

project_close
if {$failed} {
    error "one or more elaborated AD9361 up_xfer_cntrl pairs are absent or have unequal widths"
}
puts "ADC_XFER all four RX bundles paired: PASS"
