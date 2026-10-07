
set_false_path  -from [get_registers *up_xfer_cntrl:i_xfer_cntrl|d_xfer_toggle*]      -to [get_registers *up_xfer_cntrl:i_xfer_cntrl|up_xfer_state_m1*]
set_false_path  -from [get_registers *up_xfer_cntrl:i_xfer_cntrl|up_xfer_toggle*]     -to [get_registers *up_xfer_cntrl:i_xfer_cntrl|d_xfer_toggle_m1*]
# Preserve the historical cut on ADI's other bundled up_xfer_cntrl buses,
# but leave the four AD9361 RX channel data bundles available to the board
# SDC's explicit max/min-delay, max-skew and net-delay bounds. A false path
# wins over set_max_skew in Quartus and causes those physical bounds to be
# discarded as "No path is found".
set adc_rx_d_data [get_registers {*axi_ad9361_rx_channel:i_rx_channel_0|up_adc_channel:i_up_adc_channel|up_xfer_cntrl:i_xfer_cntrl|d_data_cntrl[*] *axi_ad9361_rx_channel:i_rx_channel_1|up_adc_channel:i_up_adc_channel|up_xfer_cntrl:i_xfer_cntrl|d_data_cntrl[*] *axi_ad9361_rx_channel:i_rx_channel_2|up_adc_channel:i_up_adc_channel|up_xfer_cntrl:i_xfer_cntrl|d_data_cntrl[*] *axi_ad9361_rx_channel:i_rx_channel_3|up_adc_channel:i_up_adc_channel|up_xfer_cntrl:i_xfer_cntrl|d_data_cntrl[*]}]
set adc_other_d_data [remove_from_collection [get_registers *up_xfer_cntrl:i_xfer_cntrl|d_data_cntrl*] $adc_rx_d_data]
set_false_path -from [get_registers *up_xfer_cntrl:i_xfer_cntrl|up_xfer_data*] -to $adc_other_d_data
set_false_path  -from [get_registers *up_xfer_status:i_xfer_status|up_xfer_toggle*]   -to [get_registers *up_xfer_status:i_xfer_status|d_xfer_state_m1*]
set_false_path  -from [get_registers *up_xfer_status:i_xfer_status|d_xfer_toggle*]    -to [get_registers *up_xfer_status:i_xfer_status|up_xfer_toggle_m1*]
set_false_path  -from [get_registers *up_xfer_status:i_xfer_status|d_xfer_data*]      -to [get_registers *up_xfer_status:i_xfer_status|up_data_status*] 
set_false_path  -from [get_registers *up_clock_mon:i_clock_mon|d_count_toggle*]       -to [get_registers *up_clock_mon:i_clock_mon|up_count_toggle_m1*]
set_false_path  -from [get_registers *up_clock_mon:i_clock_mon|d_count_hold*]         -to [get_registers *up_clock_mon:i_clock_mon|up_d_count*]
set_false_path  -from [get_registers *up_clock_mon:i_clock_mon|up_count_toggle*]      -to [get_registers *up_clock_mon:i_clock_mon|d_count_toggle_m1*]
set_false_path  -from [get_registers *up_core_preset*]                                -to [get_registers *ad_rst:i_core_rst_reg|ad_rst_sync_m1*]
