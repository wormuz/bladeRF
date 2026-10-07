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

# Quartus Standard's report_max_skew reports every active set_max_skew
# assignment; it does not take -from/-to filters. Keep this alongside the
# per-channel structural inventory so a matching collection cannot masquerade
# as an enforced skew limit.
report_max_skew -npaths 4 -detail path_only

project_close
if {$failed} {
    error "one or more AD9361 RX up_xfer_cntrl pairs are absent or have unequal widths"
}
puts "ADC_XFER all four RX bundles paired: PASS"
