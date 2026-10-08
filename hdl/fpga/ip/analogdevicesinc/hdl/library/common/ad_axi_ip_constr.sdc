
set_false_path  -from [get_registers *up_xfer_cntrl:i_xfer_cntrl|d_xfer_toggle*]      -to [get_registers *up_xfer_cntrl:i_xfer_cntrl|up_xfer_state_m1*]
set_false_path  -from [get_registers *up_xfer_cntrl:i_xfer_cntrl|up_xfer_toggle*]     -to [get_registers *up_xfer_cntrl:i_xfer_cntrl|d_xfer_toggle_m1*]
# Preserve the historical cut on ADI's other bundled up_xfer_cntrl buses,
# but leave the four AD9361 RX channel data bundles available to the board
# SDC's explicit max/min-delay, max-skew and net-delay bounds. A false path
# wins over set_max_skew in Quartus and causes those physical bounds to be
# discarded as "No path is found".
# All nine elaborated AD9361 up_xfer_cntrl payload bundles (four RX channels,
# four TX channels, and TX common control) are held-data toggle transfers and are
# explicitly bounded by the bladeRF platform SDC. Retain the vendor false path
# on any other ADI up_xfer_cntrl instance.
set blade_xfer_cntrl_dst [get_registers { \
    *axi_ad9361_rx_channel:i_rx_channel_0|up_adc_channel:i_up_adc_channel|up_xfer_cntrl:i_xfer_cntrl|d_data_cntrl[*] \
    *axi_ad9361_rx_channel:i_rx_channel_1|up_adc_channel:i_up_adc_channel|up_xfer_cntrl:i_xfer_cntrl|d_data_cntrl[*] \
    *axi_ad9361_rx_channel:i_rx_channel_2|up_adc_channel:i_up_adc_channel|up_xfer_cntrl:i_xfer_cntrl|d_data_cntrl[*] \
    *axi_ad9361_rx_channel:i_rx_channel_3|up_adc_channel:i_up_adc_channel|up_xfer_cntrl:i_xfer_cntrl|d_data_cntrl[*] \
    *axi_ad9361_tx_channel:i_tx_channel_0|up_dac_channel:i_up_dac_channel|up_xfer_cntrl:i_xfer_cntrl|d_data_cntrl[*] \
    *axi_ad9361_tx_channel:i_tx_channel_1|up_dac_channel:i_up_dac_channel|up_xfer_cntrl:i_xfer_cntrl|d_data_cntrl[*] \
    *axi_ad9361_tx_channel:i_tx_channel_2|up_dac_channel:i_up_dac_channel|up_xfer_cntrl:i_xfer_cntrl|d_data_cntrl[*] \
    *axi_ad9361_tx_channel:i_tx_channel_3|up_dac_channel:i_up_dac_channel|up_xfer_cntrl:i_xfer_cntrl|d_data_cntrl[*] \
    *axi_ad9361_tx:i_tx|up_dac_common:i_up_dac_common|up_xfer_cntrl:i_xfer_cntrl|d_data_cntrl[*]}]
set all_xfer_cntrl_dst [get_registers *up_xfer_cntrl:i_xfer_cntrl|d_data_cntrl*]
set other_xfer_cntrl_dst [remove_from_collection $all_xfer_cntrl_dst $blade_xfer_cntrl_dst]
if {[get_collection_size $other_xfer_cntrl_dst] > 0} {
    set_false_path -from [get_registers *up_xfer_cntrl:i_xfer_cntrl|up_xfer_data*] -to $other_xfer_cntrl_dst
}
set_false_path  -from [get_registers *up_xfer_status:i_xfer_status|up_xfer_toggle*]   -to [get_registers *up_xfer_status:i_xfer_status|d_xfer_state_m1*]
set_false_path  -from [get_registers *up_xfer_status:i_xfer_status|d_xfer_toggle*]    -to [get_registers *up_xfer_status:i_xfer_status|up_xfer_toggle_m1*]
# Keep status data paths visible to the board SDC for the AD9361 instance.
# Its six up_xfer_status data bundles use a held-data toggle protocol and are
# explicitly bounded there. Preserve the historical false path for other
# ADI IP instances in the same Qsys design.
set blade_status_data_dst [get_registers { \
    *axi_ad9361_rx_channel:i_rx_channel_0|up_adc_channel:i_up_adc_channel|up_xfer_status:i_xfer_status|up_data_status[*] \
    *axi_ad9361_rx_channel:i_rx_channel_1|up_adc_channel:i_up_adc_channel|up_xfer_status:i_xfer_status|up_data_status[*] \
    *axi_ad9361_rx_channel:i_rx_channel_2|up_adc_channel:i_up_adc_channel|up_xfer_status:i_xfer_status|up_data_status[*] \
    *axi_ad9361_rx_channel:i_rx_channel_3|up_adc_channel:i_up_adc_channel|up_xfer_status:i_xfer_status|up_data_status[*] \
    *axi_ad9361_rx:i_rx|up_adc_common:i_up_adc_common|up_xfer_status:i_xfer_status|up_data_status[*] \
    *axi_ad9361_tx:i_tx|up_dac_common:i_up_dac_common|up_xfer_status:i_xfer_status|up_data_status[*]}]
set all_status_data_dst [get_registers *up_xfer_status:i_xfer_status|up_data_status*]
set other_status_data_dst [remove_from_collection $all_status_data_dst $blade_status_data_dst]
if {[get_collection_size $other_status_data_dst] > 0} {
    set_false_path -from [get_registers *up_xfer_status:i_xfer_status|d_xfer_data*] -to $other_status_data_dst
}
set_false_path  -from [get_registers *up_clock_mon:i_clock_mon|d_count_toggle*]       -to [get_registers *up_clock_mon:i_clock_mon|up_count_toggle_m1*]
# As above, leave the AD9361 RX/TX clock monitor data snapshots available for
# board-specific physical bounds while retaining cuts on other clock monitors.
set blade_clock_mon_dst [get_registers { \
    *axi_ad9361_rx:i_rx|up_adc_common:i_up_adc_common|up_clock_mon:i_clock_mon|up_d_count[*] \
    *axi_ad9361_tx:i_tx|up_dac_common:i_up_dac_common|up_clock_mon:i_clock_mon|up_d_count[*]}]
set all_clock_mon_dst [get_registers *up_clock_mon:i_clock_mon|up_d_count*]
set other_clock_mon_dst [remove_from_collection $all_clock_mon_dst $blade_clock_mon_dst]
if {[get_collection_size $other_clock_mon_dst] > 0} {
    set_false_path -from [get_registers *up_clock_mon:i_clock_mon|d_count_hold*] -to $other_clock_mon_dst
}
set_false_path  -from [get_registers *up_clock_mon:i_clock_mon|up_count_toggle*]      -to [get_registers *up_clock_mon:i_clock_mon|d_count_toggle_m1*]
set_false_path  -from [get_registers *up_core_preset*]                                -to [get_registers *ad_rst:i_core_rst_reg|ad_rst_sync_m1*]
