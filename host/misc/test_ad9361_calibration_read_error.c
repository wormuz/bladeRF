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
int32_t ad9361_hdl_loopback(struct ad9361_rf_phy *phy, bool enable)
{
    (void)phy;
    (void)enable;
    return 0;
}

static unsigned read_count;
static unsigned write_count;
static unsigned delay_count;
static int32_t spi_status;
static bool register_map_enabled;
static int32_t fail_read_reg = -1;
static int32_t fail_write_reg = -1;
static int32_t fail_write_once_reg = -1;
static int32_t state_after_ensm_write = -1;
static uint8_t fake_regs[1024];

void *no_os_malloc(size_t size) { return malloc(size); }
void no_os_free(void *ptr) { free(ptr); }
int32_t no_os_gpio_set_value(struct no_os_gpio_desc *desc, uint8_t value)
{
    (void)desc;
    (void)value;
    return 0;
}
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
        ++write_count;
        if ((int32_t)reg == fail_write_once_reg) {
            fail_write_once_reg = -1;
            return -EIO;
        }
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
    int32_t ret;
    uint64_t lo_frequency = UINT64_MAX;
    uint32_t tx_attenuation = UINT32_MAX;
    uint32_t rfpll_rate = UINT32_MAX;
    uint32_t tx_rssi = UINT32_MAX;
    int32_t rx_gain = INT32_MAX;
    uint8_t rx_gc_mode = UINT8_MAX;
    struct rf_rssi rx_rssi = { .ant = UINT8_MAX };
    struct refclk_scale invalid_rfpll_clock = {0};
    struct refclk_scale invalid_clock_factor = {0};
    phy.spi = &spi;
    phy.pdata = &pdata;

    /* Both public LO paths must reject missing objects/output before any
     * dereference or clock/RFIC operation. */
    if (ad9361_set_rx_lo_freq(NULL, 1) != -EINVAL ||
        ad9361_set_tx_lo_freq(NULL, 1) != -EINVAL ||
        ad9361_get_rx_lo_freq(&phy, NULL) != -EINVAL ||
        ad9361_get_tx_lo_freq(&phy, NULL) != -EINVAL ||
        ad9361_get_rx_lo_freq(NULL, &lo_frequency) != -EINVAL ||
        ad9361_get_tx_lo_freq(NULL, &lo_frequency) != -EINVAL ||
        lo_frequency != UINT64_MAX) {
        fprintf(stderr, "AD9361 LO APIs accepted missing arguments\n");
        return EXIT_FAILURE;
    }

    invalid_rfpll_clock.phy = &phy;
    invalid_rfpll_clock.spi = &spi;
    invalid_rfpll_clock.source = NUM_AD9361_CLKS;
    invalid_clock_factor.phy = &phy;
    if (clk_get_rate_checked(NULL, &invalid_rfpll_clock, &rfpll_rate) != -EINVAL ||
        clk_set_rate(NULL, &invalid_rfpll_clock, 1000000000) != -EINVAL ||
        clk_get_rate_checked(&phy, &invalid_rfpll_clock, &rfpll_rate) != -EINVAL ||
        clk_set_rate(&phy, &invalid_rfpll_clock, 1000000000) != -EINVAL ||
        ad9361_clk_factor_recalc_rate_checked(
            &invalid_clock_factor, 40000000, &rfpll_rate) != -EINVAL ||
        ad9361_bbpll_recalc_rate_checked(
            &invalid_clock_factor, 40000000, &rfpll_rate) != -EINVAL ||
        ad9361_bbpll_set_rate(
            &invalid_clock_factor, 1000000000, 40000000) != -EINVAL ||
        ad9361_bbpll_recalc_rate_checked(NULL, 40000000,
                                          &rfpll_rate) != -EINVAL ||
        ad9361_bbpll_set_rate(NULL, 1000000000, 40000000) != -EINVAL ||
        ad9361_rfpll_int_recalc_rate_checked(NULL, 1, &rfpll_rate) != -EINVAL ||
        ad9361_rfpll_int_round_rate(NULL, 1000000000, NULL) != -EINVAL ||
        ad9361_rfpll_recalc_rate_checked(NULL, &rfpll_rate) != -EINVAL ||
        ad9361_rfpll_round_rate(NULL, 1000000000) != -EINVAL ||
        ad9361_rfpll_set_rate(NULL, 1000000000) != -EINVAL ||
        ad9361_clk_mux_set_parent(NULL, 0) != -EINVAL ||
        ad9361_rfpll_dummy_recalc_rate(NULL) != 0 ||
        ad9361_rfpll_dummy_set_rate(NULL, 1000000000) != -EINVAL ||
        ad9361_rfpll_set_rate(&invalid_rfpll_clock, 1000000000) != -EINVAL ||
        ad9361_rfpll_round_rate(&invalid_rfpll_clock, 1000000000) != -EINVAL ||
        ad9361_clk_mux_set_parent(&invalid_rfpll_clock, 2) != -EINVAL ||
        ad9361_rfpll_int_recalc_rate_checked(
            &invalid_rfpll_clock, 1, &rfpll_rate) != -EINVAL ||
        ad9361_rfpll_int_round_rate(
            &invalid_rfpll_clock, 1000000000, NULL) != -EINVAL ||
        rfpll_rate != UINT32_MAX || read_count != 0 || write_count != 0) {
        fprintf(stderr, "RFPLL APIs accepted invalid context or changed output\n");
        return EXIT_FAILURE;
    }
    invalid_rfpll_clock.source = TX_RFPLL_INT;
    phy.fastlock.current_profile[1] = 9;
    if (ad9361_rfpll_int_recalc_rate_checked(
            &invalid_rfpll_clock, 40000000, &rfpll_rate) != -EINVAL ||
        rfpll_rate != UINT32_MAX || read_count != 0 || write_count != 0) {
        fprintf(stderr, "Out-of-range TX fastlock profile reached SPI\n");
        return EXIT_FAILURE;
    }
    phy.fastlock.current_profile[1] = 0;

    /* Clock scaler candidates become cache only after validation/programming
     * succeeds; failed SPI writes and invalid divider choices keep the last
     * confirmed multiplier/divider pair. */
    {
        struct refclk_scale clock = {
            .spi = &spi,
            .phy = &phy,
            .mult = 2,
            .div = 1,
            .source = BB_REFCLK,
        };
        register_map_enabled = true;
        ret = ad9361_clk_factor_round_rate(&clock, 40000000,
                                           &(uint32_t){40000000});
        if (ret != 40000000 || clock.mult != 2 || clock.div != 1) {
            fprintf(stderr, "Clock-rate rounding changed confirmed cache\n");
            return EXIT_FAILURE;
        }

        fail_write_reg = REG_CLOCK_CTRL;
        ret = ad9361_clk_factor_set_rate(&clock, 40000000, 40000000);
        if (ret != -EIO || clock.mult != 2 || clock.div != 1) {
            fprintf(stderr, "Failed clock-scaler write changed its cache\n");
            return EXIT_FAILURE;
        }

        fail_write_reg = -1;
        clock.source = RX_SAMPL_CLK;
        clock.mult = 7;
        clock.div = 8;
        ret = ad9361_clk_factor_round_rate(&clock, 40, &(uint32_t){100});
        if (ret != -EINVAL || clock.mult != 7 || clock.div != 8) {
            fprintf(stderr, "Invalid clock-scaler candidate changed its cache\n");
            return EXIT_FAILURE;
        }
        register_map_enabled = false;
        read_count = 0;
        write_count = 0;
    }

    /* VCO-table selection is cache state: a failed partial RFPLL setup must
     * not publish the selected table as if every SPI write had completed. */
    {
        bool selected_tdd_table = true;
        register_map_enabled = true;
        pdata.fdd = true;
        pdata.fdd_independent_mode = false;
        phy.current_tx_lo_freq = 900000000;
        phy.current_rx_lo_freq = 1800000000;
        phy.current_tx_use_tdd_table = true;
        phy.current_rx_use_tdd_table = false;
        fail_write_reg = REG_RX_VCO_OUTPUT;
        ret = ad9361_rfpll_vco_init(&phy, false, 8500000000ULL, 40000000,
                                    true, &selected_tdd_table);
        if (ret != -EIO || !phy.current_rx_use_tdd_table ||
            phy.current_tx_use_tdd_table || !selected_tdd_table) {
            fprintf(stderr, "Failed RFPLL VCO setup published table cache\n");
            return EXIT_FAILURE;
        }

        fail_write_reg = -1;
        ret = ad9361_rfpll_vco_init(&phy, false, 8500000000ULL, 40000000,
                                    true, &selected_tdd_table);
        if (ret != 0 || selected_tdd_table ||
            !phy.current_rx_use_tdd_table ||
            phy.current_tx_use_tdd_table) {
            fprintf(stderr, "Successful RFPLL VCO setup changed cache early\n");
            return EXIT_FAILURE;
        }
        register_map_enabled = false;
        pdata.fdd = false;
        phy.current_tx_lo_freq = 0;
        phy.current_rx_lo_freq = 0;
        read_count = 0;
        write_count = 0;
    }

    /* A nonexistent 1x1 TX channel must not become a successful no-op, and
     * invalid RX/TX lane reads must fail before touching SPI or outputs. */
    if (ad9361_set_tx_attenuation(&phy, 1, 10000) != -EINVAL ||
        ad9361_set_tx_attenuation(&phy, 2, 10000) != -EINVAL ||
        ad9361_get_tx_attenuation(&phy, 1, &tx_attenuation) != -EINVAL ||
        ad9361_get_tx_rssi(&phy, 1, &tx_rssi) != -EINVAL ||
        ad9361_get_rx_rssi(&phy, 1, &rx_rssi) != -EINVAL ||
        ad9361_set_rx_rf_gain(&phy, 1, 20) != -EINVAL ||
        ad9361_set_rx_rf_gain(&phy, UINT8_MAX, 20) != -EINVAL ||
        ad9361_get_rx_rf_gain(&phy, 1, &rx_gain) != -EINVAL ||
        ad9361_set_rx_gain_control_mode(&phy, 1, RF_GAIN_MGC) != -EINVAL ||
        ad9361_set_rx_gain_control_mode(&phy, UINT8_MAX, RF_GAIN_MGC) != -EINVAL ||
        ad9361_get_rx_gain_control_mode(&phy, 1, &rx_gc_mode) != -EINVAL ||
        read_count != 0 || write_count != 0 ||
        tx_attenuation != UINT32_MAX || tx_rssi != UINT32_MAX ||
        rx_gain != INT32_MAX || rx_gc_mode != UINT8_MAX ||
        rx_rssi.ant != UINT8_MAX) {
        fprintf(stderr, "AD9361 accepted invalid channel or touched SPI\n");
        return EXIT_FAILURE;
    }

    /* In 1x1 mode with physical TX2 selected, logical channel zero must
     * report the same packed RSSI field as 2x2 channel one. */
    {
        uint32_t rssi_1x1, rssi_2x2;
        register_map_enabled = true;
        pdata.rx1tx1_mode_use_tx_num = 2;
        fake_regs[REG_TX_RSSI_LSB] = TX_RSSI_2;
        fake_regs[REG_TX_RSSI_LSB + 1] = 5;
        fake_regs[REG_TX_RSSI_LSB + 2] = 1;
        if (ad9361_get_tx_rssi(&phy, 0, &rssi_1x1) != 0) {
            fprintf(stderr, "1x1 TX2 RSSI read failed\n");
            return EXIT_FAILURE;
        }
        pdata.rx2tx2 = true;
        if (ad9361_get_tx_rssi(&phy, 1, &rssi_2x2) != 0 ||
            rssi_1x1 != rssi_2x2) {
            fprintf(stderr, "1x1 TX2 RSSI mapped to wrong physical lane\n");
            return EXIT_FAILURE;
        }
        pdata.rx2tx2 = false;
        register_map_enabled = false;
        read_count = 0;
    }

    /* If disabling tracking fails partway through a calibration setup,
     * restore the configured tracking state before returning the primary
     * SPI error. */
    {
        const uint8_t tracking_mask = ENABLE_BB_DC_OFFSET_TRACKING |
                                     ENABLE_RF_OFFSET_TRACKING;
        register_map_enabled = true;
        memset(fake_regs, 0, sizeof(fake_regs));
        phy.bbdc_track_en = true;
        phy.rfdc_track_en = true;
        phy.quad_track_en = true;
        spi_status = 0;
        fail_write_once_reg = REG_CALIBRATION_CONFIG_3;
        ret = ad9361_do_calib_run(&phy, RFDC_CAL, -1);
        if (ret != -EIO || fail_write_once_reg != -1 ||
            (fake_regs[REG_DC_OFFSET_CONFIG2] & tracking_mask) !=
                tracking_mask) {
            fprintf(stderr,
                    "Calibration setup failure left tracking disabled: ret=%" PRId32
                    " tracking=0x%02x\n", ret,
                    fake_regs[REG_DC_OFFSET_CONFIG2]);
            return EXIT_FAILURE;
        }
        register_map_enabled = false;
        read_count = write_count = 0;
    }

    spi_status = -EIO;
    ret = ad9361_check_cal_done(&phy, REG_CALIBRATION_CTRL, 1, 1);
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

    /* Setup-write errors on temporary AUX ADC power changes are ambiguous;
     * both telemetry APIs must still restore the power-down bit. */
    {
        register_map_enabled = true;
        memset(fake_regs, 0, sizeof(fake_regs));
        fake_regs[REG_AUXADC_CONFIG] = AUXADC_POWER_DOWN;
        fail_write_once_reg = REG_AUXADC_CONFIG;
        write_count = 0;
        ret = ad9361_get_temp(&phy);
        if (ret != -EIO || fail_write_once_reg != -1 || write_count != 2 ||
            (fake_regs[REG_AUXADC_CONFIG] & AUXADC_POWER_DOWN) != 0) {
            fprintf(stderr,
                    "Temperature read skipped AUX ADC restore after setup error\n");
            return EXIT_FAILURE;
        }

        fake_regs[REG_AUXADC_CONFIG] = AUXADC_POWER_DOWN;
        fail_write_once_reg = REG_AUXADC_CONFIG;
        write_count = 0;
        ret = ad9361_get_auxadc(&phy);
        if (ret != -EIO || fail_write_once_reg != -1 || write_count != 2 ||
            (fake_regs[REG_AUXADC_CONFIG] & AUXADC_POWER_DOWN) != 0) {
            fprintf(stderr,
                    "AUX ADC read skipped power restore after setup error\n");
            return EXIT_FAILURE;
        }
        register_map_enabled = false;
        read_count = write_count = 0;
    }

    /* An ambiguous failure while temporarily clearing RX2 inversion must
     * still attempt the restore write before returning the primary error. */
    {
        struct no_os_clk rx_clock = { .rate = 30000000 };
        struct no_os_clk tx_clock = { .rate = 30000000 };
        struct refclk_scale clkrf = {
            .spi = &spi, .phy = &phy, .source = CLKRF_CLK,
            .parent_source = ADC_CLK,
        };
        struct refclk_scale clktf = {
            .spi = &spi, .phy = &phy, .source = CLKTF_CLK,
            .parent_source = DAC_CLK,
        };

        register_map_enabled = true;
        memset(fake_regs, 0, sizeof(fake_regs));
        phy.clks[ADC_CLK] = &rx_clock;
        phy.clks[DAC_CLK] = &tx_clock;
        phy.ref_clk_scale[CLKRF_CLK] = &clkrf;
        phy.ref_clk_scale[CLKTF_CLK] = &clktf;
        pdata.rx1rx2_phase_inversion_en = true;
        fail_write_once_reg = REG_PARALLEL_PORT_CONF_2;
        ret = ad9361_tx_quad_calib(&phy, 10000000, 10000000, -1);
        if (ret != -EIO || fail_write_once_reg != -1 ||
            (fake_regs[REG_PARALLEL_PORT_CONF_2] & INVERT_RX2) == 0) {
            fprintf(stderr,
                    "TX quad calibration skipped phase-inversion restore: "
                    "ret=%" PRId32 " reg=0x%02x\n", ret,
                    fake_regs[REG_PARALLEL_PORT_CONF_2]);
            return EXIT_FAILURE;
        }
        pdata.rx1rx2_phase_inversion_en = false;
        phy.ref_clk_scale[CLKRF_CLK] = NULL;
        phy.ref_clk_scale[CLKTF_CLK] = NULL;
        phy.clks[ADC_CLK] = NULL;
        phy.clks[DAC_CLK] = NULL;
        register_map_enabled = false;
        read_count = write_count = 0;
    }

    fail_write_reg = -1;

    /* Fastlock profile cache must only advance after the complete profile
     * write, and failures must still attempt to stop the programming clock. */
    {
        uint8_t profile_data[RX_FAST_LOCK_CONFIG_WORD_NUM] = {0};
        phy.fastlock.entry[0][0].flags = 0;
        fail_write_reg = REG_RX_FAST_LOCK_PROGRAM_CTRL;
        ret = ad9361_fastlock_load(&phy, false, 0, profile_data);
        if (ret != -EIO || phy.fastlock.entry[0][0].flags == FASTLOOK_INIT) {
            fprintf(stderr, "Fastlock loader hid profile SPI failure: ret=%" PRId32
                    " flags=%" PRIu32 "\n", ret,
                    phy.fastlock.entry[0][0].flags);
            return EXIT_FAILURE;
        }
        fail_write_reg = -1;
        ret = ad9361_fastlock_load(&phy, false, 8, profile_data);
        if (ret != -EINVAL) {
            fprintf(stderr, "Fastlock loader accepted invalid profile: ret=%" PRId32 "\n", ret);
            return EXIT_FAILURE;
        }
    }

    /* Public status/configuration APIs must not report cached or fabricated
     * values when their underlying SPI transaction fails. */
    {
        uint32_t ensm_mode = UINT32_MAX;
        struct rf_rssi rssi = { .ant = 1 };
        struct rf_rx_gain rx_gain = { .ant = 1, .gain_db = 0x1234 };
        uint8_t old_agc_mode = phy.agc_mode[0];

        fail_read_reg = REG_STATE;
        ret = ad9361_get_en_state_machine_mode(&phy, &ensm_mode);
        if (ret != -EIO || ensm_mode != UINT32_MAX) {
            fprintf(stderr, "ENSM getter hid state read failure: ret=%" PRId32
                    " mode=%" PRIu32 "\n", ret, ensm_mode);
            return EXIT_FAILURE;
        }
        fail_read_reg = REG_ENSM_CONFIG_1;
        ret = ad9361_get_en_state_machine_mode(&phy, &ensm_mode);
        if (ret != -EIO || ensm_mode != UINT32_MAX) {
            fprintf(stderr, "ENSM getter hid config read failure: ret=%" PRId32
                    " mode=%" PRIu32 "\n", ret, ensm_mode);
            return EXIT_FAILURE;
        }
        fail_read_reg = REG_PREAMBLE_LSB;
        ret = ad9361_read_rssi(&phy, &rssi);
        if (ret != -EIO) {
            fprintf(stderr, "RSSI getter hid register read failure: ret=%" PRId32
                    "\n", ret);
            return EXIT_FAILURE;
        }
        fail_read_reg = REG_AGC_CONFIG_1;
        ret = ad9361_set_rx_gain_control_mode(&phy, 0, RF_GAIN_MGC);
        if (ret != -EIO || phy.agc_mode[0] != old_agc_mode) {
            fprintf(stderr, "AGC setter hid SPI failure or updated cache: ret=%" PRId32
                    " mode=%u\n", ret, phy.agc_mode[0]);
            return EXIT_FAILURE;
        }
        phy.gt_info = ad9361_adi_gt_info;
        phy.current_table = TBL_200_1300_MHZ;
        register_map_enabled = true;
        fake_regs[REG_RX_ENABLE_FILTER_CTRL] = RX_CHANNEL_ENABLE(RX_1);
        fail_read_reg = REG_RX_ENABLE_FILTER_CTRL;
        ret = ad9361_get_rx_gain(&phy, 1, &rx_gain);
        if (ret != -EIO || rx_gain.gain_db != 0x1234) {
            fprintf(stderr, "RX gain getter hid enable read failure or changed output: ret=%"
                    PRId32 " gain=%" PRId32 "\n", ret, rx_gain.gain_db);
            return EXIT_FAILURE;
        }
        fail_read_reg = REG_AGC_CONFIG_1;
        ret = ad9361_get_rx_gain(&phy, 1, &rx_gain);
        if (ret != -EIO || rx_gain.gain_db != 0x1234) {
            fprintf(stderr, "RX gain getter hid AGC read failure or changed output: ret=%"
                    PRId32 " gain=%" PRId32 "\n", ret, rx_gain.gain_db);
            return EXIT_FAILURE;
        }
        {
            int32_t gain_db = 4321;
            ret = ad9361_get_rx_rf_gain(&phy, 0, &gain_db);
            if (ret != -EIO || gain_db != 4321) {
                fprintf(stderr, "RX gain API hid SPI error or changed output\n");
                return EXIT_FAILURE;
            }
        }
        fail_read_reg = REG_GAIN_RX1;
        ret = ad9361_get_rx_gain(&phy, 1, &rx_gain);
        if (ret != -EIO || rx_gain.gain_db != 0x1234) {
            fprintf(stderr, "RX gain getter hid full-table read failure or changed output: ret=%"
                    PRId32 " gain=%" PRId32 "\n", ret, rx_gain.gain_db);
            return EXIT_FAILURE;
        }
        fail_read_reg = -1;
        fake_regs[REG_GAIN_RX1] = 0xff;
        ret = ad9361_get_rx_gain(&phy, 1, &rx_gain);
        if (ret != -EIO || rx_gain.gain_db != 0x1234) {
            fprintf(stderr, "RX gain getter accepted invalid table index: ret=%" PRId32
                    " gain=%" PRId32 "\n", ret, rx_gain.gain_db);
            return EXIT_FAILURE;
        }
        fake_regs[REG_GAIN_RX1] = 0;
        phy.pdata->split_gt = true;
        phy.current_table = TBL_200_1300_MHZ + RXGAIN_TBLS_END;
        fail_read_reg = REG_GAIN_TABLE_ADDRESS;
        ret = ad9361_get_rx_gain(&phy, 1, &rx_gain);
        if (ret != -EIO || rx_gain.gain_db != 0x1234) {
            fprintf(stderr, "RX gain getter hid split-table address read failure: ret=%"
                    PRId32 " gain=%" PRId32 "\n", ret, rx_gain.gain_db);
            return EXIT_FAILURE;
        }
        fail_read_reg = -1;
        fail_write_reg = REG_GAIN_TABLE_ADDRESS;
        ret = ad9361_get_rx_gain(&phy, 1, &rx_gain);
        if (ret != -EIO || rx_gain.gain_db != 0x1234) {
            fprintf(stderr, "RX gain getter hid split-table address write failure: ret=%"
                    PRId32 " gain=%" PRId32 "\n", ret, rx_gain.gain_db);
            return EXIT_FAILURE;
        }
        fail_write_reg = -1;
        phy.pdata->split_gt = false;
        phy.current_table = TBL_200_1300_MHZ;
        fail_read_reg = -1;
        register_map_enabled = false;
    }

    /* Both FIR coefficient readers must propagate setup, address, data, and
     * clock-restore SPI failures. */
    {
        AD9361_RXFIRConfig rx_fir = {0};
        AD9361_TXFIRConfig tx_fir = {0};
        int16_t fir_coef[16] = {0};

        fail_write_reg = REG_RX_FILTER_CONFIG;
        ret = ad9361_get_rx_fir_config(&phy, 0, &rx_fir);
        if (ret != -EIO) {
            fprintf(stderr, "RX FIR getter hid setup write failure: ret=%" PRId32 "\n", ret);
            return EXIT_FAILURE;
        }
        fail_write_reg = REG_RX_FILTER_COEF_ADDR;
        ret = ad9361_get_rx_fir_config(&phy, 0, &rx_fir);
        if (ret != -EIO) {
            fprintf(stderr, "RX FIR getter hid coefficient address failure: ret=%" PRId32 "\n", ret);
            return EXIT_FAILURE;
        }
        fail_write_reg = REG_TX_FILTER_CONF;
        ret = ad9361_get_tx_fir_config(&phy, 0, &tx_fir);
        if (ret != -EIO) {
            fprintf(stderr, "TX FIR getter hid setup write failure: ret=%" PRId32 "\n", ret);
            return EXIT_FAILURE;
        }
        fail_write_reg = REG_TX_FILTER_COEF_ADDR;
        ret = ad9361_get_tx_fir_config(&phy, 0, &tx_fir);
        if (ret != -EIO) {
            fprintf(stderr, "TX FIR getter hid coefficient address failure: ret=%" PRId32 "\n", ret);
            return EXIT_FAILURE;
        }
		memset(&rx_fir, 0x5a, sizeof(rx_fir));
		fail_write_reg = -1;
		fail_read_reg = REG_RX_FILTER_COEF_READ_DATA_1;
		ret = ad9361_get_rx_fir_config(&phy, 0, &rx_fir);
		for (size_t i = 0; i < sizeof(rx_fir); ++i) {
			if (((const unsigned char *)&rx_fir)[i] != 0x5a) {
				fprintf(stderr, "RX FIR getter changed output on failed read\n");
				return EXIT_FAILURE;
			}
		}
		if (ret != -EIO) {
			fprintf(stderr, "RX FIR getter hid coefficient read failure\n");
			return EXIT_FAILURE;
		}
		memset(&tx_fir, 0x6b, sizeof(tx_fir));
		fail_read_reg = REG_TX_FILTER_COEF_READ_DATA_1;
		ret = ad9361_get_tx_fir_config(&phy, 0, &tx_fir);
		for (size_t i = 0; i < sizeof(tx_fir); ++i) {
			if (((const unsigned char *)&tx_fir)[i] != 0x6b) {
				fprintf(stderr, "TX FIR getter changed output on failed read\n");
				return EXIT_FAILURE;
			}
		}
		if (ret != -EIO) {
			fprintf(stderr, "TX FIR getter hid coefficient read failure\n");
			return EXIT_FAILURE;
		}
		fail_read_reg = -1;
        fail_write_reg = -1;

        register_map_enabled = true;
        fake_regs[REG_STATE] = ENSM_STATE_ALERT;
        phy.rx_fir_ntaps = 64;
        fail_write_reg = REG_RX_FILTER_GAIN;
        ret = ad9361_load_fir_filter_coef(&phy, FIR_RX1, 0, 16, fir_coef);
        if (ret != -EIO || phy.rx_fir_ntaps != 64) {
            fprintf(stderr, "RX FIR loader hid gain write failure or changed cache: ret=%"
                    PRId32 " taps=%" PRIu32 "\n", ret, phy.rx_fir_ntaps);
            return EXIT_FAILURE;
        }
        {
            AD9361_RXFIRConfig rx_cfg = { .rx = 1, .rx_dec = 2,
                                         .rx_coef_size = 16 };
            phy.rx_fir_dec = 4;
            fail_write_reg = REG_RX_FILTER_GAIN;
            ret = ad9361_set_rx_fir_config(&phy, rx_cfg);
            if (ret != -EIO || phy.rx_fir_dec != 4) {
                fprintf(stderr, "RX FIR API hid load failure or changed decimation cache\n");
                return EXIT_FAILURE;
            }
        }
        {
            AD9361_TXFIRConfig tx_cfg = { .tx = 1, .tx_int = 2,
                                         .tx_coef_size = 16 };
            phy.tx_fir_int = 1;
            fail_write_reg = REG_TX_FILTER_CONF;
            ret = ad9361_set_tx_fir_config(&phy, tx_cfg);
            if (ret != -EIO || phy.tx_fir_int != 1) {
                fprintf(stderr, "TX FIR API hid load failure or changed interpolation cache\n");
                return EXIT_FAILURE;
            }
        }
        fail_write_reg = REG_RX_FILTER_COEF_ADDR;
        ret = ad9361_load_fir_filter_coef(&phy, FIR_RX1, 0, 16, fir_coef);
        if (ret != -EIO || phy.rx_fir_ntaps != 64) {
            fprintf(stderr, "RX FIR loader hid coefficient write failure or changed cache: ret=%"
                    PRId32 " taps=%" PRIu32 "\n", ret, phy.rx_fir_ntaps);
            return EXIT_FAILURE;
        }
        fail_write_reg = -1;
        register_map_enabled = false;
    }
    /* Measurement and multi-register helpers must return transport errors
     * instead of manufacturing values or committing partial cache state. */
    {
        uint8_t old_rx_delay = pdata.port_ctrl.rx_clk_data_delay;
        uint8_t old_tx_delay = pdata.port_ctrl.tx_clk_data_delay;

        register_map_enabled = true;
        fail_read_reg = REG_TEMPERATURE;
        ret = ad9361_get_temp(&phy);
        if (ret != -EIO) {
            fprintf(stderr, "Temperature getter hid SPI read failure: ret=%" PRId32 "\n", ret);
            return EXIT_FAILURE;
        }
        {
            int32_t temperature = 1234;
            ret = ad9361_get_temperature(&phy, &temperature);
            if (ret != -EIO || temperature != 1234) {
                fprintf(stderr, "Temperature API hid SPI error or changed output\n");
                return EXIT_FAILURE;
            }
        }
        fail_read_reg = REG_AUXADC_LSB;
        ret = ad9361_get_auxadc(&phy);
        if (ret != -EIO) {
            fprintf(stderr, "AuxADC getter hid SPI read failure: ret=%" PRId32 "\n", ret);
            return EXIT_FAILURE;
        }
        fail_read_reg = REG_RX_CLOCK_DATA_DELAY;
        ret = ad9361_read_clock_data_delays(&phy);
        if (ret != -EIO || pdata.port_ctrl.rx_clk_data_delay != old_rx_delay ||
            pdata.port_ctrl.tx_clk_data_delay != old_tx_delay) {
            fprintf(stderr, "Clock-delay getter hid RX failure or changed cached data\n");
            return EXIT_FAILURE;
        }
        fail_read_reg = REG_TX_CLOCK_DATA_DELAY;
        ret = ad9361_read_clock_data_delays(&phy);
        if (ret != -EIO || pdata.port_ctrl.rx_clk_data_delay != old_rx_delay ||
            pdata.port_ctrl.tx_clk_data_delay != old_tx_delay) {
            fprintf(stderr, "Clock-delay getter hid TX failure or partially changed cache\n");
            return EXIT_FAILURE;
        }
        fail_read_reg = -1;
        fail_write_reg = REG_RX_FAST_LOCK_SETUP;
        phy.fastlock.current_profile[0] = 7;
        fake_regs[REG_RX_FAST_LOCK_SETUP] = RX_FAST_LOCK_MODE_ENABLE;
        ret = ad9361_fastlock_exit_foreign(&phy, false);
        if (ret != -EIO || phy.fastlock.current_profile[0] != 7) {
            fprintf(stderr, "Fastlock exit hid setup write failure: ret=%" PRId32 "\n", ret);
            return EXIT_FAILURE;
        }
        fail_write_reg = -1;
        phy.fastlock.current_profile[0] = 0;
        fail_read_reg = REG_RX_INTEGER_BYTE_0;
        fail_write_reg = -1;
        ret = ad9361_fastlock_store(&phy, false, 0);
        if (ret != -EIO) {
            fprintf(stderr, "Fastlock store hid profile read failure: ret=%" PRId32 "\n", ret);
            return EXIT_FAILURE;
        }
        phy.tx1_atten_cached = 111;
        phy.tx2_atten_cached = 222;
        fail_read_reg = REG_TX1_ATTEN_1;
        ret = ad9361_tx_mute(&phy, 1);
        if (ret != -EIO || phy.tx1_atten_cached != 111 ||
            phy.tx2_atten_cached != 222) {
            fprintf(stderr, "TX mute hid attenuation read failure or changed cache\n");
            return EXIT_FAILURE;
        }
        fail_read_reg = REG_AGC_CONFIG_1;
        ret = ad9361_set_rx_gain(&phy, 1, &(struct rf_rx_gain){ .gain_db = 10 });
        if (ret != -EIO) {
            fprintf(stderr, "RX gain setter hid AGC read failure: ret=%" PRId32 "\n", ret);
            return EXIT_FAILURE;
        }
        fail_read_reg = -1;
        phy.cached_synth_pd[1] = 0x5a;
        fail_read_reg = REG_MULTICHIP_SYNC_AND_TX_MON_CTRL;
        ret = ad9361_trx_ext_lo_control(&phy, false, true);
        if (ret != -EIO || phy.cached_synth_pd[1] != 0x5a) {
            fprintf(stderr, "External LO control hid state read failure or changed cache\n");
            return EXIT_FAILURE;
        }
        fail_read_reg = -1;
        fail_write_reg = REG_ENSM_CONFIG_2;
        ret = ad9361_trx_ext_lo_control(&phy, false, true);
        if (ret != -EIO || phy.cached_synth_pd[1] != 0x5a) {
            fprintf(stderr, "External LO control hid first write failure or changed cache\n");
            return EXIT_FAILURE;
        }
        fail_write_reg = -1;
        fail_write_reg = REG_RX_CLOCK_DATA_DELAY;
        ret = ad9361_write_clock_data_delays(&phy);
        if (ret != -EIO) {
            fprintf(stderr, "Clock-delay writer hid RX write failure: ret=%" PRId32 "\n", ret);
            return EXIT_FAILURE;
        }
        fail_write_reg = -1;
        register_map_enabled = false;
    }
    ret = ad9361_set_rx_lo_int_ext(&phy, 2);
    if (ret != -EINVAL || pdata.use_ext_rx_lo != 0) {
        fprintf(stderr, "RX LO selector accepted invalid mode or changed cache\n");
        return EXIT_FAILURE;
    }
    ret = ad9361_set_tx_lo_int_ext(&phy, 2);
    if (ret != -EINVAL || pdata.use_ext_tx_lo != 0) {
        fprintf(stderr, "TX LO selector accepted invalid mode or changed cache\n");
        return EXIT_FAILURE;
    }
    fail_read_reg = REG_QUAD_CAL_STATUS_TX1;
    {
        uint8_t status = 0xff;
        /* In 2x2 mode, the 1x1 fallback setting must not select TX2 as
         * the first status register and silently skip TX1 convergence. */
        pdata.rx2tx2 = true;
        pdata.rx1tx1_mode_use_tx_num = 2;
        ret = __ad9361_tx_quad_calib(&phy, 0, 0, 2, &status);
        if (ret != -EIO) {
            fprintf(stderr,
                    "2x2 TX quad calibration skipped TX1 status: ret=%" PRId32
                    "\n", ret);
            return EXIT_FAILURE;
        }
        pdata.rx2tx2 = false;
        pdata.rx1tx1_mode_use_tx_num = 1;
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

    /* Public tracking setters must not publish a new software state when the
     * shared calibration-control register write fails. */
    {
        phy.bbdc_track_en = false;
        phy.rfdc_track_en = false;
        phy.quad_track_en = false;
        fail_write_reg = REG_CALIBRATION_CONFIG_2;
        ret = ad9361_set_rx_rfdc_track_en_dis(&phy, 1);
        if (ret != -EIO || phy.rfdc_track_en != 0) {
            fprintf(stderr, "RFDC tracking setter hid error or committed cache\n");
            return EXIT_FAILURE;
        }
        ret = ad9361_set_rx_bbdc_track_en_dis(&phy, 1);
        if (ret != -EIO || phy.bbdc_track_en != 0) {
            fprintf(stderr, "BB DC tracking setter hid error or committed cache\n");
            return EXIT_FAILURE;
        }
        ret = ad9361_set_rx_quad_track_en_dis(&phy, 1);
        if (ret != -EIO || phy.quad_track_en != 0) {
            fprintf(stderr, "Quadrature tracking setter hid error or committed cache\n");
            return EXIT_FAILURE;
        }
        fail_write_reg = -1;
        if (ad9361_set_rx_rfdc_track_en_dis(&phy, 2) != -EINVAL ||
            ad9361_set_rx_bbdc_track_en_dis(&phy, 2) != -EINVAL ||
            ad9361_set_rx_quad_track_en_dis(&phy, 2) != -EINVAL) {
            fprintf(stderr, "Tracking setters accepted invalid enable value\n");
            return EXIT_FAILURE;
        }
    }

    fail_write_reg = REG_ANALOG_POWER_DOWN_OVERRIDE;
    ret = ad9361_txmon_enable(&phy, TX_1 | TX_2);
    if (ret != -EIO) {
        fprintf(stderr, "TX monitor enable hid first register failure\n");
        return EXIT_FAILURE;
    }
    fail_write_reg = REG_TPM_MODE_ENABLE;
    ret = ad9361_txmon_enable(&phy, TX_1 | TX_2);
    if (ret != -EIO) {
        fprintf(stderr, "TX monitor enable hid later register failure\n");
        return EXIT_FAILURE;
    }
    fail_write_reg = REG_ENSM_CONFIG_2;
    ret = ad9361_mcs(&phy, 1);
    if (ret != -EIO || ad9361_mcs(&phy, 6) != -EINVAL) {
        fprintf(stderr, "MCS hid SPI failure or accepted invalid step\n");
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
    phy.bist_prbs_mode = BIST_INJ_TX;
    phy.bist_config = 0x55;
    fail_write_reg = REG_BIST_CONFIG;
    ret = ad9361_bist_prbs(&phy, BIST_DISABLE);
    if (ret != -EIO || phy.bist_prbs_mode != BIST_INJ_TX ||
        phy.bist_config != 0x55) {
        fprintf(stderr, "BIST PRBS hid write failure or changed cache\n");
        return EXIT_FAILURE;
    }
    fail_write_reg = -1;
    ret = ad9361_bist_prbs(&phy, (enum ad9361_bist_mode)99);
    if (ret != -EINVAL || phy.bist_prbs_mode != BIST_INJ_TX ||
        phy.bist_config != 0x55) {
        fprintf(stderr, "BIST PRBS accepted invalid mode or changed cache\n");
        return EXIT_FAILURE;
    }

    phy.bist_loopback_mode = 2;
    fail_read_reg = REG_OBSERVE_CONFIG;
    ret = ad9361_bist_loopback(&phy, 0);
    if (ret != -EIO || phy.bist_loopback_mode != 2) {
        fprintf(stderr, "BIST loopback hid config read failure or changed cache\n");
        return EXIT_FAILURE;
    }
    fail_read_reg = -1;
    ret = ad9361_bist_loopback(&phy, 3);
    if (ret != -EINVAL || phy.bist_loopback_mode != 2) {
        fprintf(stderr, "BIST loopback accepted invalid mode or changed cache\n");
        return EXIT_FAILURE;
    }
    fail_write_reg = REG_OBSERVE_CONFIG;
    ret = ad9361_bist_loopback(&phy, 0);
    if (ret != -EIO || phy.bist_loopback_mode != 2) {
        fprintf(stderr, "BIST loopback hid config write failure or changed cache\n");
        return EXIT_FAILURE;
    }
    fail_write_reg = -1;

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
	phy.bist_config = 0x1234;
	fail_write_reg = REG_BIST_AND_DATA_PORT_TEST_CONFIG;
	ret = ad9361_write_bist_reg(&phy, 0x5678);
	if (ret != -EIO || phy.bist_config != 0x1234) {
		fprintf(stderr, "BIST register API committed cache after SPI error\n");
		return EXIT_FAILURE;
	}
	fail_write_reg = -1;

	/* RX and TX RF-port setters must keep their cached selection aligned with
	 * a confirmed register write. */
	phy.pdata->rf_rx_input_sel = 0;
	fail_write_reg = REG_INPUT_SELECT;
	ret = ad9361_set_rx_rf_port_input(&phy, 1);
	if (ret != -EIO || phy.pdata->rf_rx_input_sel != 0) {
		fprintf(stderr, "RX RF-port API hid error or committed cache\n");
		return EXIT_FAILURE;
	}
	phy.pdata->rf_tx_output_sel = 0;
	ret = ad9361_set_tx_rf_port_output(&phy, 1);
	if (ret != -EIO || phy.pdata->rf_tx_output_sel != 0 ||
	    ad9361_set_tx_rf_port_output(&phy, 2) != -EINVAL) {
		fprintf(stderr, "TX RF-port API hid error, committed cache, or accepted invalid mode\n");
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

    /* A failed fastlock-profile read must remain an SPI error, never become
     * a low-byte LO value through assignment to uint8_t. */
    {
        uint64_t frequency = UINT64_C(0xfeedface);
        unsigned reads_before = read_count;

        fail_read_reg = REG_RX_FAST_LOCK_PROGRAM_READ;
        ret = ad9361_rx_fastlock_get_freq(&phy, 0, &frequency);
        if (ret != -EIO || frequency != UINT64_C(0xfeedface)) {
            fprintf(stderr, "RX fastlock getter hid profile SPI error: ret=%" PRId32
                    " frequency=%" PRIu64 "\n", ret, frequency);
            return EXIT_FAILURE;
        }
        fail_read_reg = -1;

        reads_before = read_count;
        ret = ad9361_tx_fastlock_get_freq(&phy, 8, &frequency);
        if (ret != -EINVAL || frequency != UINT64_C(0xfeedface) ||
            read_count != reads_before) {
            fprintf(stderr, "TX fastlock getter accepted invalid profile: ret=%" PRId32
                    " frequency=%" PRIu64 " reads=%u/%u\n", ret,
                    frequency, read_count, reads_before);
            return EXIT_FAILURE;
        }

        fail_read_reg = REG_TX_FAST_LOCK_PROGRAM_READ;
        ret = ad9361_tx_fastlock_get_freq(&phy, 0, &frequency);
        if (ret != -EIO || frequency != UINT64_C(0xfeedface)) {
            fprintf(stderr, "TX fastlock getter hid profile SPI error: ret=%" PRId32
                    " frequency=%" PRIu64 "\n", ret, frequency);
            return EXIT_FAILURE;
        }
        fail_read_reg = -1;
    }

    /* Malformed clocks/durations are rejected before arithmetic or writes. */
    {
        struct refclk_scale factor = { .spi = &spi, .phy = &phy };
        struct tx_monitor_control txmon = {0};
        struct gain_control gain = {0};
        uint32_t zero_rate = 0;
        unsigned writes_before = read_count;

        ret = ad9361_clk_factor_round_rate(&factor, 1000000, &zero_rate);
        if (ret != -EINVAL) {
            fprintf(stderr, "Clock factor accepted zero parent rate: ret=%" PRId32 "\n", ret);
            return EXIT_FAILURE;
        }
        ret = ad9361_bbpll_round_rate(&factor, 983040000, &zero_rate);
        if (ret != -EINVAL) {
            fprintf(stderr, "BBPLL accepted zero parent rate: ret=%" PRId32 "\n", ret);
            return EXIT_FAILURE;
        }
        ret = ad9361_set_ref_clk_cycles(&phy, 0);
        if (ret != -EINVAL) {
            fprintf(stderr, "Reference-cycle setup accepted zero clock: ret=%" PRId32 "\n", ret);
            return EXIT_FAILURE;
        }
        ret = ad9361_gc_setup(&phy, &gain);
        if (ret != -EINVAL) {
            fprintf(stderr, "AGC setup accepted zero measurement duration: ret=%" PRId32 "\n", ret);
            return EXIT_FAILURE;
        }
        ret = ad9361_txmon_setup(&phy, &txmon);
        if (ret != -EINVAL) {
            fprintf(stderr, "TX monitor accepted zero duration: ret=%" PRId32 "\n", ret);
            return EXIT_FAILURE;
        }
        phy.rxbbf_div = 0;
        ret = ad9361_rx_adc_setup(&phy, 983040000, 40000000);
        if (ret != -EIO || read_count != writes_before) {
            fprintf(stderr, "RX ADC setup accepted zero filter divider: ret=%" PRId32
                    " reads=%u/%u\n", ret, read_count, writes_before);
            return EXIT_FAILURE;
        }
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

    puts("AD9361 calibration, clock, ENSM, AGC, RSSI, and FIR SPI-error propagation: PASS");
    return EXIT_SUCCESS;
}
