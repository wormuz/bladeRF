/* Regression test: calibration status reads must propagate SPI errors. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../thirdparty/analogdevicesinc/no-OS/drivers/rf-transceiver/ad9361/ad9361.c"
#include "../../thirdparty/analogdevicesinc/no-OS/drivers/rf-transceiver/ad9361/ad9361_util.c"

static int32_t fake_clock_chain_status;
static unsigned fake_bandwidth_update_count;

static int32_t test_calculate_rf_clock_chain(struct ad9361_rf_phy *phy,
                                            uint32_t tx_sample_rate,
                                            uint32_t rate_governor,
                                            uint32_t *rx_path_clks,
                                            uint32_t *tx_path_clks);
static int32_t test_set_trx_clock_chain(struct ad9361_rf_phy *phy,
                                        uint32_t *rx_path_clks,
                                        uint32_t *tx_path_clks);
static int32_t test_update_rf_bandwidth(struct ad9361_rf_phy *phy,
                                        uint32_t rx_bw, uint32_t tx_bw);

#define ad9361_calculate_rf_clock_chain test_calculate_rf_clock_chain
#define ad9361_set_trx_clock_chain test_set_trx_clock_chain
#define ad9361_update_rf_bandwidth test_update_rf_bandwidth
#include "../../thirdparty/analogdevicesinc/no-OS/drivers/rf-transceiver/ad9361/ad9361_api.c"
#undef ad9361_calculate_rf_clock_chain
#undef ad9361_set_trx_clock_chain
#undef ad9361_update_rf_bandwidth

static int32_t test_calculate_rf_clock_chain(struct ad9361_rf_phy *phy,
                                            uint32_t tx_sample_rate,
                                            uint32_t rate_governor,
                                            uint32_t *rx_path_clks,
                                            uint32_t *tx_path_clks)
{
    (void)phy;
    (void)tx_sample_rate;
    (void)rate_governor;
    rx_path_clks[0] = 1;
    tx_path_clks[0] = 1;
    return 0;
}

static int32_t test_set_trx_clock_chain(struct ad9361_rf_phy *phy,
                                        uint32_t *rx_path_clks,
                                        uint32_t *tx_path_clks)
{
    (void)phy;
    (void)rx_path_clks;
    (void)tx_path_clks;
    return fake_clock_chain_status;
}

static int32_t test_update_rf_bandwidth(struct ad9361_rf_phy *phy,
                                        uint32_t rx_bw, uint32_t tx_bw)
{
    (void)phy;
    (void)rx_bw;
    (void)tx_bw;
    ++fake_bandwidth_update_count;
    return 0;
}

int32_t ad9361_adjust_rx_ext_band_settings(struct ad9361_rf_phy *phy,
                                           uint64_t freq)
{
    (void)phy;
    (void)freq;
    return 0;
}
int32_t ad9361_adjust_tx_ext_band_settings(struct ad9361_rf_phy *phy,
                                           uint64_t freq)
{
    (void)phy;
    (void)freq;
    return 0;
}

static unsigned read_count;
static unsigned delay_count;
static int32_t spi_status;
static bool register_map_enabled;
static int32_t fail_read_reg = -1;
static int32_t fail_write_reg = -1;
static int32_t state_after_ensm_write = -1;
static uint8_t fake_regs[1024];

void *no_os_malloc(size_t size) { return malloc(size); }
void no_os_free(void *ptr) { free(ptr); }
uint64_t no_os_do_div(uint64_t *n, uint64_t base)
{
    uint64_t remainder = *n % base;
    *n /= base;
    return remainder;
}
void no_os_udelay(uint32_t usecs) { (void)usecs; ++delay_count; }
void no_os_mdelay(uint32_t msecs) { (void)msecs; ++delay_count; }

int32_t no_os_spi_write_and_read(struct no_os_spi_desc *desc, uint8_t *data,
                                 uint16_t bytes_number)
{
    uint16_t command;
    uint16_t reg;

    (void)desc;
    if (bytes_number < 3)
        return -EINVAL;

    command = (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
    reg = command & 0x3ff;
    if (command & AD_WRITE) {
        if ((int32_t)reg == fail_write_reg)
            return -EIO;
        if (register_map_enabled) {
            fake_regs[reg] = data[2];
            if (reg == REG_ENSM_CONFIG_1 && state_after_ensm_write >= 0)
                fake_regs[REG_STATE] = (uint8_t)state_after_ensm_write;
        }
        return 0;
    }

    ++read_count;
    if ((int32_t)reg == fail_read_reg)
        return -EIO;
    if (spi_status < 0) return spi_status;
    data[2] = register_map_enabled ? fake_regs[reg] : (uint8_t)spi_status;
    return 0;
}

int main(void)
{
    struct ad9361_rf_phy phy = {0};
    struct ad9361_phy_platform_data pdata = {0};
    struct no_os_spi_desc spi = {0};
    phy.spi = &spi;
    phy.pdata = &pdata;

    spi_status = -EIO;
    int32_t ret = ad9361_check_cal_done(&phy, REG_CALIBRATION_CTRL, 1, 1);
    if (ret != -EIO || read_count != 1 || delay_count != 0) {
        fprintf(stderr, "SPI read error not propagated: ret=%" PRId32
                " reads=%u delays=%u\n", ret, read_count, delay_count);
        return EXIT_FAILURE;
    }

    read_count = delay_count = 0;
    spi_status = 1;
    ret = ad9361_check_cal_done(&phy, REG_CALIBRATION_CTRL, 1, 1);
    if (ret != 0 || read_count != 1 || delay_count != 0) {
        fprintf(stderr, "completed calibration mishandled: ret=%" PRId32
                " reads=%u delays=%u\n", ret, read_count, delay_count);
        return EXIT_FAILURE;
    }

    /* TX quadrature calibration must fail closed on setup writes and status
     * reads instead of reporting a successful calibration. */
    spi_status = 0;
    fail_write_reg = REG_QUAD_CAL_NCO_FREQ_PHASE_OFFSET;
    ret = __ad9361_tx_quad_calib(&phy, 0, 0, 2, NULL);
    if (ret != -EIO) {
        fprintf(stderr, "TX quad calibration hid NCO setup write failure: ret=%" PRId32
                "\n", ret);
        return EXIT_FAILURE;
    }

    fail_write_reg = REG_QUAD_CAL_CTRL;
    ret = __ad9361_tx_quad_calib(&phy, 0, 0, 2, NULL);
    if (ret != -EIO) {
        fprintf(stderr, "TX quad calibration hid control write failure: ret=%" PRId32
                "\n", ret);
        return EXIT_FAILURE;
    }

    fail_write_reg = -1;
    fail_read_reg = REG_QUAD_CAL_STATUS_TX1;
    {
        uint8_t status = 0xff;
        ret = __ad9361_tx_quad_calib(&phy, 0, 0, 2, &status);
        if (ret != -EIO) {
            fprintf(stderr, "TX quad calibration hid status read failure: ret=%" PRId32
                    "\n", ret);
            return EXIT_FAILURE;
        }
    }
    fail_read_reg = -1;

    /* Initialization calibrations must not accept a stale completion bit
     * after a configuration write fails. */
    fail_write_reg = REG_RX_BBF_TUNE_DIVIDE;
    ret = ad9361_rx_bb_analog_filter_calib(&phy, 20000000, 983040000);
    if (ret != -EIO) {
        fprintf(stderr, "RX BB filter calibration hid setup write failure: ret=%" PRId32
                "\n", ret);
        return EXIT_FAILURE;
    }

    fail_write_reg = REG_TX_BBF_TUNE_DIVIDER;
    ret = ad9361_tx_bb_analog_filter_calib(&phy, 20000000, 983040000);
    if (ret != -EIO) {
        fprintf(stderr, "TX BB filter calibration hid setup write failure: ret=%" PRId32
                "\n", ret);
        return EXIT_FAILURE;
    }

    fail_write_reg = REG_RX_CP_LEVEL_DETECT;
    spi_status = CP_CAL_VALID;
    ret = ad9361_txrx_synth_cp_calib(&phy, 40000000, false);
    if (ret != -EIO) {
        fprintf(stderr, "synth CP calibration hid setup write failure: ret=%" PRId32
                "\n", ret);
        return EXIT_FAILURE;
    }

    fail_write_reg = REG_BB_DC_OFFSET_COUNT;
    ret = ad9361_bb_dc_offset_calib(&phy);
    if (ret != -EIO) {
        fprintf(stderr, "BB DC calibration hid setup write failure: ret=%" PRId32
                "\n", ret);
        return EXIT_FAILURE;
    }

    fail_read_reg = REG_RX_BBF_C3_MSB;
    ret = ad9361_rx_tia_calib(&phy, 20000000);
    if (ret != -EIO) {
        fprintf(stderr, "RX TIA calibration hid source read failure: ret=%" PRId32
                "\n", ret);
        return EXIT_FAILURE;
    }
    fail_read_reg = -1;

    fail_write_reg = REG_WAIT_COUNT;
    ret = ad9361_rf_dc_offset_calib_with_timeout(&phy, 1800000000ULL, 1000);
    if (ret != -EIO) {
        fprintf(stderr, "RF DC calibration hid setup write failure: ret=%" PRId32
                "\n", ret);
        return EXIT_FAILURE;
    }

    fail_write_reg = REG_DCXO_COARSE_TUNE;
    ret = ad9361_set_dcxo_tune(&phy, 0, 0);
    if (ret != -EIO) {
        fprintf(stderr, "DCXO setup hid coarse-tune write failure: ret=%" PRId32
                "\n", ret);
        return EXIT_FAILURE;
    }

    {
        struct auxdac_control auxdac = {0};
        struct gpo_control gpo = {0};
        struct ctrl_outs_control ctrl_outs = {0};
        struct gain_control gain = {0};
        struct tx_monitor_control txmon = {0};
        struct elna_control elna = {0};

        fail_write_reg = REG_AUXDAC_ENABLE_CTRL;
        ret = ad9361_auxdac_setup(&phy, &auxdac);
        if (ret != -EIO) {
            fprintf(stderr, "AuxDAC setup hid enable write failure: ret=%" PRId32
                    "\n", ret);
            return EXIT_FAILURE;
        }

        fail_write_reg = REG_AUTO_GPO;
        ret = ad9361_gpo_setup(&phy, &gpo);
        if (ret != -EIO) {
            fprintf(stderr, "GPO setup hid auto-mode write failure: ret=%" PRId32
                    "\n", ret);
            return EXIT_FAILURE;
        }

        fail_write_reg = REG_CTRL_OUTPUT_POINTER;
        ret = ad9361_ctrl_outs_setup(&phy, &ctrl_outs);
        if (ret != -EIO) {
            fprintf(stderr, "Control-output setup hid pointer write failure: ret=%" PRId32
                    "\n", ret);
            return EXIT_FAILURE;
        }

        fail_write_reg = REG_AGC_CONFIG_1;
        ret = ad9361_gc_setup(&phy, &gain);
        if (ret != -EIO) {
            fprintf(stderr, "AGC setup hid initial configuration write failure: ret=%" PRId32
                    "\n", ret);
            return EXIT_FAILURE;
        }

        fail_write_reg = REG_PARALLEL_PORT_CONF_1;
        ret = ad9361_pp_port_setup(&phy, false);
        if (ret != -EIO) {
            fprintf(stderr, "Parallel-port setup hid configuration write failure: ret=%" PRId32
                    "\n", ret);
            return EXIT_FAILURE;
        }

        txmon.tx_mon_duration = 160;
        fail_write_reg = REG_TPM_MODE_ENABLE;
        ret = ad9361_txmon_setup(&phy, &txmon);
        if (ret != -EIO) {
            fprintf(stderr, "TX-monitor setup hid mode write failure: ret=%" PRId32
                    "\n", ret);
            return EXIT_FAILURE;
        }

        fail_write_reg = REG_EXTERNAL_LNA_CTRL;
        ret = ad9361_setup_ext_lna(&phy, &elna);
        if (ret != -EIO) {
            fprintf(stderr, "External-LNA setup hid control write failure: ret=%" PRId32
                    "\n", ret);
            return EXIT_FAILURE;
        }

        fail_write_reg = REG_GM_SUB_TABLE_CONFIG;
        ret = ad9361_load_mixer_gm_subtable(&phy);
        if (ret != -EIO) {
            fprintf(stderr, "Mixer Gm-table setup hid start write failure: ret=%" PRId32
                    "\n", ret);
            return EXIT_FAILURE;
        }

        fail_write_reg = REG_TX2_DIG_ATTEN;
        ret = ad9361_set_tx_atten(&phy, 1000, true, false, true);
        if (ret != -EIO) {
            fprintf(stderr, "TX attenuation hid immediate-update write failure: ret=%" PRId32
                    "\n", ret);
            return EXIT_FAILURE;
        }

        fail_write_reg = REG_RX_FAST_LOCK_SETUP_INIT_DELAY;
        ret = ad9361_fastlock_prepare(&phy, false, 0, true);
        if (ret != -EIO) {
            fprintf(stderr, "Fastlock setup hid delay write failure: ret=%" PRId32
                    "\n", ret);
            return EXIT_FAILURE;
        }

        fail_read_reg = REG_RX_BBF_C3_MSB;
        ret = ad9361_rx_adc_setup(&phy, 983040000, 40000000);
        if (ret != -EIO) {
            fprintf(stderr, "RX ADC setup hid filter read failure: ret=%" PRId32
                    "\n", ret);
            return EXIT_FAILURE;
        }
        fail_read_reg = -1;
    }
    fail_write_reg = -1;

    register_map_enabled = true;
    read_count = delay_count = 0;
    fail_read_reg = REG_STATE;
    ret = ad9361_ensm_force_state_checked(&phy, ENSM_STATE_RX);
    if (ret != -EIO || read_count != 1) {
        fprintf(stderr, "ENSM force hid initial SPI read failure: ret=%" PRId32
                " reads=%u\n", ret, read_count);
        return EXIT_FAILURE;
    }

    fail_read_reg = REG_ENSM_CONFIG_1;
    ret = ad9361_ensm_restore_state_checked(&phy, ENSM_STATE_RX);
    if (ret != -EIO) {
        fprintf(stderr, "ENSM restore hid configuration read failure: ret=%" PRId32
                "\n", ret);
        return EXIT_FAILURE;
    }

    fail_read_reg = -1;
    fail_write_reg = REG_ENSM_CONFIG_1;
    ret = ad9361_ensm_restore_state_checked(&phy, ENSM_STATE_RX);
    if (ret != -EIO) {
        fprintf(stderr, "ENSM restore hid configuration write failure: ret=%" PRId32
                "\n", ret);
        return EXIT_FAILURE;
    }

    fail_write_reg = -1;
    fake_regs[REG_STATE] = ENSM_STATE_RX;
    fake_regs[REG_ENSM_CONFIG_1] = 0;
    ret = ad9361_ensm_force_state_checked(&phy, ENSM_STATE_ALERT);
    if (ret != -ETIMEDOUT) {
        fprintf(stderr, "ENSM force accepted an unconfirmed state: ret=%" PRId32
                "\n", ret);
        return EXIT_FAILURE;
    }

    fake_regs[REG_STATE] = ENSM_STATE_RX;
    state_after_ensm_write = ENSM_STATE_ALERT;
    ret = ad9361_ensm_force_state_checked(&phy, ENSM_STATE_ALERT);
    if (ret != 0 || phy.prev_ensm_state != ENSM_STATE_RX) {
        fprintf(stderr, "ENSM force did not confirm ALERT: ret=%" PRId32
                " previous=%u\n", ret, phy.prev_ensm_state);
        return EXIT_FAILURE;
    }

    state_after_ensm_write = ENSM_STATE_RX;
    ret = ad9361_ensm_restore_prev_state_checked(&phy);
    if (ret != 0) {
        fprintf(stderr, "ENSM restore rejected confirmed RX state: ret=%" PRId32
                "\n", ret);
        return EXIT_FAILURE;
    }

    fail_write_reg = REG_ENSM_MODE;
    ret = ad9361_set_ensm_mode(&phy, false, false);
    if (ret != -EIO) {
        fprintf(stderr, "ENSM mode hid mode write failure: ret=%" PRId32
                "\n", ret);
        return EXIT_FAILURE;
    }
    fail_write_reg = -1;
    fail_read_reg = REG_ENSM_CONFIG_2;
    ret = ad9361_set_ensm_mode(&phy, false, false);
    if (ret != -EIO) {
        fprintf(stderr, "ENSM mode hid configuration read failure: ret=%" PRId32
                "\n", ret);
        return EXIT_FAILURE;
    }
    fail_read_reg = -1;

    fail_write_reg = REG_CALIBRATION_CONFIG_2;
    ret = ad9361_tracking_control(&phy, true, true, true);
    if (ret != -EIO) {
        fprintf(stderr, "Tracking setup hid calibration-control write failure: ret=%" PRId32
                "\n", ret);
        return EXIT_FAILURE;
    }
    fail_write_reg = -1;

    {
        struct refclk_scale bbpll = { .spi = &spi, .phy = &phy };

        fail_write_reg = REG_CP_CURRENT;
        ret = ad9361_bbpll_set_rate(&bbpll, 983040000, 40000000);
        if (ret != -EIO) {
            fprintf(stderr, "BBPLL setup hid loop-filter write failure: ret=%" PRId32
                    "\n", ret);
            return EXIT_FAILURE;
        }
        fail_write_reg = -1;
    }

    {
        struct no_os_clk refclk = { .rate = 40000000 };
        struct no_os_clk bbpll_clk = { .rate = 1 };
        struct refclk_scale bbpll = {
            .spi = &spi,
            .phy = &phy,
            .source = BBPLL_CLK,
            .parent_source = BB_REFCLK,
        };

        phy.clk_refin = &refclk;
        phy.clks[BB_REFCLK] = &refclk;
        phy.clks[BBPLL_CLK] = &bbpll_clk;
        phy.ref_clk_scale[BBPLL_CLK] = &bbpll;
        fail_write_reg = REG_CP_CURRENT;
        ret = clk_set_rate(&phy, &bbpll, 983040000);
        if (ret != -EIO) {
            fprintf(stderr, "Clock framework hid BBPLL programming failure: ret=%" PRId32
                    "\n", ret);
            return EXIT_FAILURE;
        }
        fail_write_reg = -1;

        fake_regs[REG_CH_1_OVERFLOW] = BBPLL_LOCK;
        fail_read_reg = REG_INTEGER_BB_FREQ_WORD;
        ret = clk_set_rate(&phy, &bbpll, 983040000);
        if (ret != -EIO || bbpll_clk.rate != 1 || phy.bbpll_initialized) {
            fprintf(stderr, "Clock framework hid BBPLL readback failure: ret=%" PRId32
                    " cached_rate=%" PRIu32 " initialized=%d\n",
                    ret, bbpll_clk.rate, phy.bbpll_initialized);
            return EXIT_FAILURE;
        }
        fail_read_reg = -1;
    }

    {
        struct no_os_clk rx_refclk = { .rate = 40000000 };
        struct refclk_scale rxpll = {
            .spi = &spi,
            .phy = &phy,
            .source = RX_RFPLL_INT,
            .parent_source = RX_REFCLK,
        };
        uint64_t lo_freq = UINT64_MAX;

        phy.clks[RX_REFCLK] = &rx_refclk;
        phy.ref_clk_scale[RX_RFPLL_INT] = &rxpll;
        phy.pdata->use_ext_rx_lo = false;
        phy.fastlock.current_profile[0] = 0;
        fail_read_reg = REG_RX_FRACT_BYTE_2;
        ret = ad9361_get_rx_lo_freq(&phy, &lo_freq);
        if (ret != -EIO || lo_freq != UINT64_MAX) {
            fprintf(stderr, "RX LO readback hid SPI failure: ret=%" PRId32
                    " freq=%" PRIu64 "\n", ret, lo_freq);
            return EXIT_FAILURE;
        }

        fail_read_reg = -1;
        phy.fastlock.current_profile[0] = 1;
        fail_write_reg = REG_RX_FAST_LOCK_PROGRAM_ADDR;
        ret = ad9361_get_rx_lo_freq(&phy, &lo_freq);
        if (ret != -EIO || lo_freq != UINT64_MAX) {
            fprintf(stderr, "RX LO fastlock readback hid address write failure: ret=%" PRId32
                    " freq=%" PRIu64 "\n", ret, lo_freq);
            return EXIT_FAILURE;
        }
        fail_write_reg = -1;
        fail_read_reg = -1;
        phy.fastlock.current_profile[0] = 0;
    }

    /* Initialization-time ENSM wake must propagate SPI failures. */
    fail_write_reg = REG_CLOCK_ENABLE;
    phy.curr_ensm_state = ENSM_STATE_SLEEP;
    ret = ad9361_ensm_set_state(&phy, ENSM_STATE_ALERT, false);
    if (ret != -EIO) {
        fprintf(stderr, "ENSM wake hid clock-enable write failure: ret=%" PRId32
                "\n", ret);
        return EXIT_FAILURE;
    }

    fail_write_reg = REG_ENSM_CONFIG_1;
    phy.curr_ensm_state = ENSM_STATE_SLEEP;
    ret = ad9361_ensm_set_state(&phy, ENSM_STATE_ALERT, false);
    if (ret != -EIO) {
        fprintf(stderr, "ENSM wake hid ALERT write failure: ret=%" PRId32
                "\n", ret);
        return EXIT_FAILURE;
    }

    fail_write_reg = REG_RX_PFD_CONFIG;
    phy.curr_ensm_state = ENSM_STATE_SLEEP;
    ret = ad9361_ensm_set_state(&phy, ENSM_STATE_ALERT, false);
    if (ret != -EIO) {
        fprintf(stderr, "ENSM wake hid RX VCO-cal control failure: ret=%" PRId32
                "\n", ret);
        return EXIT_FAILURE;
    }
    fail_write_reg = -1;

    /* BIST must propagate both writes and update cached state only after the
     * complete register transaction succeeds. */
    phy.bist_tone_mode = BIST_INJ_TX;
    phy.bist_tone_freq_Hz = 7;
    phy.bist_tone_level_dB = 12;
    phy.bist_tone_mask = 3;
    phy.bist_config = 0x1234;
    fail_write_reg = REG_BIST_AND_DATA_PORT_TEST_CONFIG;
    ret = ad9361_bist_tone(&phy, BIST_DISABLE, 0, 0, 0);
    if (ret != -EIO || phy.bist_tone_mode != BIST_INJ_TX ||
        phy.bist_config != 0x1234) {
        fprintf(stderr, "BIST hid mask write failure or changed cache: ret=%" PRId32
                " mode=%d config=0x%" PRIx32 "\n",
                ret, phy.bist_tone_mode, phy.bist_config);
        return EXIT_FAILURE;
    }

    fail_write_reg = REG_BIST_CONFIG;
    ret = ad9361_bist_tone(&phy, BIST_DISABLE, 0, 0, 0);
    if (ret != -EIO || phy.bist_tone_mode != BIST_INJ_TX ||
        phy.bist_config != 0x1234) {
        fprintf(stderr, "BIST hid config write failure or changed cache: ret=%" PRId32
                " mode=%d config=0x%" PRIx32 "\n",
                ret, phy.bist_tone_mode, phy.bist_config);
        return EXIT_FAILURE;
    }
    fail_write_reg = -1;

    /* Public sampling-rate setters must not hide clock-chain failures behind
     * a later successful bandwidth update. */
    fake_clock_chain_status = -EIO;
    fake_bandwidth_update_count = 0;
    ret = ad9361_set_rx_sampling_freq(&phy, 1000000);
    if (ret != -EIO || fake_bandwidth_update_count != 0) {
        fprintf(stderr, "RX sample-rate setter hid clock-chain failure: ret=%" PRId32
                " bandwidth_updates=%u\n", ret, fake_bandwidth_update_count);
        return EXIT_FAILURE;
    }
    ret = ad9361_set_tx_sampling_freq(&phy, 1000000);
    if (ret != -EIO || fake_bandwidth_update_count != 0) {
        fprintf(stderr, "TX sample-rate setter hid clock-chain failure: ret=%" PRId32
                " bandwidth_updates=%u\n", ret, fake_bandwidth_update_count);
        return EXIT_FAILURE;
    }

    /* Checked clock-rate reads preserve the SPI error, and the public clock
     * tree getter cannot claim success with a zero-filled clock path. */
    {
        struct no_os_clk bbpll_clk = { .rate = 983040000 };
        struct refclk_scale adc_clk = {
            .spi = &spi,
            .phy = &phy,
            .source = ADC_CLK,
            .parent_source = BBPLL_CLK,
        };
        uint32_t rx_path[6] = {0};

        phy.clks[BBPLL_CLK] = &bbpll_clk;
        phy.ref_clk_scale[ADC_CLK] = &adc_clk;
        phy.clks[ADC_CLK] = &(struct no_os_clk){ .rate = 491520000 };
        fail_read_reg = REG_BBPLL;
        ret = ad9361_get_trx_clock_chain(&phy, rx_path, NULL);
        if (ret != -EIO) {
            fprintf(stderr, "Clock-chain getter hid SPI read failure: ret=%" PRId32
                    "\n", ret);
            return EXIT_FAILURE;
        }
        fail_read_reg = -1;
    }

    {
        struct refclk_scale rx_sample_clk = {
            .spi = &spi,
            .phy = &phy,
            .source = RX_SAMPL_CLK,
            .parent_source = CLKRF_CLK,
        };
        struct no_os_clk clkrf_clk = { .rate = 30720000 };
        uint32_t sample_rate = UINT32_MAX;

        phy.clks[CLKRF_CLK] = &clkrf_clk;
        phy.ref_clk_scale[RX_SAMPL_CLK] = &rx_sample_clk;
        fail_read_reg = REG_RX_ENABLE_FILTER_CTRL;
        ret = ad9361_get_rx_sampling_freq(&phy, &sample_rate);
        if (ret != -EIO || sample_rate != UINT32_MAX) {
            fprintf(stderr, "RX sample-rate getter hid clock read failure: ret=%" PRId32
                    " rate=%" PRIu32 "\n", ret, sample_rate);
            return EXIT_FAILURE;
        }
        fail_read_reg = -1;
    }

    /* Initial clock registration must fail and free a partial tree if its
     * first register read fails. */
    {
        struct no_os_clk ext_ref = { .rate = 40000000 };
        size_t i;

        for (i = 0; i < NUM_AD9361_CLKS; ++i) {
            phy.clks[i] = NULL;
            phy.ref_clk_scale[i] = NULL;
        }
        phy.clk_refin = &ext_ref;
        fail_read_reg = REG_REF_DIVIDE_CONFIG_2;
        ret = ad9361_register_clocks(&phy);
        if (ret != -EIO) {
            fprintf(stderr, "Clock registration hid scaler read failure: ret=%" PRId32
                    "\n", ret);
            return EXIT_FAILURE;
        }
        fail_read_reg = -1;
        for (i = 0; i < NUM_AD9361_CLKS; ++i) {
            if (phy.clks[i] != NULL || phy.ref_clk_scale[i] != NULL) {
                fprintf(stderr, "Clock registration left partial clock %zu\n", i);
                return EXIT_FAILURE;
            }
        }
    }

    puts("AD9361 calibration, clock, and ENSM SPI-error propagation: PASS");
    return EXIT_SUCCESS;
}
