-- Copyright (c) 2017 Nuand LLC
--
-- Permission is hereby granted, free of charge, to any person obtaining a copy
-- of this software and associated documentation files (the "Software"), to deal
-- in the Software without restriction, including without limitation the rights
-- to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
-- copies of the Software, and to permit persons to whom the Software is
-- furnished to do so, subject to the following conditions:
--
-- The above copyright notice and this permission notice shall be included in
-- all copies or substantial portions of the Software.
--
-- THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
-- IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
-- FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
-- AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
-- LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
-- OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
-- THE SOFTWARE.
--
-- sweep revision: thin wrapper around bladerf_core. ENABLE_SWEEP_ANALYZER
-- and ENABLE_GAIN_SEQUENCER are on here and off in hosted, so the analyser
-- and its settling sequencer cost the hosted image nothing.
-- ENABLE_TRIGGER_CAPTURE brings in the 4096-sample pre-trigger ring (13 M10K).

library ieee;
    use ieee.std_logic_1164.all;
    use ieee.numeric_std.all;
    use ieee.math_real.all;
    use ieee.math_complex.all;

library work;
    use work.bladerf;
    use work.bladerf_p.all;
    use work.fifo_readwrite_p.all;

architecture sweep_bladerf of bladerf is

begin

    U_core : entity work.bladerf_core
        generic map (
            ENABLE_SWEEP_ANALYZER  => true,
            ENABLE_TRIGGER_CAPTURE => false,
            ENABLE_GAIN_SEQUENCER  => false
        )
        port map (
            c5_clock2      => c5_clock2,
            si_clock_sel   => si_clock_sel,
            c5_clock2_oe   => c5_clock2_oe,
            ufl_clock_oe   => ufl_clock_oe,
            exp_clock_oe   => exp_clock_oe,
            dac_sclk       => dac_sclk,
            dac_sdi        => dac_sdi,
            dac_csn        => dac_csn,
            led            => led,
            ps_sync_1p1    => ps_sync_1p1,
            ps_sync_1p8    => ps_sync_1p8,
            pwr_sda        => pwr_sda,
            pwr_scl        => pwr_scl,
            pwr_status     => pwr_status,
            adi_rx_clock   => adi_rx_clock,
            adi_rx_data    => adi_rx_data,
            adi_rx_frame   => adi_rx_frame,
            adi_rx_spdt1_v => adi_rx_spdt1_v,
            adi_rx_spdt2_v => adi_rx_spdt2_v,
            rx_bias_en     => rx_bias_en,
            adi_tx_clock   => adi_tx_clock,
            adi_tx_data    => adi_tx_data,
            adi_tx_frame   => adi_tx_frame,
            adi_tx_spdt1_v => adi_tx_spdt1_v,
            adi_tx_spdt2_v => adi_tx_spdt2_v,
            tx_bias_en     => tx_bias_en,
            adi_spi_sclk   => adi_spi_sclk,
            adi_spi_csn    => adi_spi_csn,
            adi_spi_sdi    => adi_spi_sdi,
            adi_spi_sdo    => adi_spi_sdo,
            adi_reset_n    => adi_reset_n,
            adi_enable     => adi_enable,
            adi_txnrx      => adi_txnrx,
            adi_en_agc     => adi_en_agc,
            adi_ctrl_in    => adi_ctrl_in,
            adi_ctrl_out   => adi_ctrl_out,
            adi_sync_in    => adi_sync_in,
            adf_sclk       => adf_sclk,
            adf_csn        => adf_csn,
            adf_sdi        => adf_sdi,
            adf_ce         => adf_ce,
            adf_muxout     => adf_muxout,
            fx3_pclk       => fx3_pclk,
            fx3_gpif       => fx3_gpif,
            fx3_ctl        => fx3_ctl,
            fx3_uart_rxd   => fx3_uart_rxd,
            fx3_uart_txd   => fx3_uart_txd,
            fx3_uart_cts   => fx3_uart_cts,
            exp_present    => exp_present,
            exp_clock_req  => exp_clock_req,
            exp_i2c_sda    => exp_i2c_sda,
            exp_i2c_scl    => exp_i2c_scl,
            exp_gpio       => exp_gpio,
            mini_exp1      => mini_exp1,
            mini_exp2      => mini_exp2,
            hw_rev         => hw_rev
        );

end architecture;
