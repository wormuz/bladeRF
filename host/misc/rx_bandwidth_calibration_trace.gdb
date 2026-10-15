set debuginfod enabled off
set breakpoint pending on

break /home/bonho/projects/bladerf/host/build/common/thirdparty/ad936x/ad9361.c:3941 if real_rx_bandwidth == 300000
commands
  silent
  printf "AFTER_RX_BBF ret=%d rx_bw=%u tx_bw=%u bbpll=%u\n", ret, real_rx_bandwidth, real_tx_bandwidth, bbpll_freq
  continue
end

break /home/bonho/projects/bladerf/host/build/common/thirdparty/ad936x/ad9361.c:3947 if real_rx_bandwidth == 300000
commands
  silent
  printf "AFTER_TX_BBF ret=%d rx_bw=%u tx_bw=%u bbpll=%u\n", ret, real_rx_bandwidth, real_tx_bandwidth, bbpll_freq
  continue
end

break /home/bonho/projects/bladerf/host/build/common/thirdparty/ad936x/ad9361.c:3951 if real_rx_bandwidth == 300000
commands
  silent
  printf "AFTER_RX_TIA ret=%d rx_bw=%u tx_bw=%u\n", ret, real_rx_bandwidth, real_tx_bandwidth
  continue
end

break /home/bonho/projects/bladerf/host/build/common/thirdparty/ad936x/ad9361.c:3955 if real_rx_bandwidth == 300000
commands
  silent
  printf "AFTER_TX_SECOND ret=%d rx_bw=%u tx_bw=%u\n", ret, real_rx_bandwidth, real_tx_bandwidth
  continue
end
