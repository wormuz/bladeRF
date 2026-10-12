/* Regression test: calibration status reads must propagate SPI errors. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../thirdparty/analogdevicesinc/no-OS/drivers/rf-transceiver/ad9361/ad9361.c"
#include "../../thirdparty/analogdevicesinc/no-OS/drivers/rf-transceiver/ad9361/ad9361_util.c"
#include "../../thirdparty/analogdevicesinc/no-OS/drivers/axi_core/axi_adc_core/axi_adc_core.c"
#include "../../thirdparty/analogdevicesinc/no-OS/drivers/axi_core/axi_dac_core/axi_dac_core.c"

static int32_t fake_clock_chain_status;
static unsigned fake_bandwidth_update_count;
static bool fake_mux_commit_parent;
static int32_t fake_mux_parent_status;

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
static int32_t test_clk_mux_set_parent(struct refclk_scale *clk_priv,
                                       uint8_t index);

static int32_t test_clk_mux_set_parent(struct refclk_scale *clk_priv,
                                       uint8_t index)
{
    if (fake_mux_commit_parent)
        clk_priv->mult = index;
    return fake_mux_parent_status;
}

#define ad9361_calculate_rf_clock_chain test_calculate_rf_clock_chain
#define ad9361_set_trx_clock_chain test_set_trx_clock_chain
#define ad9361_update_rf_bandwidth test_update_rf_bandwidth
#define ad9361_clk_mux_set_parent test_clk_mux_set_parent
#include "../../thirdparty/analogdevicesinc/no-OS/drivers/rf-transceiver/ad9361/ad9361_api.c"
#undef ad9361_calculate_rf_clock_chain
#undef ad9361_set_trx_clock_chain
#undef ad9361_update_rf_bandwidth
#undef ad9361_clk_mux_set_parent

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

#include "../../thirdparty/analogdevicesinc/no-OS/drivers/rf-transceiver/ad9361/ad9361_conv.c"

static unsigned read_count;
static unsigned write_count;
static unsigned delay_count;
static int32_t spi_status;
static int32_t fake_spi_remove_status;
static int32_t fake_gpio_remove_status[4];
static unsigned fake_spi_remove_count;
static unsigned fake_gpio_remove_count;
static bool register_map_enabled;
static int32_t fail_read_reg = -1;
static int32_t fail_write_reg = -1;
static uint8_t last_fir_high_byte;
static uint16_t last_spi_write_addr;
static int32_t fail_write_once_reg = -1;
static unsigned fail_write_once_reg_occurrence = 1;
static bool apply_failed_write_once;
static int32_t state_after_ensm_write = -1;
static uint8_t fake_regs[1024];
static unsigned fake_write_counts[1024];
static uint32_t fake_axi_regs[0x5000 / sizeof(uint32_t)];
static unsigned fake_axi_read_count;
static unsigned fake_axi_write_count;
static int32_t fake_axi_fail_write_offset = -1;
static int32_t fake_axi_fail_write_once_offset = -1;
static bool fake_axi_apply_failed_write_once;
static int32_t fake_axi_fail_read_offset = -1;

int32_t no_os_axi_io_read(uint32_t base, uint32_t offset, uint32_t *data)
{
    (void)base;
    if (!data || offset / sizeof(uint32_t) >=
        sizeof(fake_axi_regs) / sizeof(fake_axi_regs[0]))
        return -EINVAL;
    ++fake_axi_read_count;
    if ((int32_t)offset == fake_axi_fail_read_offset)
        return -EIO;
    *data = fake_axi_regs[offset / sizeof(uint32_t)];
    return 0;
}

int32_t no_os_axi_io_write(uint32_t base, uint32_t offset, uint32_t data)
{
    (void)base;
    if (offset / sizeof(uint32_t) >=
        sizeof(fake_axi_regs) / sizeof(fake_axi_regs[0]))
        return -EINVAL;
    ++fake_axi_write_count;
    if ((int32_t)offset == fake_axi_fail_write_once_offset) {
        fake_axi_fail_write_once_offset = -1;
        if (fake_axi_apply_failed_write_once)
            fake_axi_regs[offset / sizeof(uint32_t)] = data;
        fake_axi_apply_failed_write_once = false;
        return -EIO;
    }
    if ((int32_t)offset == fake_axi_fail_write_offset)
        return -EIO;
    fake_axi_regs[offset / sizeof(uint32_t)] = data;
    return 0;
}

void *no_os_malloc(size_t size) { return malloc(size); }
void *no_os_calloc(size_t nitems, size_t size) { return calloc(nitems, size); }
void no_os_free(void *ptr) { free(ptr); }
int32_t no_os_gpio_get(struct no_os_gpio_desc **desc,
                       const struct no_os_gpio_init_param *param)
{
    (void)desc;
    (void)param;
    return -ENOSYS;
}
int32_t no_os_gpio_get_optional(struct no_os_gpio_desc **desc,
                                const struct no_os_gpio_init_param *param)
{
    (void)desc;
    (void)param;
    return -ENOSYS;
}
int32_t no_os_gpio_remove(struct no_os_gpio_desc *desc)
{
    (void)desc;
    if (fake_gpio_remove_count >=
        sizeof(fake_gpio_remove_status) / sizeof(fake_gpio_remove_status[0]))
        return -EINVAL;
    return fake_gpio_remove_status[fake_gpio_remove_count++];
}
int32_t no_os_gpio_direction_output(struct no_os_gpio_desc *desc,
                                    uint8_t value)
{
    (void)desc;
    (void)value;
    return -ENOSYS;
}
int32_t no_os_gpio_set_value(struct no_os_gpio_desc *desc, uint8_t value)
{
    (void)desc;
    (void)value;
    return 0;
}
int32_t no_os_spi_init(struct no_os_spi_desc **desc,
                       const struct no_os_spi_init_param *param)
{
    (void)desc;
    (void)param;
    return -ENOSYS;
}
int32_t no_os_spi_remove(struct no_os_spi_desc *desc)
{
    (void)desc;
    ++fake_spi_remove_count;
    return fake_spi_remove_status;
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
    uint16_t i;

    (void)desc;
    if (bytes_number < 3)
        return -EINVAL;

    command = (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
    reg = command & 0x3ff;
    if (command & AD_WRITE) {
        ++write_count;
        last_spi_write_addr = reg;
        if (register_map_enabled && reg == REG_RX_FILTER_COEF_DATA_2 &&
            fake_regs[REG_RX_FILTER_COEF_ADDR] == 0) {
            last_fir_high_byte = data[2];
        }
        ++fake_write_counts[reg];
        if ((int32_t)reg == fail_write_once_reg) {
            if (fail_write_once_reg_occurrence > 1) {
                --fail_write_once_reg_occurrence;
            } else {
                fail_write_once_reg = -1;
                fail_write_once_reg_occurrence = 1;
                if (apply_failed_write_once && register_map_enabled)
                    fake_regs[reg] = data[2];
                apply_failed_write_once = false;
                return -EIO;
            }
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
    if (spi_status < 0) return spi_status;
    for (i = 0; i < bytes_number - 2; i++) {
        uint16_t read_reg = reg - i;
        if ((int32_t)read_reg == fail_read_reg)
            return -EIO;
        data[2 + i] = register_map_enabled ? fake_regs[read_reg] :
                      (uint8_t)spi_status;
    }
    return 0;
}

int main(void)
{
    struct ad9361_rf_phy phy = {0};
    struct ad9361_rf_phy phy_without_spi = {0};
    struct ad9361_phy_platform_data pdata = {0};
    struct gain_control gain = {0};
    struct no_os_spi_desc spi = {0};
    int32_t ret;
    struct axi_adc calibration_adc = { .num_channels = 2 };
    struct axi_adc adc_init_error = { .name = "fake", .clock_hz = 123 };
    struct axi_adc *initialized_adc = &calibration_adc;
    struct axi_adc_init invalid_adc_init = {0};
    struct axi_dac tx_dac = { .num_channels = 2, .clock_hz = 30000000 };
    struct axi_dac *initialized_dac = &tx_dac;
    struct axiadc_chip_info loopback_chip = { .num_channels = 2 };
    struct axiadc_converter loopback_conv = { .chip_info = &loopback_chip };
    struct axi_adc loopback_adc = {0};
    struct ad9361_rf_phy loopback_phy = {
        .adc_conv = &loopback_conv,
        .rx_adc = &loopback_adc,
        .pdata = &pdata,
        .bist_loopback_valid = true,
    };
    int32_t calib_integer = INT32_MIN;
    int32_t calib_fraction = INT32_MIN;
    uint64_t sampling_frequency = UINT64_MAX;
    uint32_t dds_frequency = UINT32_C(0x12345678);
    uint32_t sine_length = UINT32_MAX;
    unsigned axi_accesses_before;
    uint64_t lo_frequency = UINT64_MAX;
    uint32_t tx_attenuation = UINT32_MAX;
    uint32_t rfpll_rate = UINT32_MAX;
    uint32_t tx_rssi = UINT32_MAX;
    uint32_t bandwidth = UINT32_MAX;
    uint32_t sampling_rate = UINT32_MAX;
    uint32_t rx_rf_port = UINT32_MAX;
    uint32_t tx_rf_port = UINT32_MAX;
    int32_t rx_gain = INT32_MAX;
        struct rf_rx_gain current_rx_gain = {0};
    uint8_t rx_gc_mode = UINT8_MAX;
    uint8_t tracking_flag = UINT8_MAX;
    struct rf_rssi rx_rssi = { .ant = UINT8_MAX };
    AD9361_RXFIRConfig rx_fir = {0};
    AD9361_TXFIRConfig tx_fir = {0};

    /* AXI calibration helpers must reject invalid channels and pointers
     * before touching register space; bias is integer-only and initializes
     * both output components on read. */
    axi_accesses_before = fake_axi_read_count + fake_axi_write_count;
    if (axi_adc_set_calib_scale(&calibration_adc, 2, 1, 0) != -EINVAL ||
        axi_adc_set_calib_bias(NULL, 0, 0, 0) != -EINVAL ||
        axi_adc_set_pnsel(&calibration_adc, 0, (enum axi_adc_pn_sel)3) != -EINVAL ||
        axi_adc_pn_mon(NULL, AXI_ADC_PN9, 0) != -EINVAL ||
        axi_adc_get_sampling_freq(&calibration_adc, 2,
                                  &sampling_frequency) != -EINVAL ||
        sampling_frequency != UINT64_MAX ||
        axi_adc_idelay_set(&calibration_adc, 32, 0) != -EINVAL ||
        axi_adc_delay_set(&calibration_adc, 33, 0) != -EINVAL ||
        axi_adc_read(NULL, 0, &fake_axi_regs[0]) != -EINVAL ||
        axi_adc_update_active_channels(NULL, 1) != -EINVAL ||
        axi_adc_init_begin(&initialized_adc, NULL) != -EINVAL ||
        initialized_adc != NULL ||
        axi_adc_init(&initialized_adc, NULL) != -EINVAL ||
        initialized_adc != NULL ||
        axi_adc_get_calib_scale(&calibration_adc, 0, NULL,
                                &calib_fraction) != -EINVAL ||
        axi_adc_get_calib_bias(&calibration_adc, 0, &calib_integer,
                               NULL) != -EINVAL ||
        axi_adc_set_calib_bias(&calibration_adc, 0, 3, 1) != -EINVAL ||
        fake_axi_read_count + fake_axi_write_count != axi_accesses_before)
        return EXIT_FAILURE;
    fake_axi_fail_read_offset = AXI_ADC_REG_STATUS;
    if (axi_adc_init_finish(&adc_init_error) != -EIO ||
        adc_init_error.clock_hz != 0)
        return EXIT_FAILURE;
    fake_axi_fail_read_offset = -1;
    if (axi_adc_update_active_channels(&calibration_adc, 4) != -EINVAL ||
        calibration_adc.mask_valid)
        return EXIT_FAILURE;
    fake_axi_fail_write_offset = AXI_ADC_REG_CHAN_CNTRL(1);
    if (axi_adc_update_active_channels(&calibration_adc, 3) != -EIO ||
        calibration_adc.mask_valid)
        return EXIT_FAILURE;
    fake_axi_fail_write_offset = -1;
    if (axi_adc_update_active_channels(&calibration_adc, 3) != 0 ||
        !calibration_adc.mask_valid || calibration_adc.mask != 3)
        return EXIT_FAILURE;
    fake_axi_regs[AXI_ADC_REG_CHAN_CNTRL_1(0) / sizeof(uint32_t)] =
        AXI_ADC_DCFILT_OFFSET(0xfffe);
    if (axi_adc_get_calib_bias(&calibration_adc, 0, &calib_integer,
                               &calib_fraction) != 0 ||
        calib_integer != 0xfffe || calib_fraction != 0)
        return EXIT_FAILURE;

    if (axi_dac_read(NULL, 0, &fake_axi_regs[0]) != -EINVAL ||
        axi_dac_set_ddr(NULL, true) != -EINVAL ||
        axi_dac_set_datasel(&tx_dac, 2, AXI_DAC_DATA_SEL_DMA) != -EINVAL ||
        axi_dac_dds_set_phase(&tx_dac, 0, 360001) != -EINVAL ||
        axi_dac_init_begin(&initialized_dac, NULL) != -EINVAL ||
        initialized_dac != NULL ||
        axi_dac_init(&initialized_dac, NULL) != -EINVAL ||
        initialized_dac != NULL ||
        axi_dac_dds_get_frequency(&tx_dac, 4, &dds_frequency) != -EINVAL ||
        dds_frequency != UINT32_C(0x12345678))
        return EXIT_FAILURE;
    fake_axi_fail_read_offset = 0x48;
    axi_accesses_before = fake_axi_write_count;
    if (axi_dac_set_ddr(&tx_dac, true) != -EIO ||
        fake_axi_write_count != axi_accesses_before)
        return EXIT_FAILURE;
    fake_axi_fail_read_offset = AXI_DAC_REG_DDS_INIT_INCR(0);
    if (axi_dac_dds_set_frequency(&tx_dac, 0, 1000000) != -EIO)
        return EXIT_FAILURE;
    fake_axi_fail_read_offset = -1;
    fake_axi_fail_write_offset = 0;
    if (axi_dac_set_sine_lut(&tx_dac, 0, &sine_length) != -EIO ||
        sine_length != 0)
        return EXIT_FAILURE;
    fake_axi_fail_write_offset = -1;
    struct refclk_scale invalid_rfpll_clock = {0};
    struct refclk_scale invalid_clock_factor = {0};
    phy.spi = &spi;
    phy.pdata = &pdata;
    phy_without_spi.pdata = &pdata;

    /* RX/TX channel enums are masks (RX2/TX2 == 2), while the selected
     * single-channel register field is shifted to bit 7. Verify that the
     * ADI helpers encode channel 2 as field value 1 and preserve channel 1. */
    {
        bool saved_register_map_enabled = register_map_enabled;
        unsigned saved_read_count = read_count;
        unsigned saved_write_count = write_count;
        uint8_t saved_rx_enable = fake_regs[REG_RX_ENABLE_FILTER_CTRL];
        uint8_t saved_tx_enable = fake_regs[REG_TX_ENABLE_FILTER_CTRL];

        register_map_enabled = true;
        fake_regs[REG_RX_ENABLE_FILTER_CTRL] = 0x05;
        fake_regs[REG_TX_ENABLE_FILTER_CTRL] = 0x05;
        if (ad9361_en_dis_rx(&phy, RX_2, RX_2) != 0 ||
            fake_regs[REG_RX_ENABLE_FILTER_CTRL] != 0x85 ||
            ad9361_en_dis_rx(&phy, RX_2, RX_DISABLE) != 0 ||
            fake_regs[REG_RX_ENABLE_FILTER_CTRL] != 0x05 ||
            ad9361_en_dis_tx(&phy, TX_2, TX_2) != 0 ||
            fake_regs[REG_TX_ENABLE_FILTER_CTRL] != 0x85 ||
            ad9361_en_dis_tx(&phy, TX_2, TX_DISABLE) != 0 ||
            fake_regs[REG_TX_ENABLE_FILTER_CTRL] != 0x05) {
            fprintf(stderr, "RX2/TX2 channel enable field encoding failed\n");
            return EXIT_FAILURE;
        }
        fake_regs[REG_RX_ENABLE_FILTER_CTRL] = saved_rx_enable;
        fake_regs[REG_TX_ENABLE_FILTER_CTRL] = saved_tx_enable;
        register_map_enabled = saved_register_map_enabled;
        read_count = saved_read_count;
        write_count = saved_write_count;
    }

    {
        struct ad9361_rf_phy cleared_phy = {
            .rfpll_requested_valid = { true, true },
            .bbpll_initialized = true,
            .current_bw_valid = true,
            .rfpll_source_valid = { true, true },
            .fir_config_valid = true,
            .rx_fir_coefficients_valid_mask = 3,
            .tx_fir_coefficients_valid_mask = 3,
            .filt_valid = true,
            .rf_port_select_valid = true,
            .tracking_config_valid = true,
            .tx1_atten_cached_valid = true,
            .tx2_atten_cached_valid = true,
            .tx_mute_restore_valid = true,
            .tx1_muted_valid = true,
            .tx2_muted_valid = true,
            .bist_loopback_valid = true,
            .bist_config_valid = true,
        };

        ad9361_clear_state(&cleared_phy);
        if (cleared_phy.rfpll_requested_valid[0] ||
            cleared_phy.rfpll_requested_valid[1] ||
            cleared_phy.bbpll_initialized ||
            cleared_phy.current_bw_valid ||
            cleared_phy.rfpll_source_valid[0] ||
            cleared_phy.rfpll_source_valid[1] ||
            cleared_phy.fir_config_valid ||
            cleared_phy.rx_fir_coefficients_valid_mask ||
            cleared_phy.tx_fir_coefficients_valid_mask ||
            cleared_phy.filt_valid || cleared_phy.rf_port_select_valid ||
            cleared_phy.tracking_config_valid ||
            cleared_phy.tx1_atten_cached_valid ||
            cleared_phy.tx2_atten_cached_valid ||
            cleared_phy.tx_mute_restore_valid ||
            cleared_phy.tx1_muted_valid || cleared_phy.tx2_muted_valid ||
            cleared_phy.bist_loopback_valid || cleared_phy.bist_config_valid) {
            fprintf(stderr,
                    "AD9361 clear-state retained prior RF validity\n");
            return EXIT_FAILURE;
        }
    }

    /* A mux may commit its new source and then fail while restoring ENSM or
     * relocking the internal PLL. Keep the software source aligned only when
     * the mux confirms that commit; roll it back when the mux stayed put. */
    {
        struct refclk_scale rx_mux = { .phy = &phy, .source = RX_RFPLL };
        struct refclk_scale tx_mux = { .phy = &phy, .source = TX_RFPLL };
        struct refclk_scale *old_rx_mux = phy.ref_clk_scale[RX_RFPLL];
        struct refclk_scale *old_tx_mux = phy.ref_clk_scale[TX_RFPLL];
        enum dev_id old_dev_sel = phy.dev_sel;
        phy.ref_clk_scale[RX_RFPLL] = &rx_mux;
        phy.ref_clk_scale[TX_RFPLL] = &tx_mux;
        phy.dev_sel = ID_AD9361;
        fake_mux_parent_status = -EIO;
        fake_mux_commit_parent = true;
        pdata.use_ext_rx_lo = INT_LO;
        if (ad9361_set_rx_lo_int_ext(&phy, EXT_LO) != -EIO ||
            pdata.use_ext_rx_lo != EXT_LO) {
            fprintf(stderr,
                    "RX LO source cache rolled back after mux commit\n");
            return EXIT_FAILURE;
        }
        pdata.use_ext_tx_lo = INT_LO;
        if (ad9361_set_tx_lo_int_ext(&phy, EXT_LO) != -EIO ||
            pdata.use_ext_tx_lo != EXT_LO) {
            fprintf(stderr,
                    "TX LO source cache rolled back after mux commit\n");
            return EXIT_FAILURE;
        }

        fake_mux_commit_parent = false;
        rx_mux.mult = INT_LO;
        pdata.use_ext_rx_lo = INT_LO;
        if (ad9361_set_rx_lo_int_ext(&phy, EXT_LO) != -EIO ||
            pdata.use_ext_rx_lo != INT_LO) {
            fprintf(stderr,
                    "RX LO source cache changed without mux commit\n");
            return EXIT_FAILURE;
        }
        tx_mux.mult = INT_LO;
        pdata.use_ext_tx_lo = INT_LO;
        if (ad9361_set_tx_lo_int_ext(&phy, EXT_LO) != -EIO ||
            pdata.use_ext_tx_lo != INT_LO) {
            fprintf(stderr,
                    "TX LO source cache changed without mux commit\n");
            return EXIT_FAILURE;
        }

        fake_mux_parent_status = 0;
        phy.ref_clk_scale[RX_RFPLL] = old_rx_mux;
        phy.ref_clk_scale[TX_RFPLL] = old_tx_mux;
        phy.dev_sel = old_dev_sel;
        pdata.use_ext_rx_lo = INT_LO;
        pdata.use_ext_tx_lo = INT_LO;
    }

    /* Channel selectors and enable masks must not silently truncate bits. */
    {
        unsigned writes_before = write_count;
        if (ad9361_en_dis_tx(&phy, 0, TX_1) != -EINVAL ||
            ad9361_en_dis_tx(&phy, TX_1 | 4, TX_1) != -EINVAL ||
            ad9361_en_dis_tx(&phy, TX_1, TX_1 | 4) != -EINVAL ||
            ad9361_en_dis_rx(&phy, 0, RX_1) != -EINVAL ||
            ad9361_en_dis_rx(&phy, RX_1 | 4, RX_1) != -EINVAL ||
            ad9361_en_dis_rx(&phy, RX_1, RX_1 | 4) != -EINVAL ||
            write_count != writes_before) {
            fprintf(stderr, "AD9361 channel masks were not rejected pre-SPI\n");
            return EXIT_FAILURE;
        }
    }

    /* Public lifecycle boundaries must reject missing arguments before
     * allocation, SPI, or teardown side effects. */
    {
        AD9361_InitParam init_param = {0};
        struct ad9361_rf_phy *init_result = &phy;
        unsigned reads_before = read_count;
        unsigned writes_before = write_count;

        if (ad9361_init(NULL, &init_param) != -EINVAL ||
            ad9361_init(&init_result, NULL) != -EINVAL || init_result != NULL ||
            ad9361_remove(NULL) != -EINVAL ||
            read_count != reads_before || write_count != writes_before) {
            fprintf(stderr,
                    "AD9361 lifecycle API accepted missing state or touched SPI\n");
            return EXIT_FAILURE;
        }
    }

    /* An init failure must still attempt every acquired-resource teardown,
     * and a failed teardown must be returned instead of hiding behind the
     * original GPIO-init error. */
    {
        AD9361_InitParam init_param = {0};
        unsigned failure_site;

        for (failure_site = 0; failure_site <= 4; ++failure_site) {
            struct ad9361_rf_phy *init_result = NULL;
            int32_t expected = failure_site == 0 ? -ENOSYS : -EIO;

            memset(fake_gpio_remove_status, 0,
                   sizeof(fake_gpio_remove_status));
            if (failure_site != 0)
                fake_gpio_remove_status[failure_site - 1] = -EIO;
            fake_gpio_remove_count = 0;
            fake_spi_remove_count = 0;

            if (ad9361_init(&init_result, &init_param) != expected ||
                init_result != NULL || fake_gpio_remove_count != 4 ||
                fake_spi_remove_count != 0) {
                fprintf(stderr,
                        "AD9361 init cleanup hid or skipped failure %u\n",
                        failure_site);
                return EXIT_FAILURE;
            }
        }
    }

    /* Teardown must attempt every platform cleanup and report the first
     * failure. ad9361_remove() consumes/frees the PHY even on failure, which
     * the NIOS owner must account for before propagating the status. */
    {
        unsigned failure_site;

        for (failure_site = 0; failure_site <= 4; ++failure_site) {
            struct ad9361_rf_phy *remove_phy = calloc(1, sizeof(*remove_phy));

            if (!remove_phy) {
                fprintf(stderr, "failed to allocate teardown test PHY\n");
                return EXIT_FAILURE;
            }
            remove_phy->spi = (struct no_os_spi_desc *)(uintptr_t)1;
            remove_phy->gpio_desc_resetb =
                (struct no_os_gpio_desc *)(uintptr_t)1;
            remove_phy->gpio_desc_sync =
                (struct no_os_gpio_desc *)(uintptr_t)1;
            remove_phy->gpio_desc_cal_sw1 =
                (struct no_os_gpio_desc *)(uintptr_t)1;
            remove_phy->gpio_desc_cal_sw2 =
                (struct no_os_gpio_desc *)(uintptr_t)1;
            fake_spi_remove_status = failure_site == 0 ? -EIO : 0;
            memset(fake_gpio_remove_status, 0,
                   sizeof(fake_gpio_remove_status));
            if (failure_site != 0)
                fake_gpio_remove_status[failure_site - 1] = -EIO;
            fake_spi_remove_count = 0;
            fake_gpio_remove_count = 0;

            if (ad9361_remove(remove_phy) != -EIO ||
                fake_spi_remove_count != 1 || fake_gpio_remove_count != 4) {
                fprintf(stderr,
                        "AD9361 teardown hid a cleanup error or skipped cleanup %u\n",
                        failure_site);
                return EXIT_FAILURE;
            }
        }
    }

    /* post_setup is public and can be reached outside normal init. Its shared
     * preflight must reject each missing dependency before AXI is touched. */
    {
        struct ad9361_rf_phy incomplete = {0};
        struct axiadc_converter converter = {0};
        struct axiadc_chip_info chip_info = {0};
        struct axi_adc rx_adc = {0};
        struct refclk_scale sample_clock = {0};
		struct {
			char buf[AD9361_TIMING_ANALYSIS_BUFFER_SIZE];
			char canary;
		} report = {{0}, (char)0x6b};
		uint8_t timing_field[16][16] = {{0}};
		int32_t report_len;
        struct {
            char buf[4];
            char canary;
        } bounded_output = {{0}, (char)0x5a};
        size_t output_len = 0;

        incomplete.pdata = &pdata;
        if (ad9361_post_setup_validate(NULL) != -EINVAL ||
            ad9361_post_setup_validate(&incomplete) != -EINVAL)
            return EXIT_FAILURE;

        incomplete.adc_conv = &converter;
        if (ad9361_post_setup_validate(&incomplete) != -EINVAL)
            return EXIT_FAILURE;

        converter.chip_info = &chip_info;
        chip_info.num_channels = 4;
        if (ad9361_post_setup_validate(&incomplete) != -EINVAL)
            return EXIT_FAILURE;

        incomplete.rx_adc = &rx_adc;
        incomplete.pdata = NULL;
        if (ad9361_post_setup_validate(&incomplete) != -EINVAL)
            return EXIT_FAILURE;

        incomplete.pdata = &pdata;
        incomplete.spi = &spi;
        if (ad9361_post_setup_validate(&incomplete) != 0)
            return EXIT_FAILURE;

        incomplete.ref_clk_scale[RX_SAMPL_CLK] = &sample_clock;
		if (ad9361_timing_analysis_validate(&incomplete, NULL,
						    sizeof(report.buf)) != -EINVAL ||
		    ad9361_timing_analysis_validate(&incomplete, report.buf, 0) != -ENOSPC ||
		    ad9361_timing_analysis_validate(
			    &incomplete, report.buf,
			    AD9361_TIMING_ANALYSIS_BUFFER_SIZE - 1) != -ENOSPC ||
		    ad9361_timing_analysis_validate(
			    &incomplete, report.buf, sizeof(report.buf)) != 0 ||
            ad9361_timing_append(bounded_output.buf,
                                 sizeof(bounded_output.buf), &output_len,
                                 "%s", "more than four bytes") != -ENOSPC ||
            bounded_output.canary != (char)0x5a ||
            bounded_output.buf[sizeof(bounded_output.buf) - 1] != '\0') {
            fprintf(stderr,
                    "AD9361 timing-analysis validation exceeded its buffer\n");
            return EXIT_FAILURE;
        }
		report_len = ad9361_format_timing_report(
			report.buf, sizeof(report.buf), UINT32_MAX, timing_field);
		if (report_len <= 0 || report_len >= (int32_t)sizeof(report.buf) ||
		    report.buf[report_len] != '\0' || report.canary != (char)0x6b ||
		    ad9361_format_timing_report(
			    report.buf, AD9361_TIMING_ANALYSIS_BUFFER_SIZE - 1,
			    UINT32_MAX, timing_field) != -ENOSPC ||
		    report.canary != (char)0x6b) {
			fprintf(stderr,
				"AD9361 timing report formatter exceeded its buffer\n");
			return EXIT_FAILURE;
		}
    }

    /* The exported gain-control primitive must reject incomplete state
     * before it derives the SPI pointer or performs any register access. */
    {
        struct ad9361_rf_phy phy_without_pdata = { .spi = &spi };
        struct ad9361_rf_phy gain_table_phy = {0};
        struct rf_gain_ctrl gain_ctrl = {
            .ant = 1,
            .mode = RF_GAIN_MGC,
        };
        uint8_t raw_byte = 0xa5;
        uint32_t raw_value = UINT32_C(0xdeadbeef);
        const unsigned reads_before = read_count;
        const unsigned writes_before = write_count;

        ad9361_clear_state(NULL);
        if (ad9361_init_gain_tables(NULL) != -EINVAL ||
            ad9361_init_gain_tables(&gain_table_phy) != 0 ||
            gain_table_phy.current_table != UINT32_MAX ||
            gain_table_phy.gt_info != ad9361_adi_gt_info ||
            ad9361_set_gain_ctrl_mode(NULL, &gain_ctrl) != -EINVAL ||
            ad9361_set_gain_ctrl_mode(&phy_without_spi, &gain_ctrl) != -EINVAL ||
            ad9361_set_gain_ctrl_mode(&phy_without_pdata, &gain_ctrl) != -EINVAL ||
            ad9361_set_gain_ctrl_mode(&phy, NULL) != -EINVAL ||
            ad9361_ensm_set_state(NULL, ENSM_STATE_ALERT, false) != -EINVAL ||
            ad9361_ensm_set_state(&phy_without_spi, ENSM_STATE_ALERT, false) != -EINVAL ||
            ad9361_set_ensm_mode(NULL, true, false) != -EINVAL ||
            ad9361_get_trx_clock_chain(NULL, NULL, NULL) != -EINVAL ||
            ad9361_get_trx_clock_chain(&phy, NULL, NULL) != -EINVAL ||
            ad9361_calculate_rf_clock_chain(NULL, 1000000, 0, NULL, NULL) != -EINVAL ||
            ad9361_calculate_rf_clock_chain(&phy, 1000000, 0, NULL, NULL) != -EINVAL ||
            ad9361_set_trx_clock_chain_freq(NULL, 1000000) != -EINVAL ||
            ad9361_reg_read(NULL, 0, &raw_value) != -EINVAL ||
            ad9361_reg_read(&phy_without_spi, 0, &raw_value) != -EINVAL ||
            ad9361_reg_read(&phy, 0, NULL) != -EINVAL ||
            raw_value != UINT32_C(0xdeadbeef) ||
            ad9361_reg_write(NULL, 0, 0) != -EINVAL ||
            ad9361_reg_write(&phy_without_spi, 0, 0) != -EINVAL ||
            ad9361_reset(NULL) != -EINVAL ||
            ad9361_reset(&phy_without_spi) != -EINVAL ||
            ad9361_en_dis_tx(NULL, TX_1, 1) != -EINVAL ||
            ad9361_en_dis_rx(NULL, RX_1, 1) != -EINVAL ||
            ad9361_ensm_get_state(NULL) != -EINVAL ||
            ad9361_set_dcxo_tune(NULL, 0, 0) != -EINVAL ||
            ad9361_get_temp(NULL) != -EINVAL ||
            ad9361_update_rf_bandwidth(NULL, 1000000, 1000000) != -EINVAL ||
            ad9361_validate_rf_bw(NULL, 18000000) != 0 ||
            ad9361_spi_readm(NULL, 0, &raw_byte, 1) != -EINVAL ||
            ad9361_spi_readm(&spi, 0, NULL, 1) != -EINVAL ||
            ad9361_spi_readm(&spi, 0, &raw_byte, 0) != -EINVAL ||
            ad9361_spi_readm(&spi, 0, &raw_byte, 9) != -EINVAL ||
            ad9361_spi_readm(&spi, 0x400, &raw_byte, 1) != -EINVAL ||
            ad9361_spi_readm(&spi, 0x3ff, &raw_byte, 2) != -EINVAL ||
            raw_byte != 0xa5 ||
            ad9361_spi_read(NULL, 0) != -EINVAL ||
            ad9361_spi_write(NULL, 0, 0) != -EINVAL ||
            ad9361_spi_write(&spi, 0x400, 0) != -EINVAL ||
            ad9361_spi_write(&spi, 0, 0x100) != -EINVAL ||
            ad9361_1rx1tx_channel_map(NULL, false, 0) != -EINVAL ||
            ad9361_mcs(NULL, 1) != -EINVAL ||
            ad9361_gt(NULL) != 0 ||
            ad9361_setup(NULL) != -EINVAL ||
            ad9361_validate_enable_fir(NULL) != -EINVAL ||
            ad9361_register_clocks(NULL) != -EINVAL ||
            ad9361_unregister_clocks(NULL) != -EINVAL ||
            ad9361_rssi_gain_step_calib(NULL) != -EINVAL ||
            ad9361_rssi_program_lna_gain(NULL) != -EINVAL ||
            ad9361_rssi_write_err_tbl(NULL) != -EINVAL ||
            read_count != reads_before || write_count != writes_before) {
            fprintf(stderr,
                    "AD9361 API accepted incomplete state or touched SPI\n");
            return EXIT_FAILURE;
        }
    }

    /* A failed filter/calibration/tracking/ENSM step preserves the last
     * requested values for recovery, but marks them untrusted so a request
     * matching those values cannot be mistaken for a hardware no-op. */
    {
        const uint32_t old_rx_bw = 18000000;
        const uint32_t old_tx_bw = 16000000;
        uint32_t queried_bw = UINT32_C(0xdeadbeef);
        phy.current_rx_bw_Hz = old_rx_bw;
        phy.current_tx_bw_Hz = old_tx_bw;
        phy.current_bw_valid = true;
        if (ad9361_commit_rf_bandwidth_cache_if_complete(
                &phy, 20000000, 18000000, -EIO, 0, 0) != -EIO ||
            phy.current_rx_bw_Hz != old_rx_bw ||
            phy.current_tx_bw_Hz != old_tx_bw ||
            phy.current_bw_valid ||
            ad9361_commit_rf_bandwidth_cache_if_complete(
                &phy, 20000000, 18000000, 0, -EIO, 0) != -EIO ||
            phy.current_rx_bw_Hz != old_rx_bw ||
            phy.current_tx_bw_Hz != old_tx_bw ||
            phy.current_bw_valid ||
            ad9361_commit_rf_bandwidth_cache_if_complete(
                &phy, 20000000, 18000000, 0, 0, -EIO) != -EIO ||
            phy.current_rx_bw_Hz != old_rx_bw ||
            phy.current_tx_bw_Hz != old_tx_bw ||
            phy.current_bw_valid ||
            ad9361_commit_rf_bandwidth_cache_if_complete(
                &phy, 20000000, 18000000, 0, 0, 0) != 0 ||
            phy.current_rx_bw_Hz != 20000000 ||
            phy.current_tx_bw_Hz != 18000000 ||
            !phy.current_bw_valid) {
            fprintf(stderr,
                    "Bandwidth cache changed before full RF recovery success\n");
            return EXIT_FAILURE;
        }

        phy.current_bw_valid = false;
        queried_bw = UINT32_C(0xdeadbeef);
        if (ad9361_get_rx_rf_bandwidth(&phy, &queried_bw) != -EIO ||
            queried_bw != UINT32_C(0xdeadbeef) ||
            ad9361_get_tx_rf_bandwidth(&phy, &queried_bw) != -EIO ||
            queried_bw != UINT32_C(0xdeadbeef)) {
            fprintf(stderr,
                    "Untrusted bandwidth cache was exposed as hardware state\n");
            return EXIT_FAILURE;
        }
    }

    /* FIR text is caller supplied through the IIO attribute. Reject more
     * taps than the fixed coefficient arrays can hold before any SPI write. */
    {
        char oversized_fir[2048];
        size_t used = 0;
        unsigned reads_before = read_count;
        unsigned writes_before = write_count;
        int written;
        unsigned tap;

        written = snprintf(oversized_fir, sizeof(oversized_fir),
                           "TX 1 GAIN 0 INT 1\nRX 1 GAIN 0 DEC 1\n");
        if (written < 0 || (size_t)written >= sizeof(oversized_fir))
            return EXIT_FAILURE;
        used = (size_t)written;
        for (tap = 0; tap < 129; ++tap) {
            written = snprintf(oversized_fir + used,
                               sizeof(oversized_fir) - used, "1,1\n");
            if (written < 0 || (size_t)written >= sizeof(oversized_fir) - used)
                return EXIT_FAILURE;
            used += (size_t)written;
        }

        phy.filt_valid = true;
        if (ad9361_parse_fir(&phy, oversized_fir, (uint32_t)used) != -EINVAL ||
            phy.filt_valid || read_count != reads_before ||
            write_count != writes_before ||
            ad9361_parse_fir(NULL, oversized_fir, (uint32_t)used) != -EINVAL ||
            ad9361_parse_fir(&phy, NULL, 1) != -EINVAL ||
            ad9361_parse_fir(&phy, oversized_fir, 0) != -EINVAL) {
            fprintf(stderr,
                    "Invalid FIR input was not rejected before touching RFIC\n");
            return EXIT_FAILURE;
        }
    }

    /* Public paired-path and diagnostic APIs must reject missing state or
     * output storage before touching SPI or publishing caller-visible data. */
    {
        struct ad9361_rf_phy phy_without_pdata = { .spi = &spi };
        struct ad9361_phy_platform_data mode_pdata = {0};
        struct no_os_clk mode_ref_clock = {0};
        struct ad9361_rf_phy phy_missing_mode_clocks = {
            .spi = &spi,
            .pdata = &mode_pdata,
            .clk_refin = &mode_ref_clock,
            .adc_conv = (void *)1,
        };
        struct ad9361_rf_phy phy_missing_mode_refclk = {
            .spi = &spi,
            .pdata = &mode_pdata,
        };
        struct ad9361_rf_phy phy_missing_mode_adc = {
            .spi = &spi,
            .pdata = &mode_pdata,
            .clk_refin = &mode_ref_clock,
        };
        uint32_t rx_clks[NUM_AD9361_CLKS] = {0};
        uint32_t tx_clks[NUM_AD9361_CLKS] = {0};
        uint32_t rate_governor = UINT32_MAX;
        uint32_t ensm_mode = UINT32_MAX;
        int32_t temperature = INT32_MAX;
        uint8_t tracking = UINT8_MAX;
        struct ad9361_dig_tune_data dig_tune = {0};
        unsigned reads_before = read_count;
        unsigned writes_before = write_count;

        mode_ref_clock.rate = 38400000;
        phy.rate_governor = UINT32_MAX;

        if (ad9361_do_mcs(NULL, &phy) != -EINVAL ||
            ad9361_do_mcs(&phy, NULL) != -EINVAL ||
            ad9361_do_mcs(&phy, &phy_without_spi) != -EINVAL ||
            ad9361_set_trx_path_clks(NULL, rx_clks, tx_clks) != -EINVAL ||
            ad9361_set_trx_path_clks(&phy_without_pdata, rx_clks, tx_clks) != -EINVAL ||
            ad9361_set_trx_path_clks(&phy, NULL, tx_clks) != -EINVAL ||
            ad9361_set_trx_path_clks(&phy, rx_clks, NULL) != -EINVAL ||
            ad9361_get_trx_path_clks(NULL, rx_clks, tx_clks) != -EINVAL ||
            ad9361_get_trx_path_clks(&phy, NULL, tx_clks) != -EINVAL ||
            ad9361_get_trx_path_clks(&phy, rx_clks, NULL) != -EINVAL ||
            ad9361_set_trx_clock_chain(NULL, rx_clks, tx_clks) != -EINVAL ||
            ad9361_set_trx_clock_chain(&phy_without_pdata, rx_clks, tx_clks) != -EINVAL ||
            ad9361_set_trx_clock_chain(&phy_without_spi, rx_clks, tx_clks) != -EINVAL ||
            ad9361_set_trx_clock_chain(&phy, NULL, tx_clks) != -EINVAL ||
            ad9361_set_trx_clock_chain(&phy, rx_clks, NULL) != -EINVAL ||
            ad9361_set_no_ch_mode(NULL, 1) != -EINVAL ||
            ad9361_set_no_ch_mode(&phy_without_pdata, 1) != -EINVAL ||
            ad9361_set_no_ch_mode(&phy_without_spi, 1) != -EINVAL ||
            ad9361_set_no_ch_mode(&phy_missing_mode_refclk, 2) != -EINVAL ||
            ad9361_set_no_ch_mode(&phy_missing_mode_adc, 2) != -EINVAL ||
            ad9361_set_no_ch_mode(&phy_missing_mode_clocks, 2) != -EINVAL ||
            mode_pdata.rx2tx2 != 0 || read_count != reads_before ||
            write_count != writes_before ||
            ad9361_set_trx_fir_en_dis(NULL, 1) != -EINVAL ||
            ad9361_set_trx_fir_en_dis(&phy_without_pdata, 1) != -EINVAL ||
            ad9361_set_trx_fir_en_dis(&phy_without_spi, 1) != -EINVAL ||
            ad9361_set_trx_rate_gov(NULL, 0) != -EINVAL ||
            ad9361_set_trx_rate_gov(&phy, 2) != -EINVAL ||
            ad9361_get_trx_rate_gov(NULL, &rate_governor) != -EINVAL ||
            ad9361_get_trx_rate_gov(&phy, NULL) != -EINVAL ||
            ad9361_do_calib(NULL, RFDC_CAL, -1) != -EINVAL ||
            ad9361_do_calib(&phy, RFDC_CAL, -1) != -EINVAL ||
            ad9361_do_calib(&phy, UINT32_MAX, -1) != -EINVAL ||
            ad9361_do_calib_timeout(NULL, RFDC_CAL, -1, 1000) != -EINVAL ||
            ad9361_do_calib_timeout(&phy_without_spi, RFDC_CAL, -1, 1000) != -EINVAL ||
            ad9361_do_calib_timeout(&phy, RFDC_CAL, -1, 0) != -EINVAL ||
            ad9361_do_calib_timeout(&phy, RFDC_CAL, -1, 1000) != -EINVAL ||
            ad9361_do_calib(&phy_without_pdata, RFDC_CAL, -1) != -EINVAL ||
            ad9361_do_calib(&phy_without_spi, RFDC_CAL, -1) != -EINVAL ||
            ad9361_trx_load_enable_fir(NULL, rx_fir, tx_fir) != -EINVAL ||
            ad9361_trx_load_enable_fir(&phy_without_spi, rx_fir, tx_fir) != -EINVAL ||
            ad9361_do_dcxo_tune_coarse(NULL, 1) != -EINVAL ||
            ad9361_do_dcxo_tune_coarse(&phy_without_pdata, 1) != -EINVAL ||
            ad9361_do_dcxo_tune_coarse(&phy_without_spi, 1) != -EINVAL ||
            ad9361_do_dcxo_tune_fine(NULL, 1) != -EINVAL ||
            ad9361_do_dcxo_tune_fine(&phy_without_pdata, 1) != -EINVAL ||
            ad9361_do_dcxo_tune_fine(&phy_without_spi, 1) != -EINVAL ||
            ad9361_get_rx_gain(NULL, 1, &current_rx_gain) != -EINVAL ||
            ad9361_get_rx_gain(&phy_without_spi, 1,
                               &current_rx_gain) != -EINVAL ||
            ad9361_get_rx_gain(&phy, 1, NULL) != -EINVAL ||
            ad9361_set_rx_gain(NULL, 1, &current_rx_gain) != -EINVAL ||
            ad9361_set_rx_gain(&phy_without_spi, 1,
                               &current_rx_gain) != -EINVAL ||
            ad9361_set_rx_gain(&phy, 1, NULL) != -EINVAL ||
            ad9361_get_temperature(NULL, &temperature) != -EINVAL ||
            ad9361_get_temperature(&phy_without_spi, &temperature) != -EINVAL ||
            ad9361_get_temperature(&phy, NULL) != -EINVAL ||
            ad9361_set_en_state_machine_mode(NULL, ENSM_MODE_RX) != -EINVAL ||
            ad9361_set_en_state_machine_mode(&phy_without_spi, ENSM_MODE_RX) != -EINVAL ||
            ad9361_get_en_state_machine_mode(NULL, &ensm_mode) != -EINVAL ||
            ad9361_get_en_state_machine_mode(&phy_without_spi, &ensm_mode) != -EINVAL ||
            ad9361_get_en_state_machine_mode(&phy_without_pdata, &ensm_mode) != -EINVAL ||
            ad9361_get_en_state_machine_mode(&phy, NULL) != -EINVAL ||
            ad9361_set_rx_rf_gain(&phy_without_spi, 0, 0) != -EINVAL ||
            ad9361_set_rx_rf_gain(&phy_without_pdata, 0, 0) != -EINVAL ||
            ad9361_get_rx_rf_gain(&phy_without_spi, 0, &rx_gain) != -EINVAL ||
            ad9361_set_rx_rf_bandwidth(&phy_without_spi, 1000000) != -EINVAL ||
            ad9361_set_rx_rf_bandwidth(&phy_without_pdata, 1000000) != -EINVAL ||
            ad9361_set_rx_sampling_freq(&phy_without_spi, 1000000) != -EINVAL ||
            ad9361_set_rx_sampling_freq(&phy_without_pdata, 1000000) != -EINVAL ||
            ad9361_set_rx_lo_freq(&phy_without_spi, 1000000000) != -EINVAL ||
            ad9361_set_rx_lo_freq(&phy_without_pdata, 1000000000) != -EINVAL ||
            ad9361_set_rx_lo_int_ext(&phy_without_spi, INT_LO) != -EINVAL ||
            ad9361_set_rx_lo_int_ext(&phy_without_pdata, INT_LO) != -EINVAL ||
            ad9361_set_rx_gain_control_mode(&phy_without_spi, 0,
                                            RF_GAIN_MGC) != -EINVAL ||
            ad9361_set_rx_gain_control_mode(&phy_without_pdata, 0,
                                            RF_GAIN_MGC) != -EINVAL ||
            ad9361_set_rx_fir_en_dis(&phy_without_spi, 1) != -EINVAL ||
            ad9361_set_rx_rfdc_track_en_dis(NULL, 1) != -EINVAL ||
            ad9361_set_rx_rfdc_track_en_dis(&phy_without_spi, 1) != -EINVAL ||
            ad9361_set_rx_rfdc_track_en_dis(&phy_without_pdata, 1) != -EINVAL ||
            ad9361_get_rx_rfdc_track_en_dis(NULL, &tracking) != -EINVAL ||
            ad9361_get_rx_rfdc_track_en_dis(&phy, NULL) != -EINVAL ||
            ad9361_set_rx_bbdc_track_en_dis(NULL, 1) != -EINVAL ||
            ad9361_set_rx_bbdc_track_en_dis(&phy_without_spi, 1) != -EINVAL ||
            ad9361_set_rx_bbdc_track_en_dis(&phy_without_pdata, 1) != -EINVAL ||
            ad9361_get_rx_bbdc_track_en_dis(NULL, &tracking) != -EINVAL ||
            ad9361_get_rx_bbdc_track_en_dis(&phy, NULL) != -EINVAL ||
            ad9361_set_rx_quad_track_en_dis(NULL, 1) != -EINVAL ||
            ad9361_set_rx_quad_track_en_dis(&phy_without_spi, 1) != -EINVAL ||
            ad9361_set_rx_quad_track_en_dis(&phy_without_pdata, 1) != -EINVAL ||
            ad9361_get_rx_quad_track_en_dis(NULL, &tracking) != -EINVAL ||
            ad9361_get_rx_quad_track_en_dis(&phy, NULL) != -EINVAL ||
            ad9361_txmon_enable(NULL, TX_1) != -EINVAL ||
            ad9361_txmon_enable(&phy_without_spi, TX_1) != -EINVAL ||
            ad9361_tx_mute(&phy, 0) != -EINVAL ||
            ad9361_tx_mute(NULL, 0) != -EINVAL ||
            ad9361_tx_mute(NULL, 1) != -EINVAL ||
            ad9361_tx_mute(&phy_without_spi, 0) != -EINVAL ||
            ad9361_tx_mute(&phy, 2) != -EINVAL ||
            ad9361_set_tx_attenuation(&phy_without_spi, 0, 1000) != -EINVAL ||
            ad9361_set_tx_attenuation(&phy_without_pdata, 0, 1000) != -EINVAL ||
            ad9361_set_tx_rf_bandwidth(&phy_without_spi, 1000000) != -EINVAL ||
            ad9361_set_tx_rf_bandwidth(&phy_without_pdata, 1000000) != -EINVAL ||
            ad9361_set_tx_sampling_freq(&phy_without_spi, 1000000) != -EINVAL ||
            ad9361_set_tx_sampling_freq(&phy_without_pdata, 1000000) != -EINVAL ||
            ad9361_set_tx_lo_freq(&phy_without_spi, 1000000000) != -EINVAL ||
            ad9361_set_tx_lo_freq(&phy_without_pdata, 1000000000) != -EINVAL ||
            ad9361_set_tx_lo_int_ext(&phy_without_spi, INT_LO) != -EINVAL ||
            ad9361_set_tx_lo_int_ext(&phy_without_pdata, INT_LO) != -EINVAL ||
            ad9361_set_tx_fir_en_dis(&phy_without_spi, 1) != -EINVAL ||
            ad9361_read_clock_data_delays(NULL) != -EINVAL ||
            ad9361_read_clock_data_delays(&phy_without_spi) != -EINVAL ||
            ad9361_write_clock_data_delays(NULL) != -EINVAL ||
            ad9361_write_clock_data_delays(&phy_without_spi) != -EINVAL ||
            ad9361_get_dig_tune_data(NULL, &dig_tune) != -EINVAL ||
            ad9361_get_dig_tune_data(&phy_without_pdata, &dig_tune) != -EINVAL ||
            ad9361_get_dig_tune_data(&phy, NULL) != -EINVAL ||
            ad9361_write_bist_reg(NULL, 0) != -EINVAL ||
            ad9361_write_bist_reg(&phy_without_spi, 0) != -EINVAL ||
            ad9361_ensm_mode_disable_pinctrl(NULL) != -EINVAL ||
            ad9361_ensm_mode_disable_pinctrl(&phy_without_spi) != -EINVAL ||
            ad9361_ensm_mode_restore_pinctrl(NULL) != -EINVAL ||
            ad9361_ensm_mode_restore_pinctrl(&phy_without_spi) != -EINVAL ||
            ad9361_set_cal_sw(NULL, 0) != -EINVAL ||
            ad9361_set_cal_sw(&phy_without_spi, 0) != -EINVAL ||
            ad9361_gc_update(NULL) != -EINVAL ||
            ad9361_gc_update(&phy_without_spi) != -EINVAL ||
            ad9361_gc_setup(NULL, &gain) != -EINVAL ||
            ad9361_gc_setup(&phy_without_spi, &gain) != -EINVAL ||
            read_count != reads_before || write_count != writes_before ||
            phy.rate_governor != UINT32_MAX ||
            rate_governor != UINT32_MAX || temperature != INT32_MAX ||
            ensm_mode != UINT32_MAX || tracking != UINT8_MAX) {
            fprintf(stderr,
                    "AD9361 paired-path API accepted missing state/output\n");
            return EXIT_FAILURE;
        }
        phy.rate_governor = 0;
    }

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
    if (ad9361_get_rx_lo_freq(&phy_without_spi, &lo_frequency) != -EINVAL ||
        ad9361_get_tx_lo_freq(&phy_without_spi, &lo_frequency) != -EINVAL ||
        lo_frequency != UINT64_MAX) {
        fprintf(stderr, "AD9361 LO getters accepted missing PLL state\n");
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

    /* Calibration must not consume stale cached bandwidth or an
     * unconfirmed RFPLL source after a failed transition. */
    {
        struct no_os_clk rxpll = { .rate = 2400000000U };
        struct no_os_clk *old_rxpll = phy.clks[RX_RFPLL];
        bool old_rx_source_valid = phy.rfpll_source_valid[0];
        bool old_tx_source_valid = phy.rfpll_source_valid[1];
        bool old_bw_valid = phy.current_bw_valid;
        uint64_t lo_readback = UINT64_MAX;
        unsigned reads_before = read_count;
        unsigned writes_before = write_count;

        phy.clks[RX_RFPLL] = &rxpll;
        phy.rfpll_source_valid[0] = false;
        if (ad9361_do_calib(&phy, RFDC_CAL, -1) != -EIO ||
            ad9361_do_calib_timeout(&phy, RFDC_CAL, -1, 1000) != -EIO ||
            ad9361_set_rx_lo_freq(&phy, 1000000000) != -EIO ||
            ad9361_set_tx_lo_freq(&phy, 1000000000) != -EIO ||
            ad9361_get_rx_lo_freq(&phy, &lo_readback) != -EIO ||
            lo_readback != UINT64_MAX || read_count != reads_before ||
            write_count != writes_before) {
            fprintf(stderr,
                    "LO operation accepted an unconfirmed source\n");
            return EXIT_FAILURE;
        }

        phy.rfpll_source_valid[0] = true;
        phy.rfpll_source_valid[1] = true;
        phy.current_bw_valid = false;
        if (ad9361_do_calib(&phy, TX_QUAD_CAL, -1) != -EIO ||
            read_count != reads_before || write_count != writes_before) {
            fprintf(stderr,
                    "TX quadrature calibration accepted an invalid bandwidth cache\n");
            return EXIT_FAILURE;
        }

        phy.clks[RX_RFPLL] = old_rxpll;
        phy.rfpll_source_valid[0] = old_rx_source_valid;
        phy.rfpll_source_valid[1] = old_tx_source_valid;
        phy.current_bw_valid = old_bw_valid;
    }

    /* AD9361 burst reads descend through register addresses. The integer
     * BBPLL word is at 0x44, followed by fractional bytes 3, 2, 1. */
    {
        struct refclk_scale bbpll = { .spi = &spi, .phy = &phy };

        memset(fake_regs, 0, sizeof(fake_regs));
        fake_regs[REG_INTEGER_BB_FREQ_WORD] = 25;
        fake_regs[REG_FRACT_BB_FREQ_WORD_3] = 0x13;
        fake_regs[REG_FRACT_BB_FREQ_WORD_2] = 0x20;
        fake_regs[REG_FRACT_BB_FREQ_WORD_1] = 0x00;
        register_map_enabled = true;
        read_count = 0;
        ret = ad9361_bbpll_recalc_rate_checked(&bbpll, 38400000, &rfpll_rate);
        register_map_enabled = false;
        if (ret != 0 || rfpll_rate != 983040000 || read_count != 1) {
            fprintf(stderr,
                    "BBPLL burst read decoded wrong word: ret=%" PRId32
                    " rate=%" PRIu32 " reads=%u\n",
                    ret, rfpll_rate, read_count);
            return EXIT_FAILURE;
        }
    }

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
        if (ret != -EIO || phy.current_rx_use_tdd_table ||
            !phy.current_tx_use_tdd_table || !selected_tdd_table) {
            fprintf(stderr, "Failed RFPLL VCO setup published table cache\n");
            return EXIT_FAILURE;
        }

        fail_write_reg = -1;
        ret = ad9361_rfpll_vco_init(&phy, false, 8500000000ULL, 40000000,
                                    true, &selected_tdd_table);
        if (ret != 0 || selected_tdd_table ||
            phy.current_rx_use_tdd_table ||
            !phy.current_tx_use_tdd_table) {
            fprintf(stderr, "RFPLL VCO helper changed published cache\n");
            return EXIT_FAILURE;
        }

        {
            struct refclk_scale rxpll = {
                .spi = &spi,
                .phy = &phy,
                .source = RX_RFPLL_INT,
                .parent_source = RX_REFCLK,
            };

            phy.current_rx_lo_freq = 1800000000;
            fail_write_reg = REG_RX_VCO_OUTPUT;
            ret = ad9361_rfpll_int_set_rate(
                &rxpll, ad9361_to_clk(1900000000), 40000000);
            if (ret != -EIO || phy.current_rx_lo_freq != 0) {
                fprintf(stderr,
                        "Failed RX RFPLL programming retained old LO cache: "
                        "ret=%" PRId32 " cached=%" PRIu64 "\n",
                        ret, phy.current_rx_lo_freq);
                return EXIT_FAILURE;
            }
            fail_write_reg = -1;
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

    /* Public TX/RX status getters and their shared RSSI helper must reject a
     * missing SPI descriptor before dereference or output publication. */
    {
        struct no_os_spi_desc *saved_spi = phy.spi;
        struct rf_rssi rssi = { .ant = 1, .symbol = 0x1234 };
        uint32_t attenuation = UINT32_MAX;
        uint32_t tx_rssi_value = UINT32_MAX;
        phy.spi = NULL;
        pdata.rx2tx2 = true;
        read_count = 0;
        if (ad9361_get_tx_atten(&phy, 1) != -EINVAL ||
            ad9361_get_tx_attenuation(&phy, 0, &attenuation) != -EINVAL ||
            ad9361_get_rx_rssi(&phy, 0, &rssi) != -EINVAL ||
            ad9361_get_tx_rssi(&phy, 0, &tx_rssi_value) != -EINVAL ||
            ad9361_read_rssi(&phy, &rssi) != -EINVAL ||
            ad9361_read_rssi(NULL, &rssi) != -EINVAL ||
            ad9361_read_rssi(&phy, NULL) != -EINVAL ||
            read_count != 0 || attenuation != UINT32_MAX ||
            tx_rssi_value != UINT32_MAX || rssi.symbol != 0x1234) {
            fprintf(stderr, "AD9361 status getter accepted missing SPI state\n");
            return EXIT_FAILURE;
        }
        phy.spi = saved_spi;
        pdata.rx2tx2 = false;
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
        struct no_os_clk rxpll_clock = { .rate = 2400000000U };
        struct no_os_clk *saved_rxpll_clock = phy.clks[RX_RFPLL];
        bool saved_rx_source_valid = phy.rfpll_source_valid[0];
        register_map_enabled = true;
        memset(fake_regs, 0, sizeof(fake_regs));
        phy.clks[RX_RFPLL] = &rxpll_clock;
        phy.rfpll_source_valid[0] = true;
        phy.bbdc_track_en = true;
        phy.rfdc_track_en = true;
        phy.quad_track_en = true;
        spi_status = 0;
        fail_write_once_reg = REG_CALIBRATION_CONFIG_3;
        ret = ad9361_do_calib_run(&phy, RFDC_CAL, -1);
        if (ret != -EIO || fail_write_once_reg != -1 ||
            !phy.tracking_config_valid ||
            (fake_regs[REG_DC_OFFSET_CONFIG2] & tracking_mask) !=
                tracking_mask) {
            fprintf(stderr,
                    "Calibration setup failure left tracking disabled: ret=%" PRId32
                    " tracking=0x%02x\n", ret,
                    fake_regs[REG_DC_OFFSET_CONFIG2]);
            return EXIT_FAILURE;
        }
        phy.clks[RX_RFPLL] = saved_rxpll_clock;
        phy.rfpll_source_valid[0] = saved_rx_source_valid;
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
        pdata.rx2tx2 = true;
        phy.ref_clk_scale[CLKRF_CLK] = &clkrf;
        phy.ref_clk_scale[CLKTF_CLK] = &clktf;
        pdata.rx1rx2_phase_inversion_en = true;
        fail_write_once_reg = REG_PARALLEL_PORT_CONF_2;
        ret = ad9361_tx_quad_calib(&phy, 20000000, 20000000, -1);
        if (ret != -EIO || fail_write_once_reg != -1 ||
            (fake_regs[REG_PARALLEL_PORT_CONF_2] & INVERT_RX2) == 0) {
            fprintf(stderr,
                    "TX quad calibration skipped phase-inversion restore: "
                    "ret=%" PRId32 " reg=0x%02x\n", ret,
                    fake_regs[REG_PARALLEL_PORT_CONF_2]);
            return EXIT_FAILURE;
        }
        pdata.rx1rx2_phase_inversion_en = false;
        pdata.rx2tx2 = false;
        phy.ref_clk_scale[CLKRF_CLK] = NULL;
        phy.ref_clk_scale[CLKTF_CLK] = NULL;
        phy.clks[ADC_CLK] = NULL;
        phy.clks[DAC_CLK] = NULL;
        register_map_enabled = false;
        read_count = write_count = 0;
    }

    /* Temporary RX/TX BBF tune enables are ambiguous writes: even when SPI
     * reports failure, the tune circuit may have been enabled. Require the
     * cleanup write on both directions. */
    {
        register_map_enabled = true;
        memset(fake_regs, 0, sizeof(fake_regs));
        fail_write_reg = -1;
        fail_write_once_reg = REG_RX1_TUNE_CTRL;
        ret = ad9361_rx_bb_analog_filter_calib(&phy, 20000000, 983040000);
        if (ret != -EIO ||
            fake_regs[REG_RX1_TUNE_CTRL] !=
                (RX1_TUNE_RESAMPLE | RX1_PD_TUNE) ||
            fake_regs[REG_RX2_TUNE_CTRL] !=
                (RX2_TUNE_RESAMPLE | RX2_PD_TUNE)) {
            fprintf(stderr,
                    "RX BBF calibration skipped tune cleanup after setup error\n");
            return EXIT_FAILURE;
        }

        fail_write_once_reg = REG_TX_TUNE_CTRL;
        ret = ad9361_tx_bb_analog_filter_calib(&phy, 10000000, 983040000);
        if (ret != -EIO ||
            fake_regs[REG_TX_TUNE_CTRL] !=
                (TUNER_RESAMPLE | TUNE_CTRL(1) | PD_TUNE)) {
            fprintf(stderr,
                    "TX BBF calibration skipped tune cleanup after setup error\n");
            return EXIT_FAILURE;
        }
        fail_write_once_reg = -1;
        register_map_enabled = false;
        read_count = write_count = 0;
    }

    fail_write_reg = -1;

    /* HDL loopback is a multi-lane update. If a write reports failure after
     * reaching hardware, every attempted lane must be rolled back. */
    fake_axi_regs[0x4000 / sizeof(uint32_t)] = UINT32_C(8) << 16;
    fake_axi_regs[0x4418 / sizeof(uint32_t)] = 0x11;
    fake_axi_regs[0x4458 / sizeof(uint32_t)] = 0x22;
    fake_axi_fail_write_once_offset = 0x4458;
    fake_axi_apply_failed_write_once = true;
    ret = ad9361_hdl_loopback(&loopback_phy, true);
    if (ret != -EIO || fake_axi_regs[0x4418 / sizeof(uint32_t)] != 0x11 ||
        fake_axi_regs[0x4458 / sizeof(uint32_t)] != 0x22 ||
        loopback_conv.scratch_reg[0] != 0 ||
        loopback_conv.scratch_reg[1] != 0 ||
        !loopback_phy.bist_loopback_valid) {
        fprintf(stderr,
                "HDL loopback rollback failed: ret=%" PRId32
                " lane0=0x%08" PRIx32 " lane1=0x%08" PRIx32
                " scratch0=0x%08" PRIx32 " scratch1=0x%08" PRIx32
                " valid=%d\n",
                ret, fake_axi_regs[0x4418 / sizeof(uint32_t)],
                fake_axi_regs[0x4458 / sizeof(uint32_t)],
                loopback_conv.scratch_reg[0], loopback_conv.scratch_reg[1],
                loopback_phy.bist_loopback_valid);
        return EXIT_FAILURE;
    }
    fake_axi_fail_write_offset = 0x4458;
    ret = ad9361_hdl_loopback(&loopback_phy, true);
    fake_axi_fail_write_offset = -1;
    if (ret != -EIO ||
        fake_axi_regs[0x4418 / sizeof(uint32_t)] != 0x11 ||
        loopback_phy.bist_loopback_valid) {
        fprintf(stderr, "HDL loopback did not invalidate state after rollback failure\n");
        return EXIT_FAILURE;
    }

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
        if (ret != -EINVAL ||
            ad9361_fastlock_load(NULL, false, 0, profile_data) != -EINVAL ||
            ad9361_fastlock_store(NULL, true, 0) != -EINVAL ||
            ad9361_fastlock_save(&phy, false, 0, NULL) != -EINVAL) {
            fprintf(stderr, "Fastlock loader accepted invalid profile: ret=%" PRId32 "\n", ret);
            return EXIT_FAILURE;
        }
    }

    /* Fastlock recall must propagate read, ALC workaround, and prepare
     * failures without publishing the selected profile as active. */
    {
        struct ad9361_fastlock_entry *profile = &phy.fastlock.entry[0][0];
        unsigned writes_before;
        uint8_t saved_profile[RX_FAST_LOCK_CONFIG_WORD_NUM];

        writes_before = write_count;
        if (ad9361_fastlock_recall(NULL, false, 0) != -EINVAL ||
            ad9361_fastlock_recall(&phy, false, 8) != -EINVAL) {
            fprintf(stderr, "Fastlock recall accepted missing/invalid arguments\n");
            return EXIT_FAILURE;
        }
        phy.fastlock.current_profile[0] = 9;
        if (ad9361_fastlock_recall(&phy, false, 0) != -EINVAL ||
            write_count != writes_before) {
            fprintf(stderr, "Fastlock recall indexed an invalid cached profile\n");
            return EXIT_FAILURE;
        }
        phy.fastlock.current_profile[0] = 0;

        register_map_enabled = true;
        memset(fake_regs, 0, sizeof(fake_regs));
        memset(saved_profile, 0xa5, sizeof(saved_profile));
        fail_read_reg = REG_RX_FAST_LOCK_PROGRAM_READ;
        ret = ad9361_fastlock_save(&phy, false, 0, saved_profile);
        if (ret != -EIO || saved_profile[0] != 0xa5 ||
            ad9361_fastlock_save(NULL, false, 0, saved_profile) != -EINVAL ||
            ad9361_fastlock_save(&phy, false, 8, saved_profile) != -EINVAL) {
            fprintf(stderr, "RX fastlock save hid SPI/argument error\n");
            return EXIT_FAILURE;
        }
        fail_read_reg = -1;
        memset(saved_profile, 0xa5, sizeof(saved_profile));
        fail_read_reg = REG_TX_FAST_LOCK_PROGRAM_READ;
        ret = ad9361_fastlock_save(&phy, true, 0, saved_profile);
        if (ret != -EIO || saved_profile[0] != 0xa5) {
            fprintf(stderr, "TX fastlock save hid SPI read failure\n");
            return EXIT_FAILURE;
        }
        fail_read_reg = -1;

        profile->flags = FASTLOOK_INIT;
        profile->alc_written = 0x10;
        profile->alc_orig = 0x10;
        phy.fastlock.current_profile[0] = 0;
        fail_read_reg = REG_RX_FORCE_ALC;
        ret = ad9361_fastlock_recall(&phy, false, 0);
        if (ret != -EIO || phy.fastlock.current_profile[0] != 0) {
            fprintf(stderr, "Fastlock recall hid ALC status read failure\n");
            return EXIT_FAILURE;
        }

        fail_read_reg = -1;
        profile->alc_written = 0;
        profile->alc_orig = 0;
        fail_write_once_reg = REG_RX_FAST_LOCK_PROGRAM_ADDR;
        write_count = 0;
        ret = ad9361_fastlock_recall(&phy, false, 0);
        if (ret != -EIO || profile->flags != 0 ||
            phy.fastlock.current_profile[0] != 0 ||
            fake_regs[REG_RX_FAST_LOCK_PROGRAM_CTRL] != 0) {
            fprintf(stderr,
                    "Fastlock recall hid ALC profile write failure: ret=%" PRId32
                    " flags=%" PRIu32 " current=%u\n", ret, profile->flags,
                    phy.fastlock.current_profile[0]);
            return EXIT_FAILURE;
        }

        profile->flags = FASTLOOK_INIT;
        profile->alc_written = 0x10;
        profile->alc_orig = 0x10;
        fail_write_reg = REG_RX_FAST_LOCK_SETUP_INIT_DELAY;
        ret = ad9361_fastlock_recall(&phy, false, 0);
        if (ret != -EIO || phy.fastlock.current_profile[0] != 0) {
            fprintf(stderr,
                    "Fastlock recall hid prepare failure: ret=%" PRId32
                    " current=%u\n", ret,
                    phy.fastlock.current_profile[0]);
            return EXIT_FAILURE;
        }

        fail_write_reg = -1;
        fail_write_once_reg = REG_ENSM_CONFIG_2;
        ret = ad9361_fastlock_recall(&phy, false, 0);
        if (ret != -EIO || phy.fastlock.current_profile[0] != 0 ||
            (fake_regs[REG_RX_FAST_LOCK_SETUP] & RX_FAST_LOCK_MODE_ENABLE) != 0) {
            fprintf(stderr,
                    "Fastlock recall did not clean up partial preparation: "
                    "ret=%" PRId32 " current=%u setup=0x%02x\n", ret,
                    phy.fastlock.current_profile[0],
                    fake_regs[REG_RX_FAST_LOCK_SETUP]);
            return EXIT_FAILURE;
        }
        fail_write_once_reg = -1;

        /* The same shared recall implementation serves TX profiles; check
         * that the TX register bank reports failures and bounds-checks its
         * independent active-profile cache as well. */
        phy.fastlock.entry[1][0].flags = FASTLOOK_INIT;
        phy.fastlock.entry[1][0].alc_written = 0x10;
        phy.fastlock.entry[1][0].alc_orig = 0x10;
        phy.fastlock.current_profile[1] = 0;
        fail_read_reg = REG_TX_FORCE_ALC;
        ret = ad9361_fastlock_recall(&phy, true, 0);
        if (ret != -EIO || phy.fastlock.current_profile[1] != 0) {
            fprintf(stderr, "TX fastlock recall hid ALC status read failure\n");
            return EXIT_FAILURE;
        }
        fail_read_reg = -1;
        phy.fastlock.current_profile[1] = 9;
        writes_before = write_count;
        if (ad9361_fastlock_recall(&phy, true, 0) != -EINVAL ||
            write_count != writes_before) {
            fprintf(stderr, "TX fastlock recall indexed invalid cached profile\n");
            return EXIT_FAILURE;
        }
        phy.fastlock.current_profile[1] = 0;
        register_map_enabled = false;
        read_count = write_count = 0;
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
#if HAVE_SPLIT_GAIN_TABLE
		{
			const uint32_t gain_regs[] = {
				REG_RX1_MANUAL_LMT_FULL_GAIN,
				REG_RX1_MANUAL_LMT_FULL_GAIN + 1,
				REG_RX1_MANUAL_LMT_FULL_GAIN + 2
			};
			uint8_t old_gain_regs[3];

			phy.pdata->split_gt = true;
			phy.pdata->gain_ctrl.dig_gain_en = true;
			fake_regs[REG_AGC_CONFIG_1] = 0; /* RX1 manual gain control */
			fake_regs[gain_regs[0]] = 0x11;
			fake_regs[gain_regs[1]] = 0x22;
			fake_regs[gain_regs[2]] = 0x33;
			for (size_t i = 0; i < 3; i++)
				old_gain_regs[i] = fake_regs[gain_regs[i]];

			rx_gain = (struct rf_rx_gain) {
				.gain_db = 10,
				.fgt_lmt_index = 0,
				.lpf_gain = 1,
				.digital_gain = 1
			};
			fail_write_once_reg = gain_regs[1];
			fail_write_once_reg_occurrence = 1;
			apply_failed_write_once = true;
			ret = ad9361_set_rx_gain(&phy, 1, &rx_gain);
			if (ret != -EIO) {
				fprintf(stderr, "RX split gain hid a partial write failure\n");
				return EXIT_FAILURE;
			}
			for (size_t i = 0; i < 3; i++) {
				if (fake_regs[gain_regs[i]] != old_gain_regs[i]) {
					fprintf(stderr,
						"RX split gain register 0x%03" PRIx32
						" was not restored\n", gain_regs[i]);
					return EXIT_FAILURE;
				}
			}
			fail_write_once_reg = -1;
			fail_write_once_reg_occurrence = 1;
			apply_failed_write_once = false;
			phy.pdata->split_gt = false;
			phy.pdata->gain_ctrl.dig_gain_en = false;
		}
#endif
        phy.pdata->split_gt = true;
        phy.current_table = TBL_200_1300_MHZ + RXGAIN_TBLS_END;
        rx_gain.gain_db = 0x1234;
        fail_read_reg = REG_GAIN_TABLE_ADDRESS;
        ret = ad9361_get_rx_gain(&phy, 1, &rx_gain);
        if (ret != -EIO || rx_gain.gain_db != 0x1234) {
            fprintf(stderr, "RX gain getter hid split-table address read failure: ret=%"
                    PRId32 " gain=%" PRId32 "\n", ret, rx_gain.gain_db);
            return EXIT_FAILURE;
        }
        fail_read_reg = -1;
        rx_gain.gain_db = 0x1234;
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
        {
            int16_t signed_coef[16] = {0};
            signed_coef[0] = -1;
            last_fir_high_byte = 0;
            ret = ad9361_load_fir_filter_coef(&phy, FIR_RX1, 0, 16,
                                               signed_coef);
            if (ret != 0 || last_fir_high_byte != 0xFF) {
                fprintf(stderr,
                        "negative FIR coefficient high byte was not serialized: "
                        "ret=%" PRId32 " high=0x%02x writes=%u last=0x%03x\n",
                        ret, last_fir_high_byte, write_count,
                        last_spi_write_addr);
                return EXIT_FAILURE;
            }
        }
        {
            int32_t old_state_after_write = state_after_ensm_write;
            unsigned ensm_writes = fake_write_counts[REG_ENSM_CONFIG_1];

            fake_regs[REG_STATE] = ENSM_STATE_RX;
            state_after_ensm_write = ENSM_STATE_RX;
            fail_write_once_reg = REG_ENSM_CONFIG_1;
            fail_write_once_reg_occurrence = 1;
            apply_failed_write_once = true;
            ret = ad9361_load_fir_filter_coef(&phy, FIR_TX1, 0, 16,
                                               fir_coef);
            fail_write_once_reg = -1;
            state_after_ensm_write = old_state_after_write;
            if (ret != -EIO || fake_regs[REG_STATE] != ENSM_STATE_RX ||
                fake_write_counts[REG_ENSM_CONFIG_1] < ensm_writes + 3) {
                fprintf(stderr,
                        "FIR loader skipped ENSM recovery after failed ALERT request\n");
                return EXIT_FAILURE;
            }
        }
	{
		char fir_text[512];
		size_t used;
		unsigned rx_gain_writes = fake_write_counts[REG_RX_FILTER_GAIN];
		int written;
		unsigned tap;

		written = snprintf(fir_text, sizeof(fir_text),
				   "TX 1 GAIN 0 INT 1\nRX 1 GAIN 0 DEC 1\n");
		if (written < 0 || (size_t)written >= sizeof(fir_text))
			return EXIT_FAILURE;
		used = (size_t)written;
		for (tap = 0; tap < 16; ++tap) {
			written = snprintf(fir_text + used, sizeof(fir_text) - used,
					   "0,0\n");
			if (written < 0 || (size_t)written >= sizeof(fir_text) - used)
				return EXIT_FAILURE;
			used += (size_t)written;
		}

		fake_regs[REG_STATE] = ENSM_STATE_ALERT;
		fail_write_reg = REG_TX_FILTER_CONF;
		ret = ad9361_parse_fir(&phy, fir_text, (uint32_t)used);
		fail_write_reg = -1;
		if (ret != -EIO ||
		    fake_write_counts[REG_RX_FILTER_GAIN] != rx_gain_writes) {
			fprintf(stderr,
				"FIR parser hid TX load error or continued into RX load\n");
			return EXIT_FAILURE;
		}
	}
        {
            struct refclk_scale rx_mux = {
                .spi = &spi, .phy = &phy, .source = RX_RFPLL,
                .mult = INT_LO,
            };
            struct refclk_scale tx_mux = {
                .spi = &spi, .phy = &phy, .source = TX_RFPLL,
                .mult = INT_LO,
            };
            uint64_t lo_readback = UINT64_MAX;
            uint8_t old_ensm_state = fake_regs[REG_STATE];
            int32_t old_state_after_ensm_write = state_after_ensm_write;

            fake_regs[REG_STATE] = ENSM_STATE_ALERT;
            state_after_ensm_write = ENSM_STATE_ALERT;
            phy.rfpll_source_valid[0] = true;
            phy.rfpll_source_valid[1] = true;
            fail_write_once_reg = REG_ENSM_CONFIG_2;
            fail_write_once_reg_occurrence = 2;
            apply_failed_write_once = true;
            ret = ad9361_clk_mux_set_parent(&rx_mux, EXT_LO);
            fail_write_once_reg = -1;
            if (ret != -EIO || phy.rfpll_source_valid[0] ||
                rx_mux.mult != INT_LO ||
                ad9361_get_rx_lo_freq(&phy, &lo_readback) != -EIO ||
                lo_readback != UINT64_MAX) {
                fprintf(stderr,
                        "Partial RX LO mux update was exposed as valid\n");
                return EXIT_FAILURE;
            }

            fail_write_once_reg = REG_ENSM_CONFIG_2;
            fail_write_once_reg_occurrence = 2;
            apply_failed_write_once = true;
            ret = ad9361_clk_mux_set_parent(&tx_mux, EXT_LO);
            fail_write_once_reg = -1;
            if (ret != -EIO || phy.rfpll_source_valid[1] ||
                tx_mux.mult != INT_LO ||
                ad9361_get_tx_lo_freq(&phy, &lo_readback) != -EIO ||
                lo_readback != UINT64_MAX) {
                fprintf(stderr,
                        "Partial TX LO mux update was exposed as valid\n");
                return EXIT_FAILURE;
            }

            if (ad9361_clk_mux_set_parent(&rx_mux, EXT_LO) != 0 ||
                ad9361_clk_mux_set_parent(&tx_mux, EXT_LO) != 0 ||
                !phy.rfpll_source_valid[0] || !phy.rfpll_source_valid[1] ||
                rx_mux.mult != EXT_LO || tx_mux.mult != EXT_LO) {
                fprintf(stderr,
                        "Successful LO mux recovery did not restore validity\n");
                return EXIT_FAILURE;
            }
            fake_regs[REG_STATE] = old_ensm_state;
            state_after_ensm_write = old_state_after_ensm_write;
        }
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
        fail_write_reg = -1;
        phy.fastlock.current_profile[0] = 7;
        fake_regs[REG_RX_FAST_LOCK_SETUP] = RX_FAST_LOCK_MODE_ENABLE;
        fail_write_once_reg = REG_RX_FAST_LOCK_SETUP;
        fail_write_once_reg_occurrence = 1;
        apply_failed_write_once = true;
        ret = ad9361_fastlock_exit_foreign(&phy, false);
        if (ret != -EIO || phy.fastlock.current_profile[0] != 0xffu ||
            (fake_regs[REG_RX_FAST_LOCK_SETUP] & RX_FAST_LOCK_MODE_ENABLE) != 0) {
            fprintf(stderr, "Fastlock exit hid setup write failure: ret=%" PRId32 "\n", ret);
            return EXIT_FAILURE;
        }
        ret = ad9361_fastlock_prepare(&phy, false, 0, true);
        if (ret != -EIO) {
            fprintf(stderr, "RX fastlock prepare accepted unknown cleanup state\n");
            return EXIT_FAILURE;
        }
        ret = ad9361_fastlock_exit_foreign(&phy, false);
        if (ret != 0 || phy.fastlock.current_profile[0] != 0 ||
            (fake_regs[REG_RX_FORCE_ALC] & FORCE_ALC_ENABLE) != 0) {
            fprintf(stderr,
                    "RX fastlock exit failed to finish cleanup after ambiguous mode clear\n");
            return EXIT_FAILURE;
        }

        phy.fastlock.current_profile[1] = 8;
        fake_regs[REG_TX_FAST_LOCK_SETUP] = RX_FAST_LOCK_MODE_ENABLE;
        fail_write_once_reg = REG_TX_FAST_LOCK_SETUP;
        fail_write_once_reg_occurrence = 1;
        apply_failed_write_once = true;
        ret = ad9361_fastlock_exit_foreign(&phy, true);
        if (ret != -EIO || phy.fastlock.current_profile[1] != 0xffu ||
            (fake_regs[REG_TX_FAST_LOCK_SETUP] & RX_FAST_LOCK_MODE_ENABLE) != 0) {
            fprintf(stderr,
                    "TX fastlock exit lost partial-cleanup state: ret=%" PRId32 "\n",
                    ret);
            return EXIT_FAILURE;
        }
        ret = ad9361_fastlock_prepare(&phy, true, 0, true);
        if (ret != -EIO) {
            fprintf(stderr, "TX fastlock prepare accepted unknown cleanup state\n");
            return EXIT_FAILURE;
        }
        ret = ad9361_fastlock_exit_foreign(&phy, true);
        if (ret != 0 || phy.fastlock.current_profile[1] != 0 ||
            (fake_regs[REG_TX_FORCE_ALC] & FORCE_ALC_ENABLE) != 0) {
            fprintf(stderr,
                    "TX fastlock exit failed to finish cleanup after ambiguous mode clear\n");
            return EXIT_FAILURE;
        }
        fail_write_once_reg = -1;
        fail_write_once_reg_occurrence = 1;
        apply_failed_write_once = false;

        fail_read_reg = REG_RX_INTEGER_BYTE_0;
        fail_write_reg = -1;
        ret = ad9361_fastlock_store(&phy, false, 0);
        if (ret != -EIO) {
            fprintf(stderr, "Fastlock store hid profile read failure: ret=%" PRId32 "\n", ret);
            return EXIT_FAILURE;
        }
        phy.tx1_atten_cached = 111;
        phy.tx2_atten_cached = 222;
        phy.tx_mute_restore_atten[0] = 111;
        phy.tx_mute_restore_atten[1] = 222;
        phy.tx_mute_restore_valid = true;
        fail_read_reg = REG_TX1_ATTEN_1;
        ret = ad9361_tx_mute(&phy, 1);
        if (ret != -EIO || phy.tx1_atten_cached != 111 ||
            phy.tx2_atten_cached != 222 || phy.tx_mute_restore_valid ||
            phy.tx_mute_restore_atten[0] != 111 ||
            phy.tx_mute_restore_atten[1] != 222) {
            fprintf(stderr, "TX mute hid attenuation read failure or changed cache\n");
            return EXIT_FAILURE;
        }
        fail_read_reg = -1;
        {
            unsigned writes_before = write_count;
            ret = ad9361_tx_mute(&phy, 0);
            if (ret != -EINVAL || write_count != writes_before) {
                fprintf(stderr,
                        "TX cleanup restored a stale attenuation snapshot\n");
                return EXIT_FAILURE;
            }
        }
        phy.tx_mute_restore_valid = true;
        fail_read_reg = REG_TX2_ATTEN_1;
        ret = ad9361_tx_mute(&phy, 1);
        if (ret != -EIO || phy.tx_mute_restore_valid) {
            fprintf(stderr,
                    "TX mute retained stale cache after TX2 read failure\n");
            return EXIT_FAILURE;
        }
        fail_read_reg = -1;
        {
            unsigned writes_before = write_count;
            ret = ad9361_tx_mute(&phy, 0);
            if (ret != -EINVAL || write_count != writes_before) {
                fprintf(stderr,
                        "TX2 read cleanup restored a stale attenuation snapshot\n");
                return EXIT_FAILURE;
            }
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
		phy.cached_synth_pd[0] = 0x12;
		phy.cached_synth_pd[1] = 0x34;
		fail_write_reg = REG_TX_SYNTH_POWER_DOWN_OVERRIDE;
		ret = ad9361_synth_lo_powerdown(&phy, LO_OFF, LO_ON);
		if (ret != -EIO || phy.cached_synth_pd[0] != 0x12 ||
		    phy.cached_synth_pd[1] != 0x34) {
			fprintf(stderr, "LO powerdown failure committed unconfirmed cache\n");
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
        AD9361_RXFIRConfig rx_fir = {0};
        AD9361_TXFIRConfig tx_fir = {0};
        unsigned tx_atten_write_count;
        struct no_os_clk agc_rf_clock = { .rate = 30720000U };
        struct no_os_clk agc_sample_clock = { .rate = 1000000U };
        struct no_os_clk *saved_agc_rf_clock = phy.clks[CLKRF_CLK];
        struct no_os_clk *saved_agc_sample_clock = phy.clks[RX_SAMPL_CLK];
        struct gain_control saved_gain_ctrl = pdata.gain_ctrl;

        gain.lmt_overload_high_thresh = 16;
        gain.lmt_overload_low_thresh = 16;
        gain.dec_pow_measuremnt_duration = 16;
        gain.f_agc_dec_pow_measuremnt_duration = 16;
        gain.f_agc_power_measurement_duration_in_state5 = 16;
        gain.gain_update_interval_us = 1000;
        phy.clks[CLKRF_CLK] = &agc_rf_clock;
        phy.clks[RX_SAMPL_CLK] = &agc_sample_clock;
        pdata.gain_ctrl = gain;

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

        fail_write_reg = -1;
        tx_atten_write_count = write_count;
        if (ad9361_set_tx_atten(&phy, 1000, false, false, false) != -EINVAL ||
            ad9361_set_tx_atten(&phy, 1000, false, false, true) != -EINVAL ||
            ad9361_set_tx_atten(NULL, 1000, true, false, true) != -EINVAL ||
            write_count != tx_atten_write_count) {
            fprintf(stderr, "TX attenuation accepted empty/missing target\n");
            return EXIT_FAILURE;
        }

        register_map_enabled = true;
        memset(fake_regs, 0, sizeof(fake_regs));
        fake_write_counts[REG_TX2_DIG_ATTEN] = 0;
        fail_write_once_reg = REG_TX2_DIG_ATTEN;
        fail_write_once_reg_occurrence = 1;
        apply_failed_write_once = true;
        ret = ad9361_set_tx_atten(&phy, 1000, true, false, true);
        if (ret != -EIO ||
            !(fake_regs[REG_TX2_DIG_ATTEN] & IMMEDIATELY_UPDATE_TPC_ATTEN) ||
            fake_write_counts[REG_TX2_DIG_ATTEN] != 2) {
            fprintf(stderr,
                    "TX attenuation did not restore immediate update after disable error\n");
            return EXIT_FAILURE;
        }

        memset(fake_regs, 0, sizeof(fake_regs));
        fake_write_counts[REG_TX2_DIG_ATTEN] = 0;
        fail_write_once_reg = REG_TX1_ATTEN_1;
        fail_write_once_reg_occurrence = 1;
        apply_failed_write_once = true;
        ret = ad9361_set_tx_atten(&phy, 1000, true, false, true);
        if (ret != -EIO ||
            !(fake_regs[REG_TX2_DIG_ATTEN] & IMMEDIATELY_UPDATE_TPC_ATTEN) ||
            fake_write_counts[REG_TX2_DIG_ATTEN] != 2) {
            fprintf(stderr,
                    "TX attenuation did not restore immediate update after data error\n");
            return EXIT_FAILURE;
        }

        memset(fake_regs, 0, sizeof(fake_regs));
        fake_write_counts[REG_TX2_DIG_ATTEN] = 0;
        fail_write_once_reg = REG_TX2_DIG_ATTEN;
        fail_write_once_reg_occurrence = 2;
        apply_failed_write_once = true;
        ret = ad9361_set_tx_atten(&phy, 1000, true, false, true);
        if (ret != -EIO ||
            !(fake_regs[REG_TX2_DIG_ATTEN] & IMMEDIATELY_UPDATE_TPC_ATTEN) ||
            fake_write_counts[REG_TX2_DIG_ATTEN] != 3) {
            fprintf(stderr,
                    "TX attenuation did not retry ambiguous immediate update\n");
            return EXIT_FAILURE;
        }

        /* A partial two-channel update must restore both attenuation words,
         * including a failed SPI byte that may already have reached the RFIC. */
        memset(fake_regs, 0, sizeof(fake_regs));
        fake_regs[REG_TX1_ATTEN_1] = 0x01;
        fake_regs[REG_TX1_ATTEN_1 + 1] = 0x23;
        fake_regs[REG_TX2_ATTEN_1] = 0x04;
        fake_regs[REG_TX2_ATTEN_1 + 1] = 0x56;
        fake_regs[REG_TX2_DIG_ATTEN] = IMMEDIATELY_UPDATE_TPC_ATTEN;
        fail_write_once_reg = REG_TX2_ATTEN_1;
        fail_write_once_reg_occurrence = 1;
        apply_failed_write_once = true;
        ret = ad9361_set_tx_atten(&phy, 2000, true, true, true);
        if (ret != -EIO ||
            fake_regs[REG_TX1_ATTEN_1] != 0x01 ||
            fake_regs[REG_TX1_ATTEN_1 + 1] != 0x23 ||
            fake_regs[REG_TX2_ATTEN_1] != 0x04 ||
            fake_regs[REG_TX2_ATTEN_1 + 1] != 0x56 ||
            !(fake_regs[REG_TX2_DIG_ATTEN] &
              IMMEDIATELY_UPDATE_TPC_ATTEN)) {
            fprintf(stderr,
                    "Partial TX1/TX2 attenuation update was not rolled back\n");
            return EXIT_FAILURE;
        }

        /* Distinct saved channel levels are restored as one operation. An
         * ambiguous TX2 failure must not leave TX1 unmuted. */
        fake_regs[REG_TX1_ATTEN_1] = 0x01;
        fake_regs[REG_TX1_ATTEN_1 + 1] = 0x67;
        fake_regs[REG_TX2_ATTEN_1] = 0x01;
        fake_regs[REG_TX2_ATTEN_1 + 1] = 0x67;
        fake_regs[REG_TX2_DIG_ATTEN] = IMMEDIATELY_UPDATE_TPC_ATTEN;
        phy.tx_mute_restore_atten[0] = 1000;
        phy.tx_mute_restore_atten[1] = 2000;
        phy.tx_mute_restore_valid = true;
        fail_write_once_reg = REG_TX2_ATTEN_1;
        fail_write_once_reg_occurrence = 1;
        apply_failed_write_once = true;
        ret = ad9361_tx_mute(&phy, 0);
        if (ret != -EIO || !phy.tx_mute_restore_valid ||
            fake_regs[REG_TX1_ATTEN_1] != 0x01 ||
            fake_regs[REG_TX1_ATTEN_1 + 1] != 0x67 ||
            fake_regs[REG_TX2_ATTEN_1] != 0x01 ||
            fake_regs[REG_TX2_ATTEN_1 + 1] != 0x67 ||
            !(fake_regs[REG_TX2_DIG_ATTEN] &
              IMMEDIATELY_UPDATE_TPC_ATTEN)) {
            fprintf(stderr,
                    "Partial two-channel TX unmute was not rolled back\n");
            return EXIT_FAILURE;
        }
        fail_write_once_reg = -1;
        fail_write_once_reg_occurrence = 1;
        apply_failed_write_once = false;
        register_map_enabled = false;

        /* A failed ambiguous FIR setup write must still restore the original
         * register and leave the caller's output structure untouched. */
        register_map_enabled = true;
        fake_regs[REG_RX_FILTER_CONFIG] = FIR_SELECT(2) | FIR_NUM_TAPS(3);
        rx_fir.rx_coef[0] = 0x55aa;
        fail_write_once_reg = REG_RX_FILTER_CONFIG;
        ret = ad9361_get_rx_fir_config(&phy, 0, &rx_fir);
        if (ret != -EIO ||
            fake_regs[REG_RX_FILTER_CONFIG] !=
                (FIR_SELECT(2) | FIR_NUM_TAPS(3)) ||
            rx_fir.rx_coef[0] != 0x55aa) {
            fprintf(stderr,
                    "RX FIR getter failed to restore ambiguous setup write\n");
            return EXIT_FAILURE;
        }

        fake_regs[REG_TX_FILTER_CONF] = FIR_SELECT(2) | FIR_NUM_TAPS(3);
        tx_fir.tx_coef[0] = 0x55aa;
        fail_write_once_reg = REG_TX_FILTER_CONF;
        ret = ad9361_get_tx_fir_config(&phy, 0, &tx_fir);
        if (ret != -EIO ||
            fake_regs[REG_TX_FILTER_CONF] !=
                (FIR_SELECT(2) | FIR_NUM_TAPS(3)) ||
            tx_fir.tx_coef[0] != 0x55aa) {
            fprintf(stderr,
                    "TX FIR getter failed to restore ambiguous setup write\n");
            return EXIT_FAILURE;
        }
        fail_write_once_reg = -1;
        register_map_enabled = false;

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
        phy.clks[CLKRF_CLK] = saved_agc_rf_clock;
        phy.clks[RX_SAMPL_CLK] = saved_agc_sample_clock;
        pdata.gain_ctrl = saved_gain_ctrl;
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

    /* PINCTRL_FDD_INDEP must program FDD_EXTERNAL_CTRL_ENABLE from the
     * requested mode, not the previous platform-data cache. */
    {
        bool old_fdd = pdata.fdd;
        bool old_fdd_independent = pdata.fdd_independent_mode;
        uint8_t old_state = fake_regs[REG_STATE];
        uint8_t old_config_2 = fake_regs[REG_ENSM_CONFIG_2];
        uint8_t old_agc_mode_0 = phy.agc_mode[0];
        uint8_t old_agc_mode_1 = phy.agc_mode[1];
        uint8_t old_curr_state = phy.curr_ensm_state;
        int32_t old_state_after_write = state_after_ensm_write;

        register_map_enabled = true;
        fail_read_reg = -1;
        fail_write_reg = -1;
        pdata.fdd = true;
        pdata.fdd_independent_mode = false;
        phy.agc_mode[0] = 0;
        phy.agc_mode[1] = 0;
        phy.curr_ensm_state = ENSM_STATE_ALERT;
        state_after_ensm_write = ENSM_STATE_FDD;
        fake_regs[REG_STATE] = ENSM_STATE_ALERT;
        ret = ad9361_set_en_state_machine_mode(
                  &phy, ENSM_MODE_PINCTRL_FDD_INDEP);
        if (ret != 0 || !pdata.fdd_independent_mode ||
            !(fake_regs[REG_ENSM_CONFIG_2] & FDD_EXTERNAL_CTRL_ENABLE)) {
            fprintf(stderr,
                    "ENSM independent FDD mode cache/register mismatch: ret=%" PRId32
                    " cache=%u config2=0x%02x\n",
                    ret, pdata.fdd_independent_mode,
                    fake_regs[REG_ENSM_CONFIG_2]);
            return EXIT_FAILURE;
        }

        ret = ad9361_set_en_state_machine_mode(&phy, ENSM_MODE_FDD);
        if (ret != 0 || pdata.fdd_independent_mode ||
            (fake_regs[REG_ENSM_CONFIG_2] & FDD_EXTERNAL_CTRL_ENABLE)) {
            fprintf(stderr,
                    "ENSM regular FDD mode cache/register mismatch: ret=%" PRId32
                    " cache=%u config2=0x%02x\n",
                    ret, pdata.fdd_independent_mode,
                    fake_regs[REG_ENSM_CONFIG_2]);
            return EXIT_FAILURE;
        }

        pdata.fdd = old_fdd;
        pdata.fdd_independent_mode = old_fdd_independent;
        fake_regs[REG_STATE] = old_state;
        fake_regs[REG_ENSM_CONFIG_2] = old_config_2;
        phy.agc_mode[0] = old_agc_mode_0;
        phy.agc_mode[1] = old_agc_mode_1;
        phy.curr_ensm_state = old_curr_state;
        state_after_ensm_write = old_state_after_write;
    }

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

        /* A later-register failure leaves the preceding tracking writes in
         * hardware unless the setter actively restores the old complete
         * tracking register set. */
        {
            const uint16_t tracking_regs[] = {
                REG_CALIBRATION_CONFIG_2,
                REG_CALIBRATION_CONFIG_3,
                REG_DC_OFFSET_CONFIG2,
                REG_RX_QUAD_GAIN2,
                REG_CALIBRATION_CONFIG_1
            };
            uint8_t old_tracking_regs[sizeof(tracking_regs) /
                                      sizeof(tracking_regs[0])];
            unsigned config2_writes;
            size_t i;

            ret = ad9361_tracking_control(&phy, false, false, false);
            if (ret != 0) {
                fprintf(stderr, "Unable to establish baseline tracking image\n");
                return EXIT_FAILURE;
            }
            config2_writes = fake_write_counts[REG_CALIBRATION_CONFIG_2];
            for (i = 0; i < sizeof(tracking_regs) /
                            sizeof(tracking_regs[0]); i++)
                old_tracking_regs[i] = fake_regs[tracking_regs[i]];

            fail_write_once_reg = REG_CALIBRATION_CONFIG_1;
            ret = ad9361_set_rx_rfdc_track_en_dis(&phy, 1);
            if (ret != -EIO || phy.rfdc_track_en != 0) {
                fprintf(stderr, "Partial tracking failure was not reported\n");
                return EXIT_FAILURE;
            }
            for (i = 0; i < sizeof(tracking_regs) /
                            sizeof(tracking_regs[0]); i++) {
                if (fake_regs[tracking_regs[i]] != old_tracking_regs[i]) {
                    fprintf(stderr,
                            "Tracking register %u was not restored after failure\n",
                            tracking_regs[i]);
                    return EXIT_FAILURE;
                }
            }
            ret = ad9361_set_rx_rfdc_track_en_dis(&phy, 0);
            if (ret != 0 ||
                fake_write_counts[REG_CALIBRATION_CONFIG_2] != config2_writes + 3) {
                fprintf(stderr,
                        "Tracking failure rollback or no-op replay did not run\n");
                return EXIT_FAILURE;
            }

            /* If the rollback also fails, getters must not report the old
             * cached bits as a confirmed hardware state. A complete later
             * write must recover the cache and make getters available again. */
            fail_write_reg = REG_CALIBRATION_CONFIG_1;
            ret = ad9361_set_rx_rfdc_track_en_dis(&phy, 1);
            if (ret != -EIO || phy.tracking_config_valid) {
                fprintf(stderr,
                        "Tracking rollback failure retained a valid cache\n");
                return EXIT_FAILURE;
            }
            {
                uint8_t rfdc = UINT8_MAX;
                uint8_t bbdc = UINT8_MAX;
                uint8_t quad = UINT8_MAX;
                if (ad9361_get_rx_rfdc_track_en_dis(&phy, &rfdc) != -EIO ||
                    ad9361_get_rx_bbdc_track_en_dis(&phy, &bbdc) != -EIO ||
                    ad9361_get_rx_quad_track_en_dis(&phy, &quad) != -EIO ||
                    rfdc != UINT8_MAX || bbdc != UINT8_MAX ||
                    quad != UINT8_MAX) {
                    fprintf(stderr,
                            "Unknown tracking state was reported as valid\n");
                    return EXIT_FAILURE;
                }
            }
            fail_write_reg = -1;
            ret = ad9361_set_rx_rfdc_track_en_dis(&phy, 0);
            if (ret != 0 || !phy.tracking_config_valid) {
                fprintf(stderr, "Tracking configuration did not recover\n");
                return EXIT_FAILURE;
            }
        }
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

        register_map_enabled = true;
        memset(fake_regs, 0, sizeof(fake_regs));
        fake_write_counts[REG_SDM_CTRL_1] = 0;
        fail_write_once_reg = REG_SDM_CTRL_1;
        fail_write_once_reg_occurrence = 1;
        apply_failed_write_once = true;
        ret = ad9361_bbpll_set_rate(&bbpll, 983040000, 40000000);
        if (ret != -EIO || fake_regs[REG_SDM_CTRL_1] != BBPLL_RESET_BAR ||
            fake_write_counts[REG_SDM_CTRL_1] != 2) {
            fprintf(stderr,
                    "BBPLL start cleanup mismatch: ret=%" PRId32
                    " reg=0x%02x writes=%u\n", ret,
                    fake_regs[REG_SDM_CTRL_1],
                    fake_write_counts[REG_SDM_CTRL_1]);
            return EXIT_FAILURE;
        }

        fake_write_counts[REG_SDM_CTRL_1] = 0;
        fail_write_once_reg = REG_SDM_CTRL_1;
        fail_write_once_reg_occurrence = 2;
        apply_failed_write_once = true;
        ret = ad9361_bbpll_set_rate(&bbpll, 983040000, 40000000);
        if (ret != -EIO || fake_regs[REG_SDM_CTRL_1] != BBPLL_RESET_BAR ||
            fake_write_counts[REG_SDM_CTRL_1] != 3) {
            fprintf(stderr,
                    "BBPLL failed calibration-clear write skipped cleanup\n");
            return EXIT_FAILURE;
        }
        fail_write_once_reg = -1;
        fail_write_once_reg_occurrence = 1;
        apply_failed_write_once = false;
        register_map_enabled = false;
    }

    {
        struct no_os_clk clock_nodes[NUM_AD9361_CLKS] = {0};
        struct refclk_scale clock_scales[NUM_AD9361_CLKS] = {0};
        static const enum ad9361_clocks parents[NUM_AD9361_CLKS] = {
            [BB_REFCLK] = EXT_REF_CLK,
            [RX_REFCLK] = BB_REFCLK,
            [TX_REFCLK] = BB_REFCLK,
            [BBPLL_CLK] = BB_REFCLK,
            [ADC_CLK] = BBPLL_CLK,
            [R2_CLK] = ADC_CLK,
            [R1_CLK] = R2_CLK,
            [CLKRF_CLK] = R1_CLK,
            [RX_SAMPL_CLK] = CLKRF_CLK,
            [DAC_CLK] = BBPLL_CLK,
            [T2_CLK] = DAC_CLK,
            [T1_CLK] = T2_CLK,
            [CLKTF_CLK] = T1_CLK,
            [TX_SAMPL_CLK] = CLKTF_CLK,
            [RX_RFPLL_INT] = RX_REFCLK,
            [TX_RFPLL_INT] = TX_REFCLK,
            [RX_RFPLL_DUMMY] = RX_REFCLK,
            [TX_RFPLL_DUMMY] = TX_REFCLK,
            [RX_RFPLL] = RX_RFPLL_INT,
            [TX_RFPLL] = TX_RFPLL_INT,
        };
        struct refclk_scale *bbpll = &clock_scales[BBPLL_CLK];
        unsigned int i;

        for (i = 0; i < NUM_AD9361_CLKS; i++) {
            clock_nodes[i].rate = 123;
            phy.clks[i] = &clock_nodes[i];
            clock_scales[i].spi = &spi;
            clock_scales[i].phy = &phy;
            clock_scales[i].source = i;
            clock_scales[i].parent_source = parents[i];
            phy.ref_clk_scale[i] = &clock_scales[i];
        }
        clock_nodes[BB_REFCLK].rate = 40000000;
        clock_nodes[BBPLL_CLK].rate = 983040000;
        clock_nodes[ADC_CLK].rate = 491520000;
        phy.clk_refin = &clock_nodes[BB_REFCLK];
        phy.bbpll_initialized = true;

        /* Reject overflowing startup frequencies before setup performs any
         * RFIC writes or narrows them to the shifted PLL representation. */
        {
            uint64_t saved_rx_freq = pdata.rx_synth_freq;
            uint64_t saved_tx_freq = pdata.tx_synth_freq;
            unsigned writes_before = write_count;

            pdata.rx_synth_freq = 18000000000ULL;
            pdata.tx_synth_freq = 1800000000ULL;
            ret = ad9361_setup(&phy);
            if (ret != -EINVAL || write_count != writes_before) {
                fprintf(stderr,
                        "Invalid RX startup LO was not rejected before SPI: "
                        "ret=%" PRId32 " writes=%u->%u\n",
                        ret, writes_before, write_count);
                return EXIT_FAILURE;
            }
            pdata.rx_synth_freq = 1800000000ULL;
            pdata.tx_synth_freq = 18000000000ULL;
            ret = ad9361_setup(&phy);
            if (ret != -EINVAL || write_count != writes_before) {
                fprintf(stderr,
                        "Invalid TX startup LO was not rejected before SPI: "
                        "ret=%" PRId32 " writes=%u->%u\n",
                        ret, writes_before, write_count);
                return EXIT_FAILURE;
            }
            pdata.rx_synth_freq = saved_rx_freq;
            pdata.tx_synth_freq = saved_tx_freq;
        }

        /* Legacy round-rate returns the high-band encoded rate through a
         * signed type. Preserve its valid bit pattern above INT32_MAX. */
        {
            uint32_t high_lo_rate = ad9361_to_clk(5500000000ULL);
            int32_t signed_round_rate = ad9361_rfpll_round_rate(
                &clock_scales[RX_RFPLL], high_lo_rate);

            if ((uint32_t)signed_round_rate != high_lo_rate) {
                fprintf(stderr,
                        "Valid high-band RX LO rate was not preserved: "
                        "rate=%" PRIu32 " rounded=%" PRId32 "\n",
                        high_lo_rate, signed_round_rate);
                return EXIT_FAILURE;
            }
        }

        /* An out-of-range RFPLL round-rate error must remain an error; it
         * must not become an unsigned LO rate and reach the SPI writer. */
        {
            uint32_t saved_rxpll_rate = clock_nodes[RX_RFPLL].rate;
            uint32_t saved_rxpll_int_rate = clock_nodes[RX_RFPLL_INT].rate;
            uint32_t saved_rxpll_request = phy.rfpll_requested[0];
            bool saved_rxpll_request_valid = phy.rfpll_requested_valid[0];
            bool saved_rx_source_valid = phy.rfpll_source_valid[0];
            bool saved_tx_source_valid = phy.rfpll_source_valid[1];
            unsigned writes_before = write_count;

            phy.rfpll_source_valid[0] = true;
            phy.rfpll_source_valid[1] = true;
            ret = ad9361_set_rx_lo_freq(&phy, 7000000000ULL);
            if (ret != -EINVAL || write_count != writes_before) {
                fprintf(stderr,
                        "Out-of-range RX LO request reached RFPLL programming: "
                        "ret=%" PRId32 " writes=%u->%u\n",
                        ret, writes_before, write_count);
                return EXIT_FAILURE;
            }
            ret = ad9361_set_rx_lo_freq(&phy, 18000000000ULL);
            if (ret != -EINVAL || write_count != writes_before) {
                fprintf(stderr,
                        "Overflowing RX LO request reached RFPLL programming: "
                        "ret=%" PRId32 " writes=%u->%u\n",
                        ret, writes_before, write_count);
                return EXIT_FAILURE;
            }
            ret = ad9361_set_tx_lo_freq(&phy, 18000000000ULL);
            if (ret != -EINVAL || write_count != writes_before) {
                fprintf(stderr,
                        "Overflowing TX LO request reached RFPLL programming: "
                        "ret=%" PRId32 " writes=%u->%u\n",
                        ret, writes_before, write_count);
                return EXIT_FAILURE;
            }
            ret = clk_set_rate(&phy, &clock_scales[RX_RFPLL_INT],
                               ad9361_to_clk(7000000000ULL));
            if (ret != -EINVAL || write_count != writes_before) {
                fprintf(stderr,
                        "Out-of-range internal RX PLL request reached SPI: "
                        "ret=%" PRId32 " writes=%u->%u\n",
                        ret, writes_before, write_count);
                return EXIT_FAILURE;
            }
            ret = ad9361_rfpll_int_set_rate(
                &clock_scales[RX_RFPLL_INT],
                ad9361_to_clk(7000000000ULL),
                clock_nodes[RX_REFCLK].rate);
            if (ret != -EINVAL || write_count != writes_before) {
                fprintf(stderr,
                        "Out-of-range low-level RX PLL request reached SPI: "
                        "ret=%" PRId32 " writes=%u->%u\n",
                        ret, writes_before, write_count);
                return EXIT_FAILURE;
            }
            ret = ad9361_rfpll_set_rate(
                &clock_scales[RX_RFPLL], ad9361_to_clk(7000000000ULL));
            if (ret != -EINVAL || write_count != writes_before) {
                fprintf(stderr,
                        "Out-of-range RX RFPLL request reached SPI: "
                        "ret=%" PRId32 " writes=%u->%u\n",
                        ret, writes_before, write_count);
                return EXIT_FAILURE;
            }
            clock_nodes[RX_RFPLL].rate = saved_rxpll_rate;
            clock_nodes[RX_RFPLL_INT].rate = saved_rxpll_int_rate;
            phy.rfpll_requested[0] = saved_rxpll_request;
            phy.rfpll_requested_valid[0] = saved_rxpll_request_valid;
            phy.rfpll_source_valid[0] = saved_rx_source_valid;
            phy.rfpll_source_valid[1] = saved_tx_source_valid;
        }

        fail_write_reg = REG_CP_CURRENT;
        ret = clk_set_rate(&phy, bbpll, 960000000);
        if (ret != -EIO || clock_nodes[BBPLL_CLK].rate != 0 ||
            clock_nodes[ADC_CLK].rate != 0 ||
            clock_nodes[BB_REFCLK].rate != 40000000 ||
            phy.bbpll_initialized) {
            fprintf(stderr,
                    "Clock framework did not invalidate failed BBPLL subtree: "
                    "ret=%" PRId32 " bbpll=%" PRIu32 " adc=%" PRIu32
                    " ref=%" PRIu32 " initialized=%d\n",
                    ret, clock_nodes[BBPLL_CLK].rate,
                    clock_nodes[ADC_CLK].rate,
                    clock_nodes[BB_REFCLK].rate, phy.bbpll_initialized);
            return EXIT_FAILURE;
        }
        fail_write_reg = -1;

        fake_regs[REG_CH_1_OVERFLOW] = BBPLL_LOCK;
        fail_read_reg = REG_INTEGER_BB_FREQ_WORD;
        ret = clk_set_rate(&phy, bbpll, 983040000);
        if (ret != -EIO || clock_nodes[BBPLL_CLK].rate != 0 ||
            clock_nodes[ADC_CLK].rate != 0 || phy.bbpll_initialized) {
            fprintf(stderr,
                    "Clock framework hid BBPLL readback failure: ret=%" PRId32
                    " bbpll=%" PRIu32 " adc=%" PRIu32 " initialized=%d\n",
                    ret, clock_nodes[BBPLL_CLK].rate,
                    clock_nodes[ADC_CLK].rate, phy.bbpll_initialized);
            return EXIT_FAILURE;
        }
        fail_read_reg = -1;

        {
            unsigned int writes_before = write_count;
            ret = clk_set_rate(&phy, bbpll, 983040000);
            if (ret != 0 || write_count == writes_before ||
                clock_nodes[BBPLL_CLK].rate == 0 ||
                !phy.bbpll_initialized) {
                fprintf(stderr,
                        "BBPLL recovery request was skipped after readback "
                        "failure: ret=%" PRId32 " writes=%u->%u rate=%" PRIu32
                        " initialized=%d\n", ret, writes_before, write_count,
                        clock_nodes[BBPLL_CLK].rate, phy.bbpll_initialized);
                return EXIT_FAILURE;
            }
        }
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
        phy.rfpll_source_valid[0] = true;
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
    /* BIST loopback now includes the FPGA HDL path; provide a minimally valid
     * converter/AXI fixture so these checks reach the injected RFIC writes. */
    phy.adc_conv = &loopback_conv;
    phy.rx_adc = &loopback_adc;
    loopback_chip.num_channels = 2;
    fake_axi_regs[0x4000 / sizeof(uint32_t)] = UINT32_C(8) << 16;
    fake_axi_regs[0x4418 / sizeof(uint32_t)] = 0x11;
    fake_axi_regs[0x4458 / sizeof(uint32_t)] = 0x22;
    phy.bist_prbs_mode = BIST_INJ_TX;
    phy.bist_config = 0x55;
    phy.bist_config_valid = true;
    fail_write_reg = REG_BIST_CONFIG;
    ret = ad9361_bist_prbs(&phy, BIST_DISABLE);
    if (ret != -EIO || phy.bist_prbs_mode != BIST_INJ_TX ||
        phy.bist_config != 0x55 || phy.bist_config_valid) {
        fprintf(stderr, "BIST PRBS hid write failure or changed cache\n");
        return EXIT_FAILURE;
    }
    {
        enum ad9361_bist_mode mode_sentinel = (enum ad9361_bist_mode)99;
        if (ad9361_get_bist_prbs(&phy, &mode_sentinel) != -EIO ||
            mode_sentinel != (enum ad9361_bist_mode)99) {
            fprintf(stderr, "BIST PRBS getter exposed unknown cached state\n");
            return EXIT_FAILURE;
        }
    }
    fail_write_reg = -1;
    ret = ad9361_bist_prbs(&phy, (enum ad9361_bist_mode)99);
    if (ret != -EINVAL || phy.bist_prbs_mode != BIST_INJ_TX ||
        phy.bist_config != 0x55) {
        fprintf(stderr, "BIST PRBS accepted invalid mode or changed cache\n");
        return EXIT_FAILURE;
    }
    ret = ad9361_bist_prbs(&phy, BIST_DISABLE);
    if (ret != 0 || !phy.bist_config_valid) {
        fprintf(stderr, "BIST PRBS did not revalidate confirmed state\n");
        return EXIT_FAILURE;
    }

    phy.bist_loopback_mode = 2;
    phy.bist_loopback_valid = true;
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
    if (ret != -EIO || phy.bist_loopback_mode != 2 ||
        phy.bist_loopback_valid) {
        fprintf(stderr, "BIST loopback hid config write failure or changed cache\n");
        return EXIT_FAILURE;
    }
    fail_write_reg = -1;
    {
        int32_t mode_sentinel = 99;
        if (ad9361_get_bist_loopback(&phy, &mode_sentinel) != -EIO ||
            mode_sentinel != 99) {
            fprintf(stderr, "BIST getter exposed an unconfirmed loopback mode\n");
            return EXIT_FAILURE;
        }
    }
    /* Reinitialize the cached state for following independent API checks. */
    phy.bist_loopback_valid = true;

    phy.bist_tone_mode = BIST_INJ_TX;
    phy.bist_tone_freq_Hz = 7;
    phy.bist_tone_level_dB = 12;
    phy.bist_tone_mask = 3;
    phy.bist_config = 0x1234;
    {
        unsigned writes_before = write_count;
        if (ad9361_bist_tone(&phy, BIST_DISABLE, 0, 24, 0) != -EINVAL ||
            ad9361_bist_tone(&phy, BIST_DISABLE, 0, 5, 0) != -EINVAL ||
            ad9361_bist_tone(&phy, BIST_DISABLE, 0, 0, 0x10) != -EINVAL ||
            write_count != writes_before) {
            fprintf(stderr, "BIST tone accepted aliased level/mask arguments\n");
            return EXIT_FAILURE;
        }
    }
    fail_write_reg = REG_BIST_AND_DATA_PORT_TEST_CONFIG;
    ret = ad9361_bist_tone(&phy, BIST_DISABLE, 0, 0, 0);
    if (ret != -EIO || phy.bist_tone_mode != BIST_INJ_TX ||
        phy.bist_config != 0x1234 || phy.bist_config_valid) {
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
    {
        enum ad9361_bist_mode mode_sentinel = (enum ad9361_bist_mode)99;
        uint32_t frequency_sentinel = UINT32_MAX;
        uint32_t level_sentinel = UINT32_MAX;
        uint32_t mask_sentinel = UINT32_MAX;
        if (ad9361_get_bist_tone(&phy, &mode_sentinel,
                                 &frequency_sentinel, &level_sentinel,
                                 &mask_sentinel) != -EIO ||
            mode_sentinel != (enum ad9361_bist_mode)99 ||
            frequency_sentinel != UINT32_MAX ||
            level_sentinel != UINT32_MAX || mask_sentinel != UINT32_MAX) {
            fprintf(stderr, "BIST tone getter exposed unknown cached state\n");
            return EXIT_FAILURE;
        }
    }
    fail_write_reg = -1;
	phy.bist_config = 0x1234;
	fail_write_reg = REG_BIST_AND_DATA_PORT_TEST_CONFIG;
	ret = ad9361_write_bist_reg(&phy, 0x78);
	if (ret != -EIO || phy.bist_config != 0x1234) {
		fprintf(stderr, "BIST register API committed cache after SPI error\n");
		return EXIT_FAILURE;
	}
	fail_write_reg = -1;

	/* RX and TX RF-port setters must keep their cached selection aligned with
	 * a confirmed register write. */
	phy.pdata->rf_rx_input_sel = 0;
	phy.pdata->rf_tx_output_sel = 0;
	fail_write_reg = REG_INPUT_SELECT;
	ret = ad9361_set_rx_rf_port_input(&phy, 1);
	rx_rf_port = UINT32_MAX;
	tx_rf_port = UINT32_MAX;
	if (ret != -EIO || phy.pdata->rf_rx_input_sel != 0 ||
	    phy.rf_port_select_valid ||
	    ad9361_get_rx_rf_port_input(&phy, &rx_rf_port) != -EIO ||
	    ad9361_get_tx_rf_port_output(&phy, &tx_rf_port) != -EIO ||
	    rx_rf_port != UINT32_MAX || tx_rf_port != UINT32_MAX) {
		fprintf(stderr, "RX RF-port API hid error or committed cache\n");
		return EXIT_FAILURE;
	}
	{
		unsigned writes_before = write_count;
		if (ad9361_rf_port_setup(&phy, false, 0, 2) != -EINVAL ||
		    write_count != writes_before) {
			fprintf(stderr, "RF-port helper truncated an invalid TX selector\n");
			return EXIT_FAILURE;
		}
	}
	phy_without_spi.pdata = &pdata;
	if (ad9361_set_rx_rf_port_input(NULL, 0) != -EINVAL ||
	    ad9361_set_rx_rf_port_input(&phy_without_spi, 0) != -EINVAL ||
	    ad9361_set_rx_rf_port_input(&phy, 12) != -EINVAL ||
	    ad9361_get_rx_rf_port_input(NULL, &rx_rf_port) != -EINVAL ||
	    ad9361_get_rx_rf_port_input(&phy, NULL) != -EINVAL ||
	    rx_rf_port != UINT32_MAX || phy.pdata->rf_rx_input_sel != 0) {
		fprintf(stderr, "RX RF-port API accepted invalid state/mode/output\n");
		return EXIT_FAILURE;
	}
	fail_write_reg = -1;
	ret = ad9361_set_rx_rf_port_input(&phy, 0);
	rx_rf_port = UINT32_MAX;
	tx_rf_port = UINT32_MAX;
	if (ret != 0 || !phy.rf_port_select_valid ||
	    ad9361_get_rx_rf_port_input(&phy, &rx_rf_port) != 0 ||
	    ad9361_get_tx_rf_port_output(&phy, &tx_rf_port) != 0 ||
	    rx_rf_port != 0 || tx_rf_port != 0) {
		fprintf(stderr, "RF-port cache did not recover after full write\n");
		return EXIT_FAILURE;
	}
	phy.pdata->rf_tx_output_sel = 0;
	fail_write_reg = REG_INPUT_SELECT;
	ret = ad9361_set_tx_rf_port_output(&phy, 1);
	rx_rf_port = UINT32_MAX;
	tx_rf_port = UINT32_MAX;
	if (ret != -EIO || phy.pdata->rf_tx_output_sel != 0 ||
	    phy.rf_port_select_valid ||
	    ad9361_get_rx_rf_port_input(&phy, &rx_rf_port) != -EIO ||
	    ad9361_get_tx_rf_port_output(&phy, &tx_rf_port) != -EIO ||
	    rx_rf_port != UINT32_MAX || tx_rf_port != UINT32_MAX ||
	    ad9361_set_tx_rf_port_output(&phy, 2) != -EINVAL) {
		fprintf(stderr, "TX RF-port API hid error, committed cache, or accepted invalid mode\n");
		return EXIT_FAILURE;
	}
	fail_write_reg = -1;
	ret = ad9361_set_tx_rf_port_output(&phy, 0);
	if (ret != 0 || !phy.rf_port_select_valid ||
	    ad9361_get_tx_rf_port_output(&phy, &tx_rf_port) != 0 ||
	    tx_rf_port != 0) {
		fprintf(stderr, "TX RF-port cache did not recover after full write\n");
		return EXIT_FAILURE;
	}
	phy_without_spi.pdata = &pdata;
	tx_rf_port = UINT32_MAX;
	if (ad9361_set_tx_rf_port_output(NULL, 0) != -EINVAL ||
	    ad9361_set_tx_rf_port_output(&phy_without_spi, 0) != -EINVAL ||
	    ad9361_get_tx_rf_port_output(NULL, &tx_rf_port) != -EINVAL ||
	    ad9361_get_tx_rf_port_output(&phy, NULL) != -EINVAL ||
	    tx_rf_port != UINT32_MAX) {
		fprintf(stderr, "TX RF-port API accepted missing state/output\n");
		return EXIT_FAILURE;
	}
	if (ad9361_set_rx_rf_bandwidth(NULL, 1000000) != -EINVAL ||
	    ad9361_set_tx_rf_bandwidth(NULL, 1000000) != -EINVAL ||
	    ad9361_get_rx_rf_bandwidth(NULL, &bandwidth) != -EINVAL ||
	    ad9361_get_tx_rf_bandwidth(NULL, &bandwidth) != -EINVAL ||
	    ad9361_get_rx_rf_bandwidth(&phy, NULL) != -EINVAL ||
	    ad9361_get_tx_rf_bandwidth(&phy, NULL) != -EINVAL ||
	    ad9361_set_rx_sampling_freq(NULL, 1000000) != -EINVAL ||
	    ad9361_set_tx_sampling_freq(NULL, 1000000) != -EINVAL ||
	    ad9361_get_rx_sampling_freq(NULL, &sampling_rate) != -EINVAL ||
	    ad9361_get_tx_sampling_freq(NULL, &sampling_rate) != -EINVAL ||
	    bandwidth != UINT32_MAX || sampling_rate != UINT32_MAX) {
		fprintf(stderr, "RX/TX rate API accepted missing state/output\n");
		return EXIT_FAILURE;
	}
	if (ad9361_set_rx_lo_int_ext(NULL, INT_LO) != -EINVAL ||
	    ad9361_set_tx_lo_int_ext(NULL, INT_LO) != -EINVAL ||
	    ad9361_set_rx_fir_config(NULL, rx_fir) != -EINVAL ||
	    ad9361_set_tx_fir_config(NULL, tx_fir) != -EINVAL ||
	    ad9361_set_rx_fir_en_dis(NULL, 1) != -EINVAL ||
	    ad9361_set_tx_fir_en_dis(NULL, 1) != -EINVAL ||
	    ad9361_get_rx_fir_en_dis(NULL, &tracking_flag) != -EINVAL ||
	    ad9361_get_tx_fir_en_dis(NULL, &tracking_flag) != -EINVAL ||
	    ad9361_get_rx_lo_power(NULL, &tracking_flag) != -EINVAL ||
	    ad9361_get_tx_lo_power(NULL, &tracking_flag) != -EINVAL ||
	    ad9361_set_tx_auto_cal_en_dis(NULL, 1) != -EINVAL ||
	    ad9361_get_tx_auto_cal_en_dis(NULL, &tracking_flag) != -EINVAL ||
	    tracking_flag != UINT8_MAX) {
		fprintf(stderr, "RX/TX LO/FIR APIs accepted missing state/output\n");
		return EXIT_FAILURE;
	}
	{
		unsigned int writes_before = write_count;
		if (ad9361_synth_lo_powerdown(NULL, LO_ON, LO_DONTCARE) != -EINVAL ||
		    ad9361_synth_lo_powerdown(&phy, (enum synth_pd_ctrl)99,
					      LO_DONTCARE) != -EINVAL ||
		    ad9361_rx_lo_powerdown(NULL, 0) != -EINVAL ||
		    ad9361_tx_lo_powerdown(NULL, 0) != -EINVAL ||
		    ad9361_rx_lo_powerdown(&phy, 2) != -EINVAL ||
		    ad9361_tx_lo_powerdown(&phy, 2) != -EINVAL ||
		    write_count != writes_before) {
			fprintf(stderr, "LO power control accepted invalid state/option\n");
			return EXIT_FAILURE;
		}
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
        struct no_os_spi_desc *saved_spi = phy.spi;
        uint32_t saved_rx_profile_flags = phy.fastlock.entry[0][0].flags;
        uint32_t saved_tx_profile_flags = phy.fastlock.entry[1][0].flags;
        uint64_t saved_current_rx_lo = phy.current_rx_lo_freq;
        uint8_t saved_cached_rx_div = phy.cached_rx_rfpll_div;
        uint8_t saved_current_rx_profile = phy.fastlock.current_profile[0];
        bool saved_rx_request_valid = phy.rfpll_requested_valid[0];

        /* Force the getter through its SPI fallback rather than an already
         * cached profile image. */
        phy.fastlock.entry[0][0].flags = 0;
        phy.fastlock.entry[1][0].flags = 0;
        phy.spi = NULL;
        ret = ad9361_rx_fastlock_get_freq(&phy, 0, &frequency);
        if (ret != -EINVAL || frequency != UINT64_C(0xfeedface) ||
            read_count != reads_before) {
            fprintf(stderr, "RX fastlock getter accepted missing SPI: ret=%" PRId32
                    " frequency=%" PRIu64 "\n", ret, frequency);
            return EXIT_FAILURE;
        }
        ret = ad9361_tx_fastlock_get_freq(&phy, 0, &frequency);
        if (ret != -EINVAL || frequency != UINT64_C(0xfeedface) ||
            read_count != reads_before) {
            fprintf(stderr, "TX fastlock getter accepted missing SPI: ret=%" PRId32
                    " frequency=%" PRIu64 "\n", ret, frequency);
            return EXIT_FAILURE;
        }
        phy.spi = saved_spi;

        fail_read_reg = REG_RX_FAST_LOCK_PROGRAM_READ;
        ret = ad9361_rx_fastlock_get_freq(&phy, 0, &frequency);
        if (ret != -EIO || frequency != UINT64_C(0xfeedface)) {
            fprintf(stderr, "RX fastlock getter hid profile SPI error: ret=%" PRId32
                    " frequency=%" PRIu64 "\n", ret, frequency);
            return EXIT_FAILURE;
        }
        phy.current_rx_lo_freq = 1800000000;
        phy.cached_rx_rfpll_div = 2;
        phy.rfpll_requested_valid[0] = true;
        phy.fastlock.entry[0][0].flags = 0;
        ret = ad9361_fastlock_sync_current_lo(&phy, false, 0,
                                               1800000000, &frequency);
        if (ret != -EIO || phy.current_rx_lo_freq != 0 ||
            phy.cached_rx_rfpll_div != 0 || phy.rfpll_requested_valid[0]) {
            fprintf(stderr,
                    "Failed fastlock sync retained stale RX RFPLL cache: "
                    "ret=%" PRId32 " cached=%" PRIu64 " div=%u valid=%u\n",
                    ret, phy.current_rx_lo_freq, phy.cached_rx_rfpll_div,
                    phy.rfpll_requested_valid[0]);
            return EXIT_FAILURE;
        }
        if (phy.fastlock.current_profile[0] != 0xffu) {
            fprintf(stderr,
                    "Failed fastlock sync did not mark RX profile unknown: %u\n",
                    phy.fastlock.current_profile[0]);
            return EXIT_FAILURE;
        }
        phy.fastlock.entry[0][0].flags = saved_rx_profile_flags;
        phy.current_rx_lo_freq = saved_current_rx_lo;
        phy.cached_rx_rfpll_div = saved_cached_rx_div;
        phy.fastlock.current_profile[0] = saved_current_rx_profile;
        phy.rfpll_requested_valid[0] = saved_rx_request_valid;
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
        phy.fastlock.entry[1][0].flags = saved_tx_profile_flags;
    }

    /* An out-of-band profile-bank write invalidates only the corresponding
     * direction's cached bytes, forcing subsequent LO verification to read
     * the actual RFIC profile. */
    {
        uint8_t saved_rx0_flags = phy.fastlock.entry[0][0].flags;
        uint8_t saved_rx7_flags = phy.fastlock.entry[0][7].flags;
        uint8_t saved_tx0_flags = phy.fastlock.entry[1][0].flags;

        phy.fastlock.entry[0][0].flags = FASTLOOK_INIT;
        phy.fastlock.entry[0][7].flags = FASTLOOK_INIT;
        phy.fastlock.entry[1][0].flags = FASTLOOK_INIT;
        ad9361_fastlock_invalidate_cache(&phy, false);
        if (phy.fastlock.entry[0][0].flags != 0 ||
            phy.fastlock.entry[0][7].flags != 0 ||
            phy.fastlock.entry[1][0].flags != FASTLOOK_INIT) {
            fprintf(stderr, "RX fastlock invalidation changed wrong cache entries\n");
            return EXIT_FAILURE;
        }
        phy.fastlock.entry[0][0].flags = FASTLOOK_INIT;
        phy.fastlock.entry[0][7].flags = FASTLOOK_INIT;
        ad9361_fastlock_invalidate_cache(&phy, true);
        if (phy.fastlock.entry[0][0].flags != FASTLOOK_INIT ||
            phy.fastlock.entry[0][7].flags != FASTLOOK_INIT ||
            phy.fastlock.entry[1][0].flags != 0) {
            fprintf(stderr, "TX fastlock invalidation changed wrong cache entries\n");
            return EXIT_FAILURE;
        }
        phy.fastlock.entry[0][0].flags = saved_rx0_flags;
        phy.fastlock.entry[0][7].flags = saved_rx7_flags;
        phy.fastlock.entry[1][0].flags = saved_tx0_flags;
    }

    /* A confirmed fastlock profile sync updates the active ADI LO/divider
     * model and invalidates the host frequency-request cache. */
    {
        struct no_os_clk refclk = { .rate = 40000000 };
        struct no_os_clk rxpll_clk = { .rate = 1 };
        struct refclk_scale rx_refclk = {
            .spi = &spi,
            .phy = &phy,
            .source = RX_REFCLK,
        };
        uint8_t saved_profile_data[RX_FAST_LOCK_CONFIG_WORD_NUM];
        uint8_t saved_rx_ref_divider_msb =
            fake_regs[REG_REF_DIVIDE_CONFIG_1];
        uint8_t saved_rx_ref_divider_lsb =
            fake_regs[REG_REF_DIVIDE_CONFIG_2];
        uint64_t saved_lo = phy.current_rx_lo_freq;
        uint8_t saved_div = phy.cached_rx_rfpll_div;
        uint8_t saved_current_profile = phy.fastlock.current_profile[0];
        bool saved_requested_valid = phy.rfpll_requested_valid[0];
        bool saved_register_map_enabled = register_map_enabled;
        struct no_os_clk *saved_refclk = phy.clk_refin;
        struct no_os_clk *saved_rxpll = phy.clks[RX_RFPLL];
        struct refclk_scale *saved_rx_refclk = phy.ref_clk_scale[RX_REFCLK];
        uint32_t saved_profile_flags = phy.fastlock.entry[0][0].flags;
        uint64_t frequency = 0;
        uint8_t *profile_data = phy.fastlock.entry[0][0].profile_data;

        memcpy(saved_profile_data, profile_data, sizeof(saved_profile_data));
        memset(profile_data, 0, RX_FAST_LOCK_CONFIG_WORD_NUM);
        profile_data[0] = 190; /* 40 MHz * 190 = 7.6 GHz VCO. */
        profile_data[12] = 1; /* Divide VCO by four for 1.9 GHz LO. */
        phy.fastlock.entry[0][0].flags = FASTLOOK_INIT;
        phy.clk_refin = &refclk;
        phy.clks[RX_RFPLL] = &rxpll_clk;
        phy.ref_clk_scale[RX_REFCLK] = &rx_refclk;
        phy.current_rx_lo_freq = ad9361_to_clk(1800000000);
        phy.cached_rx_rfpll_div = 2;
        phy.rfpll_requested_valid[0] = true;
        /* This case models an undivided 40 MHz RX reference. Other test
         * cases leave register-map state behind, so establish the scaler
         * explicitly before checking the 6.2 GHz out-of-range profile. */
        fake_regs[REG_REF_DIVIDE_CONFIG_1] = 0;
        fake_regs[REG_REF_DIVIDE_CONFIG_2] = 0;
        register_map_enabled = true;
        ret = ad9361_fastlock_sync_current_lo(
            &phy, false, 0, 1900000000, &frequency);
        register_map_enabled = saved_register_map_enabled;
        if (ret != 0 || frequency != 1900000000 ||
            phy.current_rx_lo_freq != ad9361_to_clk(1900000000) ||
            phy.cached_rx_rfpll_div != 1 ||
            phy.fastlock.current_profile[0] != 1 ||
            phy.rfpll_requested_valid[0]) {
            fprintf(stderr,
                    "Fastlock sync did not publish profile state: ret=%" PRId32
                    " freq=%" PRIu64 " cached=%" PRIu64 " div=%u "
                    "profile=%u valid=%u\n",
                    ret, frequency, phy.current_rx_lo_freq,
                    phy.cached_rx_rfpll_div,
                    phy.fastlock.current_profile[0],
                    phy.rfpll_requested_valid[0]);
            return EXIT_FAILURE;
        }

        /* NIOS can replace the RFIC fastlock bank without updating this
         * driver's cached profile image. Invalidate the direction before
         * syncing a NIOS-owned recall so readback comes from the bank. */
        {
            unsigned reads_before_invalidation = read_count;

            fake_regs[REG_RX_FAST_LOCK_PROGRAM_READ] = 0;
            ad9361_fastlock_invalidate_cache(&phy, false);
            frequency = UINT64_C(0xfeedface);
            ret = ad9361_fastlock_sync_current_lo(
                &phy, false, 0, 0, &frequency);
            if (ret != -EINVAL ||
                frequency != UINT64_C(0xfeedface) ||
                read_count <= reads_before_invalidation ||
                phy.fastlock.entry[0][0].flags != 0) {
                fprintf(stderr,
                        "Fastlock sync trusted stale bytes after out-of-band overwrite: "
                        "ret=%" PRId32 " freq=%" PRIu64 " reads=%u/%u flags=%" PRIu32 "\n",
                        ret, frequency, read_count, reads_before_invalidation,
                        phy.fastlock.entry[0][0].flags);
                return EXIT_FAILURE;
            }
        }

        /* A cached profile can still be malformed or externally modified.
         * With no expected host frequency, sync must reject an out-of-band
         * decoded LO instead of publishing its truncated cache value. */
        phy.fastlock.entry[0][0].flags = FASTLOOK_INIT;
        profile_data[0] = 0x36; /* Integer 310 with profile_data[1]. */
        profile_data[1] = 0x01;
        profile_data[12] = 0; /* 12.4 GHz VCO / 2 = 6.2 GHz LO. */
        frequency = UINT64_C(0xfeedface);
        register_map_enabled = true;
        ret = ad9361_fastlock_sync_current_lo(
            &phy, false, 0, 0, &frequency);
        register_map_enabled = saved_register_map_enabled;
        if (ret != -EINVAL || frequency != UINT64_C(0xfeedface) ||
            phy.current_rx_lo_freq != 0 ||
            phy.fastlock.current_profile[0] != 0xffu ||
            phy.rfpll_requested_valid[0]) {
            fprintf(stderr,
                    "Out-of-range fastlock profile certified RX LO: "
                    "ret=%" PRId32 " freq=%" PRIu64 " cached=%" PRIu64
                    " profile=%u request_valid=%u\n",
                    ret, frequency, phy.current_rx_lo_freq,
                    phy.fastlock.current_profile[0],
                    phy.rfpll_requested_valid[0]);
            return EXIT_FAILURE;
        }

        memcpy(profile_data, saved_profile_data, sizeof(saved_profile_data));
        phy.fastlock.entry[0][0].flags = saved_profile_flags;
        phy.current_rx_lo_freq = saved_lo;
        phy.cached_rx_rfpll_div = saved_div;
        phy.fastlock.current_profile[0] = saved_current_profile;
        phy.rfpll_requested_valid[0] = saved_requested_valid;
        phy.clk_refin = saved_refclk;
        phy.clks[RX_RFPLL] = saved_rxpll;
        phy.ref_clk_scale[RX_REFCLK] = saved_rx_refclk;
        fake_regs[REG_REF_DIVIDE_CONFIG_1] = saved_rx_ref_divider_msb;
        fake_regs[REG_REF_DIVIDE_CONFIG_2] = saved_rx_ref_divider_lsb;
    }

    /* Malformed clocks/durations are rejected before arithmetic or writes. */
    {
        struct refclk_scale factor = { .spi = &spi, .phy = &phy };
        struct tx_monitor_control txmon = {0};
        struct auxadc_control auxadc = {
            .temp_time_inteval_ms = 1000,
            .temp_sensor_decimation = 32768,
            .auxadc_clock_rate = 40000000,
            .auxadc_decimation = 32768,
            .periodic_temp_measuremnt = true,
        };
        struct rssi_control rssi = {
            .rssi_delay = UINT32_MAX,
            .rssi_wait = UINT32_MAX,
            .rssi_duration = 512,
        };
        struct gain_control gain = {0};
        struct ad9361_phy_platform_data agc_pdata = {0};
        struct ad9361_rf_phy agc_phy = { .spi = &spi, .pdata = &agc_pdata };
        struct no_os_clk agc_rf_clock = { .rate = 30720000 };
        struct no_os_clk agc_sample_clock = { .rate = 1000000 };
        uint32_t gain_update_counter = UINT32_MAX;
        uint32_t zero_rate = 0;
        unsigned writes_before = write_count;
        unsigned reads_before;

        agc_phy.clks[CLKRF_CLK] = &agc_rf_clock;
        agc_phy.clks[RX_SAMPL_CLK] = &agc_sample_clock;
        agc_pdata.gain_ctrl.dec_pow_measuremnt_duration = 16;
        agc_pdata.gain_ctrl.f_agc_dec_pow_measuremnt_duration = 16;
        agc_pdata.gain_ctrl.f_agc_power_measurement_duration_in_state5 = 16;
        agc_pdata.gain_ctrl.lmt_overload_high_thresh = 16;
        agc_pdata.gain_ctrl.lmt_overload_low_thresh = 16;

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
        writes_before = write_count;
        ret = ad9361_gc_setup(&agc_phy, &agc_pdata.gain_ctrl);
        if (ret != -EINVAL || write_count != writes_before ||
            agc_phy.agc_mode[0] != 0 || agc_phy.agc_mode[1] != 0) {
            fprintf(stderr,
                    "AGC setup partially configured invalid timing: "
                    "ret=%" PRId32 " writes=%u/%u modes=%u/%u\n",
                    ret, write_count, writes_before,
                    agc_phy.agc_mode[0], agc_phy.agc_mode[1]);
            return EXIT_FAILURE;
        }
        agc_pdata.gain_ctrl.gain_update_interval_us = 100;
        agc_pdata.gain_ctrl.lmt_overload_low_thresh = 15;
        writes_before = write_count;
        ret = ad9361_gc_setup(&agc_phy, &agc_pdata.gain_ctrl);
        if (ret != -EINVAL || write_count != writes_before ||
            agc_phy.agc_mode[0] != 0 || agc_phy.agc_mode[1] != 0) {
            fprintf(stderr,
                    "AGC setup accepted invalid LMT threshold: "
                    "ret=%" PRId32 " writes=%u/%u modes=%u/%u\n",
                    ret, write_count, writes_before,
                    agc_phy.agc_mode[0], agc_phy.agc_mode[1]);
            return EXIT_FAILURE;
        }
        agc_pdata.gain_ctrl.lmt_overload_low_thresh = 16;
        if (ad9361_gc_gain_update_counter(0, 30720000, 20,
                                         &gain_update_counter) != -EINVAL ||
            gain_update_counter != UINT32_MAX ||
            ad9361_gc_gain_update_counter(1, 30720000, 20,
                                         &gain_update_counter) != -EINVAL ||
            gain_update_counter != UINT32_MAX ||
            ad9361_gc_gain_update_counter(2, 30720000, 20,
                                         &gain_update_counter) != 0 ||
            gain_update_counter != 10 ||
            ad9361_gc_gain_update_counter(UINT32_MAX, 30720000, 20,
                                         &gain_update_counter) != 0 ||
            gain_update_counter != 131071) {
            fprintf(stderr,
                    "AGC update counter underflow/overflow policy failed\n");
            return EXIT_FAILURE;
        }
		agc_pdata.gain_ctrl.gain_update_interval_us = 0;
        writes_before = write_count;
        ret = ad9361_gc_update(&agc_phy);
        if (ret != -EINVAL || write_count != writes_before) {
            fprintf(stderr,
                    "AGC update accepted zero interval or wrote before reject: "
                    "ret=%" PRId32 " writes=%u/%u\n",
                    ret, write_count, writes_before);
            return EXIT_FAILURE;
        }

        /* The eLNA delay is an unconstrained 32-bit platform value. Invalid
         * low RF clocks must fail before division, and extreme delays must
         * saturate each 5-bit timing field instead of wrapping before clamp. */
        agc_pdata.gain_ctrl.gain_update_interval_us = 100;
        agc_rf_clock.rate = 499;
        writes_before = write_count;
        ret = ad9361_gc_setup(&agc_phy, &agc_pdata.gain_ctrl);
        if (ret != -EIO || write_count != writes_before ||
            agc_phy.agc_mode[0] != 0 || agc_phy.agc_mode[1] != 0) {
            fprintf(stderr,
                    "AGC setup partially configured low RF clock: "
                    "ret=%" PRId32 " writes=%u/%u modes=%u/%u\n",
                    ret, write_count, writes_before,
                    agc_phy.agc_mode[0], agc_phy.agc_mode[1]);
            return EXIT_FAILURE;
        }
        ret = ad9361_gc_update(&agc_phy);
        if (ret != -EIO || write_count != writes_before) {
            fprintf(stderr,
                    "AGC update accepted low RF clock or wrote before reject: "
                    "ret=%" PRId32 " writes=%u/%u\n",
                    ret, write_count, writes_before);
            return EXIT_FAILURE;
        }
        agc_rf_clock.rate = 30720000;
        agc_pdata.elna_ctrl.settling_delay_ns = UINT32_MAX;
        agc_pdata.gain_ctrl.f_agc_state_wait_time_ns = UINT32_MAX;
        register_map_enabled = true;
        ret = ad9361_gc_update(&agc_phy);
        if (ret != 0 || fake_regs[REG_AGC_ATTACK_DELAY] != 31 ||
            fake_regs[REG_PEAK_WAIT_TIME] != 31 ||
            fake_regs[REG_FAST_CONFIG_2_SETTLING_DELAY] != 31 ||
            fake_regs[REG_FAST_ENERGY_DETECT_COUNT] != 31) {
            fprintf(stderr,
                    "AGC timing arithmetic failed to saturate safely: "
                    "ret=%" PRId32 " attack=%u peak=%u settle=%u energy=%u\n",
                    ret, fake_regs[REG_AGC_ATTACK_DELAY],
                    fake_regs[REG_PEAK_WAIT_TIME],
                    fake_regs[REG_FAST_CONFIG_2_SETTLING_DELAY],
                    fake_regs[REG_FAST_ENERGY_DETECT_COUNT]);
            return EXIT_FAILURE;
        }
        register_map_enabled = true;
        spi_status = 0;
        fail_write_reg = -1;
        ret = ad9361_rssi_setup(&agc_phy, &rssi, false);
        if (ret != 0 || fake_regs[REG_RSSI_DELAY] != 255 ||
            fake_regs[REG_RSSI_WAIT_TIME] != 255) {
            fprintf(stderr,
                    "RSSI delay arithmetic did not saturate: "
                    "ret=%" PRId32 " delay=%u wait=%u\n",
                    ret, fake_regs[REG_RSSI_DELAY],
                    fake_regs[REG_RSSI_WAIT_TIME]);
            return EXIT_FAILURE;
        }
        writes_before = write_count;
        ret = ad9361_auxadc_setup(&phy, &auxadc, 983040000);
        if (ret != 0 || write_count != writes_before + 6 ||
            fake_regs[REG_TEMP_SENSOR_CONFIG] != 7 ||
            fake_regs[REG_AUXADC_CONFIG] != 14 ||
            fake_regs[REG_AUXADC_CLOCK_DIVIDER] != 24) {
            fprintf(stderr,
                    "AuxADC maximum valid fields were not programmed: "
                    "ret=%" PRId32 " writes=%u/%u temp=%u aux=%u div=%u\n",
                    ret, write_count, writes_before,
                    fake_regs[REG_TEMP_SENSOR_CONFIG],
                    fake_regs[REG_AUXADC_CONFIG],
                    fake_regs[REG_AUXADC_CLOCK_DIVIDER]);
            return EXIT_FAILURE;
        }
        auxadc.temp_sensor_decimation = 65536;
        writes_before = write_count;
        ret = ad9361_auxadc_setup(&phy, &auxadc, 983040000);
        if (ret != -EINVAL || write_count != writes_before) {
            fprintf(stderr,
                    "AuxADC accepted decimation beyond 3-bit range: "
                    "ret=%" PRId32 " writes=%u/%u\n",
                    ret, write_count, writes_before);
            return EXIT_FAILURE;
        }
        auxadc.temp_sensor_decimation = 256;
        auxadc.auxadc_clock_rate = 1000000;
        ret = ad9361_auxadc_setup(&phy, &auxadc, 983040000);
        if (ret != -ERANGE || write_count != writes_before) {
            fprintf(stderr,
                    "AuxADC accepted divider beyond 6-bit range: "
                    "ret=%" PRId32 " writes=%u/%u\n",
                    ret, write_count, writes_before);
            return EXIT_FAILURE;
        }
        auxadc.auxadc_clock_rate = 40000000;
        auxadc.temp_time_inteval_ms = UINT32_MAX;
        ret = ad9361_auxadc_setup(&phy, &auxadc, 983040000);
        if (ret != -ERANGE || write_count != writes_before) {
            fprintf(stderr,
                    "AuxADC interval overflow was not rejected: "
                    "ret=%" PRId32 " writes=%u/%u\n",
                    ret, write_count, writes_before);
            return EXIT_FAILURE;
        }
        register_map_enabled = false;
        ret = ad9361_txmon_setup(&phy, &txmon);
        if (ret != -EINVAL) {
            fprintf(stderr, "TX monitor accepted zero duration: ret=%" PRId32 "\n", ret);
            return EXIT_FAILURE;
        }
        phy.rxbbf_div = 0;
        reads_before = read_count;
        ret = ad9361_rx_adc_setup(&phy, 983040000, 40000000);
        if (ret != -EIO || read_count != reads_before) {
            fprintf(stderr, "RX ADC setup accepted zero filter divider: ret=%" PRId32
                    " reads=%u/%u\n", ret, read_count, reads_before);
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

    /* Error-table programming must stop its indirect-table clock even when
     * the final stop write reached hardware but the SPI response failed. */
    {
        register_map_enabled = true;
        memset(fake_regs, 0, sizeof(fake_regs));
        fake_write_counts[REG_CONFIG] = 0;
        fail_write_once_reg = REG_CONFIG;
        fail_write_once_reg_occurrence = 7;
        apply_failed_write_once = true;
        ret = ad9361_rssi_program_lna_gain(&phy);
        if (ret != -EIO || fake_regs[REG_CONFIG] != 0 ||
            fake_write_counts[REG_CONFIG] != 8) {
            fprintf(stderr,
                    "RSSI LNA table skipped clock-stop retry: ret=%" PRId32
                    " reg=0x%02x writes=%u\n", ret,
                    fake_regs[REG_CONFIG], fake_write_counts[REG_CONFIG]);
            return EXIT_FAILURE;
        }

        fake_write_counts[REG_CONFIG] = 0;
        fail_write_once_reg = REG_CONFIG;
        fail_write_once_reg_occurrence = 23;
        apply_failed_write_once = true;
        ret = ad9361_rssi_write_err_tbl(&phy);
        if (ret != -EIO || fake_regs[REG_CONFIG] != 0 ||
            fake_write_counts[REG_CONFIG] != 24) {
            fprintf(stderr,
                    "RSSI error table skipped clock-stop retry: ret=%" PRId32
                    " reg=0x%02x writes=%u\n", ret,
                    fake_regs[REG_CONFIG], fake_write_counts[REG_CONFIG]);
            return EXIT_FAILURE;
        }
        fail_write_once_reg = -1;
        fail_write_once_reg_occurrence = 1;
        apply_failed_write_once = false;
        register_map_enabled = false;
    }

    /* A rejected FIR transition must invalidate the cache. Retrying with the
     * same cached bypass state must rerun validation instead of returning
     * through the old no-op path. */
    {
        unsigned reads_before = read_count;
        unsigned writes_before = write_count;
        phy.bypass_rx_fir = true;
        phy.bypass_tx_fir = true;
        phy.rx_fir_dec = 0;
        phy.filt_valid = false;
        phy.fir_config_valid = true;
        ret = ad9361_set_rx_fir_en_dis(&phy, 1);
        if (ret != -EINVAL || !phy.bypass_rx_fir || phy.fir_config_valid ||
            read_count != reads_before || write_count != writes_before) {
            fprintf(stderr, "Rejected FIR transition retained a valid cache\n");
            return EXIT_FAILURE;
        }

        ret = ad9361_set_rx_fir_en_dis(&phy, 1);
        if (ret != -EINVAL || !phy.bypass_rx_fir || phy.fir_config_valid ||
            read_count != reads_before || write_count != writes_before) {
            fprintf(stderr, "FIR retry with stale cache was skipped\n");
            return EXIT_FAILURE;
        }
    }

    puts("AD9361 calibration, clock, ENSM, AGC, RSSI, and FIR SPI-error propagation: PASS");
    return EXIT_SUCCESS;
}
