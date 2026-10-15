/* Regression test: calibration status reads must propagate SPI errors. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../thirdparty/analogdevicesinc/no-OS/drivers/rf-transceiver/ad9361/ad9361.c"
#include "../../thirdparty/analogdevicesinc/no-OS/drivers/rf-transceiver/ad9361/ad9361_util.c"
#include "../../thirdparty/analogdevicesinc/no-OS/drivers/rf-transceiver/ad9361/ad9361_ext_band_ctrl.c"
#include "../../thirdparty/analogdevicesinc/no-OS/drivers/rf-transceiver/ad9361/ad9361_conv.c"
#include "../../thirdparty/analogdevicesinc/no-OS/drivers/axi_core/axi_adc_core/axi_adc_core.c"
#include "../../thirdparty/analogdevicesinc/no-OS/drivers/axi_core/axi_dac_core/axi_dac_core.c"

static int32_t fake_clock_chain_status;
static unsigned fake_bandwidth_update_count;
static unsigned fake_bandwidth_update_fail_call;
static int32_t fake_bandwidth_update_fail_status;
static unsigned fake_bandwidth_cleanup_fail_call;
static int32_t fake_bandwidth_cleanup_fail_status;
static int32_t fake_clock_chain_cleanup_status;
static bool fake_sample_rate_transaction_enabled;
static int32_t fake_sample_rate_calculate_status;
static int32_t fake_sample_rate_clock_readback_status;
static bool fake_sample_rate_corrupt_rollback_readback;
static unsigned fake_sample_rate_clock_set_count;
static unsigned fake_sample_rate_clock_set_attempt_count;
static unsigned fake_sample_rate_clock_set_fail_attempt;
static unsigned fake_sample_rate_bw_start_count;
static unsigned fake_sample_rate_bw_poll_count;
static unsigned fake_sample_rate_bw_fail_poll;
static uint32_t fake_sample_rate_rx_clks[NUM_RX_CLOCKS];
static uint32_t fake_sample_rate_tx_clks[NUM_TX_CLOCKS];
static bool fake_mux_commit_parent;
static int32_t fake_mux_parent_status;
static int32_t fake_mux_cleanup_status;
static int32_t fake_axi_adc_remove_status;
static int32_t fake_axi_dac_remove_status;
static unsigned fake_axi_adc_remove_count;
static unsigned fake_axi_dac_remove_count;

static int32_t test_calculate_rf_clock_chain(struct ad9361_rf_phy *phy,
                                            uint32_t tx_sample_rate,
                                            uint32_t rate_governor,
                                            uint32_t *rx_path_clks,
                                            uint32_t *tx_path_clks);
static int32_t test_set_trx_clock_chain(struct ad9361_rf_phy *phy,
                                        const uint32_t *rx_path_clks,
                                        const uint32_t *tx_path_clks);
static int32_t test_set_trx_clock_chain_with_cleanup(
    struct ad9361_rf_phy *phy, const uint32_t *rx_path_clks,
    const uint32_t *tx_path_clks, int32_t *cleanup_status);
static int32_t test_clock_chain_transaction_start(
    struct ad9361_rf_phy *phy, const uint32_t *rx_path_clks,
    const uint32_t *tx_path_clks,
    struct ad9361_clock_chain_transaction *transaction);
static int32_t test_clock_chain_transaction_poll(
    struct ad9361_rf_phy *phy,
    struct ad9361_clock_chain_transaction *transaction);
static int32_t test_get_trx_clock_chain(struct ad9361_rf_phy *phy,
                                        uint32_t *rx_path_clks,
                                        uint32_t *tx_path_clks);
static int32_t test_rf_bandwidth_transaction_start(
    struct ad9361_rf_phy *phy, uint32_t rx_bw, uint32_t tx_bw,
    struct ad9361_rf_bandwidth_transaction *transaction);
static int32_t test_rf_bandwidth_transaction_poll(
    struct ad9361_rf_phy *phy,
    struct ad9361_rf_bandwidth_transaction *transaction);
static int32_t test_update_rf_bandwidth(struct ad9361_rf_phy *phy,
                                        uint32_t rx_bw, uint32_t tx_bw);
static int32_t test_update_rf_bandwidth_with_cleanup(
    struct ad9361_rf_phy *phy, uint32_t rx_bw, uint32_t tx_bw,
    int32_t *cleanup_status);
static int32_t test_clk_mux_set_parent(struct refclk_scale *clk_priv,
                                       uint8_t index);
static int32_t test_clk_mux_set_parent_with_cleanup(
    struct refclk_scale *clk_priv, uint8_t index, int32_t *cleanup_status);
static int32_t test_axi_adc_remove(struct axi_adc *adc);
static int32_t test_axi_dac_remove(struct axi_dac *dac);

static int32_t test_clk_mux_set_parent(struct refclk_scale *clk_priv,
                                       uint8_t index)
{
    if (fake_mux_commit_parent)
        clk_priv->mult = index;
    return fake_mux_parent_status;
}

static int32_t test_clk_mux_set_parent_with_cleanup(
    struct refclk_scale *clk_priv, uint8_t index, int32_t *cleanup_status)
{
    if (cleanup_status)
        *cleanup_status = fake_mux_cleanup_status;
    return test_clk_mux_set_parent(clk_priv, index);
}

#define ad9361_calculate_rf_clock_chain test_calculate_rf_clock_chain
#define ad9361_set_trx_clock_chain test_set_trx_clock_chain
#define ad9361_set_trx_clock_chain_with_cleanup test_set_trx_clock_chain_with_cleanup
#define ad9361_clock_chain_transaction_start test_clock_chain_transaction_start
#define ad9361_clock_chain_transaction_poll test_clock_chain_transaction_poll
#define ad9361_get_trx_clock_chain test_get_trx_clock_chain
#define ad9361_rf_bandwidth_transaction_start test_rf_bandwidth_transaction_start
#define ad9361_rf_bandwidth_transaction_poll test_rf_bandwidth_transaction_poll
#define ad9361_update_rf_bandwidth test_update_rf_bandwidth
#define ad9361_update_rf_bandwidth_with_cleanup test_update_rf_bandwidth_with_cleanup
#define ad9361_clk_mux_set_parent test_clk_mux_set_parent
#define ad9361_clk_mux_set_parent_with_cleanup test_clk_mux_set_parent_with_cleanup
#define axi_adc_remove test_axi_adc_remove
#define axi_dac_remove test_axi_dac_remove
#include "../../thirdparty/analogdevicesinc/no-OS/drivers/rf-transceiver/ad9361/ad9361_api.c"
#undef ad9361_calculate_rf_clock_chain
#undef ad9361_set_trx_clock_chain
#undef ad9361_set_trx_clock_chain_with_cleanup
#undef ad9361_clock_chain_transaction_start
#undef ad9361_clock_chain_transaction_poll
#undef ad9361_get_trx_clock_chain
#undef ad9361_rf_bandwidth_transaction_start
#undef ad9361_rf_bandwidth_transaction_poll
#undef ad9361_update_rf_bandwidth
#undef ad9361_update_rf_bandwidth_with_cleanup
#undef ad9361_clk_mux_set_parent
#undef ad9361_clk_mux_set_parent_with_cleanup
#undef axi_adc_remove
#undef axi_dac_remove

static int32_t test_axi_adc_remove(struct axi_adc *adc)
{
    (void)adc;
    ++fake_axi_adc_remove_count;
    return fake_axi_adc_remove_status;
}

static int32_t test_axi_dac_remove(struct axi_dac *dac)
{
    (void)dac;
    ++fake_axi_dac_remove_count;
    return fake_axi_dac_remove_status;
}

static int32_t test_calculate_rf_clock_chain(struct ad9361_rf_phy *phy,
                                            uint32_t tx_sample_rate,
                                            uint32_t rate_governor,
                                            uint32_t *rx_path_clks,
                                            uint32_t *tx_path_clks)
{
    if (fake_sample_rate_transaction_enabled) {
        if (fake_sample_rate_calculate_status < 0)
            return fake_sample_rate_calculate_status;
        memset(rx_path_clks, 0, NUM_RX_CLOCKS * sizeof(*rx_path_clks));
        memset(tx_path_clks, 0, NUM_TX_CLOCKS * sizeof(*tx_path_clks));
        rx_path_clks[RX_SAMPL_FREQ] = tx_sample_rate;
        tx_path_clks[TX_SAMPL_FREQ] = tx_sample_rate;
        return 0;
    }
    (void)phy;
    (void)tx_sample_rate;
    (void)rate_governor;
    rx_path_clks[0] = 1;
    tx_path_clks[0] = 1;
    return 0;
}

static int32_t test_set_trx_clock_chain(struct ad9361_rf_phy *phy,
                                        const uint32_t *rx_path_clks,
                                        const uint32_t *tx_path_clks)
{
    if (fake_sample_rate_transaction_enabled) {
        ++fake_sample_rate_clock_set_attempt_count;
        if (fake_sample_rate_clock_set_fail_attempt ==
            fake_sample_rate_clock_set_attempt_count)
            return -EIO;
        if (fake_clock_chain_status < 0)
            return fake_clock_chain_status;
        memcpy(fake_sample_rate_rx_clks, rx_path_clks,
               sizeof(fake_sample_rate_rx_clks));
        memcpy(fake_sample_rate_tx_clks, tx_path_clks,
               sizeof(fake_sample_rate_tx_clks));
        ++fake_sample_rate_clock_set_count;
        phy->current_bw_valid = false;
        if (fake_sample_rate_corrupt_rollback_readback &&
            fake_sample_rate_clock_set_count == 2)
            fake_sample_rate_rx_clks[RX_SAMPL_FREQ]++;
        return 0;
    }
    (void)phy;
    (void)rx_path_clks;
    (void)tx_path_clks;
    return fake_clock_chain_status;
}

static int32_t test_clock_chain_transaction_start(
    struct ad9361_rf_phy *phy, const uint32_t *rx_path_clks,
    const uint32_t *tx_path_clks,
    struct ad9361_clock_chain_transaction *transaction)
{
    if (!phy || !transaction || !rx_path_clks || !tx_path_clks)
        return -EINVAL;
    if (transaction->active || ad9361_operation_busy(phy))
        return -EBUSY;
    memset(transaction, 0, sizeof(*transaction));
    transaction->phy = phy;
    memcpy(transaction->rx_path_clks, rx_path_clks,
           sizeof(transaction->rx_path_clks));
    memcpy(transaction->tx_path_clks, tx_path_clks,
           sizeof(transaction->tx_path_clks));
    transaction->active = true;
    transaction->stage = AD9361_CLOCK_CHAIN_TRANSACTION_BBPLL;
    phy->clock_chain_transaction = transaction;
    return 0;
}

static int32_t test_clock_chain_transaction_poll(
    struct ad9361_rf_phy *phy,
    struct ad9361_clock_chain_transaction *transaction)
{
    int32_t ret;

    if (!phy || !transaction)
        return -EINVAL;
    if (transaction->terminal)
        return transaction->terminal_status;
    if (!transaction->active || transaction->phy != phy ||
        phy->clock_chain_transaction != transaction) {
        if (transaction->phy)
            ad9361_mark_calibration_state_unknown(transaction->phy);
        return -EIO;
    }

    ret = test_set_trx_clock_chain(phy, transaction->rx_path_clks,
                                   transaction->tx_path_clks);
    transaction->active = false;
    transaction->terminal = true;
    transaction->terminal_status = ret;
    phy->clock_chain_transaction = NULL;
    return ret;
}

static int32_t test_get_trx_clock_chain(struct ad9361_rf_phy *phy,
                                        uint32_t *rx_path_clks,
                                        uint32_t *tx_path_clks)
{
    if (fake_sample_rate_transaction_enabled) {
        if (fake_sample_rate_clock_readback_status < 0)
            return fake_sample_rate_clock_readback_status;
        if (rx_path_clks != NULL)
            memcpy(rx_path_clks, fake_sample_rate_rx_clks,
                   sizeof(fake_sample_rate_rx_clks));
        if (tx_path_clks != NULL)
            memcpy(tx_path_clks, fake_sample_rate_tx_clks,
                   sizeof(fake_sample_rate_tx_clks));
        return 0;
    }
    return ad9361_get_trx_clock_chain(phy, rx_path_clks, tx_path_clks);
}

static int32_t test_rf_bandwidth_transaction_start(
    struct ad9361_rf_phy *phy, uint32_t rx_bw, uint32_t tx_bw,
    struct ad9361_rf_bandwidth_transaction *transaction)
{
    if (!fake_sample_rate_transaction_enabled)
        return ad9361_rf_bandwidth_transaction_start(
            phy, rx_bw, tx_bw, transaction);
    memset(transaction, 0, sizeof(*transaction));
    transaction->rx_bw = rx_bw;
    transaction->tx_bw = tx_bw;
    transaction->phy = phy;
    transaction->active = true;
    phy->rf_bandwidth_transaction = transaction;
    ++fake_sample_rate_bw_start_count;
    return 0;
}

static int32_t test_rf_bandwidth_transaction_poll(
    struct ad9361_rf_phy *phy,
    struct ad9361_rf_bandwidth_transaction *transaction)
{
    if (!fake_sample_rate_transaction_enabled)
        return ad9361_rf_bandwidth_transaction_poll(phy, transaction);
    if (transaction->phy != phy ||
        phy->rf_bandwidth_transaction != transaction)
        return -EIO;
    ++fake_sample_rate_bw_poll_count;
    transaction->active = false;
    transaction->terminal = true;
    phy->rf_bandwidth_transaction = NULL;
    if (fake_sample_rate_bw_fail_poll &
        (1u << (fake_sample_rate_bw_poll_count - 1u))) {
        phy->current_bw_valid = false;
        transaction->terminal_status = -EIO;
        return -EIO;
    }
    phy->current_rx_bw_Hz = transaction->rx_bw;
    phy->current_tx_bw_Hz = transaction->tx_bw;
    phy->current_bw_valid = true;
    transaction->terminal_status = 0;
    return 0;
}

static int32_t test_sample_rate_poll_until_stage(
    struct ad9361_rf_phy *phy,
    struct ad9361_sample_rate_transaction *transaction, uint8_t stage)
{
    unsigned int polls;
    int32_t ret;

    for (polls = 0; polls < 64; polls++) {
        if (transaction->stage == stage)
            return 1;
        ret = ad9361_sample_rate_transaction_poll(phy, transaction);
        if (ret != 1)
            return ret;
    }
    return -ETIMEDOUT;
}

static int32_t test_sample_rate_poll_to_terminal(
    struct ad9361_rf_phy *phy,
    struct ad9361_sample_rate_transaction *transaction)
{
    unsigned int polls;
    int32_t ret;

    for (polls = 0; polls < 64; polls++) {
        ret = ad9361_sample_rate_transaction_poll(phy, transaction);
        if (ret != 1)
            return ret;
    }
    return -ETIMEDOUT;
}

static int32_t test_update_rf_bandwidth(struct ad9361_rf_phy *phy,
                                        uint32_t rx_bw, uint32_t tx_bw)
{
    (void)phy;
    (void)rx_bw;
    (void)tx_bw;
    ++fake_bandwidth_update_count;
    if (fake_bandwidth_update_fail_call == fake_bandwidth_update_count)
        return fake_bandwidth_update_fail_status;
    return 0;
}

static int32_t test_set_trx_clock_chain_with_cleanup(
    struct ad9361_rf_phy *phy, const uint32_t *rx_path_clks,
    const uint32_t *tx_path_clks, int32_t *cleanup_status)
{
    int32_t ret;

    if (!fake_sample_rate_transaction_enabled)
        return ad9361_set_trx_clock_chain_with_cleanup(
            phy, rx_path_clks, tx_path_clks, cleanup_status);
    ret = test_set_trx_clock_chain(phy, rx_path_clks, tx_path_clks);
    if (cleanup_status)
        *cleanup_status = fake_clock_chain_cleanup_status;
    if (ret >= 0 && fake_clock_chain_cleanup_status < 0)
        ret = fake_clock_chain_cleanup_status;
    return ret;
}

static int32_t test_update_rf_bandwidth_with_cleanup(
    struct ad9361_rf_phy *phy, uint32_t rx_bw, uint32_t tx_bw,
    int32_t *cleanup_status)
{
    int32_t ret;

    if (!fake_sample_rate_transaction_enabled)
        return ad9361_update_rf_bandwidth_with_cleanup(
            phy, rx_bw, tx_bw, cleanup_status);
    ret = test_update_rf_bandwidth(phy, rx_bw, tx_bw);
    if (cleanup_status) {
        *cleanup_status =
            fake_bandwidth_cleanup_fail_call == fake_bandwidth_update_count
                ? fake_bandwidth_cleanup_fail_status : 0;
    }
    if (ret >= 0 && cleanup_status && *cleanup_status < 0)
        ret = *cleanup_status;
    return ret;
}

static unsigned read_count;
static unsigned write_count;
static unsigned delay_count;
static unsigned gpio_set_count;
static uint8_t gpio_set_values[8];
static unsigned gpio_direction_count;
static uint8_t gpio_direction_ops[32];
static int32_t gpio_direction_status;
static uint64_t fake_time_us;
static int32_t spi_status;
static int32_t fake_spi_remove_status;
static int32_t fake_gpio_remove_status[4];
static unsigned fake_spi_remove_count;
static unsigned fake_gpio_remove_count;
static unsigned no_os_free_count;
static bool register_map_enabled;
static bool fake_calibration_autocomplete;
static int32_t fake_calibration_done_status;
static unsigned fake_calibration_done_fail_call;
static unsigned fake_calibration_done_call_count;
static int32_t fail_read_reg = -1;
static int32_t fail_read_once_reg = -1;
static int32_t fail_write_reg = -1;
static uint8_t last_fir_high_byte;
static uint16_t last_spi_write_addr;
static uint16_t spi_write_addr_log[512];
static uint8_t spi_write_value_log[512];
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

static int32_t fake_calibration_done(struct ad9361_rf_phy *phy, uint32_t mask,
		bool *done)
{
	(void)phy;
	(void)mask;
	if (!done)
		return -EINVAL;
	++fake_calibration_done_call_count;
	if (fake_calibration_done_status < 0 &&
	    fake_calibration_done_call_count ==
		fake_calibration_done_fail_call)
		return fake_calibration_done_status;
	*done = true;
	return 0;
}

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
void no_os_free(void *ptr) { if (ptr) ++no_os_free_count; free(ptr); }
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
    if (gpio_direction_count < sizeof(gpio_direction_ops))
        gpio_direction_ops[gpio_direction_count] = value ? 1 : 0;
    ++gpio_direction_count;
    return gpio_direction_status;
}
int32_t no_os_gpio_direction_input(struct no_os_gpio_desc *desc)
{
    (void)desc;
    if (gpio_direction_count < sizeof(gpio_direction_ops))
        gpio_direction_ops[gpio_direction_count] = 2;
    ++gpio_direction_count;
    return gpio_direction_status;
}
int32_t no_os_gpio_set_value(struct no_os_gpio_desc *desc, uint8_t value)
{
    (void)desc;
    if (gpio_set_count < sizeof(gpio_set_values) / sizeof(gpio_set_values[0]))
        gpio_set_values[gpio_set_count] = value;
    ++gpio_set_count;
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
struct no_os_time no_os_get_time(void)
{
    struct no_os_time now;
    now.s = (unsigned int)(fake_time_us / 1000000ULL);
    now.us = (unsigned int)(fake_time_us % 1000000ULL);
    return now;
}
void no_os_udelay(uint32_t usecs)
{
    fake_time_us += usecs;
    ++delay_count;
}
void no_os_mdelay(uint32_t msecs)
{
    fake_time_us += (uint64_t)msecs * 1000ULL;
    ++delay_count;
}

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
        if (write_count < sizeof(spi_write_addr_log) /
                          sizeof(spi_write_addr_log[0])) {
            spi_write_addr_log[write_count] = reg;
            spi_write_value_log[write_count] = data[2];
        }
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
                if (apply_failed_write_once && register_map_enabled) {
                    fake_regs[reg] = data[2];
                    if (fake_calibration_autocomplete &&
                        reg == REG_CALIBRATION_CTRL)
                        fake_regs[reg] = 0;
                }
                apply_failed_write_once = false;
                return -EIO;
            }
        }
        if ((int32_t)reg == fail_write_reg)
            return -EIO;
        if (register_map_enabled) {
            fake_regs[reg] = data[2];
            if (fake_calibration_autocomplete &&
                reg == REG_CALIBRATION_CTRL)
                fake_regs[reg] = 0;
            if (reg == REG_ENSM_CONFIG_1 && state_after_ensm_write >= 0)
                fake_regs[REG_STATE] = (uint8_t)state_after_ensm_write;
        }
        return 0;
    }

    ++read_count;
    if (spi_status < 0) return spi_status;
    for (i = 0; i < bytes_number - 2; i++) {
        uint16_t read_reg = reg - i;
        if ((int32_t)read_reg == fail_read_once_reg) {
            fail_read_once_reg = -1;
            return -EIO;
        }
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

    /* Nested external-band pre/action/post traversal is inert at start,
     * preserves depth-first order, and yields through the configured delay. */
    {
        struct ad9361_phy_platform_data band_pdata = {0};
        struct ad9361_rf_phy band_phy = { .spi = &spi, .pdata = &band_pdata };
        struct ad9361_ext_band_ctl band_ctl = { .ngpios = 1 };
        struct no_os_gpio_desc band_gpio = {0};
        uint32_t pre_gpio_op = AD9361_EXT_BAND_CTL_OP_LOW;
        uint32_t root_gpio_op = AD9361_EXT_BAND_CTL_OP_HIGH;
        uint32_t post_gpio_op = AD9361_EXT_BAND_CTL_OP_INPUT;
        struct ad9361_band_setting pre = {
            .gpio_values = &pre_gpio_op,
            .ngpios = 1,
            .rf_rx_input_sel = AD9361_EXT_BAND_NO_PORT_SEL,
            .rf_tx_output_sel = AD9361_EXT_BAND_NO_PORT_SEL,
            .delay_us = 40,
        };
        struct ad9361_band_setting post = {
            .gpio_values = &post_gpio_op,
            .ngpios = 1,
            .rf_rx_input_sel = AD9361_EXT_BAND_NO_PORT_SEL,
            .rf_tx_output_sel = AD9361_EXT_BAND_NO_PORT_SEL,
        };
        struct ad9361_band_setting root = {
            .freq_min = 1800000000ULL,
            .freq_max = 1900000000ULL,
            .gpio_values = &root_gpio_op,
            .ngpios = 1,
            .rf_rx_input_sel = AD9361_EXT_BAND_NO_PORT_SEL,
            .rf_tx_output_sel = AD9361_EXT_BAND_NO_PORT_SEL,
            .pre_seq = &pre,
            .pre_seq_len = 1,
            .post_seq = &post,
            .post_seq_len = 1,
        };
        struct ad9361_band_setting *settings[] = { &root };
        struct ad9361_ext_band_transaction band_job = {0};
        struct ad9361_ext_band_ctl competing_ctl = {0};
        unsigned gpio_before = gpio_direction_count;
        unsigned polls = 0;

        band_ctl.gpios[0] = &band_gpio;
        band_ctl.rx_settings = settings;
        band_ctl.rx_count = 1;
        if (ad9361_register_ext_band_control(&band_phy, &band_ctl) != 0 ||
            ad9361_register_ext_band_control(&band_phy, &competing_ctl) !=
                -EBUSY ||
            band_phy.ext_band_ctl != &band_ctl ||
            ad9361_ext_band_transaction_start(
                &band_phy, false, 1835000000ULL, &band_job) != 0 ||
            !band_job.active || band_phy.ext_band_transaction != &band_job ||
            gpio_direction_count != gpio_before) {
            fprintf(stderr, "band transaction start mutated hardware or lost owner\n");
            return EXIT_FAILURE;
        }
        while (polls++ < 128) {
            unsigned gpio_at_start = gpio_direction_count;
            uint32_t delay_us =
                ad9361_ext_band_transaction_poll_delay_us(&band_job);
            int32_t step;

            if (delay_us)
                fake_time_us += delay_us;
            step = ad9361_ext_band_transaction_poll(&band_phy, &band_job);
            if (step != 1 && step != 0) {
                fprintf(stderr, "band transaction failed: %d\n", step);
                return EXIT_FAILURE;
            }
            if (gpio_direction_count - gpio_at_start > 1) {
                fprintf(stderr, "band poll performed multiple GPIO operations\n");
                return EXIT_FAILURE;
            }
            if (step == 0)
                break;
        }
        if (!band_job.terminal || band_job.terminal_status != 0 || polls >= 128 ||
            gpio_direction_count != gpio_before + 3 ||
            gpio_direction_ops[gpio_before] != 0 ||
            gpio_direction_ops[gpio_before + 1] != 1 ||
            gpio_direction_ops[gpio_before + 2] != 2 ||
            band_ctl.current_rx_setting != &root ||
            band_phy.ext_band_transaction != NULL) {
            fprintf(stderr, "nested band transaction order/cache was not committed\n");
            return EXIT_FAILURE;
        }

        /* A malformed graph is rejected by a bounded poll before mutation. */
        {
            struct ad9361_band_setting malformed = {
                .freq_min = 1800000000ULL,
                .freq_max = 1900000000ULL,
                .pre_seq_len = 1,
                .pre_seq = NULL,
            };
            struct ad9361_band_setting *saved_target = settings[0];
            settings[0] = &malformed;
            memset(&band_job, 0, sizeof(band_job));
            if (ad9361_ext_band_transaction_start(
                    &band_phy, false, 1835000000ULL, &band_job) != 0 ||
                !band_job.active || !band_job.validation_pending ||
                gpio_direction_count != gpio_before + 3 ||
                ad9361_ext_band_transaction_poll(&band_phy, &band_job) !=
                    -EINVAL ||
                !band_job.terminal || band_job.mutation_started ||
                band_ctl.current_rx_setting != &root) {
                fprintf(stderr,
                        "band graph validation was not retained before mutation\n");
                return EXIT_FAILURE;
            }
            settings[0] = saved_target;
        }

        /* A cycle is rejected before mutation and leaves the valid cache intact. */
        {
            struct ad9361_band_setting *saved_settings = settings[0];
            int32_t step;
            unsigned cycle_polls = 0;
            settings[0] = &pre;
            pre.freq_min = 1800000000ULL;
            pre.freq_max = 1900000000ULL;
            pre.pre_seq = &pre;
            pre.pre_seq_len = 1;
            memset(&band_job, 0, sizeof(band_job));
            step = ad9361_ext_band_transaction_start(
                &band_phy, false, 1835000000ULL, &band_job);
            while (step == 0 || step == 1) {
                if (cycle_polls++ >= 128) {
                    step = -E2BIG;
                    break;
                }
                step = ad9361_ext_band_transaction_poll(&band_phy,
                                                        &band_job);
                if (step != 1)
                    break;
            }
            if (step != -ELOOP || !band_job.terminal ||
                gpio_direction_count != gpio_before + 3 ||
                band_ctl.current_rx_setting != &root) {
                fprintf(stderr, "band cycle was not rejected before mutation\n");
                return EXIT_FAILURE;
            }
            pre.pre_seq = NULL;
            pre.pre_seq_len = 0;
            settings[0] = saved_settings;
        }

        /* A GPIO failure invalidates only the external-band cache; the
         * owning stream layer receives the terminal error and fences IQ. */
        {
            uint32_t failed_gpio_op = AD9361_EXT_BAND_CTL_OP_LOW;
            struct ad9361_band_setting failed_target = {
                .freq_min = 1800000000ULL,
                .freq_max = 1900000000ULL,
                .gpio_values = &failed_gpio_op,
                .ngpios = 1,
                .rf_rx_input_sel = AD9361_EXT_BAND_NO_PORT_SEL,
                .rf_tx_output_sel = AD9361_EXT_BAND_NO_PORT_SEL,
            };
            struct ad9361_band_setting *saved_target = settings[0];

            settings[0] = &failed_target;
            memset(&band_job, 0, sizeof(band_job));
            gpio_direction_status = -EIO;
            if (ad9361_ext_band_transaction_start(
                    &band_phy, false, 1835000000ULL, &band_job) != 0 ||
                ad9361_ext_band_transaction_poll(&band_phy, &band_job) !=
                    -EIO ||
                !band_job.terminal || band_job.terminal_status != -EIO ||
                band_ctl.current_rx_setting != NULL ||
                band_phy.ext_band_transaction != NULL ||
                ad9361_calibration_state_unknown(&band_phy)) {
                fprintf(stderr,
                        "external band failure corrupted AD9361 state\n");
                return EXIT_FAILURE;
            }
            gpio_direction_status = 0;
            settings[0] = saved_target;
        }

        /* Unregister is an explicit cancel boundary: it must release both
         * ownership markers and leave the external-band cache invalid. */
        {
            band_ctl.current_rx_setting = NULL;
            memset(&band_job, 0, sizeof(band_job));
            if (ad9361_ext_band_transaction_start(
                    &band_phy, false, 1835000000ULL, &band_job) != 0 ||
                ad9361_ext_band_transaction_poll(&band_phy, &band_job) != 1 ||
                !band_job.mutation_started) {
                fprintf(stderr, "band cancellation setup did not mutate once\n");
                return EXIT_FAILURE;
            }
            ad9361_unregister_ext_band_control(&band_phy);
            if (!band_job.terminal || band_job.terminal_status != -ECANCELED ||
                ad9361_ext_band_transaction_poll(&band_phy, &band_job) !=
                    -ECANCELED ||
                band_phy.ext_band_transaction != NULL ||
                band_ctl.active_transaction != NULL ||
                ad9361_calibration_state_unknown(&band_phy)) {
                fprintf(stderr, "band cancellation did not retire safely\n");
                return EXIT_FAILURE;
            }
        }
        ad9361_unregister_ext_band_control(&band_phy);
    }

    /* Recursive setting teardown must release nested descriptor members,
     * sequence arrays, and the root object exactly once. */
    {
        struct ad9361_band_setting *root =
            ad9361_band_setting_alloc("root", 1, 2, 1);
        unsigned freed_before = no_os_free_count;

        if (!root)
            return EXIT_FAILURE;
        root->pre_seq = no_os_calloc(1, sizeof(*root->pre_seq));
        if (!root->pre_seq)
            return EXIT_FAILURE;
        root->pre_seq_len = 1;
        root->pre_seq[0].name = no_os_calloc(1, 6);
        root->pre_seq[0].gpio_values = no_os_calloc(1, sizeof(uint32_t));
        root->pre_seq[0].ngpios = 1;
        root->pre_seq[0].post_seq = no_os_calloc(
            1, sizeof(*root->pre_seq[0].post_seq));
        if (!root->pre_seq[0].name || !root->pre_seq[0].gpio_values ||
            !root->pre_seq[0].post_seq)
            return EXIT_FAILURE;
        strcpy((char *)root->pre_seq[0].name, "child");
        root->pre_seq[0].post_seq_len = 1;
        root->pre_seq[0].post_seq[0].name = no_os_calloc(1, 6);
        root->pre_seq[0].post_seq[0].gpio_values =
            no_os_calloc(1, sizeof(uint32_t));
        if (!root->pre_seq[0].post_seq[0].name ||
            !root->pre_seq[0].post_seq[0].gpio_values)
            return EXIT_FAILURE;
        strcpy((char *)root->pre_seq[0].post_seq[0].name, "leaf");

        ad9361_band_setting_free(root);
        if (no_os_free_count - freed_before != 9) {
            fprintf(stderr, "nested setting teardown missed owned allocations\n");
            return EXIT_FAILURE;
        }
    }

    /* The controller owns each setting pointer exactly once across RX/TX. */
    {
        struct ad9361_ext_band_ctl *ctl = ad9361_ext_band_ctl_alloc();
        struct ad9361_band_setting *setting =
            ad9361_band_setting_alloc("owned", 1, 2, 0);

        if (!ctl || !setting ||
            ad9361_ext_band_ctl_add_rx_setting(ctl, setting) != 0 ||
            ad9361_ext_band_ctl_add_tx_setting(ctl, setting) != -EINVAL) {
            fprintf(stderr, "controller accepted duplicate setting ownership\n");
            return EXIT_FAILURE;
        }
        ad9361_ext_band_ctl_free(ctl);
    }

    /* Freeing a controller mid-transition cancels before releasing the
     * target descriptor and clears the PHY back-pointer. */
    {
        struct ad9361_rf_phy free_phy = { .spi = &spi };
        struct ad9361_ext_band_ctl *ctl = ad9361_ext_band_ctl_alloc();
        struct ad9361_band_setting *setting =
            ad9361_band_setting_alloc("active", 1, 2, 1);
        struct no_os_gpio_desc gpio = {0};
        struct ad9361_ext_band_transaction job = {0};

        if (!ctl || !setting ||
            ad9361_band_setting_set_gpio(
                setting, 0, AD9361_EXT_BAND_CTL_OP_LOW) != 0 ||
            ad9361_ext_band_ctl_add_gpio(ctl, &gpio) != 0 ||
            ad9361_ext_band_ctl_add_rx_setting(ctl, setting) != 0 ||
            ad9361_register_ext_band_control(&free_phy, ctl) != 0 ||
            ad9361_ext_band_transaction_start(&free_phy, false, 1, &job) !=
                0 ||
            ad9361_ext_band_transaction_poll(&free_phy, &job) != 1 ||
            !job.mutation_started) {
            fprintf(stderr, "controller free cancellation setup failed\n");
            return EXIT_FAILURE;
        }
        ad9361_ext_band_ctl_free(ctl);
        if (free_phy.ext_band_ctl != NULL ||
            free_phy.ext_band_transaction != NULL || !job.terminal ||
            job.terminal_status != -ECANCELED ||
            ad9361_ext_band_transaction_poll(&free_phy, &job) != -ECANCELED ||
            ad9361_calibration_state_unknown(&free_phy)) {
            fprintf(stderr, "free left a dangling band transaction\n");
            return EXIT_FAILURE;
        }
    }

    /* The retained constructor must preserve the legacy 1 ms RESETB
     * intervals without sleeping inside poll(). */
    {
        struct ad9361_rf_phy reset_phy = {0};
        struct no_os_spi_desc reset_spi = {0};
        struct ad9361_init_transaction init_job = {0};
        struct ad9361_rf_phy *published_phy = NULL;
        AD9361_InitParam init_param = {0};
        unsigned gpio_before = gpio_set_count;
        unsigned reads_before = read_count;
        unsigned delays_before = delay_count;

        reset_phy.spi = &reset_spi;
        reset_phy.gpio_desc_resetb =
            (struct no_os_gpio_desc *)(uintptr_t)1;
        init_job.candidate = &reset_phy;
        init_job.phy_out = &published_phy;
        init_job.init_param = &init_param;
        init_job.active = true;
        init_job.stage = AD9361_INIT_TXN_RESET_ASSERT;

        if (ad9361_init_transaction_poll(&init_job) != 1 ||
            init_job.stage != AD9361_INIT_TXN_RESET_LOW_WAIT ||
            gpio_set_count != gpio_before + 1 ||
            gpio_set_values[gpio_before] != 0 ||
            delay_count != delays_before || read_count != reads_before ||
            ad9361_init_transaction_poll_delay_us(&init_job) != 1000) {
            fprintf(stderr, "retained RESETB assert blocked or lost its deadline\n");
            return EXIT_FAILURE;
        }

        fake_time_us += 999;
        if (ad9361_init_transaction_poll(&init_job) != 1 ||
            init_job.stage != AD9361_INIT_TXN_RESET_LOW_WAIT ||
            gpio_set_count != gpio_before + 1 || read_count != reads_before) {
            fprintf(stderr, "retained RESETB low wait ended before 1 ms\n");
            return EXIT_FAILURE;
        }

        fake_time_us += 1;
        if (ad9361_init_transaction_poll(&init_job) != 1 ||
            init_job.stage != AD9361_INIT_TXN_RESET_RELEASE ||
            gpio_set_count != gpio_before + 1) {
            fprintf(stderr, "retained RESETB low deadline was not observed\n");
            return EXIT_FAILURE;
        }
        if (ad9361_init_transaction_poll(&init_job) != 1 ||
            init_job.stage != AD9361_INIT_TXN_RESET_HIGH_WAIT ||
            gpio_set_count != gpio_before + 2 ||
            gpio_set_values[gpio_before + 1] != 1 ||
            ad9361_init_transaction_poll_delay_us(&init_job) != 1000) {
            fprintf(stderr, "retained RESETB release phase is incorrect\n");
            return EXIT_FAILURE;
        }
        fake_time_us += 1000;
        if (ad9361_init_transaction_poll(&init_job) != 1 ||
            init_job.stage != AD9361_INIT_TXN_PRODUCT_ID ||
            gpio_set_count != gpio_before + 2 || read_count != reads_before) {
            fprintf(stderr, "retained RESETB high deadline was not observed\n");
            return EXIT_FAILURE;
        }
    }

    /* Retained parent transactions must carry nested cleanup failures all
     * the way to the constructor result, without replacing the primary
     * operation error when both failed. */
    {
        struct ad9361_rf_phy phy = {0};
        struct ad9361_setup_calibration_transaction calibration = {
            .phy = &phy,
            .rfdc_job = { .cleanup_status = -EIO },
        };
        struct ad9361_setup_transaction setup = {
            .phy = &phy,
            .calibration_job = { .cleanup_status = -ETIMEDOUT },
        };
        struct ad9361_init_transaction init = {
            .cleanup_status = -EIO,
        };
        int32_t status;

        phy.setup_calibration_transaction = &calibration;
        status = ad9361_setup_calibration_transaction_finish(
            &calibration, -ETIMEDOUT);
        if (status != -ETIMEDOUT ||
            calibration.cleanup_status != -EIO ||
            phy.setup_calibration_transaction != NULL) {
            fprintf(stderr,
                    "Setup calibration lost nested cleanup or primary status\n");
            return EXIT_FAILURE;
        }

        phy.setup_transaction = &setup;
        status = ad9361_setup_transaction_finish(&setup, 0);
        if (status != -ETIMEDOUT ||
            setup.cleanup_status != -ETIMEDOUT ||
            phy.setup_transaction != NULL) {
            fprintf(stderr,
                    "Setup parent reported success after child cleanup failure\n");
            return EXIT_FAILURE;
        }

        status = ad9361_init_transaction_finish(&init, 0);
        if (status != -EIO || init.terminal_status != -EIO) {
            fprintf(stderr,
                    "Init transaction reported success after cleanup failure\n");
            return EXIT_FAILURE;
        }
        status = ad9361_init_transaction_finish(&init, -ETIMEDOUT);
        if (status != -ETIMEDOUT || init.terminal_status != -ETIMEDOUT ||
            init.cleanup_status != -EIO) {
            fprintf(stderr,
                    "Init cleanup replaced primary operation failure\n");
            return EXIT_FAILURE;
        }
    }

    /* The Gm subtable is an ordered indirect write port. Its retained setup
     * phase must preserve every address/data/commit operation while yielding
     * after at most one SPI write and the original 2 us inter-row interval. */
    {
        struct ad9361_rf_phy gm_phy = { .spi = &spi };
        struct ad9361_phy_platform_data gm_pdata = {0};
        struct ad9361_setup_transaction gm_job = {0};
        struct no_os_clk gm_rf_clock = { .rate = 30720000U };
        struct no_os_clk gm_sample_clock = { .rate = 1000000U };
        unsigned writes_before = write_count;
        unsigned delays_before = delay_count;
        unsigned expected_index = writes_before;
        unsigned polls = 0;
        unsigned gc_setup_writes_before;
        unsigned row;
        int32_t step;

        register_map_enabled = true;
        gm_job.phy = &gm_phy;
        gm_job.active = true;
        gm_job.stage = AD9361_SETUP_TRANSACTION_GM_START;
        gm_phy.pdata = &gm_pdata;
        gm_phy.clks[CLKRF_CLK] = &gm_rf_clock;
        gm_phy.clks[RX_SAMPL_CLK] = &gm_sample_clock;
        gm_pdata.gain_ctrl.gain_update_interval_us = 100;
        gm_pdata.gain_ctrl.dec_pow_measuremnt_duration = 16;
        gm_pdata.gain_ctrl.f_agc_dec_pow_measuremnt_duration = 16;
        gm_pdata.gain_ctrl.f_agc_power_measurement_duration_in_state5 = 16;
        gm_pdata.gain_ctrl.lmt_overload_high_thresh = 16;
        gm_pdata.gain_ctrl.lmt_overload_low_thresh = 16;
        gm_phy.setup_transaction = &gm_job;

        while (gm_job.stage != AD9361_SETUP_TRANSACTION_GC_SETUP &&
               polls++ < 128) {
            uint8_t prior_stage = gm_job.stage;
            unsigned writes_at_start = write_count;
            uint32_t delay_us =
                ad9361_setup_transaction_poll_delay_us(&gm_job);

            if (delay_us != 0)
                fake_time_us += delay_us;
            step = ad9361_setup_transaction_poll(&gm_phy, &gm_job);
            if (step != 1 || write_count - writes_at_start > 1) {
                fprintf(stderr, "Gm subtable poll exceeded its SPI budget\n");
                return EXIT_FAILURE;
            }
            if (prior_stage == AD9361_SETUP_TRANSACTION_GM_ROW_WAIT ||
                prior_stage == AD9361_SETUP_TRANSACTION_GM_CLEAR_WAIT) {
                if (delay_us != 2 || write_count != writes_at_start ||
                    delay_count != delays_before) {
                    fprintf(stderr, "Gm subtable wait performed blocking work\n");
                    return EXIT_FAILURE;
                }
            } else if (write_count != writes_at_start + 1) {
                fprintf(stderr, "Gm subtable phase did not perform one write\n");
                return EXIT_FAILURE;
            }
        }

        if (gm_job.stage != AD9361_SETUP_TRANSACTION_GC_SETUP ||
            write_count - writes_before != 83) {
            fprintf(stderr, "Gm subtable did not reach GC in bounded phases\n");
            return EXIT_FAILURE;
        }
        if (spi_write_addr_log[expected_index] != REG_GM_SUB_TABLE_CONFIG ||
            spi_write_value_log[expected_index++] != START_GM_SUB_TABLE_CLOCK) {
            fprintf(stderr, "Gm subtable clock start order changed\n");
            return EXIT_FAILURE;
        }
        for (row = 0; row < NO_OS_ARRAY_SIZE(gm_st_ctrl); ++row) {
            uint8_t address = (uint8_t)(NO_OS_ARRAY_SIZE(gm_st_ctrl) - 1U - row);

            if (spi_write_addr_log[expected_index] != REG_GM_SUB_TABLE_ADDRESS ||
                spi_write_value_log[expected_index++] != address ||
                spi_write_addr_log[expected_index] != REG_GM_SUB_TABLE_BIAS_WRITE ||
                spi_write_value_log[expected_index++] != 0 ||
                spi_write_addr_log[expected_index] != REG_GM_SUB_TABLE_GAIN_WRITE ||
                spi_write_value_log[expected_index++] != gm_st_gain[row] ||
                spi_write_addr_log[expected_index] != REG_GM_SUB_TABLE_CTRL_WRITE ||
                spi_write_value_log[expected_index++] != gm_st_ctrl[row] ||
                spi_write_addr_log[expected_index] != REG_GM_SUB_TABLE_CONFIG ||
                spi_write_value_log[expected_index++] !=
                    (WRITE_GM_SUB_TABLE | START_GM_SUB_TABLE_CLOCK)) {
                fprintf(stderr, "Gm subtable row %u register order changed\n", row);
                return EXIT_FAILURE;
            }
        }
        if (spi_write_addr_log[expected_index] != REG_GM_SUB_TABLE_CONFIG ||
            spi_write_value_log[expected_index++] != START_GM_SUB_TABLE_CLOCK ||
            spi_write_addr_log[expected_index] != REG_GM_SUB_TABLE_CONFIG ||
            spi_write_value_log[expected_index++] != 0 ||
            expected_index != write_count) {
            fprintf(stderr, "Gm subtable final clear/stop sequence changed\n");
            return EXIT_FAILURE;
        }

        gc_setup_writes_before = write_count;
        /* The gain-control register prefix is intentionally one ordered
         * batch. It must finish before the separately retained calibration
         * start/update phases; it must not yield one NIOS turn per register. */
        polls = 0;
        while (gm_job.stage == AD9361_SETUP_TRANSACTION_GC_SETUP &&
               polls++ < 2) {
            step = ad9361_setup_transaction_poll(&gm_phy, &gm_job);
            if (step != 1) {
                fprintf(stderr, "AGC register batch failed during setup\n");
                return EXIT_FAILURE;
            }
        }
        if (gm_job.stage != AD9361_SETUP_TRANSACTION_GC_UPDATE ||
            polls != 1 || write_count <= gc_setup_writes_before) {
            fprintf(stderr,
                    "AGC register batch did not complete in one retained step\n");
            return EXIT_FAILURE;
        }
        polls = 0;
        while (gm_job.stage != AD9361_SETUP_TRANSACTION_CALIBRATION_START &&
               polls++ < 16) {
            step = ad9361_setup_transaction_poll(&gm_phy, &gm_job);
            if (step != 1) {
                fprintf(stderr, "AGC update phase failed before calibration\n");
                return EXIT_FAILURE;
            }
        }
        if (gm_job.stage != AD9361_SETUP_TRANSACTION_CALIBRATION_START ||
            polls >= 16) {
            fprintf(stderr,
                    "AGC update did not reach calibration in bounded polls\n");
            return EXIT_FAILURE;
        }
    }

    /* RX/TX synthesizer charge-pump calibration must be advanced by the
     * setup owner, retaining order and yielding between the bounded SPI
     * operations and status observations. */
    {
        struct ad9361_phy_platform_data cp_pdata = {0};
        struct ad9361_rf_phy cp_phy = { .spi = &spi, .pdata = &cp_pdata };
        struct ad9361_setup_transaction cp_job = {0};
        unsigned writes_before = write_count;
        unsigned delays_before = delay_count;
        unsigned polls = 0;
        bool rx_cp_completed = false;
        bool tx_cp_completed = false;
        int32_t step;

        cp_job.phy = &cp_phy;
        cp_job.active = true;
        cp_job.synth_ref_freq_hz = 30720000U;
        cp_job.stage = AD9361_SETUP_TRANSACTION_RX_CP_START;
        cp_phy.setup_transaction = &cp_job;
        register_map_enabled = true;

        while (cp_job.stage != AD9361_SETUP_TRANSACTION_POST_CP_BEGIN &&
               polls++ < 128) {
            uint8_t prior_stage = cp_job.stage;
            unsigned reads_at_start = read_count;
            unsigned writes_at_start = write_count;

            if ((cp_job.stage == AD9361_SETUP_TRANSACTION_RX_CP_WAIT ||
                 cp_job.stage == AD9361_SETUP_TRANSACTION_TX_CP_WAIT) &&
                cp_job.synth_cp_job.wait_started) {
                uint32_t status_reg = REG_RX_CAL_STATUS +
                    (cp_job.synth_cp_job.tx ? 0x40U : 0U);

                fake_regs[status_reg] |= CP_CAL_VALID;
                if (ad9361_setup_transaction_poll_delay_us(&cp_job) != 120U) {
                    fprintf(stderr, "CP status wait did not expose its poll cadence\n");
                    return EXIT_FAILURE;
                }
            }
            step = ad9361_setup_transaction_poll(&cp_phy, &cp_job);
            if (step != 1 || read_count - reads_at_start > 1 ||
                write_count - writes_at_start > 1) {
                fprintf(stderr, "CP setup poll exceeded one register operation\n");
                return EXIT_FAILURE;
            }
            if (prior_stage == AD9361_SETUP_TRANSACTION_RX_CP_WAIT &&
                cp_job.stage == AD9361_SETUP_TRANSACTION_TX_CP_START)
                rx_cp_completed = true;
            if (prior_stage == AD9361_SETUP_TRANSACTION_TX_CP_WAIT &&
                cp_job.stage == AD9361_SETUP_TRANSACTION_POST_CP_BEGIN)
                tx_cp_completed = true;
        }
        if (cp_job.stage != AD9361_SETUP_TRANSACTION_POST_CP_BEGIN ||
            polls >= 128 || cp_phy.synth_cp_job != NULL ||
            cp_job.synth_cp_job.tx != true ||
            !rx_cp_completed || !tx_cp_completed ||
            write_count <= writes_before || delay_count != delays_before) {
            fprintf(stderr,
                    "RX/TX CP jobs did not complete in retained ADI order\n");
            return EXIT_FAILURE;
        }
    }

    /* A failed post-CP RFPLL child start must retire the parent owner and
     * restore the requested external-LO policy even though hardware is
     * quarantined. */
    {
        struct ad9361_phy_platform_data post_cp_pdata = {
            .use_ext_rx_lo = true,
            .use_ext_tx_lo = true,
        };
        struct ad9361_rf_phy post_cp_phy = {
            .spi = &spi,
            .pdata = &post_cp_pdata,
        };
        struct ad9361_setup_transaction post_cp_job = {0};
        int32_t step;

        post_cp_job.phy = &post_cp_phy;
        post_cp_job.active = true;
        post_cp_job.stage = AD9361_SETUP_TRANSACTION_POST_CP_BEGIN;
        post_cp_phy.setup_transaction = &post_cp_job;

        step = ad9361_setup_transaction_poll(&post_cp_phy, &post_cp_job);
        if (step != 1 || post_cp_pdata.use_ext_rx_lo ||
            post_cp_pdata.use_ext_tx_lo ||
            post_cp_job.stage !=
                AD9361_SETUP_TRANSACTION_POST_CP_RX_RFPLL_START) {
            fprintf(stderr, "post-CP did not enter internal-LO programming policy\n");
            return EXIT_FAILURE;
        }
        step = ad9361_setup_transaction_poll(&post_cp_phy, &post_cp_job);
        if (step != -EINVAL || !post_cp_job.terminal ||
            post_cp_job.terminal_status != -EINVAL ||
            post_cp_phy.setup_transaction != NULL ||
            !post_cp_pdata.use_ext_rx_lo || !post_cp_pdata.use_ext_tx_lo ||
            !ad9361_calibration_state_unknown(&post_cp_phy)) {
            fprintf(stderr,
                    "post-CP RFPLL start failure did not retire/restore safely\n");
            return EXIT_FAILURE;
        }
    }

    /* Returning RX to an external LO advances ENSM, mux-register writes, and
     * ENSM restore through separate setup polls. */
    {
        struct ad9361_phy_platform_data mux_pdata = {
            .use_ext_rx_lo = true,
        };
        struct ad9361_rf_phy mux_phy = { .spi = &spi, .pdata = &mux_pdata };
        struct no_os_clk rx_pll_clk = { .rate = 1835000000U };
        struct refclk_scale rx_root = {
            .spi = &spi, .phy = &mux_phy, .source = RX_RFPLL,
        };
        struct refclk_scale rx_internal = {
            .spi = &spi, .phy = &mux_phy, .source = RX_RFPLL_INT,
            .parent_source = RX_REFCLK,
        };
        struct ad9361_setup_transaction mux_job = {0};
        unsigned writes_before = write_count;
        unsigned polls = 0;
        int32_t step = 1;

        mux_phy.clks[RX_RFPLL] = &rx_pll_clk;
        mux_phy.ref_clk_scale[RX_RFPLL] = &rx_root;
        mux_phy.ref_clk_scale[RX_RFPLL_INT] = &rx_internal;
        mux_phy.current_rx_lo_freq = 1835000000U;
        mux_phy.prev_ensm_state = ENSM_STATE_INVALID;
        fake_regs[REG_STATE] = ENSM_STATE_ALERT;
        mux_job.phy = &mux_phy;
        mux_job.active = true;
        mux_job.use_ext_rx_lo = true;
        mux_job.stage = AD9361_SETUP_TRANSACTION_POST_CP_RX_MUX;
        mux_phy.setup_transaction = &mux_job;
        register_map_enabled = true;

        while (mux_job.stage == AD9361_SETUP_TRANSACTION_POST_CP_RX_MUX &&
               polls++ < 64) {
            unsigned reads_at_start = read_count;
            unsigned writes_at_start = write_count;
            uint32_t delay_us =
                ad9361_setup_transaction_poll_delay_us(&mux_job);

            if (delay_us != 0)
                no_os_udelay(delay_us);

            step = ad9361_setup_transaction_poll(&mux_phy, &mux_job);
            if (step != 1 || read_count - reads_at_start > 1 ||
                write_count - writes_at_start > 1) {
                fprintf(stderr, "RX LO mux poll exceeded one SPI helper\n");
                return EXIT_FAILURE;
            }
        }
        if (mux_job.stage != AD9361_SETUP_TRANSACTION_POST_CP_TX_MUX ||
            polls >= 64 || write_count - writes_before != 8 ||
            mux_job.prefix_cursor != 0 || rx_root.mult != 1 ||
            !mux_phy.rfpll_source_valid[0] ||
            mux_phy.rfpll_requested_valid[0] ||
            mux_phy.setup_transaction != &mux_job) {
            fprintf(stderr,
                    "RX external-LO mux did not complete as retained phases\n");
            return EXIT_FAILURE;
        }
        mux_phy.setup_transaction = NULL;
        mux_job.active = false;
    }

    /* A pre-clock setup write failure must not run gain-table cleanup: the
     * indirect GM engine has not been started at this point. */
    {
        struct ad9361_phy_platform_data prefix_pdata = {0};
        struct ad9361_rf_phy prefix_phy = {
            .spi = &spi,
            .pdata = &prefix_pdata,
        };
        struct ad9361_setup_transaction prefix_job = {0};
        unsigned gm_cleanup_writes =
            fake_write_counts[REG_GM_SUB_TABLE_CONFIG];
        int32_t step;

        prefix_job.phy = &prefix_phy;
        prefix_job.active = true;
        prefix_job.stage = AD9361_SETUP_TRANSACTION_PRE_CTRL;
        prefix_phy.setup_transaction = &prefix_job;
        register_map_enabled = true;
        fail_write_once_reg = REG_CTRL;
        fail_write_once_reg_occurrence = 1;

        step = ad9361_setup_transaction_poll(&prefix_phy, &prefix_job);
        if (step != -EIO || !prefix_job.terminal ||
            prefix_job.terminal_status != -EIO ||
            prefix_phy.setup_transaction != NULL ||
            !ad9361_calibration_state_unknown(&prefix_phy) ||
            fake_write_counts[REG_GM_SUB_TABLE_CONFIG] != gm_cleanup_writes ||
            fail_write_once_reg != -1) {
            fprintf(stderr,
                    "Pre-clock setup failure ran gain-table cleanup or lost status\n");
            return EXIT_FAILURE;
        }
    }

    /* A failed RX CP programming write must terminate the parent setup,
     * retain the concrete error, and quarantine the RFIC before TX begins. */
    {
        struct ad9361_phy_platform_data cp_pdata = {0};
        struct ad9361_rf_phy cp_phy = { .spi = &spi, .pdata = &cp_pdata };
        struct ad9361_setup_transaction cp_job = {0};
        int32_t step;

        cp_job.phy = &cp_phy;
        cp_job.active = true;
        cp_job.synth_ref_freq_hz = 30720000U;
        cp_job.stage = AD9361_SETUP_TRANSACTION_RX_CP_START;
        cp_phy.setup_transaction = &cp_job;
        register_map_enabled = true;
        fail_write_once_reg = REG_RX_CP_LEVEL_DETECT;
        fail_write_once_reg_occurrence = 1;

        step = ad9361_setup_transaction_poll(&cp_phy, &cp_job);
        if (step != 1 || cp_job.stage != AD9361_SETUP_TRANSACTION_RX_CP_WAIT) {
            fprintf(stderr, "RX CP child was not accepted by setup owner\n");
            return EXIT_FAILURE;
        }
        step = ad9361_setup_transaction_poll(&cp_phy, &cp_job);
        if (step != -EIO || !cp_job.terminal || cp_job.terminal_status != -EIO ||
            !ad9361_calibration_state_unknown(&cp_phy) ||
            cp_phy.synth_cp_job != NULL ||
            cp_job.stage == AD9361_SETUP_TRANSACTION_TX_CP_START ||
            fail_write_once_reg != -1) {
            fprintf(stderr,
                    "RX CP write failure did not fail closed before TX CP\n");
            return EXIT_FAILURE;
        }
    }

    /* A failed indirect row write must keep the original error, attempt the
     * same table-clock cleanup, and leave the PHY faulted if row state is
     * uncertain. */
    {
        struct ad9361_rf_phy gm_phy = { .spi = &spi };
        struct ad9361_setup_transaction gm_job = {0};
        unsigned writes_before = write_count;
        unsigned polls = 0;
        int32_t step = 1;

        gm_job.phy = &gm_phy;
        gm_job.active = true;
        gm_job.stage = AD9361_SETUP_TRANSACTION_GM_START;
        gm_phy.setup_transaction = &gm_job;
        fail_write_once_reg = REG_GM_SUB_TABLE_GAIN_WRITE;
        fail_write_once_reg_occurrence = 1;
        while (step == 1 && polls++ < 16)
            step = ad9361_setup_transaction_poll(&gm_phy, &gm_job);
        if (step != -EIO || !gm_job.terminal || gm_job.status != -EIO ||
            gm_job.cleanup_status != 0 ||
            !ad9361_calibration_state_unknown(&gm_phy) ||
            gm_phy.setup_transaction != NULL ||
            write_count - writes_before != 5 ||
            last_spi_write_addr != REG_GM_SUB_TABLE_CONFIG ||
            fake_regs[REG_GM_SUB_TABLE_CONFIG] != 0) {
            fprintf(stderr, "Gm subtable failure lost primary error or cleanup\n");
            return EXIT_FAILURE;
        }
    }
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

    /* ENSM force/restore is a retained FSM: start does no I/O, polls execute
     * one SPI transaction each, and only status readback ends the transition. */
    {
        struct ad9361_rf_phy ensm_phy = { .spi = &spi };
        struct ad9361_ensm_transition_job ensm_job = {0};
        unsigned reads_before = read_count;
        unsigned writes_before = write_count;
        uint64_t saved_time_us = fake_time_us;
        int32_t step;

        memset(fake_regs, 0, sizeof(fake_regs));
        register_map_enabled = true;
        fake_regs[REG_STATE] = ENSM_STATE_RX;
        fake_regs[REG_ENSM_CONFIG_1] = ENABLE_ENSM_PIN_CTRL;
        state_after_ensm_write = ENSM_STATE_ALERT;
        ret = ad9361_ensm_transition_start(&ensm_phy, ENSM_STATE_ALERT,
                                           true, &ensm_job);
        if (ret != 0 || read_count != reads_before ||
            write_count != writes_before ||
            ensm_phy.ensm_transition_job != &ensm_job) {
            fprintf(stderr, "ENSM start performed I/O or failed to reserve PHY\n");
            return EXIT_FAILURE;
        }
        if (ad9361_reg_write(&ensm_phy, REG_PRODUCT_ID, 0) != -EBUSY ||
            ad9361_ensm_transition_start(&ensm_phy, ENSM_STATE_RX, true,
                                         &ensm_job) != -EBUSY ||
            read_count != reads_before || write_count != writes_before) {
            fprintf(stderr, "Active ENSM job allowed competing RFIC access\n");
            return EXIT_FAILURE;
        }

        /* Reads, Force Alert, ALERT wait, target write, target wait. */
        step = ad9361_ensm_transition_poll(&ensm_phy, &ensm_job);
        if (step != 1 || read_count != reads_before + 1 ||
            write_count != writes_before) {
            fprintf(stderr, "ENSM state poll was not bounded\n");
            return EXIT_FAILURE;
        }
        step = ad9361_ensm_transition_poll(&ensm_phy, &ensm_job);
        if (step != 1 || read_count != reads_before + 2 ||
            write_count != writes_before) {
            fprintf(stderr, "ENSM config poll was not bounded\n");
            return EXIT_FAILURE;
        }
        step = ad9361_ensm_transition_poll(&ensm_phy, &ensm_job);
        if (step != 1 || write_count != writes_before + 1) {
            fprintf(stderr, "ENSM Force Alert write was not bounded\n");
            return EXIT_FAILURE;
        }
        step = ad9361_ensm_transition_poll(&ensm_phy, &ensm_job);
        if (step != 1 || read_count != reads_before + 2 ||
            write_count != writes_before + 1) {
            fprintf(stderr, "ENSM ALERT wait arm was not bounded\n");
            return EXIT_FAILURE;
        }
        fake_regs[REG_STATE] = ENSM_STATE_RX_FLUSH;
        step = ad9361_ensm_transition_poll(&ensm_phy, &ensm_job);
        if (step != 1 || read_count != reads_before + 3 ||
            write_count != writes_before + 1) {
            fprintf(stderr, "ENSM issued target command before RX FLUSH ended\n");
            return EXIT_FAILURE;
        }
        fake_regs[REG_STATE] = ENSM_STATE_ALERT;
        step = ad9361_ensm_transition_poll(&ensm_phy, &ensm_job);
        if (step != 1 || read_count != reads_before + 3 ||
            write_count != writes_before + 1) {
            fprintf(stderr, "ENSM ignored ALERT wait poll deadline\n");
            return EXIT_FAILURE;
        }
        fake_time_us += 120U;
        step = ad9361_ensm_transition_poll(&ensm_phy, &ensm_job);
        if (step != 1 || read_count != reads_before + 4 ||
            write_count != writes_before + 1) {
            fprintf(stderr, "ENSM target command preceded ALERT readback\n");
            return EXIT_FAILURE;
        }
        step = ad9361_ensm_transition_poll(&ensm_phy, &ensm_job);
        if (step != 1 || write_count != writes_before + 2 ||
            ensm_phy.ensm_pin_ctl_en != true) {
            fprintf(stderr, "ENSM target-state poll was not bounded\n");
            return EXIT_FAILURE;
        }
        step = ad9361_ensm_transition_poll(&ensm_phy, &ensm_job);
        if (step != 1 || read_count != reads_before + 4) {
            fprintf(stderr, "ENSM target wait arm unexpectedly performed I/O\n");
            return EXIT_FAILURE;
        }
        {
			struct ad9361_rf_phy other_phy = { .spi = &spi };
			unsigned reads = read_count;

			if (ad9361_status_wait_poll(&other_phy, &ensm_job.wait) != -EINVAL ||
			    ensm_phy.status_wait != &ensm_job.wait ||
			    read_count != reads) {
				fprintf(stderr,
					"Status wait accepted a foreign PHY poll\n");
				return EXIT_FAILURE;
			}
		}
        step = ad9361_ensm_transition_poll(&ensm_phy, &ensm_job);
        if (step != 0 || read_count != reads_before + 5 ||
            ensm_phy.ensm_transition_job != NULL || !ensm_job.terminal ||
            ensm_job.terminal_status != 0 ||
            ensm_phy.prev_ensm_state != ENSM_STATE_RX) {
            fprintf(stderr, "ENSM transition did not finish on status evidence\n");
            return EXIT_FAILURE;
        }
        step = ad9361_ensm_transition_poll(&ensm_phy, &ensm_job);
        if (step != 0 || read_count != reads_before + 5 ||
            write_count != writes_before + 2) {
            fprintf(stderr, "ENSM terminal poll repeated hardware I/O\n");
            return EXIT_FAILURE;
        }
		{
			struct ad9361_rf_phy other_phy = { .spi = &spi };
			if (ad9361_ensm_transition_poll(&other_phy, &ensm_job) != -EINVAL ||
			    ensm_job.terminal_status != 0) {
				fprintf(stderr,
					"ENSM terminal result accepted a foreign PHY poll\n");
				return EXIT_FAILURE;
			}
		}

        reads_before = read_count;
        writes_before = write_count;
        /* INVALID is the legacy restore no-op and is terminal at start. */
        memset(&ensm_job, 0, sizeof(ensm_job));
        ret = ad9361_ensm_transition_start(&ensm_phy, ENSM_STATE_INVALID,
                                           false, &ensm_job);
        if (ret != 0 || !ensm_job.terminal ||
            ensm_phy.ensm_transition_job != NULL ||
            read_count != reads_before || write_count != writes_before) {
            fprintf(stderr, "ENSM INVALID restore was not a clean no-op\n");
            return EXIT_FAILURE;
        }

		/* A failed REG_STATE read cannot leave the PHY looking usable: the
		 * actual ENSM position is then unknown even though no write was sent. */
		{
			struct ad9361_rf_phy failed_read_phy = { .spi = &spi };
			struct ad9361_ensm_transition_job failed_read_job = {0};

			fail_read_reg = REG_STATE;
			ret = ad9361_ensm_transition_start(&failed_read_phy,
				ENSM_STATE_ALERT, true, &failed_read_job);
			if (ret != 0 || ad9361_ensm_transition_poll(
					&failed_read_phy, &failed_read_job) != -EIO ||
			    !ad9361_calibration_state_unknown(&failed_read_phy) ||
			    failed_read_phy.ensm_transition_job != NULL ||
			    !failed_read_job.terminal ||
			    failed_read_job.terminal_status != -EIO) {
				fprintf(stderr,
					"ENSM read failure did not fail-close RFIC state\n");
				return EXIT_FAILURE;
			}
			fail_read_reg = -1;
		}

		/* The retained TXQ parent must return from start with its ENSM child
		 * pending, rather than synchronously forcing ALERT inside the caller. */
		{
			struct ad9361_rf_phy txq_phy = {
				.spi = &spi,
				.pdata = &pdata,
				.current_rx_bw_Hz = 18000000U,
				.current_tx_bw_Hz = 18000000U,
				.current_bw_valid = true,
				.rfpll_source_valid = { true, true },
			};
			struct ad9361_tx_quad_calibration_transaction txq = {0};

			memset(fake_regs, 0, sizeof(fake_regs));
			fake_regs[REG_STATE] = ENSM_STATE_RX;
			fake_regs[REG_ENSM_CONFIG_1] = 0;
			state_after_ensm_write = ENSM_STATE_ALERT;
			ret = ad9361_tx_quad_calibration_transaction_start(
				&txq_phy, -1, &txq);
			if (ret != 0 || !txq.active ||
			    txq.stage != AD9361_TX_QUAD_TRANSACTION_ENSM_ALERT ||
			    !txq.ensm_job.active ||
			    txq_phy.tx_quad_transaction != &txq ||
			    txq_phy.ensm_transition_job != &txq.ensm_job ||
			    fake_regs[REG_STATE] != ENSM_STATE_RX) {
				fprintf(stderr,
					"TXQ start did not retain its ENSM transition child\n");
				return EXIT_FAILURE;
			}
			ad9361_mark_calibration_state_unknown(&txq_phy);
			if (ad9361_tx_quad_calibration_transaction_poll(
					&txq_phy, &txq) != 1 ||
			    ad9361_tx_quad_calibration_transaction_poll(
					&txq_phy, &txq) != -EIO ||
			    txq_phy.tx_quad_transaction != NULL || !txq.terminal) {
				fprintf(stderr,
					"TXQ parent did not retire a failed ENSM child\n");
				return EXIT_FAILURE;
			}
		}

		/* A TXQ primary error must remain distinct from a cleanup SPI failure
		 * in the child job and reach the parent transaction's cleanup result. */
		{
			struct ad9361_rf_phy txq_phy = {
				.spi = &spi,
				.pdata = &pdata,
			};
			struct ad9361_tx_quad_calibration_transaction txq = {0};

			register_map_enabled = true;
			fail_write_once_reg = REG_PARALLEL_PORT_CONF_2;
			fail_write_once_reg_occurrence = 1;
			apply_failed_write_once = true;
			txq.phy = &txq_phy;
			txq.active = true;
			txq.stage = AD9361_TX_QUAD_TRANSACTION_CALIBRATE;
			txq.job.active = true;
			txq.job.status = -ETIMEDOUT;
			txq.job.stage = AD9361_TX_QUAD_JOB_CLEANUP;
			txq.job.phase_inversion_touched = true;
			txq_phy.tx_quad_transaction = &txq;
			if (ad9361_tx_quad_calibration_transaction_poll(
				    &txq_phy, &txq) != 1 ||
			    txq.cleanup_status != 0 || txq.status != -ETIMEDOUT ||
			    txq.job.cleanup_status != -EIO ||
			    !txq_phy.calibration_state_unknown) {
				fprintf(stderr,
					"TXQ child did not retain separate primary and cleanup failures\n");
				return EXIT_FAILURE;
			}
			if (ad9361_tx_quad_calibration_transaction_poll(
				    &txq_phy, &txq) != -ETIMEDOUT ||
			    !txq.terminal || txq.terminal_status != -ETIMEDOUT ||
			    txq.cleanup_status != -EIO) {
				fprintf(stderr,
					"TXQ parent lost its first cleanup error or masked primary failure\n");
				return EXIT_FAILURE;
			}
			fail_write_once_reg = -1;
			fail_write_once_reg_occurrence = 1;
			apply_failed_write_once = false;
			register_map_enabled = false;
		}

		/* Enclosing bandwidth/sample-rate finish paths must aggregate the
		 * earliest child cleanup while preserving their primary status. */
		{
			struct ad9361_rf_bandwidth_transaction bandwidth = {0};
			struct ad9361_sample_rate_transaction sample_rate = {0};

			bandwidth.tx_quad_job.cleanup_status = -EIO;
			bandwidth.tracking_restore_status = -ETIMEDOUT;
			if (ad9361_rf_bandwidth_transaction_finish(
				    &bandwidth, -EINVAL) != -EINVAL ||
			    bandwidth.cleanup_status != -EIO ||
			    bandwidth.terminal_status != -EINVAL) {
				fprintf(stderr,
					"Bandwidth finish lost primary or first cleanup status\n");
				return EXIT_FAILURE;
			}
			sample_rate.bandwidth_job = bandwidth;
			if (ad9361_sample_rate_transaction_finish(
				    &sample_rate, 0) != -EIO ||
			    sample_rate.cleanup_status != -EIO ||
			    sample_rate.terminal_status != -EIO) {
				fprintf(stderr,
					"Sample-rate finish reported success after nested cleanup failure\n");
				return EXIT_FAILURE;
			}
		}

		/* RFDC is also a retained parent: start only establishes tracking
		 * state and arms its ENSM child; it must not wait through ALERT inline. */
		{
			struct refclk_scale rx_rfpll = { .rate = 2400000000ULL };
			struct ad9361_rf_phy rfdc_phy = {
				.spi = &spi,
				.pdata = &pdata,
				.clks = { [RX_RFPLL] = &rx_rfpll },
				.rfpll_source_valid = { true, false },
			};
			struct ad9361_rfdc_calibration_job rfdc = {0};
			uint8_t state_before = fake_regs[REG_STATE];

			ret = ad9361_rfdc_calibration_start_timeout(
				&rfdc_phy, 1000U, &rfdc);
			if (ret != 0 || !rfdc.active ||
			    rfdc.stage != AD9361_RFDC_JOB_ENSM_ALERT ||
			    !rfdc.ensm_job.active ||
			    rfdc_phy.rfdc_transaction != &rfdc ||
			    rfdc_phy.ensm_transition_job != &rfdc.ensm_job ||
			    !ad9361_operation_busy(&rfdc_phy) ||
			    ad9361_reg_write(&rfdc_phy, REG_PRODUCT_ID, 0) != -EBUSY ||
			    fake_regs[REG_STATE] != state_before) {
				fprintf(stderr,
					"RFDC start did not retain its ENSM child\n");
				return EXIT_FAILURE;
			}
			ad9361_mark_calibration_state_unknown(&rfdc_phy);
			if (ad9361_rfdc_calibration_poll(&rfdc_phy, &rfdc) != -EIO ||
			    rfdc_phy.rfdc_transaction != NULL || !rfdc.terminal ||
			    rfdc.terminal_status != -EIO) {
				fprintf(stderr,
					"RFDC parent did not retire failed ENSM child\n");
				return EXIT_FAILURE;
			}
		}
		/* Exercise the complete retained RFDC path through calibration and
		 * both restoration phases; the parent owner must survive every yield. */
		{
			struct refclk_scale rx_rfpll = { .rate = 2400000000ULL };
			struct refclk_scale clkrf = { .rate = 61440000ULL };
			struct ad9361_rf_phy rfdc_phy = {
				.spi = &spi,
				.pdata = &pdata,
				.clks = { [RX_RFPLL] = &rx_rfpll, [CLKRF_CLK] = &clkrf },
				.rfpll_source_valid = { true, false },
			};
			struct ad9361_rfdc_calibration_job rfdc = {0};
			bool saved_autocomplete = fake_calibration_autocomplete;
			unsigned poll;

			memset(fake_regs, 0, sizeof(fake_regs));
			register_map_enabled = true;
			fake_calibration_autocomplete = true;
			fake_regs[REG_STATE] = ENSM_STATE_RX;
			state_after_ensm_write = ENSM_STATE_ALERT;
			ret = ad9361_rfdc_calibration_start_timeout(
				&rfdc_phy, 50000U, &rfdc);
			if (ret != 0 || !ad9361_operation_busy(&rfdc_phy)) {
				fprintf(stderr, "RFDC full-path transaction failed to start\n");
				return EXIT_FAILURE;
			}
			for (poll = 0; poll < 256; ++poll) {
				ret = ad9361_rfdc_calibration_poll(&rfdc_phy, &rfdc);
				if (ret != 1)
					break;
				if (rfdc.stage == AD9361_RFDC_JOB_ENSM_RESTORE &&
				    rfdc.ensm_job.stage ==
					AD9361_ENSM_TRANSITION_WAIT_TARGET &&
				    rfdc.ensm_job.target_state == ENSM_STATE_RX)
					fake_regs[REG_STATE] = ENSM_STATE_RX;
				fake_time_us += 120U;
			}
			if (ret != 0 || !rfdc.terminal ||
			    rfdc.terminal_status != 0 ||
			    rfdc_phy.rfdc_transaction != NULL ||
			    rfdc_phy.ensm_transition_job != NULL ||
			    rfdc_phy.calibration_wait != NULL ||
			    rfdc_phy.status_wait != NULL ||
			    ad9361_operation_busy(&rfdc_phy) ||
			    ad9361_calibration_state_unknown(&rfdc_phy) ||
			    !rfdc_phy.tracking_config_valid ||
			    fake_regs[REG_STATE] != ENSM_STATE_RX) {
				fprintf(stderr,
					"RFDC did not complete calibration and restore: ret=%" PRId32
					" stage=%u active=%d terminal=%d status=%" PRId32
					" unknown=%d tracking=%d ENSM=%u\n",
					ret, rfdc.stage, rfdc.active, rfdc.terminal,
					rfdc.terminal_status,
					ad9361_calibration_state_unknown(&rfdc_phy),
					rfdc_phy.tracking_config_valid,
					fake_regs[REG_STATE]);
				return EXIT_FAILURE;
			}
			fake_calibration_autocomplete = saved_autocomplete;
			state_after_ensm_write = -1;
			register_map_enabled = false;
		}
		/* A known calibration failure still restores tracking/ENSM and keeps
		 * its primary errno. If ENSM cleanup also fails, retain that second
		 * failure separately while fail-closing the PHY. */
		{
			unsigned failure_case;
			for (failure_case = 0; failure_case < 2; ++failure_case) {
				struct refclk_scale rx_rfpll = {
					.rate = 2400000000ULL
				};
			struct refclk_scale clkrf = { .rate = 61440000ULL };
			struct ad9361_rf_phy rfdc_phy = {
				.spi = &spi,
				.pdata = &pdata,
				.clks = { [RX_RFPLL] = &rx_rfpll,
					  [CLKRF_CLK] = &clkrf },
				.rfpll_source_valid = { true, false },
				.read_cal_done = fake_calibration_done,
			};
			struct ad9361_rfdc_calibration_job rfdc = {0};
			bool saved_autocomplete = fake_calibration_autocomplete;
			unsigned poll;

			memset(fake_regs, 0, sizeof(fake_regs));
			register_map_enabled = true;
			fake_calibration_autocomplete = true;
			fake_calibration_done_status = -EAGAIN;
			fake_calibration_done_fail_call = 2;
			fake_calibration_done_call_count = 0;
			fail_write_reg = -1;
			fake_regs[REG_STATE] = ENSM_STATE_RX;
			state_after_ensm_write = ENSM_STATE_ALERT;
			ret = ad9361_rfdc_calibration_start_timeout(
				&rfdc_phy, 50000U, &rfdc);
			if (ret != 0) {
				fprintf(stderr,
					"RFDC known-failure transaction failed to start\n");
				return EXIT_FAILURE;
			}
			for (poll = 0; poll < 256; ++poll) {
				if (failure_case == 1 &&
				    rfdc.stage == AD9361_RFDC_JOB_ENSM_RESTORE &&
				    rfdc.ensm_job.stage ==
					AD9361_ENSM_TRANSITION_WRITE_STATE)
					fail_write_reg = REG_ENSM_CONFIG_1;
				ret = ad9361_rfdc_calibration_poll(&rfdc_phy, &rfdc);
				if (ret != 1)
					break;
				if (rfdc.stage == AD9361_RFDC_JOB_ENSM_RESTORE &&
				    rfdc.ensm_job.stage ==
					AD9361_ENSM_TRANSITION_WAIT_TARGET &&
				    rfdc.ensm_job.target_state == ENSM_STATE_RX)
					fake_regs[REG_STATE] = ENSM_STATE_RX;
				fake_time_us += 120U;
			}
			if (ret != -EAGAIN || !rfdc.terminal ||
			    rfdc.terminal_status != -EAGAIN ||
			    rfdc_phy.rfdc_transaction != NULL ||
			    ad9361_operation_busy(&rfdc_phy) ||
			    !rfdc_phy.tracking_config_valid ||
			    (failure_case == 0 &&
			     (rfdc.cleanup_status != 0 ||
			      ad9361_calibration_state_unknown(&rfdc_phy) ||
			      fake_regs[REG_STATE] != ENSM_STATE_RX)) ||
			    (failure_case == 1 &&
			     (rfdc.cleanup_status != -EIO ||
			      !ad9361_calibration_state_unknown(&rfdc_phy) ||
			      rfdc.ensm_job.terminal_status != -EIO))) {
				fprintf(stderr,
					"RFDC primary/cleanup failure precedence incorrect: "
					"case=%u ret=%" PRId32 " primary=%" PRId32
					" cleanup=%" PRId32 " ensm=%" PRId32
					" unknown=%d tracking=%d\n",
					failure_case, ret, rfdc.terminal_status,
					rfdc.cleanup_status, rfdc.ensm_job.terminal_status,
					ad9361_calibration_state_unknown(&rfdc_phy),
					rfdc_phy.tracking_config_valid);
				return EXIT_FAILURE;
			}
			fake_calibration_done_status = 0;
			fake_calibration_done_fail_call = 0;
			fake_calibration_done_call_count = 0;
			fake_calibration_autocomplete = saved_autocomplete;
			fail_write_reg = -1;
			state_after_ensm_write = -1;
			register_map_enabled = false;
			}
		}
		/* The blocking timeout wrapper must preserve a distinct cleanup error
		 * from the retained RFDC job instead of returning calibration success. */
		{
			struct refclk_scale rx_rfpll = {
				.rate = 2400000000ULL
			};
			struct refclk_scale clkrf = { .rate = 61440000ULL };
			struct ad9361_rf_phy rfdc_phy = {
				.spi = &spi,
				.pdata = &pdata,
				.clks = { [RX_RFPLL] = &rx_rfpll,
					  [CLKRF_CLK] = &clkrf },
				.rfpll_source_valid = { true, false },
			};
			int32_t cleanup_status = 123;

			memset(fake_regs, 0, sizeof(fake_regs));
			register_map_enabled = true;
			fake_calibration_autocomplete = true;
			fake_regs[REG_STATE] = ENSM_STATE_RX;
			state_after_ensm_write = ENSM_STATE_ALERT;
			fail_write_once_reg = REG_ENSM_CONFIG_1;
			fail_write_once_reg_occurrence = 2;
			apply_failed_write_once = false;
			ret = ad9361_do_calib_timeout_with_cleanup(
				&rfdc_phy, RFDC_CAL, 0, 50000U, &cleanup_status);
			if (ret != -EIO || cleanup_status != -EIO ||
			    !ad9361_calibration_state_unknown(&rfdc_phy)) {
				fprintf(stderr,
					"RFDC timeout wrapper lost cleanup failure: ret=%" PRId32
					" cleanup=%" PRId32 " unknown=%d\n",
					ret, cleanup_status,
					ad9361_calibration_state_unknown(&rfdc_phy));
				return EXIT_FAILURE;
			}
			fake_calibration_autocomplete = false;
			state_after_ensm_write = -1;
			fail_write_once_reg = -1;
			fail_write_once_reg_occurrence = 1;
			register_map_enabled = false;
		}
        /* The runtime bandwidth parent must retain itself while the ENSM
         * child owns/polls the same PHY, without running the child inline. */
        {
            struct ad9361_rf_phy parent_phy = { .spi = &spi };
            struct ad9361_rf_bandwidth_transaction parent = {0};
            unsigned poll;
            bool reached_filter_start = false;

            memset(fake_regs, 0, sizeof(fake_regs));
            fake_regs[REG_STATE] = ENSM_STATE_RX_FLUSH;
            state_after_ensm_write = ENSM_STATE_ALERT;
            if (ad9361_ensm_transition_start(
                    &parent_phy, ENSM_STATE_ALERT, true,
                    &parent.ensm_job) != 0) {
                fprintf(stderr, "Bandwidth ENSM child did not start\n");
                return EXIT_FAILURE;
            }
            parent.phy = &parent_phy;
            parent.active = true;
            parent.stage = AD9361_RF_BANDWIDTH_TRANSACTION_ENSM_ALERT;
            parent_phy.rf_bandwidth_transaction = &parent;
            {
                unsigned reads = read_count;
                unsigned writes = write_count;
                unsigned i;

                for (i = 0; i < 3; ++i) {
                    ret = ad9361_rf_bandwidth_transaction_poll(
                        &parent_phy, &parent);
                    if (ret != 1 || write_count != writes ||
                        parent_phy.rf_bandwidth_transaction != &parent) {
                        fprintf(stderr,
                                "ENSM command was issued from RX FLUSH state\n");
                        return EXIT_FAILURE;
                    }
                }
                if (read_count != reads + 2 ||
                    parent.ensm_job.stage !=
                        AD9361_ENSM_TRANSITION_WAIT_STABLE) {
                    fprintf(stderr, "ENSM stable-state wait was not entered\n");
                    return EXIT_FAILURE;
                }
                fake_regs[REG_STATE] = ENSM_STATE_ALERT;
                ret = ad9361_rf_bandwidth_transaction_poll(
                    &parent_phy, &parent);
                if (ret != 1 || read_count != reads + 2 ||
                    write_count != writes) {
                    fprintf(stderr, "ENSM stable-state poll ignored its deadline\n");
                    return EXIT_FAILURE;
                }
                fake_time_us += 120U;
                ret = ad9361_rf_bandwidth_transaction_poll(
                    &parent_phy, &parent);
                if (ret != 1 || read_count != reads + 3 ||
                    write_count != writes) {
                    fprintf(stderr, "ENSM did not observe stable state before command\n");
                    return EXIT_FAILURE;
                }
            }
            for (poll = 0; poll < 16; ++poll) {
                unsigned reads = read_count;
                unsigned writes = write_count;
                ret = ad9361_rf_bandwidth_transaction_poll(
                    &parent_phy, &parent);
                if (ret != 1 || parent_phy.rf_bandwidth_transaction != &parent ||
                    read_count - reads > 1 || write_count - writes > 1 ||
                    read_count - reads + write_count - writes > 1) {
                    fprintf(stderr,
                            "Bandwidth parent/ENSM handoff lost ownership or exceeded one SPI op\n");
                    return EXIT_FAILURE;
                }
                if (parent.stage ==
                    AD9361_RF_BANDWIDTH_TRANSACTION_FILTER_START) {
                    reached_filter_start = true;
                    break;
                }
            }
            if (!reached_filter_start || parent.ensm_job.active ||
                !parent.ensm_job.terminal ||
                parent_phy.ensm_transition_job != NULL) {
                fprintf(stderr,
                        "Bandwidth parent did not consume terminal ENSM event\n");
                return EXIT_FAILURE;
            }
            (void)ad9361_rf_bandwidth_transaction_finish(&parent, 0);
        }

        state_after_ensm_write = -1;
        fake_time_us = saved_time_us;
        register_map_enabled = false;
    }

    /* A 300 kHz RX BBF at a 1.024 GHz BBPLL needs divider 269. The
     * divider's high bit is in 0x1F9[0], so its low byte must still be
     * written to 0x1F8 without rejecting the valid 9-bit value. */
    {
        struct ad9361_rf_phy bbf_phy = { .spi = &spi };
        uint8_t saved_fake_regs[sizeof(fake_regs)];
        unsigned saved_fake_write_counts[sizeof(fake_write_counts) /
                                         sizeof(fake_write_counts[0])];
        unsigned saved_read_count = read_count;
        unsigned saved_write_count = write_count;
        unsigned saved_delay_count = delay_count;
        bool saved_register_map_enabled = register_map_enabled;
        bool saved_calibration_autocomplete = fake_calibration_autocomplete;

        memcpy(saved_fake_regs, fake_regs, sizeof(fake_regs));
        memcpy(saved_fake_write_counts, fake_write_counts,
               sizeof(fake_write_counts));
        memset(fake_regs, 0, sizeof(fake_regs));
        register_map_enabled = true;
        fake_calibration_autocomplete = true;
        ret = ad9361_rx_bb_analog_filter_calib(&bbf_phy, 300000, 1024000000);
        if (ret != 0 || bbf_phy.rxbbf_div != 269 ||
            fake_regs[REG_RX_BBF_TUNE_DIVIDE] != 13 ||
            (fake_regs[REG_RX_BBF_TUNE_CONFIG] & RX_BBF_TUNE_DIVIDE) == 0) {
            fprintf(stderr,
                    "RX BBF 9-bit divider programming failed: ret=%" PRId32
                    " divider=%u low=%u high=%u\n",
                    ret, bbf_phy.rxbbf_div,
                    fake_regs[REG_RX_BBF_TUNE_DIVIDE],
                    !!(fake_regs[REG_RX_BBF_TUNE_CONFIG] &
                       RX_BBF_TUNE_DIVIDE));
            return EXIT_FAILURE;
        }
        memcpy(fake_regs, saved_fake_regs, sizeof(fake_regs));
        memcpy(fake_write_counts, saved_fake_write_counts,
               sizeof(fake_write_counts));
        read_count = saved_read_count;
        write_count = saved_write_count;
        delay_count = saved_delay_count;
        register_map_enabled = saved_register_map_enabled;
        fake_calibration_autocomplete = saved_calibration_autocomplete;
    }
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

    /* Synthesizer CP start must be inert. Each subsequent poll performs one
     * complete ordered SPI write, then status arming and one completion
     * observation are separate queue turns. */
    {
        struct ad9361_rf_phy cp_phy = { .spi = &spi };
        struct ad9361_rf_phy other_phy = { .spi = &spi };
        struct ad9361_phy_platform_data cp_pdata = { .fdd = true };
        struct ad9361_phy_platform_data other_pdata = { .fdd = true };
        struct ad9361_synth_cp_calibration_job cp_job = {0};
        struct refclk_scale rx_lo_scale = {0};
        struct refclk_scale tx_lo_scale = {0};
        struct refclk_scale factor_scale = {0};
        struct refclk_scale dummy_lo_scale = {0};
        struct no_os_clk dummy_lo_clock = {0};
        struct rf_rx_gain rx_gain = {0};
        struct rf_gain_ctrl gain_ctrl = {0};
        uint32_t rx_path_rates[NUM_RX_CLOCKS] = {0};
        uint32_t tx_path_rates[NUM_TX_CLOCKS] = {0};
        uint32_t untouched_rx_path_rates[NUM_RX_CLOCKS] = {0};
        uint32_t untouched_tx_path_rates[NUM_TX_CLOCKS] = {0};
        uint8_t fastlock_values[RX_FAST_LOCK_CONFIG_WORD_NUM] = {0};
        char fir_data[] = "x";
        unsigned initial_write_count = write_count;
        unsigned initial_read_count = read_count;
        unsigned fake_write_count_snapshot[sizeof(fake_write_counts) /
                                           sizeof(fake_write_counts[0])];
        int32_t saved_spi_status = spi_status;
        int32_t saved_fail_write_reg = fail_write_reg;
        int32_t saved_fail_write_once_reg = fail_write_once_reg;
        unsigned saved_fail_write_once_occurrence =
            fail_write_once_reg_occurrence;
        uint16_t saved_last_spi_write_addr = last_spi_write_addr;
        bool saved_register_map = register_map_enabled;
        uint32_t raw_reg_value = UINT32_MAX;
        uint32_t ensm_mode = UINT32_MAX;
        uint8_t ensm_state = UINT8_MAX;
        uint32_t tx_rssi = UINT32_MAX;
        int32_t temperature = INT32_MAX;
        unsigned index;

        cp_phy.pdata = &cp_pdata;
        other_phy.pdata = &other_pdata;
        cp_phy.ref_clk_scale[RX_RFPLL] = &rx_lo_scale;
        cp_phy.ref_clk_scale[TX_RFPLL] = &tx_lo_scale;
        rx_lo_scale.phy = &cp_phy;
        rx_lo_scale.spi = &spi;
        rx_lo_scale.source = RX_RFPLL;
        tx_lo_scale.phy = &cp_phy;
        tx_lo_scale.spi = &spi;
        tx_lo_scale.source = TX_RFPLL;
        factor_scale.phy = &cp_phy;
        factor_scale.spi = &spi;
        factor_scale.source = ADC_CLK;
        factor_scale.mult = 1;
        factor_scale.div = 1;
        dummy_lo_scale.phy = &cp_phy;
        dummy_lo_scale.source = RX_RFPLL_DUMMY;
        cp_phy.clks[RX_RFPLL_DUMMY] = &dummy_lo_clock;
        dummy_lo_clock.rate = 1234;
        cp_phy.current_table = UINT32_MAX;
        memcpy(fake_write_count_snapshot, fake_write_counts,
               sizeof(fake_write_count_snapshot));
        register_map_enabled = false;
        spi_status = CP_CAL_VALID;
        fail_write_reg = -1;
        fail_write_once_reg = -1;

        ret = ad9361_synth_cp_calibration_start(
            &cp_phy, 40000000, true, &cp_job);
        if (ret != 0 || write_count != initial_write_count ||
            cp_phy.synth_cp_job != &cp_job ||
            !ad9361_operation_busy(&cp_phy) || cp_job.write_count != 12) {
            fprintf(stderr,
                    "synth CP start performed I/O or failed to reserve PHY: ret=%" PRId32
                    " writes=%u count=%u\n",
                    ret, write_count - initial_write_count,
                    cp_job.write_count);
            return EXIT_FAILURE;
        }

        if (ad9361_synth_cp_calibration_start(
                &other_phy, 40000000, false, &cp_job) != -EBUSY ||
            write_count != initial_write_count) {
            fprintf(stderr,
                    "active synth CP context was rebound across PHYs\n");
            return EXIT_FAILURE;
        }
        if (ad9361_reg_read(&cp_phy, REG_PRODUCT_ID, &raw_reg_value) !=
                -EBUSY ||
            ad9361_reg_write(&cp_phy, REG_PRODUCT_ID, 0) != -EBUSY ||
            read_count != initial_read_count ||
            write_count != initial_write_count ||
            raw_reg_value != UINT32_MAX) {
            fprintf(stderr,
                    "raw register API bypassed retained CP PHY ownership\n");
            return EXIT_FAILURE;
        }
        if (ad9361_reset(&cp_phy) != -EBUSY ||
            ad9361_remove(&cp_phy) != -EBUSY ||
            ad9361_init_gain_tables(&cp_phy) != -EBUSY ||
            ad9361_register_clocks(&cp_phy) != -EBUSY ||
            ad9361_unregister_clocks(&cp_phy) != -EBUSY ||
            ad9361_post_setup(&cp_phy) != -EBUSY ||
            cp_phy.synth_cp_job != &cp_job ||
            cp_phy.current_table != UINT32_MAX || cp_phy.gt_info != NULL ||
            read_count != initial_read_count ||
            write_count != initial_write_count) {
            fprintf(stderr,
                    "lifecycle/clock/post-setup API bypassed retained CP PHY ownership\n");
            return EXIT_FAILURE;
        }
        if (ad9361_bist_loopback(&cp_phy, 0) != -EBUSY ||
            ad9361_bist_prbs(&cp_phy, BIST_DISABLE) != -EBUSY ||
            ad9361_bist_tone(&cp_phy, BIST_DISABLE, 0, 0, 0) != -EBUSY ||
            ad9361_en_dis_tx(&cp_phy, TX_1 | TX_2, TX_1) != -EBUSY ||
            ad9361_en_dis_rx(&cp_phy, RX_1 | RX_2, RX_1) != -EBUSY ||
            ad9361_txmon_control(&cp_phy, 0) != -EBUSY ||
            ad9361_rf_port_setup(&cp_phy, true, 0, 0) != -EBUSY ||
            ad9361_mcs(&cp_phy, 0) != -EBUSY ||
            ad9361_set_rx_gain(&cp_phy, 1, &rx_gain) != -EBUSY ||
            ad9361_set_gain_ctrl_mode(&cp_phy, &gain_ctrl) != -EBUSY ||
            ad9361_set_rx_gain_control_mode(&cp_phy, 0,
                                             RF_GAIN_MGC) != -EBUSY ||
            ad9361_get_en_state_machine_mode(&cp_phy, &ensm_mode) !=
                -EBUSY ||
            ad9361_ensm_get_state(&cp_phy) != -EBUSY ||
            ad9361_ensm_read_state_checked(&cp_phy, &ensm_state) != -EBUSY ||
            ad9361_get_rx_rssi(&cp_phy, 0, &rx_rssi) != -EBUSY ||
            ad9361_get_tx_rssi(&cp_phy, 0, &tx_rssi) != -EBUSY ||
            ad9361_get_temperature(&cp_phy, &temperature) != -EBUSY ||
            ad9361_get_temp(&cp_phy) != -EBUSY ||
            ad9361_get_auxadc(&cp_phy) != -EBUSY ||
            ad9361_get_rx_lo_freq(&cp_phy, &lo_frequency) != -EBUSY ||
            ad9361_get_tx_lo_freq(&cp_phy, &lo_frequency) != -EBUSY ||
            ad9361_rx_fastlock_get_freq(&cp_phy, 0, &lo_frequency) !=
                -EBUSY ||
            ad9361_tx_fastlock_get_freq(&cp_phy, 0, &lo_frequency) !=
                -EBUSY ||
            lo_frequency != UINT64_MAX ||
            ad9361_get_trx_clock_chain(&cp_phy, rx_path_rates,
                                       tx_path_rates) != -EBUSY ||
            memcmp(rx_path_rates, untouched_rx_path_rates,
                   sizeof(rx_path_rates)) != 0 ||
            memcmp(tx_path_rates, untouched_tx_path_rates,
                   sizeof(tx_path_rates)) != 0 ||
            ad9361_rssi_gain_step_calib(&cp_phy) != -EBUSY ||
            ad9361_rssi_program_lna_gain(&cp_phy) != -EBUSY ||
            ad9361_rssi_write_err_tbl(&cp_phy) != -EBUSY ||
            ad9361_read_clock_data_delays(&cp_phy) != -EBUSY ||
            ad9361_write_clock_data_delays(&cp_phy) != -EBUSY ||
            ad9361_write_bist_reg(&cp_phy, 0) != -EBUSY ||
            ad9361_dig_interface_timing_analysis(&cp_phy, NULL, 0) !=
                -EBUSY ||
            ad9361_dig_tune(&cp_phy, 61440000, 0) != -EBUSY ||
            ad9361_set_rx_fir_en_dis(&cp_phy, 1) != -EBUSY ||
            ad9361_set_tx_fir_en_dis(&cp_phy, 1) != -EBUSY ||
            ad9361_set_trx_fir_en_dis(&cp_phy, 1) != -EBUSY ||
            ad9361_get_rx_fir_config(&cp_phy, 0, &rx_fir) != -EBUSY ||
            ad9361_get_tx_fir_config(&cp_phy, 0, &tx_fir) != -EBUSY ||
            ad9361_trx_load_enable_fir(&cp_phy, rx_fir, tx_fir) != -EBUSY ||
            ad9361_set_rx_rf_port_input(&cp_phy, 1) != -EBUSY ||
            ad9361_set_tx_rf_port_output(&cp_phy, 1) != -EBUSY ||
            ad9361_set_tx_auto_cal_en_dis(&cp_phy, 1) != -EBUSY ||
            ad9361_set_trx_rate_gov(&cp_phy, 1) != -EBUSY ||
            ad9361_set_trx_path_clks(&cp_phy, rx_path_rates,
                                      tx_path_rates) != -EBUSY ||
            ad9361_do_mcs(&cp_phy, &other_phy) != -EBUSY ||
            ad9361_do_calib(&cp_phy, RFDC_CAL, 0) != -EBUSY ||
            ad9361_set_tx_atten(&cp_phy, 10000, true, false, true) !=
                -EBUSY ||
            ad9361_get_tx_atten(&cp_phy, 1) != -EBUSY ||
            ad9361_tx_mute(&cp_phy, 1) != -EBUSY ||
            ad9361_set_dcxo_tune(&cp_phy, 0, 0) != -EBUSY ||
            ad9361_set_en_state_machine_mode(&cp_phy, ENSM_MODE_RX) !=
                -EBUSY ||
            ad9361_set_rx_lo_int_ext(&cp_phy, EXT_LO) != -EBUSY ||
            ad9361_set_tx_lo_int_ext(&cp_phy, EXT_LO) != -EBUSY ||
            ad9361_clk_factor_set_rate(&factor_scale, 2, 1) != -EBUSY ||
            ad9361_rfpll_set_rate(&rx_lo_scale,
                                  ad9361_to_clk(915000000)) != -EBUSY ||
            ad9361_clk_mux_set_parent(&rx_lo_scale, 1) != -EBUSY ||
            ad9361_rfpll_dummy_set_rate(&dummy_lo_scale, 2) != -EBUSY ||
            ad9361_set_rx_rfdc_track_en_dis(&cp_phy, 1) != -EBUSY ||
            ad9361_set_rx_bbdc_track_en_dis(&cp_phy, 1) != -EBUSY ||
            ad9361_set_rx_quad_track_en_dis(&cp_phy, 1) != -EBUSY ||
            ad9361_set_no_ch_mode(&cp_phy, 2) != -EBUSY ||
            ad9361_fastlock_load(&cp_phy, false, 0, fastlock_values) !=
                -EBUSY ||
            ad9361_fastlock_exit_foreign(&cp_phy, false) != -EBUSY ||
            ad9361_fastlock_store(&cp_phy, false, 0) != -EBUSY ||
            ad9361_fastlock_recall(&cp_phy, false, 0) != -EBUSY ||
            ad9361_fastlock_save(&cp_phy, false, 0, fastlock_values) !=
                -EBUSY ||
            ad9361_ensm_set_state(&cp_phy, ENSM_STATE_RX, false) !=
                -EBUSY ||
            ad9361_set_ensm_mode(&cp_phy, true, false) != -EBUSY ||
            ad9361_parse_fir(&cp_phy, fir_data, sizeof(fir_data)) != -EBUSY ||
            ad9361_validate_enable_fir(&cp_phy) != -EBUSY ||
            read_count != initial_read_count ||
            write_count != initial_write_count ||
            cp_phy.tx_mute_restore_valid || cp_pdata.fdd_independent_mode ||
            cp_pdata.use_ext_rx_lo || cp_pdata.use_ext_tx_lo ||
            cp_phy.rfdc_track_en || cp_phy.bbdc_track_en ||
            cp_phy.quad_track_en || cp_pdata.rx2tx2 ||
            cp_phy.bypass_rx_fir || cp_phy.bypass_tx_fir ||
            cp_phy.auto_cal_en || cp_phy.rate_governor != 0 ||
            cp_pdata.rf_rx_input_sel != 0 || cp_pdata.rf_tx_output_sel != 0 ||
            cp_phy.agc_mode[0] != 0 || ensm_mode != UINT32_MAX ||
            tx_rssi != UINT32_MAX || temperature != INT32_MAX ||
            ensm_state != UINT8_MAX ||
            factor_scale.mult != 1 || factor_scale.div != 1 ||
            dummy_lo_clock.rate != 1234) {
            fprintf(stderr,
                    "high-level RFIC API bypassed retained CP PHY ownership\n");
            return EXIT_FAILURE;
        }

        for (index = 0; index < cp_job.write_count; ++index) {
            unsigned before_poll = write_count;

            ret = ad9361_synth_cp_calibration_poll(&cp_phy, &cp_job);
            if (ret != 1 || write_count != before_poll + 1) {
                fprintf(stderr,
                        "synth CP poll did not perform one write: index=%u ret=%" PRId32
                        " writes=%u\n",
                        index, ret, write_count - before_poll);
                return EXIT_FAILURE;
            }
            if (index == 0 &&
                last_spi_write_addr != REG_RX_CP_LEVEL_DETECT + 0x40) {
                fprintf(stderr,
                        "TX synth CP sequence started at wrong register: 0x%x\n",
                        last_spi_write_addr);
                return EXIT_FAILURE;
            }
        }

        {
            unsigned writes_before = write_count;

            ret = ad9361_synth_cp_calibration_poll(&cp_phy, &cp_job);
            if (ret != 1 || write_count != writes_before ||
                !cp_job.wait_started ||
                cp_job.wait.reg != REG_RX_CAL_STATUS + 0x40 ||
                cp_job.wait.start_us != cp_job.trigger_time_us ||
                !cp_job.trigger_observed) {
                fprintf(stderr,
                        "synth CP wait arm was not a separate poll: ret=%" PRId32 "\n",
                        ret);
                return EXIT_FAILURE;
            }
        }

        ret = ad9361_synth_cp_calibration_poll(&cp_phy, &cp_job);
        if (ret != 0 || cp_phy.synth_cp_job != NULL ||
            ad9361_operation_busy(&cp_phy) ||
            ad9361_synth_cp_calibration_poll(&cp_phy, &cp_job) != 0) {
            fprintf(stderr,
                    "synth CP completion did not retire idempotently: ret=%" PRId32
                    "\n", ret);
            return EXIT_FAILURE;
        }

        /* Non-FDD mode has one atomic HALF_DUPLEX_MODE RMW in the preserved
         * sequence. It remains one poll operation even though the SPI helper
         * performs a read followed by a write. */
        {
            struct ad9361_rf_phy tdd_phy = { .spi = &spi };
            struct ad9361_phy_platform_data tdd_pdata = { .fdd = false };
            struct ad9361_synth_cp_calibration_job tdd_job = {0};

            tdd_phy.pdata = &tdd_pdata;
            ret = ad9361_synth_cp_calibration_start(
                &tdd_phy, 40000000, false, &tdd_job);
            if (ret != 0 || tdd_job.write_count != 13) {
                fprintf(stderr,
                        "non-FDD synth CP sequence setup failed: ret=%" PRId32
                        " count=%u\n", ret, tdd_job.write_count);
                return EXIT_FAILURE;
            }
            for (index = 0; index < tdd_job.write_count; ++index) {
                unsigned reads_before_poll = read_count;
                unsigned writes_before_poll = write_count;

                ret = ad9361_synth_cp_calibration_poll(&tdd_phy, &tdd_job);
                if (ret != 1 || write_count != writes_before_poll + 1 ||
                    read_count != reads_before_poll +
                        (tdd_job.write_is_field[index] ? 1U : 0U) ||
                    (index == 8 && last_spi_write_addr !=
                        REG_PARALLEL_PORT_CONF_3)) {
                    fprintf(stderr,
                            "non-FDD CP poll boundary failed at index=%u ret=%" PRId32
                            "\n", index, ret);
                    return EXIT_FAILURE;
                }
            }
            if (ad9361_synth_cp_calibration_poll(&tdd_phy, &tdd_job) != 1 ||
                ad9361_synth_cp_calibration_poll(&tdd_phy, &tdd_job) != 0 ||
                tdd_phy.synth_cp_job != NULL) {
                fprintf(stderr,
                        "non-FDD synth CP completion failed\n");
                return EXIT_FAILURE;
            }
        }

        /* Every staged write is a possible transport-failure boundary. Fail
         * each one in turn and require fail-closed state plus owner retirement;
         * do not run a compensating write sequence after an ambiguous SPI
         * completion. */
        for (index = 0; index < 12; ++index) {
            struct ad9361_rf_phy failed_phy = { .spi = &spi };
            struct ad9361_phy_platform_data failed_pdata = { .fdd = true };
            struct ad9361_synth_cp_calibration_job failed_job = {0};
            unsigned occurrence = 0;
            unsigned step;

            failed_phy.pdata = &failed_pdata;
            ret = ad9361_synth_cp_calibration_start(
                &failed_phy, 40000000, true, &failed_job);
            if (ret != 0) {
                fprintf(stderr,
                        "synth CP fault case failed to start at step=%u\n",
                        index);
                return EXIT_FAILURE;
            }
            for (step = 0; step <= index; ++step) {
                if (failed_job.write_reg[step] ==
                    failed_job.write_reg[index])
                    ++occurrence;
            }
            fail_write_once_reg = (int32_t)failed_job.write_reg[index];
            fail_write_once_reg_occurrence = occurrence;

            for (step = 0; step <= index; ++step) {
                ret = ad9361_synth_cp_calibration_poll(
                    &failed_phy, &failed_job);
                if ((step < index && ret != 1) ||
                    (step == index && ret != -EIO)) {
                    fprintf(stderr,
                            "synth CP fault was mishandled at step=%u poll=%u ret=%" PRId32
                            "\n", index, step, ret);
                    return EXIT_FAILURE;
                }
            }
            if (!ad9361_calibration_state_unknown(&failed_phy) ||
                failed_phy.synth_cp_job != NULL ||
                failed_phy.status_wait != NULL ||
                ad9361_synth_cp_calibration_poll(
                    &failed_phy, &failed_job) != -EIO) {
                fprintf(stderr,
                        "synth CP write failure leaked owner at step=%u\n",
                        index);
                return EXIT_FAILURE;
            }
        }

        register_map_enabled = saved_register_map;
        spi_status = saved_spi_status;
        fail_write_reg = saved_fail_write_reg;
        fail_write_once_reg = saved_fail_write_once_reg;
        fail_write_once_reg_occurrence = saved_fail_write_once_occurrence;
        last_spi_write_addr = saved_last_spi_write_addr;
        write_count = initial_write_count;
        read_count = initial_read_count;
        memcpy(fake_write_counts, fake_write_count_snapshot,
               sizeof(fake_write_count_snapshot));
    }

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
        int32_t cleanup_status = 123;
        phy.ref_clk_scale[RX_RFPLL] = &rx_mux;
        phy.ref_clk_scale[TX_RFPLL] = &tx_mux;
        phy.dev_sel = ID_AD9361;
        fake_mux_parent_status = -EIO;
        fake_mux_cleanup_status = -ETIMEDOUT;
        fake_mux_commit_parent = true;
        pdata.use_ext_rx_lo = INT_LO;
        if (ad9361_set_rx_lo_int_ext_with_cleanup(
                    &phy, EXT_LO, &cleanup_status) != -EIO ||
            cleanup_status != -ETIMEDOUT ||
            pdata.use_ext_rx_lo != EXT_LO) {
            fprintf(stderr,
                    "RX LO mux cleanup/cache status was lost after commit\n");
            return EXIT_FAILURE;
        }
        pdata.use_ext_tx_lo = INT_LO;
        cleanup_status = 123;
        if (ad9361_set_tx_lo_int_ext_with_cleanup(
                    &phy, EXT_LO, &cleanup_status) != -EIO ||
            cleanup_status != -ETIMEDOUT ||
            pdata.use_ext_tx_lo != EXT_LO) {
            fprintf(stderr,
                    "TX LO mux cleanup/cache status was lost after commit\n");
            return EXIT_FAILURE;
        }

        cleanup_status = 123;
        if (ad9361_set_rx_lo_int_ext_with_cleanup(
                    NULL, EXT_LO, &cleanup_status) != -EINVAL ||
            cleanup_status != 0) {
            fprintf(stderr,
                    "RX LO API kept stale cleanup status on rejection\n");
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
        fake_mux_cleanup_status = 0;
        phy.ref_clk_scale[RX_RFPLL] = old_rx_mux;
        phy.ref_clk_scale[TX_RFPLL] = old_tx_mux;
        phy.dev_sel = old_dev_sel;
        pdata.use_ext_rx_lo = INT_LO;
        pdata.use_ext_tx_lo = INT_LO;
    }

    /* Tracking writes poison RFIC state on partial SPI failure. The public
     * setter must restore only its software request and must not retry SPI
     * through that unknown state as a speculative rollback. */
    {
        struct ad9361_rf_phy tracking_phy = phy;
        uint8_t old_tracking = tracking_phy.rfdc_track_en;
        unsigned writes_before = fake_write_counts[REG_CALIBRATION_CONFIG_2];

        tracking_phy.calibration_state_unknown = false;
        tracking_phy.tracking_config_valid = true;
        fail_write_once_reg = REG_CALIBRATION_CONFIG_2;
        fail_write_once_reg_occurrence = 1;
        apply_failed_write_once = false;
        ret = ad9361_set_rx_rfdc_track_en_dis(
            &tracking_phy, (uint8_t)!old_tracking);
        fail_write_once_reg = -1;
        if (ret != -EIO || tracking_phy.rfdc_track_en != old_tracking ||
            !ad9361_calibration_state_unknown(&tracking_phy) ||
            tracking_phy.tracking_config_valid ||
            fake_write_counts[REG_CALIBRATION_CONFIG_2] != writes_before + 1) {
            fprintf(stderr,
                    "RX tracking attempted rollback after state poisoning\n");
            return EXIT_FAILURE;
        }
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

	/* An init failure must still attempt every acquired-resource teardown.
	 * If teardown also fails, return the primary init error and report cleanup
	 * independently; cleanup must not replace the initiating failure. */
    {
        AD9361_InitParam init_param = {0};
        unsigned failure_site;

        for (failure_site = 0; failure_site <= 4; ++failure_site) {
            struct ad9361_rf_phy *init_result = NULL;
			int32_t expected = -ENOSYS;

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
     * failure. The extended API reports consumption separately from errno,
     * including when a platform cleanup callback itself returns -EBUSY. */
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

    /* A teardown callback may itself return -EBUSY after ownership has
     * crossed the preflight boundary. The consumed flag distinguishes that
     * cleanup result from a preflight -EBUSY that leaves the PHY alive. */
    {
        struct ad9361_rf_phy *remove_phy = calloc(1, sizeof(*remove_phy));
        bool consumed = false;

        if (!remove_phy)
            return EXIT_FAILURE;
        remove_phy->spi = (struct no_os_spi_desc *)(uintptr_t)1;
        fake_spi_remove_status = -EBUSY;
        memset(fake_gpio_remove_status, 0, sizeof(fake_gpio_remove_status));
        fake_spi_remove_count = 0;
        fake_gpio_remove_count = 0;
        if (ad9361_remove_ex(remove_phy, false, &consumed) != -EBUSY ||
            !consumed || fake_spi_remove_count != 1 ||
            fake_gpio_remove_count != 4) {
            fprintf(stderr,
                    "remove API did not distinguish consumed cleanup -EBUSY\n");
            return EXIT_FAILURE;
        }
        fake_spi_remove_status = 0;
    }

    /* Removing a PHY detaches a caller-owned external-band controller before
     * the PHY allocation is consumed, so the controller can be reused/freed. */
    {
        struct ad9361_rf_phy *remove_phy = calloc(1, sizeof(*remove_phy));
        struct ad9361_ext_band_ctl *ctl = ad9361_ext_band_ctl_alloc();
        bool consumed = false;

        if (!remove_phy || !ctl)
            return EXIT_FAILURE;
        remove_phy->spi = &spi;
        if (ad9361_register_ext_band_control(remove_phy, ctl) != 0) {
            fprintf(stderr, "failed to register teardown controller\n");
            return EXIT_FAILURE;
        }
        fake_spi_remove_status = 0;
        memset(fake_gpio_remove_status, 0, sizeof(fake_gpio_remove_status));
        fake_spi_remove_count = 0;
        fake_gpio_remove_count = 0;
        if (ad9361_remove_ex(remove_phy, false, &consumed) != 0 ||
            !consumed || ctl->registered_phy != NULL ||
            fake_spi_remove_count != 1 || fake_gpio_remove_count != 4) {
            fprintf(stderr, "PHY teardown left a dangling controller owner\n");
            return EXIT_FAILURE;
        }
        ad9361_ext_band_ctl_free(ctl);
    }

    /* AXI ADC/DAC teardown failures are part of the same cleanup contract.
     * Both resources must be attempted, and the first cleanup errno wins. */
    {
        struct ad9361_rf_phy *remove_phy = calloc(1, sizeof(*remove_phy));

        if (!remove_phy) {
            fprintf(stderr, "failed to allocate AXI teardown test PHY\n");
            return EXIT_FAILURE;
        }
        remove_phy->rx_adc = (struct axi_adc *)(uintptr_t)1;
        remove_phy->tx_dac = (struct axi_dac *)(uintptr_t)1;
        fake_axi_adc_remove_status = -ENOMEM;
        fake_axi_dac_remove_status = -EIO;
        fake_axi_adc_remove_count = 0;
        fake_axi_dac_remove_count = 0;

        if (ad9361_remove(remove_phy) != -ENOMEM ||
            fake_axi_adc_remove_count != 1 ||
            fake_axi_dac_remove_count != 1) {
            fprintf(stderr,
                    "AD9361 teardown lost first AXI cleanup error or skipped a resource\n");
            return EXIT_FAILURE;
        }

        remove_phy = calloc(1, sizeof(*remove_phy));
        if (!remove_phy) {
            fprintf(stderr, "failed to allocate AXI teardown test PHY\n");
            return EXIT_FAILURE;
        }
        remove_phy->rx_adc = (struct axi_adc *)(uintptr_t)1;
        remove_phy->tx_dac = (struct axi_dac *)(uintptr_t)1;
        fake_axi_adc_remove_status = 0;
        fake_axi_dac_remove_status = -EIO;
        fake_axi_adc_remove_count = 0;
        fake_axi_dac_remove_count = 0;

        if (ad9361_remove(remove_phy) != -EIO ||
            fake_axi_adc_remove_count != 1 ||
            fake_axi_dac_remove_count != 1) {
            fprintf(stderr,
                    "AD9361 teardown lost DAC cleanup error or skipped a resource\n");
            return EXIT_FAILURE;
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
            ad9361_fir_enable_transaction_start(
                NULL, false, 1,
                &(struct ad9361_fir_enable_transaction){0}) != -EINVAL ||
            ad9361_fir_enable_transaction_poll(
                NULL, &(struct ad9361_fir_enable_transaction){0}) != -EINVAL ||
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

    /* Cleanup-aware FIR APIs initialize their secondary result even when
     * argument validation prevents the operation from starting. */
    {
        int32_t cleanup_status = 123;

        if (ad9361_rssi_gain_step_calib_with_cleanup(
                NULL, &cleanup_status) != -EINVAL || cleanup_status != 0) {
            fprintf(stderr,
                    "RSSI calibration left stale cleanup status on rejection\n");
            return EXIT_FAILURE;
        }
        cleanup_status = 123;
        if (ad9361_dig_interface_timing_analysis_with_cleanup(
                NULL, NULL, 0, &cleanup_status) != -EINVAL ||
            cleanup_status != 0) {
            fprintf(stderr,
                    "Timing analysis left stale cleanup status on rejection\n");
            return EXIT_FAILURE;
        }
        cleanup_status = 123;
        if (ad9361_get_rx_fir_config_with_cleanup(
                NULL, 0, NULL, &cleanup_status) != -EINVAL ||
            cleanup_status != 0) {
            fprintf(stderr,
                    "RX FIR getter left stale cleanup status on rejection\n");
            return EXIT_FAILURE;
        }
        cleanup_status = 123;
        if (ad9361_get_tx_fir_config_with_cleanup(
                NULL, 0, NULL, &cleanup_status) != -EINVAL ||
            cleanup_status != 0) {
            fprintf(stderr,
                    "TX FIR getter left stale cleanup status on rejection\n");
            return EXIT_FAILURE;
        }
        cleanup_status = 123;
        if (ad9361_validate_enable_fir_with_cleanup(
                NULL, &cleanup_status) != -EINVAL || cleanup_status != 0) {
            fprintf(stderr,
                    "FIR validation left stale cleanup status on rejection\n");
            return EXIT_FAILURE;
        }
        cleanup_status = 123;
        if (ad9361_set_rx_fir_en_dis_with_cleanup(
                NULL, 1, &cleanup_status) != -EINVAL || cleanup_status != 0) {
            fprintf(stderr,
                    "RX FIR API left stale cleanup status on rejection\n");
            return EXIT_FAILURE;
        }
        cleanup_status = 123;
        if (ad9361_set_tx_fir_en_dis_with_cleanup(
                NULL, 1, &cleanup_status) != -EINVAL || cleanup_status != 0) {
            fprintf(stderr,
                    "TX FIR API left stale cleanup status on rejection\n");
            return EXIT_FAILURE;
        }
        cleanup_status = 123;
        if (ad9361_set_trx_fir_en_dis_with_cleanup(
                NULL, 1, &cleanup_status) != -EINVAL || cleanup_status != 0) {
            fprintf(stderr,
                    "TRX FIR API left stale cleanup status on rejection\n");
            return EXIT_FAILURE;
        }
        cleanup_status = 123;
        if (ad9361_parse_fir_with_cleanup(
                NULL, NULL, 0, &cleanup_status) != -EINVAL ||
            cleanup_status != 0) {
            fprintf(stderr,
                    "FIR parser left stale cleanup status on rejection\n");
            return EXIT_FAILURE;
        }
        cleanup_status = 123;
        if (ad9361_trx_load_enable_fir_with_cleanup(
                NULL, (AD9361_RXFIRConfig){0},
                (AD9361_TXFIRConfig){0}, &cleanup_status) != -EINVAL ||
            cleanup_status != 0) {
            fprintf(stderr,
                    "TRX FIR loader left stale cleanup status on rejection\n");
            return EXIT_FAILURE;
        }
        cleanup_status = 123;
        if (ad9361_set_trx_path_clks_with_cleanup(
                NULL, NULL, NULL, &cleanup_status) != -EINVAL ||
            cleanup_status != 0) {
            fprintf(stderr,
                    "Clock-plan API left stale cleanup status on rejection\n");
            return EXIT_FAILURE;
        }
        cleanup_status = 123;
        if (ad9361_do_calib_with_cleanup(
                NULL, RFDC_CAL, -1, &cleanup_status) != -EINVAL ||
            cleanup_status != 0) {
            fprintf(stderr,
                    "Calibration API left stale cleanup status on rejection\n");
            return EXIT_FAILURE;
        }
    }

    /* The retained FIR API must keep the legacy no-op path inert and must
     * refuse an asynchronous call that would enter the synchronous digital
     * interface tune path on platforms other than the xA4 policy. */
    {
        struct ad9361_phy_platform_data fir_pdata = {
            .rx2tx2 = true,
        };
        struct ad9361_rf_phy fir_phy = {
            .spi = &spi,
            .pdata = &fir_pdata,
            .bypass_rx_fir = true,
            .bypass_tx_fir = true,
            .fir_config_valid = true,
        };
        struct ad9361_fir_enable_transaction fir_job = {0};
        unsigned reads_before = read_count;
        unsigned writes_before = write_count;

        ret = ad9361_fir_enable_transaction_start(
            &fir_phy, false, 0, &fir_job);
        if (ret != 0 || !fir_job.terminal || fir_job.active ||
            ad9361_fir_enable_transaction_poll(&fir_phy, &fir_job) != 0 ||
            ad9361_operation_busy(&fir_phy) || read_count != reads_before ||
            write_count != writes_before) {
            fprintf(stderr,
                    "FIR retained no-op performed work or held ownership\n");
            return EXIT_FAILURE;
        }

        memset(&fir_job, 0, sizeof(fir_job));
        ret = ad9361_fir_enable_pair_transaction_start(
            &fir_phy, 0, 0, &fir_job);
        if (ret != 0 || !fir_job.terminal || fir_job.active ||
            ad9361_fir_enable_transaction_poll(&fir_phy, &fir_job) != 0 ||
            ad9361_operation_busy(&fir_phy) || read_count != reads_before ||
            write_count != writes_before) {
            fprintf(stderr,
                    "FIR pair retained no-op performed work or held ownership\n");
            return EXIT_FAILURE;
        }

        memset(&fir_job, 0, sizeof(fir_job));
        ret = ad9361_fir_enable_pair_transaction_start(
            &fir_phy, 2, 1, &fir_job);
        if (ret != -EINVAL || fir_job.active ||
            !fir_phy.bypass_rx_fir || !fir_phy.bypass_tx_fir ||
            read_count != reads_before || write_count != writes_before) {
            fprintf(stderr,
                    "FIR pair accepted invalid RX enable or mutated state\n");
            return EXIT_FAILURE;
        }

        memset(&fir_job, 0, sizeof(fir_job));
        fir_phy.fir_config_valid = false;
        fir_pdata.dig_interface_tune_fir_disable = 0;
        ret = ad9361_fir_enable_transaction_start(
            &fir_phy, false, 0, &fir_job);
        if (ret != -EOPNOTSUPP || fir_job.active ||
            !fir_phy.bypass_rx_fir || read_count != reads_before ||
            write_count != writes_before) {
            fprintf(stderr,
                    "FIR async API entered unsupported digital tune path\n");
            return EXIT_FAILURE;
        }

        memset(&fir_job, 0, sizeof(fir_job));
        fir_pdata.dig_interface_tune_fir_disable = 1;
        fir_pdata.bb_clk_change_dig_tune_en = true;
        ret = ad9361_fir_enable_transaction_start(
            &fir_phy, false, 0, &fir_job);
        if (ret != -EOPNOTSUPP || fir_job.active ||
            !fir_phy.bypass_rx_fir || read_count != reads_before ||
            write_count != writes_before) {
            fprintf(stderr,
                    "FIR async API accepted blocking clock-change tune\n");
            return EXIT_FAILURE;
        }

        memset(&fir_job, 0, sizeof(fir_job));
        fir_pdata.bb_clk_change_dig_tune_en = false;
        fir_pdata.dig_interface_tune_fir_disable = 1;
        ret = ad9361_fir_enable_pair_transaction_start(
            &fir_phy, 1, 1, &fir_job);
        if (ret != -EINVAL || fir_job.active ||
            !fir_phy.bypass_rx_fir || !fir_phy.bypass_tx_fir ||
            fir_phy.fir_config_valid || read_count != reads_before ||
            write_count != writes_before) {
            fprintf(stderr,
                    "FIR pair preflight failure did not restore both bypass states\n");
            return EXIT_FAILURE;
        }

        memset(&fir_job, 0, sizeof(fir_job));
        fir_job.phy = &fir_phy;
        fir_job.stage = AD9361_FIR_ENABLE_TRANSACTION_CLOCK;
        fir_job.active = true;
        fir_job.update_rx = true;
        fir_job.update_tx = true;
        fir_job.old_rx_bypass = true;
        fir_job.old_tx_bypass = true;
        fir_job.clock_chain_job.phy = &fir_phy;
        fir_job.clock_chain_job.terminal = true;
        fir_job.clock_chain_job.terminal_status = 2;
        fir_phy.fir_enable_transaction = &fir_job;
        ret = ad9361_fir_enable_transaction_poll(&fir_phy, &fir_job);
        if (ret != -EIO || fir_job.active || !fir_job.terminal ||
            !ad9361_calibration_state_unknown(&fir_phy) ||
            fir_phy.bypass_rx_fir != fir_job.old_rx_bypass ||
            fir_phy.bypass_tx_fir != fir_job.old_tx_bypass ||
            read_count != reads_before || write_count != writes_before) {
            fprintf(stderr,
                    "FIR parent accepted noncanonical child completion status\n");
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
     * BBPLL word is at 0x44, followed by fractional bytes 3, 2, 1; word 1
     * carries the most-significant fractional bits. */
    {
        struct refclk_scale bbpll = { .spi = &spi, .phy = &phy };

        memset(fake_regs, 0, sizeof(fake_regs));
        fake_regs[REG_INTEGER_BB_FREQ_WORD] = 25;
        fake_regs[REG_FRACT_BB_FREQ_WORD_3] = 0x00;
        fake_regs[REG_FRACT_BB_FREQ_WORD_2] = 0x20;
        fake_regs[REG_FRACT_BB_FREQ_WORD_1] = 0x13;
        register_map_enabled = true;
        read_count = 0;
        ret = ad9361_bbpll_recalc_rate_checked(&bbpll, 38400000, &rfpll_rate);
        register_map_enabled = false;
        if (ret != 0 || rfpll_rate != 983040000 || read_count != 1) {
            fprintf(stderr,
                    "BBPLL burst read decoded wrong word: ret=%" PRId32
                    " rate=%" PRIu32 " reads=%u regs=%02x:%02x:%02x:%02x\n",
                    ret, rfpll_rate, read_count,
                    fake_regs[REG_INTEGER_BB_FREQ_WORD],
                    fake_regs[REG_FRACT_BB_FREQ_WORD_3],
                    fake_regs[REG_FRACT_BB_FREQ_WORD_2],
                    fake_regs[REG_FRACT_BB_FREQ_WORD_1]);
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

    /* Setup's retained divider preparation must not publish a candidate
     * before readback, and mismatched readback must leave confirmed cache
     * untouched. */
    {
        struct ad9361_rf_phy rate_phy = {0};
        struct no_os_clk parent = {.rate = 40000000};
        struct no_os_clk child = {.rate = 40000000};
        struct refclk_scale rate_scale = {
            .spi = &spi,
            .phy = &rate_phy,
            .mult = 1,
            .div = 1,
            .source = RX_REFCLK,
        };
        uint8_t candidate = 0xff;

        rate_phy.clk_refin = &parent;
        rate_phy.clks[RX_REFCLK] = &child;
        rate_phy.ref_clk_scale[RX_REFCLK] = &rate_scale;
        ret = ad9361_setup_refclk_rate_prepare(
            &rate_phy, RX_REFCLK, 20000000, &candidate);
        if (ret < 0 || candidate != 1 || rate_scale.mult != 1 ||
            rate_scale.div != 1 || child.rate != 40000000) {
            fprintf(stderr,
                    "Retained refclk prepare published unconfirmed cache\n");
            return EXIT_FAILURE;
        }
        ret = ad9361_setup_refclk_rate_commit_readback(
            &rate_phy, RX_REFCLK, 0, candidate);
        if (ret != -EIO || rate_scale.mult != 1 || rate_scale.div != 1 ||
            child.rate != 40000000) {
            fprintf(stderr,
                    "Mismatched refclk readback changed confirmed cache\n");
            return EXIT_FAILURE;
        }
        ret = ad9361_setup_refclk_rate_commit_readback(
            &rate_phy, RX_REFCLK, candidate, candidate);
        if (ret < 0 || rate_scale.mult != 1 || rate_scale.div != 2 ||
            child.rate != 20000000) {
            fprintf(stderr,
                    "Confirmed refclk readback did not publish cache\n");
            return EXIT_FAILURE;
        }
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

        /* The retained RFPLL API must hold the PHY between polls, reject a
         * competing mutation without SPI, and latch the first programming
         * error at terminal poll. */
        {
            /* The clock-framework wrapper must reject a partially built PHY
             * before inspecting platform data. This is reachable during
             * constructor/error cleanup and must fail as an API error rather
             * than dereference a missing pdata pointer. */
            {
                struct ad9361_rf_phy incomplete_phy = {0};
                struct no_os_clk incomplete_lo = {0};
                struct refclk_scale incomplete_rfpll = {
                    .spi = &spi,
                    .phy = &incomplete_phy,
                    .source = RX_RFPLL,
                };
                struct ad9361_rfpll_clock_transaction transaction = {0};
                unsigned reads_before = read_count;
                unsigned writes_before = write_count;

                incomplete_phy.spi = &spi;
                incomplete_phy.clks[RX_RFPLL] = &incomplete_lo;
                if (ad9361_rfpll_clock_transaction_start(
                        &incomplete_phy, &incomplete_rfpll,
                        ad9361_to_clk(1900000000ULL), &transaction) !=
                        -EINVAL || read_count != reads_before ||
                    write_count != writes_before) {
                    fprintf(stderr,
                            "RFPLL clock wrapper did not reject missing PHY data\n");
                    return EXIT_FAILURE;
                }
            }

            struct refclk_scale rxpll = {
                .spi = &spi,
                .phy = &phy,
                .source = RX_RFPLL_INT,
                .parent_source = RX_REFCLK,
            };
            struct ad9361_rf_phy other_phy = {
                .spi = &spi,
                .pdata = &pdata,
            };
            struct refclk_scale other_rxpll = {
                .spi = &spi,
                .phy = &other_phy,
                .source = RX_RFPLL_INT,
                .parent_source = RX_REFCLK,
            };
            struct ad9361_rfpll_rate_transaction transaction = {0};
            unsigned writes_after_start;

            phy.calibration_state_unknown = false;
            phy.rfpll_rate_transaction = NULL;
            fail_write_reg = -1;
            ret = ad9361_rfpll_rate_transaction_start(
                &rxpll, ad9361_to_clk(1900000000), 40000000,
                &transaction);
            if (ret != 0 || !transaction.active ||
                phy.rfpll_rate_transaction != &transaction ||
                !ad9361_operation_busy(&phy)) {
                fprintf(stderr,
                        "RFPLL retained start did not reserve PHY: %" PRId32 "\n",
                        ret);
                return EXIT_FAILURE;
            }
            writes_after_start = write_count;
            if (ad9361_rfpll_rate_transaction_start(
                    &other_rxpll, ad9361_to_clk(1800000000), 40000000,
                    &transaction) != -EBUSY ||
                transaction.phy != &phy || !transaction.active ||
                phy.rfpll_rate_transaction != &transaction ||
                other_phy.rfpll_rate_transaction != NULL ||
                write_count != writes_after_start) {
                fprintf(stderr,
                        "RFPLL start rebound a live context to another PHY\n");
                return EXIT_FAILURE;
            }
            if (ad9361_tracking_control(&phy, true, true, true) != -EBUSY ||
                write_count != writes_after_start) {
                fprintf(stderr,
                        "RFPLL retained owner allowed a competing mutation\n");
                return EXIT_FAILURE;
            }

            fail_write_reg = REG_RX_VCO_OUTPUT;
            ret = ad9361_rfpll_rate_transaction_poll(&phy, &transaction);
            if (ret != -EIO || !transaction.terminal ||
                transaction.terminal_status != -EIO ||
                phy.rfpll_rate_transaction != NULL ||
                !ad9361_calibration_state_unknown(&phy)) {
                fprintf(stderr,
                        "RFPLL retained poll did not latch programming failure: %" PRId32 "\n",
                        ret);
                return EXIT_FAILURE;
            }
            writes_after_start = write_count;
            if (ad9361_rfpll_rate_transaction_poll(&phy, &transaction) !=
                    -EIO || write_count != writes_after_start) {
                fprintf(stderr,
                        "RFPLL terminal poll was not idempotent\n");
                return EXIT_FAILURE;
            }
            fail_write_reg = -1;
        }

        /* Exercise both lock phases of the paired-FDD path. Different RX/TX
         * LOs require the counterpart synth to move from its old TDD LUT to
         * the FDD LUT; the caller-owned transaction must retain ownership
         * across both programming and lock observations, then publish cache. */
        {
            struct no_os_clk refclk = { .rate = 40000000 };
            struct refclk_scale rxpll = {
                .spi = &spi,
                .phy = &phy,
                .source = RX_RFPLL_INT,
                .parent_source = RX_REFCLK,
            };
            struct ad9361_rfpll_rate_transaction transaction = {0};
            struct no_os_clk *saved_refclk = phy.clk_refin;
            uint64_t saved_rx_lo = phy.current_rx_lo_freq;
            uint64_t saved_tx_lo = phy.current_tx_lo_freq;
            uint32_t saved_rx_div = phy.cached_rx_rfpll_div;
            uint32_t saved_tx_div = phy.cached_tx_rfpll_div;
            bool saved_calibration_unknown =
                phy.calibration_state_unknown;
            bool saved_rx_table = phy.current_rx_use_tdd_table;
            bool saved_tx_table = phy.current_tx_use_tdd_table;
            unsigned tx_vco_writes = fake_write_counts[REG_TX_VCO_OUTPUT];

            memset(fake_regs, 0, sizeof(fake_regs));
            phy.clk_refin = &refclk;
            phy.calibration_state_unknown = false;
            phy.status_wait = NULL;
            phy.calibration_wait = NULL;
            phy.bbpll_rate_transaction = NULL;
            phy.rfpll_rate_transaction = NULL;
            phy.current_rx_lo_freq = 1800000000ULL;
            phy.current_tx_lo_freq = 900000000ULL;
            phy.cached_rx_rfpll_div = 0;
            phy.current_rx_use_tdd_table = false;
            phy.current_tx_use_tdd_table = true;
            pdata.fdd = true;
            pdata.fdd_independent_mode = false;
            fake_time_us = 0;
            ret = ad9361_rfpll_rate_transaction_start(
                &rxpll, ad9361_to_clk(1900000000ULL), 40000000,
                &transaction);
            if (ret != 0 ||
                ad9361_rfpll_rate_transaction_poll(&phy, &transaction) != 1 ||
                transaction.stage != AD9361_RFPLL_TRANSACTION_WAIT ||
                phy.rfpll_rate_transaction != &transaction) {
                fprintf(stderr,
                        "RFPLL success test failed to enter first lock wait: %" PRId32 "\n",
                        ret);
                return EXIT_FAILURE;
            }
            ret = ad9361_rfpll_rate_transaction_poll(&phy, &transaction);
            if (ret != 1 || transaction.stage != AD9361_RFPLL_TRANSACTION_WAIT ||
                !transaction.wait.active || phy.current_rx_lo_freq != 0) {
                fprintf(stderr,
                        "RFPLL lock-pending poll published an unconfirmed LO\n");
                return EXIT_FAILURE;
            }

            fake_regs[REG_RX_CP_OVERRANGE_VCO_LOCK] |= VCO_LOCK;
            fake_time_us = transaction.wait.poll_not_before_us;
            ret = ad9361_rfpll_rate_transaction_poll(&phy, &transaction);
            if (ret != 1 || transaction.stage != AD9361_RFPLL_TRANSACTION_PROGRAM ||
                transaction.fixup_passes != 1) {
                fprintf(stderr,
                        "RFPLL paired-FDD fixup was not scheduled after RX lock: ret=%" PRId32 "\n",
                        ret);
                return EXIT_FAILURE;
            }

            ret = ad9361_rfpll_rate_transaction_poll(&phy, &transaction);
            if (ret != 1 || transaction.stage != AD9361_RFPLL_TRANSACTION_WAIT ||
                fake_write_counts[REG_TX_VCO_OUTPUT] <= tx_vco_writes) {
                fprintf(stderr,
                        "RFPLL paired-FDD pass did not program TX counterpart: ret=%" PRId32 "\n",
                        ret);
                return EXIT_FAILURE;
            }
            fake_regs[REG_TX_CP_OVERRANGE_VCO_LOCK] |= VCO_LOCK;
            fake_time_us = transaction.wait.poll_not_before_us;
            ret = ad9361_rfpll_rate_transaction_poll(&phy, &transaction);
            if (ret != 1 || transaction.stage != AD9361_RFPLL_TRANSACTION_RESTORE ||
                transaction.fixup_passes != 2) {
                fprintf(stderr,
                        "RFPLL paired-FDD second lock did not finish: ret=%" PRId32 "\n",
                        ret);
                return EXIT_FAILURE;
            }
            ret = ad9361_rfpll_rate_transaction_poll(&phy, &transaction);
            if (ret != 0 || !transaction.terminal || transaction.active ||
                phy.rfpll_rate_transaction != NULL ||
                phy.current_rx_lo_freq != ad9361_to_clk(1900000000ULL) ||
                phy.cached_rx_rfpll_div != transaction.target_vco_div ||
                phy.current_rx_use_tdd_table || phy.current_tx_use_tdd_table ||
                ad9361_calibration_state_unknown(&phy)) {
                fprintf(stderr,
                        "RFPLL paired-FDD success did not commit confirmed state: ret=%" PRId32 "\n",
                        ret);
                return EXIT_FAILURE;
            }

            /* Exercise the opposite entry direction independently: TX is
             * requested, so only the TX lock register and TX LO/divider cache
             * may authorize completion. */
            {
                struct refclk_scale txpll = {
                    .spi = &spi,
                    .phy = &phy,
                    .source = TX_RFPLL_INT,
                    .parent_source = TX_REFCLK,
                };
                struct ad9361_rfpll_rate_transaction tx_transaction = {0};

                memset(fake_regs, 0, sizeof(fake_regs));
                pdata.fdd = false;
                phy.calibration_state_unknown = false;
                phy.current_tx_lo_freq = 0;
                phy.rfpll_rate_transaction = NULL;
                fake_time_us = 0;
                ret = ad9361_rfpll_rate_transaction_start(
                    &txpll, ad9361_to_clk(1900000000ULL), 40000000,
                    &tx_transaction);
                if (ret != 0 ||
                    ad9361_rfpll_rate_transaction_poll(
                        &phy, &tx_transaction) != 1 ||
                    tx_transaction.stage != AD9361_RFPLL_TRANSACTION_WAIT ||
                    tx_transaction.wait.reg !=
                        REG_TX_CP_OVERRANGE_VCO_LOCK) {
                    fprintf(stderr,
                            "TX RFPLL request did not arm the TX lock wait: %" PRId32 "\n",
                            ret);
                    return EXIT_FAILURE;
                }
                fake_regs[REG_TX_CP_OVERRANGE_VCO_LOCK] |= VCO_LOCK;
                ret = ad9361_rfpll_rate_transaction_poll(
                    &phy, &tx_transaction);
                if (ret != 1 ||
                    tx_transaction.stage != AD9361_RFPLL_TRANSACTION_RESTORE ||
                    phy.current_tx_lo_freq != 0) {
                    fprintf(stderr,
                            "TX RFPLL lock observation committed too early: %" PRId32 "\n",
                            ret);
                    return EXIT_FAILURE;
                }
                ret = ad9361_rfpll_rate_transaction_poll(
                    &phy, &tx_transaction);
                if (ret != 0 ||
                    phy.current_tx_lo_freq !=
                        ad9361_to_clk(1900000000ULL) ||
                    phy.cached_tx_rfpll_div !=
                        tx_transaction.target_vco_div ||
                    phy.rfpll_rate_transaction != NULL ||
                    ad9361_calibration_state_unknown(&phy)) {
                    fprintf(stderr,
                            "TX RFPLL lock success did not commit TX cache: %" PRId32 "\n",
                            ret);
                    return EXIT_FAILURE;
                }
            }

            /* A TX lock timeout is terminal failure, never a successful
             * cache commit. The fake clock advances to the hardware-derived
             * deadline without running a wall-clock wait. */
            {
                struct refclk_scale txpll = {
                    .spi = &spi,
                    .phy = &phy,
                    .source = TX_RFPLL_INT,
                    .parent_source = TX_REFCLK,
                };
                struct ad9361_rfpll_rate_transaction tx_timeout = {0};

                memset(fake_regs, 0, sizeof(fake_regs));
                phy.calibration_state_unknown = false;
                phy.current_tx_lo_freq = 0;
                phy.rfpll_rate_transaction = NULL;
                fake_time_us = 0;
                ret = ad9361_rfpll_rate_transaction_start(
                    &txpll, ad9361_to_clk(1900000000ULL), 40000000,
                    &tx_timeout);
                if (ret != 0 ||
                    ad9361_rfpll_rate_transaction_poll(
                        &phy, &tx_timeout) != 1 ||
                    ad9361_rfpll_rate_transaction_poll(
                        &phy, &tx_timeout) != 1 ||
                    !tx_timeout.wait.active) {
                    fprintf(stderr,
                            "TX RFPLL timeout case did not enter lock wait: %" PRId32 "\n",
                            ret);
                    return EXIT_FAILURE;
                }
                fake_time_us = tx_timeout.wait.start_us +
                    tx_timeout.wait.timeout_us;
                ret = ad9361_rfpll_rate_transaction_poll(
                    &phy, &tx_timeout);
                if (ret != -ETIMEDOUT || !tx_timeout.terminal ||
                    phy.rfpll_rate_transaction != NULL ||
                    !ad9361_calibration_state_unknown(&phy) ||
                    phy.current_tx_lo_freq != 0) {
                    fprintf(stderr,
                            "TX RFPLL timeout was not fail-closed: ret=%" PRId32 "\n",
                            ret);
                    return EXIT_FAILURE;
                }
            }

            phy.clk_refin = saved_refclk;
            phy.current_rx_lo_freq = saved_rx_lo;
            phy.current_tx_lo_freq = saved_tx_lo;
            phy.cached_rx_rfpll_div = saved_rx_div;
            phy.cached_tx_rfpll_div = saved_tx_div;
            phy.current_rx_use_tdd_table = saved_rx_table;
            phy.current_tx_use_tdd_table = saved_tx_table;
            phy.calibration_state_unknown = saved_calibration_unknown;
            phy.rfpll_rate_transaction = NULL;
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

    /* A completion status observed after a delayed cooperative poll is still
     * hardware completion evidence. The deadline fails only while the status
     * remains absent; scheduling latency alone must not poison a completed
     * transceiver. */
    {
        struct ad9361_rf_phy late_done_phy = { .spi = &spi };
        struct ad9361_status_wait wait = {0};

        register_map_enabled = true;
        fake_time_us = 0;
        fake_regs[REG_STATE] = 1;
        if (ad9361_status_wait_start(&late_done_phy, REG_STATE, 1, 1,
                                     100, 10, &wait) != 0)
            return EXIT_FAILURE;
        fake_time_us = 1000;
        if (ad9361_status_wait_poll(&late_done_phy, &wait) != 0 ||
            ad9361_calibration_state_unknown(&late_done_phy) ||
            late_done_phy.status_wait != NULL) {
            fprintf(stderr,
                    "late but confirmed status was treated as timeout\n");
            return EXIT_FAILURE;
        }

        {
            struct ad9361_rf_phy overdue_phy = { .spi = &spi };
            struct ad9361_status_wait overdue_wait = {0};

            fake_regs[REG_STATE] = 0;
            fake_time_us = 2000;
            if (ad9361_status_wait_start(&overdue_phy, REG_STATE, 1, 1,
                                         100, 10, &overdue_wait) != 0)
                return EXIT_FAILURE;
            fake_time_us = 2200;
            if (ad9361_status_wait_poll(&overdue_phy, &overdue_wait) !=
                    -ETIMEDOUT ||
                !ad9361_calibration_state_unknown(&overdue_phy) ||
                overdue_phy.status_wait != NULL) {
                fprintf(stderr,
                        "absent status did not fail at watchdog deadline\n");
                return EXIT_FAILURE;
            }
        }

        /* One live wait context cannot be rebound to another PHY: doing so
         * would orphan the first PHY's status owner and completion record. */
        {
            struct ad9361_rf_phy owner_phy = { .spi = &spi };
            struct ad9361_rf_phy contender_phy = { .spi = &spi };
            struct ad9361_status_wait shared_wait = {0};
            unsigned reads_before;

            fake_regs[REG_STATE] = 0;
            fake_time_us = 3000;
            if (ad9361_status_wait_start(&owner_phy, REG_STATE, 1, 1,
                                         100, 10, &shared_wait) != 0)
                return EXIT_FAILURE;
            reads_before = read_count;
            if (ad9361_status_wait_start(&contender_phy, REG_STATE, 1, 1,
                                         100, 10, &shared_wait) != -EBUSY ||
                owner_phy.status_wait != &shared_wait ||
                contender_phy.status_wait != NULL || !shared_wait.active ||
                read_count != reads_before) {
                fprintf(stderr,
                        "status-wait start rebound a live context across PHYs\n");
                return EXIT_FAILURE;
            }
            fake_regs[REG_STATE] = 1;
            if (ad9361_status_wait_poll(&owner_phy, &shared_wait) != 0 ||
                owner_phy.status_wait != NULL ||
                ad9361_calibration_state_unknown(&owner_phy)) {
                fprintf(stderr,
                        "original status-wait owner could not retire after rejected rebind\n");
                return EXIT_FAILURE;
            }
        }
        register_map_enabled = false;
    }

    /* Composite transaction contexts also remain bound to their original
     * PHY until terminal retirement. A second PHY must not overwrite it. */
    {
        struct ad9361_rf_phy owner_phy = {
            .spi = &spi,
            .pdata = &pdata,
        };
        struct ad9361_rf_phy contender_phy = {
            .spi = &spi,
            .pdata = &pdata,
        };
        struct ad9361_rf_bandwidth_transaction transaction = {
            .phy = &owner_phy,
            .active = true,
        };
        unsigned reads_before = read_count;
        unsigned writes_before = write_count;

        owner_phy.rf_bandwidth_transaction = &transaction;
        if (ad9361_rf_bandwidth_transaction_start(
                &contender_phy, 5000000, 5000000, &transaction) != -EBUSY ||
            transaction.phy != &owner_phy || !transaction.active ||
            owner_phy.rf_bandwidth_transaction != &transaction ||
            contender_phy.rf_bandwidth_transaction != NULL ||
            read_count != reads_before || write_count != writes_before) {
            fprintf(stderr,
                    "bandwidth start rebound a live context across PHYs\n");
            return EXIT_FAILURE;
        }
        owner_phy.rf_bandwidth_transaction = NULL;
    }

    /* A failed start write can have reached the RFIC and completed before a
     * readback. A clear busy bit cannot prove that the calibration was never
     * accepted; retain a board-level fail-closed PHY state. */
    {
        struct ad9361_rf_phy start_fault_phy = { .spi = &spi };
        struct ad9361_calibration_wait wait = {0};
        struct ad9361_bbdc_calibration_job job = {0};
        unsigned writes_before;

        register_map_enabled = true;
        fake_calibration_autocomplete = true;
        fail_write_once_reg = REG_CALIBRATION_CTRL;
        apply_failed_write_once = true;
        ret = ad9361_calibration_wait_start(
            &start_fault_phy, BBDC_CAL, 1000, &wait);
        if (ret != -EIO ||
            fake_regs[REG_CALIBRATION_CTRL] != 0 ||
            !ad9361_calibration_state_unknown(&start_fault_phy) ||
            !start_fault_phy.calibration_wait_quarantined ||
            start_fault_phy.calibration_wait != NULL) {
            fprintf(stderr,
                    "ambiguous completed calibration start was accepted: ret=%" PRId32
                    " ctrl=0x%02x unknown=%u owner=%p\n", ret,
                    fake_regs[REG_CALIBRATION_CTRL],
                    ad9361_calibration_state_unknown(&start_fault_phy),
                    (void *)start_fault_phy.calibration_wait);
            return EXIT_FAILURE;
        }
        writes_before = write_count;
        if (ad9361_bbdc_calibration_start(&start_fault_phy, &job) != -EIO ||
            write_count != writes_before) {
            fprintf(stderr,
                    "poisoned PHY accepted a follow-on BBDC mutation\n");
            return EXIT_FAILURE;
        }
        register_map_enabled = false;
        fake_calibration_autocomplete = false;
    }

    /* The synthesizer CP path owns status_wait rather than calibration_wait.
     * Other calibration/tracking starts must respect that same exclusive
     * RFIC owner. */
    {
        struct ad9361_rf_phy wait_owner_phy = {
            .spi = &spi,
            .pdata = &pdata,
        };
        struct no_os_clk wait_owner_bbpll = { .rate = 983040000 };
        struct refclk_scale wait_owner_bbpll_scale = {
            .source = BBPLL_CLK,
            .phy = &wait_owner_phy,
        };
        struct ad9361_status_wait status_wait = {0};
        struct ad9361_calibration_wait calibration_wait = {0};
        struct ad9361_bbdc_calibration_job job = {0};
        uint32_t checked_rate;
        unsigned writes_before;

        register_map_enabled = true;
        wait_owner_phy.clks[BBPLL_CLK] = &wait_owner_bbpll;
        if (ad9361_status_wait_start(&wait_owner_phy, REG_STATE, 1, 1,
                                     1000, 10, &status_wait) != 0)
            return EXIT_FAILURE;
        writes_before = write_count;
        if (ad9361_bbdc_calibration_start(&wait_owner_phy, &job) != -EBUSY ||
            ad9361_tracking_control(&wait_owner_phy, false, false, false) !=
                -EBUSY ||
            clk_get_rate_checked(&wait_owner_phy,
                                 &wait_owner_bbpll_scale,
                                 &checked_rate) != -EBUSY ||
            write_count != writes_before) {
            fprintf(stderr,
                    "clock read or RFIC mutation overlapped status-wait owner\n");
            return EXIT_FAILURE;
        }
        fake_regs[REG_STATE] = 1;
        if (ad9361_status_wait_poll(&wait_owner_phy, &status_wait) != 0 ||
            wait_owner_phy.status_wait != NULL) {
            fprintf(stderr, "status-wait owner did not retire cleanly\n");
            return EXIT_FAILURE;
        }

        if (ad9361_calibration_wait_start(&wait_owner_phy, BBDC_CAL,
                                          1000, &calibration_wait) != 0 ||
            clk_get_rate_checked(&wait_owner_phy,
                                 &wait_owner_bbpll_scale,
                                 &checked_rate) != -EBUSY) {
            fprintf(stderr,
                    "checked clock read escaped active calibration owner\n");
            return EXIT_FAILURE;
        }
        fake_regs[REG_CALIBRATION_CTRL] = 0;
        if (ad9361_calibration_wait_poll(&wait_owner_phy,
                                         &calibration_wait) != 0 ||
            wait_owner_phy.calibration_wait != NULL) {
            fprintf(stderr,
                    "calibration-wait owner did not retire cleanly\n");
            return EXIT_FAILURE;
        }

        {
            struct ad9361_rf_phy lost_wait_phy = { .spi = &spi };
            struct ad9361_status_wait lost_wait = {0};

            if (ad9361_status_wait_start(&lost_wait_phy, REG_STATE, 1, 1,
                                         1000, 10, &lost_wait) != 0)
                return EXIT_FAILURE;
            lost_wait.active = false;
            if (ad9361_status_wait_poll(&lost_wait_phy, &lost_wait) != -EIO ||
                !ad9361_calibration_state_unknown(&lost_wait_phy) ||
                lost_wait_phy.status_wait != &lost_wait) {
                fprintf(stderr,
                        "lost status-wait context was not quarantined\n");
                return EXIT_FAILURE;
            }
        }
        register_map_enabled = false;
    }

    /* Calibration wait contexts are equally single-owner. Reject cross-PHY
     * reuse before the contender can issue a second start write. */
    {
        struct ad9361_rf_phy owner_phy = { .spi = &spi };
        struct ad9361_rf_phy contender_phy = { .spi = &spi };
        struct ad9361_calibration_wait shared_wait = {0};
        unsigned writes_before;

        register_map_enabled = true;
        fake_calibration_autocomplete = true;
        fake_regs[REG_CALIBRATION_CTRL] = 0;
        if (ad9361_calibration_wait_start(
                &owner_phy, BBDC_CAL, 1000, &shared_wait) != 0)
            return EXIT_FAILURE;
        writes_before = write_count;
        if (ad9361_calibration_wait_start(
                &contender_phy, RFDC_CAL, 1000, &shared_wait) != -EBUSY ||
            owner_phy.calibration_wait != &shared_wait ||
            contender_phy.calibration_wait != NULL || !shared_wait.active ||
            write_count != writes_before) {
            fprintf(stderr,
                    "calibration-wait start rebound a live context across PHYs\n");
            return EXIT_FAILURE;
        }
        if (ad9361_calibration_wait_poll(&owner_phy, &shared_wait) != 0 ||
            owner_phy.calibration_wait != NULL ||
            ad9361_calibration_state_unknown(&owner_phy)) {
            fprintf(stderr,
                    "original calibration-wait owner could not retire after rejected rebind\n");
            return EXIT_FAILURE;
        }
        fake_calibration_autocomplete = false;
        register_map_enabled = false;
    }

    /* An uncertain calibration must become a PHY-owned quarantine rather
     * than leave a pointer to a stack wait or make ordinary remove succeed.
     * A confirmed-clear fallback may report the read error but safely retire. */
    {
        struct ad9361_rf_phy *quarantined = calloc(1, sizeof(*quarantined));
        struct ad9361_calibration_wait timeout_wait = {0};
        struct ad9361_calibration_wait read_error_wait = {0};
        struct ad9361_calibration_wait idle_error_wait = {0};
        bool consumed = false;

        if (!quarantined)
            return EXIT_FAILURE;
        register_map_enabled = true;
        fake_calibration_autocomplete = false;
        fake_regs[REG_CALIBRATION_CTRL] = 0;
        fake_time_us = 10000;
        quarantined->spi = &spi;
        if (ad9361_calibration_wait_start(quarantined, BBDC_CAL, 100,
                                          &timeout_wait) != 0)
            return EXIT_FAILURE;
        fake_time_us = timeout_wait.start_us + timeout_wait.timeout_us +
                       timeout_wait.poll_delay_us;
        if (ad9361_calibration_wait_poll(quarantined, &timeout_wait) !=
                -ETIMEDOUT ||
            !quarantined->calibration_wait_quarantined ||
            quarantined->calibration_wait != NULL ||
            !ad9361_operation_busy(quarantined) ||
            ad9361_remove_ex(quarantined, false, &consumed) != -EBUSY ||
            consumed) {
            fprintf(stderr,
                    "busy calibration timeout lost PHY quarantine/ownership\n");
            return EXIT_FAILURE;
        }
        if (ad9361_bbdc_calibration_start(quarantined,
                                          &(struct ad9361_bbdc_calibration_job){0}) != -EIO) {
            fprintf(stderr, "quarantined PHY accepted a later calibration\n");
            return EXIT_FAILURE;
        }

        /* Reset is a board-level assertion: the no-OS test records that
         * precondition by passing true, then checks removal reports consumption. */
        quarantined->spi = NULL;
        fake_gpio_remove_count = 0;
        memset(fake_gpio_remove_status, 0, sizeof(fake_gpio_remove_status));
        if (ad9361_remove_ex(quarantined, true, &consumed) < 0 || !consumed) {
            fprintf(stderr, "RESETB recovery did not retire quarantined PHY\n");
            return EXIT_FAILURE;
        }

        {
            struct ad9361_rf_phy idle_phy = { .spi = &spi };

            fake_time_us = 20000;
            fake_calibration_autocomplete = true;
            fake_regs[REG_CALIBRATION_CTRL] = 0;
            if (ad9361_calibration_wait_start(&idle_phy, BBDC_CAL, 1000,
                                              &idle_error_wait) != 0)
                return EXIT_FAILURE;
            fail_read_once_reg = REG_CALIBRATION_CTRL;
            if (ad9361_calibration_wait_poll(&idle_phy, &idle_error_wait) !=
                    -EIO ||
                idle_phy.calibration_wait != NULL ||
                idle_phy.calibration_wait_quarantined ||
                ad9361_calibration_state_unknown(&idle_phy)) {
                fprintf(stderr,
                        "confirmed-idle read error was not retired safely\n");
                return EXIT_FAILURE;
            }
            fake_calibration_autocomplete = false;
        }

        {
            struct ad9361_rf_phy gpio_idle_phy = {
                .spi = &spi,
                .read_cal_done = fake_calibration_done,
            };
            struct ad9361_calibration_wait gpio_idle_wait = {0};

            fake_time_us = 30000;
            fake_calibration_done_status = 0;
            fake_calibration_done_fail_call = 0;
            fake_calibration_done_call_count = 0;
            fake_calibration_autocomplete = true;
            fake_regs[REG_CALIBRATION_CTRL] = 0;
            if (ad9361_calibration_wait_start(
                    &gpio_idle_phy, BBDC_CAL, 1000,
                    &gpio_idle_wait) != 0)
                return EXIT_FAILURE;
            fail_read_once_reg = REG_CALIBRATION_CTRL;
            if (ad9361_calibration_wait_poll(
                    &gpio_idle_phy, &gpio_idle_wait) != -EIO ||
                gpio_idle_phy.calibration_wait != NULL ||
                gpio_idle_phy.calibration_wait_quarantined ||
                ad9361_calibration_state_unknown(&gpio_idle_phy)) {
                fprintf(stderr,
                        "GPIO-done fallback failed to confirm idle status\n");
                return EXIT_FAILURE;
            }
            fake_calibration_autocomplete = false;
        }

        {
            struct ad9361_rf_phy unreadable_phy = { .spi = &spi };

            fake_regs[REG_CALIBRATION_CTRL] = 0;
            if (ad9361_calibration_wait_start(&unreadable_phy, BBDC_CAL,
                                              1000, &read_error_wait) != 0)
                return EXIT_FAILURE;
            fail_read_reg = REG_CALIBRATION_CTRL;
            if (ad9361_calibration_wait_poll(&unreadable_phy,
                                             &read_error_wait) != -EIO ||
                !unreadable_phy.calibration_wait_quarantined ||
                unreadable_phy.calibration_wait != NULL ||
                !ad9361_operation_busy(&unreadable_phy)) {
                fprintf(stderr,
                        "unreadable calibration status did not quarantine PHY\n");
                return EXIT_FAILURE;
            }
            fail_read_reg = -1;
        }

        {
            struct ad9361_rf_phy *mismatched_phy =
                calloc(1, sizeof(*mismatched_phy));
            struct ad9361_calibration_wait active_owner = {
                .active = true,
            };
            struct ad9361_calibration_wait wrong_context = {
                .active = true,
                .timeout_us = 1000,
                .poll_delay_us = 100,
            };
            struct ad9361_rfdc_calibration_job retained_parent = {0};

            if (!mismatched_phy)
                return EXIT_FAILURE;
            mismatched_phy->spi = &spi;
            wrong_context.phy = mismatched_phy;
            mismatched_phy->calibration_wait = &active_owner;
            if (ad9361_calibration_wait_poll(mismatched_phy,
                                             &wrong_context) != -EIO ||
                !mismatched_phy->calibration_wait_quarantined ||
                mismatched_phy->calibration_wait != NULL) {
                fprintf(stderr,
                        "mismatched wait context left a stale PHY owner\n");
                return EXIT_FAILURE;
            }
            mismatched_phy->spi = NULL;
            mismatched_phy->rfdc_transaction = &retained_parent;
            if (ad9361_remove_ex(mismatched_phy, true, &consumed) != -EBUSY ||
                consumed ||
                !mismatched_phy->calibration_wait_quarantined) {
                fprintf(stderr,
                        "RESETB discarded a still-owned calibration parent\n");
                return EXIT_FAILURE;
            }
            mismatched_phy->rfdc_transaction = NULL;
            if (ad9361_remove_ex(mismatched_phy, true, &consumed) < 0 ||
                !consumed) {
                fprintf(stderr,
                        "RESETB did not clear mismatched wait quarantine\n");
                return EXIT_FAILURE;
            }
        }
        register_map_enabled = false;
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
		{
			struct ad9361_bbf_calibration_job job = {0};
			struct ad9361_rf_bandwidth_transaction transaction = {0};
			unsigned writes_before = write_count;

			fake_regs[REG_STATE] = CALIBRATION_SEQUENCE_STATE(2);
			ret = ad9361_bbf_calibration_start(
				&phy, false, 10000000, 983040000, &job);
			if (ret != -EBUSY || write_count != writes_before ||
			    phy.calibration_state_unknown || job.active) {
				fprintf(stderr,
					"BBF calibration started while AD9361 sequencer was active: "
					"ret=%" PRId32 " writes=%u/%u unknown=%u\n",
					ret, write_count, writes_before,
					phy.calibration_state_unknown);
				return EXIT_FAILURE;
			}
			ret = ad9361_rf_bandwidth_transaction_start(
				&phy, 20000000, 20000000, &transaction);
			if (ret != -EBUSY || write_count != writes_before ||
			    phy.rf_bandwidth_transaction != NULL ||
			    phy.calibration_state_unknown) {
				fprintf(stderr,
					"Bandwidth transaction mutated RFIC while calibration sequencer was active: "
					"ret=%" PRId32 " writes=%u/%u\n",
					ret, write_count, writes_before);
				return EXIT_FAILURE;
			}
			memset(fake_regs, 0, sizeof(fake_regs));
		}
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

	/* The shared filter child performs at most one register read/write per
	 * poll, preserves TIA -> TX second filter -> ADC order, and keeps the
	 * enclosing bandwidth owner through all 40 ADC writes. */
	{
		struct ad9361_rf_bandwidth_transaction transaction = {0};
		struct ad9361_rf_bandwidth_transaction *saved_owner =
			phy.rf_bandwidth_transaction;
		struct ad9361_rf_bandwidth_filter_transaction *saved_filter_owner =
			phy.rf_bandwidth_filter_transaction;
		struct no_os_clk bbpll_clock = { .rate = 983040000 };
		struct refclk_scale adc_scale = {
			.spi = &spi, .phy = &phy, .source = ADC_CLK,
			.parent_source = BBPLL_CLK,
		};
		struct no_os_clk *saved_bbpll_clock = phy.clks[BBPLL_CLK];
		struct refclk_scale *saved_adc_scale = phy.ref_clk_scale[ADC_CLK];
		uint8_t saved_regs[sizeof(fake_regs)];
		uint32_t saved_rxbbf_div = phy.rxbbf_div;
		bool saved_manual_tx_quad_cal_en = phy.manual_tx_quad_cal_en;
		unsigned i, before;

		memcpy(saved_regs, fake_regs, sizeof(fake_regs));
		memset(fake_regs, 0, sizeof(fake_regs));
		fake_regs[REG_RX_BBF_C3_MSB] = 1;
		fake_regs[REG_RX_BBF_C3_LSB] = 0;
		fake_regs[REG_RX_BBF_R2346] = 1;
		register_map_enabled = true;
		phy.clks[BBPLL_CLK] = &bbpll_clock;
		phy.ref_clk_scale[ADC_CLK] = &adc_scale;
		fake_regs[REG_BBPLL] = 1;
		phy.rxbbf_div = 8;
		phy.manual_tx_quad_cal_en = true;
		transaction.phy = &phy;
		transaction.rx_bw = 10000000;
		transaction.tx_bw = 10000000;
		transaction.filter_job.bbpll_freq = 983040000;
		transaction.filter_job.adc_freq = 491520000;
		transaction.filter_job.adc_freq_valid = true;
		transaction.stage = AD9361_RF_BANDWIDTH_TRANSACTION_FILTER;
		transaction.filter_job.stage = AD9361_RF_BANDWIDTH_FILTER_TIA_READ;
		transaction.active = true;
		transaction.filter_job.active = true;
		transaction.filter_job.phy = &phy;
		phy.rf_bandwidth_transaction = &transaction;
		phy.rf_bandwidth_filter_transaction = &transaction.filter_job;

		/* Exercise the real uncached ADC_CLK path while the filter child owns
		 * the PHY. Its checked public accessor would reject its own owner; the
		 * owner-validated low-level scaler must perform one REG_BBPLL read. */
		transaction.filter_job.adc_freq_valid = false;
		before = read_count;
		if (ad9361_rf_bandwidth_transaction_poll(
			    &phy, &transaction) != 1 || read_count != before + 1 ||
		    transaction.filter_job.adc_freq != 491520000 ||
		    !transaction.filter_job.adc_freq_valid) {
			fprintf(stderr,
				"Owned ADC clock read failed or exceeded one SPI read per poll\n");
			return EXIT_FAILURE;
		}
		for (i = 0; i < 3; i++) {
			before = read_count;
			if (ad9361_rf_bandwidth_transaction_poll(
				    &phy, &transaction) != 1 || read_count != before + 1 ||
			    write_count != 0) {
				fprintf(stderr,
					"TIA retained read exceeded one SPI operation per poll\n");
				return EXIT_FAILURE;
			}
		}
		if (transaction.filter_job.stage != AD9361_RF_BANDWIDTH_FILTER_TIA_WRITE)
			return EXIT_FAILURE;
		for (i = 0; i < 5; i++) {
			before = write_count;
			if (ad9361_rf_bandwidth_transaction_poll(
				    &phy, &transaction) != 1 || write_count != before + 1) {
				fprintf(stderr,
					"TIA retained write exceeded one SPI operation per poll\n");
				return EXIT_FAILURE;
			}
		}
		if (transaction.filter_job.stage != AD9361_RF_BANDWIDTH_FILTER_TX_SECOND)
			return EXIT_FAILURE;
		for (i = 0; i < 3; i++) {
			before = write_count;
			if (ad9361_rf_bandwidth_transaction_poll(
				    &phy, &transaction) != 1 || write_count != before + 1) {
				fprintf(stderr,
					"TX second-filter step exceeded one SPI write per poll\n");
				return EXIT_FAILURE;
			}
		}
		for (i = 0; i < 3; i++) {
			before = read_count;
			if (ad9361_rf_bandwidth_transaction_poll(
				    &phy, &transaction) != 1 || read_count != before + 1) {
				fprintf(stderr,
					"ADC retained read exceeded one SPI operation per poll\n");
				return EXIT_FAILURE;
			}
		}
		if (transaction.filter_job.stage != AD9361_RF_BANDWIDTH_FILTER_ADC_WRITE)
			return EXIT_FAILURE;
		for (i = 0; i < NO_OS_ARRAY_SIZE(transaction.filter_job.adc_values); i++) {
			before = write_count;
			if (ad9361_rf_bandwidth_transaction_poll(
				    &phy, &transaction) != 1 || write_count != before + 1 ||
			    phy.rf_bandwidth_transaction != &transaction) {
				fprintf(stderr,
					"ADC retained write/owner invariant failed at index %u\n", i);
				return EXIT_FAILURE;
			}
		}
		if (transaction.stage != AD9361_RF_BANDWIDTH_TRANSACTION_RESTORE)
			return EXIT_FAILURE;
		phy.rf_bandwidth_transaction = saved_owner;
		phy.rf_bandwidth_filter_transaction = saved_filter_owner;
		phy.clks[BBPLL_CLK] = saved_bbpll_clock;
		phy.ref_clk_scale[ADC_CLK] = saved_adc_scale;
		phy.rxbbf_div = saved_rxbbf_div;
		phy.manual_tx_quad_cal_en = saved_manual_tx_quad_cal_en;
		memcpy(fake_regs, saved_regs, sizeof(fake_regs));
		register_map_enabled = false;
		read_count = write_count = 0;
	}

	/* TXQ temporary-bandwidth work must poll the filter-only child instead of
	 * entering the synchronous full-bandwidth helper. Inject an ADC write
	 * failure and prove one bus operation, terminal child retirement, and a
	 * fail-closed parent result. */
	{
		struct ad9361_tx_quad_calibration_job job = {0};
		struct ad9361_rf_bandwidth_filter_transaction *saved_filter_owner =
			phy.rf_bandwidth_filter_transaction;
		bool saved_unknown = phy.calibration_state_unknown;
		unsigned before = write_count;

		memset(&job.bandwidth_job, 0, sizeof(job.bandwidth_job));
		job.active = true;
		job.bandwidth_changed = true;
		job.stage = AD9361_TX_QUAD_JOB_BANDWIDTH_SETUP;
		job.bandwidth_job.phy = &phy;
		job.bandwidth_job.active = true;
		job.bandwidth_job.stage = AD9361_RF_BANDWIDTH_FILTER_ADC_WRITE;
		job.bandwidth_job.adc_index = 0;
		phy.rf_bandwidth_filter_transaction = &job.bandwidth_job;
		phy.calibration_state_unknown = false;
		register_map_enabled = true;
		fail_write_once_reg = 0x200;
		ret = ad9361_tx_quad_calibration_job_poll(&phy, &job);
		if (ret != 1 || write_count != before + 1 ||
		    job.stage != AD9361_TX_QUAD_JOB_CLEANUP || job.status >= 0 ||
		    !phy.calibration_state_unknown ||
		    phy.rf_bandwidth_filter_transaction != NULL) {
			fprintf(stderr,
				"TXQ filter child did not fail closed on one ADC write\\n");
			return EXIT_FAILURE;
		}
		ret = ad9361_tx_quad_calibration_job_poll(&phy, &job);
		if (ret >= 0 || !job.terminal) {
			fprintf(stderr, "TXQ filter child failure was not terminal\\n");
			return EXIT_FAILURE;
		}
		phy.rf_bandwidth_filter_transaction = saved_filter_owner;
		phy.calibration_state_unknown = saved_unknown;
		register_map_enabled = false;
		fail_write_once_reg = -1;
		read_count = write_count = 0;
	}

	/* Run the real TXQ job through temporary filter setup, one successful
	 * calibration attempt, and restoration of the caller's bandwidth pair. */
	{
		struct ad9361_tx_quad_calibration_job job = {0};
		struct no_os_clk bb_ref = { .rate = 40000000 };
		struct no_os_clk bbpll = { .rate = 960000000 };
		struct no_os_clk adc = { .rate = 240000000 };
		struct no_os_clk r1 = { .rate = 30720000 };
		struct no_os_clk clkrf = { .rate = 30720000 };
		struct no_os_clk t1 = { .rate = 30720000 };
		struct no_os_clk clktf = { .rate = 30720000 };
		struct refclk_scale bbpll_scale = {
			.spi = &spi, .phy = &phy, .source = BBPLL_CLK,
			.parent_source = BB_REFCLK,
		};
		struct refclk_scale adc_scale = {
			.spi = &spi, .phy = &phy, .source = ADC_CLK,
			.parent_source = BBPLL_CLK,
		};
		struct refclk_scale clkrf_scale = {
			.spi = &spi, .phy = &phy, .source = CLKRF_CLK,
			.parent_source = R1_CLK,
		};
		struct refclk_scale clktf_scale = {
			.spi = &spi, .phy = &phy, .source = CLKTF_CLK,
			.parent_source = T1_CLK,
		};
		struct no_os_clk *saved_clks[] = {
			phy.clks[BB_REFCLK], phy.clks[BBPLL_CLK], phy.clks[ADC_CLK],
			phy.clks[R1_CLK], phy.clks[CLKRF_CLK], phy.clks[T1_CLK],
			phy.clks[CLKTF_CLK],
		};
		struct refclk_scale *saved_scales[] = {
			phy.ref_clk_scale[BBPLL_CLK], phy.ref_clk_scale[ADC_CLK],
			phy.ref_clk_scale[CLKRF_CLK], phy.ref_clk_scale[CLKTF_CLK],
		};
		struct ad9361_calibration_wait *saved_calibration_wait =
			phy.calibration_wait;
		struct ad9361_rf_bandwidth_filter_transaction *saved_filter_owner =
			phy.rf_bandwidth_filter_transaction;
		int32_t (*saved_read_cal_done)(struct ad9361_rf_phy *, uint32_t,
					      bool *) = phy.read_cal_done;
		bool saved_auto_complete = fake_calibration_autocomplete;
		bool saved_unknown = phy.calibration_state_unknown;
		uint32_t saved_rxbbf_div = phy.rxbbf_div;
		uint8_t saved_last_phase = phy.last_tx_quad_cal_phase;
		uint8_t saved_regs[sizeof(fake_regs)];
		unsigned saved_fake_write_counts[NO_OS_ARRAY_SIZE(fake_write_counts)];
		struct ad9361_tx_quad_calibration_transaction *saved_txq_owner =
			phy.tx_quad_transaction;
		unsigned i;
		unsigned rx_mhz_write_base =
			fake_write_counts[REG_RX_BBBW_MHZ];
		bool saw_restore = false;
		bool saw_expansion_before_txq = false;
		bool saw_cal_done_before_restore = false;

		register_map_enabled = true;
		fake_calibration_autocomplete = true;
		memcpy(saved_regs, fake_regs, sizeof(fake_regs));
		memcpy(saved_fake_write_counts, fake_write_counts,
		       sizeof(fake_write_counts));
		memset(fake_regs, 0, sizeof(fake_regs));
		/* The TXQ job is a child of an ALERT-owning transaction. */
		fake_regs[REG_STATE] = ENSM_STATE_ALERT;
		fake_regs[REG_INTEGER_BB_FREQ_WORD] = 24;
		fake_regs[REG_BBPLL] = 2;
		fake_regs[REG_QUAD_CAL_STATUS_TX1] =
			TX1_LO_CONV | TX1_SSB_CONV;
		phy.clks[BB_REFCLK] = &bb_ref;
		phy.clks[BBPLL_CLK] = &bbpll;
		phy.clks[ADC_CLK] = &adc;
		phy.clks[R1_CLK] = &r1;
		phy.clks[CLKRF_CLK] = &clkrf;
		phy.clks[T1_CLK] = &t1;
		phy.clks[CLKTF_CLK] = &clktf;
		phy.ref_clk_scale[BBPLL_CLK] = &bbpll_scale;
		phy.ref_clk_scale[ADC_CLK] = &adc_scale;
		phy.ref_clk_scale[CLKRF_CLK] = &clkrf_scale;
		phy.ref_clk_scale[CLKTF_CLK] = &clktf_scale;
		phy.read_cal_done = fake_calibration_done;
		phy.calibration_state_unknown = false;
		ret = ad9361_tx_quad_calibration_job_start_with_restore_bw(
			&phy, 500000, 500000, -1, 5000000, 5000000, &job);
		if (ret != 0 || !job.bandwidth_changed ||
		    job.stage != AD9361_TX_QUAD_JOB_BANDWIDTH_SETUP ||
		    job.bandwidth_job.rx_bw != 7680000 ||
		    job.bandwidth_job.tx_bw != 7680000) {
			fprintf(stderr,
				"TXQ did not start expected temporary bandwidth: ret=%" PRId32
				" stage=%u temp=(%" PRIu32 ",%" PRIu32 ")\n",
				ret, job.stage, job.bandwidth_job.rx_bw,
				job.bandwidth_job.tx_bw);
			return EXIT_FAILURE;
		}
		for (i = 0; i < 512 && !job.terminal; i++) {
			ret = ad9361_tx_quad_calibration_job_poll(&phy, &job);
			if (ret < 0)
				break;
			if (job.stage == AD9361_TX_QUAD_JOB_INITIAL &&
			    phy.calibration_wait == &job.attempt.wait) {
				saw_expansion_before_txq =
					fake_regs[REG_RX_BBBW_MHZ] == 3 &&
					fake_regs[REG_RX_BBBW_KHZ] == 108;
			}
			if (job.stage == AD9361_TX_QUAD_JOB_BANDWIDTH_RESTORE) {
				saw_restore = true;
				saw_cal_done_before_restore =
					job.attempt.terminal &&
					job.attempt.terminal_status == 0 &&
					fake_regs[REG_RX_BBBW_MHZ] == 3 &&
					fake_regs[REG_RX_BBBW_KHZ] == 108;
				if (job.bandwidth_job.rx_bw != 5000000 ||
				    job.bandwidth_job.tx_bw != 5000000) {
					fprintf(stderr,
						"TXQ restore child lost caller bandwidth pair\n");
					return EXIT_FAILURE;
				}
			}
		}
		if (!job.terminal || ret != 0 || !saw_restore ||
		    !saw_expansion_before_txq || !saw_cal_done_before_restore ||
		    !job.bandwidth_restore_complete || job.status != 0 ||
		    phy.calibration_state_unknown ||
		    fake_write_counts[REG_RX_BBBW_MHZ] != rx_mhz_write_base + 2 ||
		    fake_regs[REG_RX_BBBW_MHZ] != 2 ||
		    fake_regs[REG_RX_BBBW_KHZ] != 64) {
			fprintf(stderr,
				"TXQ temporary bandwidth setup/restore failed: ret=%" PRId32
				" stage=%u terminal=%u restored=%u status=%" PRId32
				" unknown=%u polls=%u\n",
				ret, job.stage, job.terminal,
				job.bandwidth_restore_complete, job.status,
				phy.calibration_state_unknown, i);
			return EXIT_FAILURE;
		}
		phy.clks[BB_REFCLK] = saved_clks[0];
		phy.clks[BBPLL_CLK] = saved_clks[1];
		phy.clks[ADC_CLK] = saved_clks[2];
		phy.clks[R1_CLK] = saved_clks[3];
		phy.clks[CLKRF_CLK] = saved_clks[4];
		phy.clks[T1_CLK] = saved_clks[5];
		phy.clks[CLKTF_CLK] = saved_clks[6];
		phy.ref_clk_scale[BBPLL_CLK] = saved_scales[0];
		phy.ref_clk_scale[ADC_CLK] = saved_scales[1];
		phy.ref_clk_scale[CLKRF_CLK] = saved_scales[2];
		phy.ref_clk_scale[CLKTF_CLK] = saved_scales[3];
		phy.read_cal_done = saved_read_cal_done;
		phy.calibration_state_unknown = saved_unknown;
		phy.calibration_wait = saved_calibration_wait;
		phy.rf_bandwidth_filter_transaction = saved_filter_owner;
		phy.tx_quad_transaction = saved_txq_owner;
		phy.rxbbf_div = saved_rxbbf_div;
		phy.last_tx_quad_cal_phase = saved_last_phase;
		fake_calibration_autocomplete = saved_auto_complete;
		memcpy(fake_regs, saved_regs, sizeof(fake_regs));
		memcpy(fake_write_counts, saved_fake_write_counts,
		       sizeof(fake_write_counts));
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

    /* Gain-table loading is a retained row transaction: start is inert, each
     * poll is bus-bounded, and partial-row failure retires ownership and
     * invalidates the active table. */
    {
        struct ad9361_rf_phy gain_phy = phy;
        struct ad9361_gain_table_transaction gain_job = {0};
        unsigned polls = 0;
        unsigned writes_before;
        unsigned reads_before;
        int32_t poll_status = -EIO;

        register_map_enabled = true;
        fail_read_reg = -1;
        fail_write_reg = -1;
        gain_phy.gain_table_transaction = NULL;
        gain_phy.current_table = TBL_200_1300_MHZ;
        fake_regs[REG_RX1_MANUAL_LMT_FULL_GAIN] = 0;
        fake_regs[REG_RX2_MANUAL_LMT_FULL_GAIN] = 0;
        writes_before = write_count;
        reads_before = read_count;
        ret = ad9361_gain_table_transaction_start(
            &gain_phy, 1800000000ULL, GT_RX1 | GT_RX2, &gain_job);
        if (ret < 0 || write_count != writes_before ||
            read_count != reads_before ||
            gain_phy.gain_table_transaction != &gain_job ||
            !ad9361_operation_busy(&gain_phy)) {
            fprintf(stderr, "Gain-table start performed I/O or lost ownership\n");
            return EXIT_FAILURE;
        }
        do {
            unsigned poll_reads = read_count;
            unsigned poll_writes = write_count;
            uint32_t delay;

            poll_status = ad9361_gain_table_transaction_poll(
                &gain_phy, &gain_job);
            if (read_count - poll_reads > 1 ||
                write_count - poll_writes > 1) {
                fprintf(stderr,
                        "Gain-table poll exceeded one read/write helper\n");
                return EXIT_FAILURE;
            }
            delay = ad9361_gain_table_transaction_poll_delay_us(&gain_job);
            if (delay)
                no_os_udelay(delay);
            if (++polls > 1024) {
                fprintf(stderr, "Gain-table transaction did not terminate\n");
                return EXIT_FAILURE;
            }
        } while (poll_status == 1);
        if (poll_status != 0 || gain_phy.current_table !=
            TBL_1300_4000_MHZ || gain_phy.gain_table_transaction != NULL) {
            fprintf(stderr,
                    "Gain-table transaction failed or published wrong table\n");
            return EXIT_FAILURE;
        }

        gain_job = (struct ad9361_gain_table_transaction){0};
        gain_phy.current_table = TBL_200_1300_MHZ;
        fail_write_reg = REG_GAIN_TABLE_WRITE_DATA2;
        writes_before = write_count;
        ret = ad9361_gain_table_transaction_start(
            &gain_phy, 1800000000ULL, GT_RX1 | GT_RX2, &gain_job);
        while (ret >= 0 &&
               (poll_status = ad9361_gain_table_transaction_poll(
                    &gain_phy, &gain_job)) == 1)
            ret = 0;
        if (ret >= 0)
            ret = poll_status;
        if (ret != -EIO || gain_phy.current_table != NO_GAIN_TABLE ||
            gain_phy.gain_table_transaction != NULL ||
            write_count == writes_before) {
            fprintf(stderr,
                    "Gain-table partial write failure did not fail closed\n");
            return EXIT_FAILURE;
        }
        fail_write_reg = -1;
        gain_phy.calibration_state_unknown = false;
        register_map_enabled = false;
        read_count = write_count = 0;
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
            struct ad9361_rf_phy fir_fault_phy = {
                .spi = &spi,
                .pdata = &pdata,
            };
            int32_t cleanup_status = 123;
            unsigned writes_before = write_count;
            int32_t old_state_after_write = state_after_ensm_write;

            /* The ALERT write may reach the device even when SPI reports an
             * error. The driver then refuses speculative restore writes
             * because ENSM state is unknown; preserve that cleanup outcome
             * separately from the original transport error. */
            fake_regs[REG_STATE] = ENSM_STATE_RX;
            state_after_ensm_write = ENSM_STATE_RX;
            fail_write_once_reg = REG_ENSM_CONFIG_1;
            fail_write_once_reg_occurrence = 1;
            apply_failed_write_once = true;
            ret = ad9361_load_fir_filter_coef_with_cleanup(
                &fir_fault_phy, FIR_TX1, 0, 16, fir_coef,
                &cleanup_status);
            fail_write_once_reg = -1;
            state_after_ensm_write = old_state_after_write;
            if (ret != -EIO || cleanup_status != -EIO ||
                !fir_fault_phy.calibration_state_unknown ||
                write_count != writes_before + 1) {
                fprintf(stderr,
                        "FIR loader lost primary/cleanup ENSM errors: "
                        "ret=%" PRId32 " cleanup=%" PRId32
                        " unknown=%d writes=%u\n",
                        ret, cleanup_status,
                        fir_fault_phy.calibration_state_unknown,
                        write_count - writes_before);
                return EXIT_FAILURE;
            }
        }
        {
            struct ad9361_rf_phy fir_get_phy = {
                .spi = &spi,
                .pdata = &pdata,
            };
            AD9361_RXFIRConfig fir_cfg = { .rx_coef_size = 777 };
            uint32_t old_filter_conf = fake_regs[REG_RX_FILTER_CONFIG];
            unsigned writes_before =
                fake_write_counts[REG_RX_FILTER_CONFIG];
            int32_t cleanup_status = 123;

            fake_regs[REG_RX_FILTER_CONFIG] = 0;
            fail_write_once_reg = REG_RX_FILTER_CONFIG;
            fail_write_once_reg_occurrence = 1;
            apply_failed_write_once = false;
            ret = ad9361_get_rx_fir_config_with_cleanup(
                &fir_get_phy, 0, &fir_cfg, &cleanup_status);
            fail_write_once_reg = -1;
            if (ret != -EIO || cleanup_status != 0 ||
                !fir_get_phy.calibration_state_unknown ||
                fir_cfg.rx_coef_size != 777 ||
                fake_write_counts[REG_RX_FILTER_CONFIG] != writes_before + 1) {
                fprintf(stderr,
                        "RX FIR getter wrote after failed selection or changed output\n");
                return EXIT_FAILURE;
            }
            fake_regs[REG_RX_FILTER_CONFIG] = old_filter_conf;
        }
	{
		struct ad9361_rf_phy fir_get_phy = {
			.spi = &spi,
			.pdata = &pdata,
		};
		AD9361_TXFIRConfig fir_cfg = { .tx_coef_size = 777 };
		uint32_t old_filter_conf = fake_regs[REG_TX_FILTER_CONF];
		unsigned writes_before = fake_write_counts[REG_TX_FILTER_CONF];
		int32_t cleanup_status = 123;

		fake_regs[REG_TX_FILTER_CONF] = 0;
		fail_write_once_reg = REG_TX_FILTER_CONF;
		fail_write_once_reg_occurrence = 1;
		apply_failed_write_once = false;
		ret = ad9361_get_tx_fir_config_with_cleanup(
			&fir_get_phy, 0, &fir_cfg, &cleanup_status);
		fail_write_once_reg = -1;
		if (ret != -EIO || cleanup_status != 0 ||
		    !fir_get_phy.calibration_state_unknown ||
		    fir_cfg.tx_coef_size != 777 ||
		    fake_write_counts[REG_TX_FILTER_CONF] != writes_before + 1) {
			fprintf(stderr,
				"TX FIR getter wrote after failed selection or changed output\n");
			return EXIT_FAILURE;
		}
		fake_regs[REG_TX_FILTER_CONF] = old_filter_conf;
	}
        {
            struct ad9361_rf_phy fir_get_phy = {
                .spi = &spi,
                .pdata = &pdata,
            };
            AD9361_RXFIRConfig fir_cfg = { .rx_coef_size = 777 };
            uint32_t old_filter_conf = fake_regs[REG_RX_FILTER_CONFIG];
			uint32_t old_filter_gain = fake_regs[REG_RX_FILTER_GAIN];
            int32_t cleanup_status = 0;

            fake_regs[REG_RX_FILTER_CONFIG] = 0;
            fake_regs[REG_RX_FILTER_GAIN] = 0;
            fail_read_reg = REG_RX_FILTER_COEF_READ_DATA_1;
            fail_write_once_reg = REG_RX_FILTER_CONFIG;
            fail_write_once_reg_occurrence = 2;
            apply_failed_write_once = false;
            ret = ad9361_get_rx_fir_config_with_cleanup(
                &fir_get_phy, 0, &fir_cfg, &cleanup_status);
            fail_read_reg = -1;
            fail_write_once_reg = -1;
            if (ret != -EIO || cleanup_status != -EIO ||
                !fir_get_phy.calibration_state_unknown ||
                fir_cfg.rx_coef_size != 777) {
                fprintf(stderr,
                        "RX FIR getter lost primary/restore failure or changed output\n");
                return EXIT_FAILURE;
            }
            fake_regs[REG_RX_FILTER_CONFIG] = old_filter_conf;
			fake_regs[REG_RX_FILTER_GAIN] = old_filter_gain;
        }
	{
		struct ad9361_rf_phy fir_get_phy = {
			.spi = &spi,
			.pdata = &pdata,
		};
		AD9361_TXFIRConfig fir_cfg = { .tx_coef_size = 777 };
		uint32_t old_filter_conf = fake_regs[REG_TX_FILTER_CONF];
		int32_t cleanup_status = 0;

		fake_regs[REG_TX_FILTER_CONF] = 0;
		fail_read_reg = REG_TX_FILTER_COEF_READ_DATA_1;
		fail_write_once_reg = REG_TX_FILTER_CONF;
		fail_write_once_reg_occurrence = 2;
		apply_failed_write_once = false;
		ret = ad9361_get_tx_fir_config_with_cleanup(
			&fir_get_phy, 0, &fir_cfg, &cleanup_status);
		fail_read_reg = -1;
		fail_write_once_reg = -1;
		if (ret != -EIO || cleanup_status != -EIO ||
		    !fir_get_phy.calibration_state_unknown ||
		    fir_cfg.tx_coef_size != 777) {
			fprintf(stderr,
				"TX FIR getter lost primary/restore failure or changed output\n");
			return EXIT_FAILURE;
		}
		fake_regs[REG_TX_FILTER_CONF] = old_filter_conf;
	}
	{
		struct ad9361_rf_phy timing_phy = {
			.spi = &spi,
			.pdata = &pdata,
		};
		int32_t cleanup_status = 0;
		int32_t old_state_after_write = state_after_ensm_write;
		uint8_t old_ensm_state = fake_regs[REG_STATE];
		uint8_t old_rx_delay = fake_regs[REG_RX_CLOCK_DATA_DELAY];

		fake_regs[REG_STATE] = ENSM_STATE_RX;
		state_after_ensm_write = ENSM_STATE_ALERT;
		fail_write_once_reg = REG_ENSM_CONFIG_1;
		fail_write_once_reg_occurrence = 2;
		apply_failed_write_once = false;
		ret = ad9361_set_intf_delay(&timing_phy, false, 0, 0, true,
					    &cleanup_status);
		fail_write_once_reg = -1;
		state_after_ensm_write = old_state_after_write;
		if (ret != -EIO || cleanup_status != -EIO ||
		    !timing_phy.calibration_state_unknown) {
			fprintf(stderr,
				"Timing delay helper lost ENSM restore failure\n");
			return EXIT_FAILURE;
		}
		fake_regs[REG_STATE] = old_ensm_state;
		fake_regs[REG_RX_CLOCK_DATA_DELAY] = old_rx_delay;
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
            uint8_t old_prev_ensm_state = phy.prev_ensm_state;

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

            /* A failed status read during the mux operation is primary;
             * restoration still runs and its independent ENSM write failure
             * must remain visible through the cleanup output. */
            fake_regs[REG_STATE] = ENSM_STATE_RX;
            state_after_ensm_write = ENSM_STATE_ALERT;
            phy.prev_ensm_state = ENSM_STATE_RX;
            rx_mux.mult = INT_LO;
            fail_read_reg = REG_MULTICHIP_SYNC_AND_TX_MON_CTRL;
            fail_write_once_reg = REG_ENSM_CONFIG_1;
            fail_write_once_reg_occurrence = 2;
            apply_failed_write_once = false;
            {
                int32_t cleanup_status = 0;

                ret = ad9361_clk_mux_set_parent_with_cleanup(
                    &rx_mux, EXT_LO, &cleanup_status);
                fail_read_reg = -1;
                fail_write_once_reg = -1;
                if (ret != -EIO || cleanup_status != -EIO ||
                    !ad9361_calibration_state_unknown(&phy) ||
                    rx_mux.mult != INT_LO) {
                    fprintf(stderr,
                            "LO mux lost primary or ENSM cleanup failure\n");
                    return EXIT_FAILURE;
                }
            }
            phy.calibration_state_unknown = false;
            phy.rfpll_source_valid[0] = true;
            phy.rfpll_source_valid[1] = true;
            rx_mux.mult = INT_LO;
            fake_regs[REG_STATE] = old_ensm_state;
            state_after_ensm_write = old_state_after_ensm_write;
            phy.prev_ensm_state = old_prev_ensm_state;
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
        {
            uint8_t cursor = 0;
            ret = ad9361_setup_auxdac_step(&phy, &cursor);
        }
        if (ret != -EIO) {
            fprintf(stderr, "AuxDAC setup step hid enable write failure: ret=%" PRId32
                    "\n", ret);
            return EXIT_FAILURE;
        }

        fail_write_reg = REG_AUTO_GPO;
        {
            uint8_t cursor = 0;
            ret = ad9361_setup_gpo_step(&phy, &cursor);
        }
        if (ret != -EIO) {
            fprintf(stderr, "GPO setup step hid auto-mode write failure: ret=%" PRId32
                    "\n", ret);
            return EXIT_FAILURE;
        }

        fail_write_reg = REG_CTRL_OUTPUT_POINTER;
        {
            uint8_t cursor = 0;
            ret = ad9361_ctrl_outs_setup_step(&phy, &ctrl_outs, &cursor);
        }
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
        {
            uint8_t cursor = 0;
            ret = ad9361_setup_ext_lna_step(&phy, &elna, &cursor);
        }
        if (ret != -EIO) {
            fprintf(stderr, "External-LNA setup hid control write failure: ret=%" PRId32
                    "\n", ret);
            return EXIT_FAILURE;
        }

        /* The retained parallel-port cursor emits one ordered SPI helper per
         * poll, including both optional RX phase-inversion writes. */
        {
            struct ad9361_phy_platform_data pp_pdata = {0};
            struct ad9361_rf_phy pp_phy = {0};
            static const uint32_t expected_regs[] = {
                REG_PARALLEL_PORT_CONF_1, REG_PARALLEL_PORT_CONF_2,
                REG_PARALLEL_PORT_CONF_3, REG_RX_CLOCK_DATA_DELAY,
                REG_TX_CLOCK_DATA_DELAY, REG_LVDS_BIAS_CTRL,
                REG_DIGITAL_IO_CTRL, REG_DIGITAL_IO_CTRL,
                REG_DIGITAL_IO_CTRL, REG_DIGITAL_IO_CTRL,
                REG_LVDS_INVERT_CTRL1, REG_LVDS_INVERT_CTRL2,
                REG_PARALLEL_PORT_CONF_2, REG_INVERT_BITS,
            };
            uint8_t saved_regs[sizeof(fake_regs)];
            unsigned saved_counts[sizeof(fake_write_counts) /
                                  sizeof(fake_write_counts[0])];
            uint16_t saved_addr_log[sizeof(spi_write_addr_log) /
                                    sizeof(spi_write_addr_log[0])];
            uint8_t saved_value_log[sizeof(spi_write_value_log) /
                                    sizeof(spi_write_value_log[0])];
            unsigned saved_read_count = read_count;
            unsigned saved_write_count = write_count;
            uint16_t saved_last_write = last_spi_write_addr;
            uint16_t cursor = 0;
            unsigned polls = 0;
            unsigned i;

            memcpy(saved_regs, fake_regs, sizeof(fake_regs));
            memcpy(saved_counts, fake_write_counts, sizeof(fake_write_counts));
            memcpy(saved_addr_log, spi_write_addr_log,
                   sizeof(spi_write_addr_log));
            memcpy(saved_value_log, spi_write_value_log,
                   sizeof(spi_write_value_log));
            write_count = 0;
            pp_phy.spi = &spi;
            pp_phy.pdata = &pp_pdata;
            pp_pdata.rx1rx2_phase_inversion_en = true;

            do {
                unsigned reads_before = read_count;
                unsigned writes_before = write_count;

                ret = ad9361_pp_port_setup_step(&pp_phy, false, &cursor);
                if (ret < 0 || read_count - reads_before > 1 ||
                    write_count - writes_before > 1) {
                    fprintf(stderr,
                            "Parallel-port retained step exceeded its SPI bound\n");
                    return EXIT_FAILURE;
                }
                polls++;
            } while (ret == 1 && polls < 20);
            if (ret != 0 || polls != sizeof(expected_regs) /
                                      sizeof(expected_regs[0]) ||
                write_count != sizeof(expected_regs) /
                               sizeof(expected_regs[0])) {
                fprintf(stderr,
                        "Parallel-port retained cursor did not terminate at expected count\n");
                return EXIT_FAILURE;
            }
            for (i = 0; i < sizeof(expected_regs) /
                            sizeof(expected_regs[0]); ++i) {
                if (spi_write_addr_log[i] != expected_regs[i]) {
                    fprintf(stderr,
                            "Parallel-port retained register order changed at %u\n",
                            i);
                    return EXIT_FAILURE;
                }
            }
            memcpy(fake_regs, saved_regs, sizeof(fake_regs));
            memcpy(fake_write_counts, saved_counts, sizeof(fake_write_counts));
            memcpy(spi_write_addr_log, saved_addr_log,
                   sizeof(spi_write_addr_log));
            memcpy(spi_write_value_log, saved_value_log,
                   sizeof(spi_write_value_log));
            read_count = saved_read_count;
            write_count = saved_write_count;
            last_spi_write_addr = saved_last_write;
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
        if (ret != -EIO ||
            fake_regs[REG_SDM_CTRL_1] !=
                (INIT_BB_FO_CAL | BBPLL_RESET_BAR) ||
            fake_write_counts[REG_SDM_CTRL_1] != 1) {
            fprintf(stderr,
                    "BBPLL ambiguous start failure issued cleanup: ret=%" PRId32
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
            fake_write_counts[REG_SDM_CTRL_1] != 2) {
            fprintf(stderr,
                    "BBPLL failed calibration-clear write issued cleanup\n");
            return EXIT_FAILURE;
        }
        fail_write_once_reg = -1;
        fail_write_once_reg_occurrence = 1;
        apply_failed_write_once = false;
        register_map_enabled = false;
    }

    {
        struct ad9361_rf_phy bbpll_phy = { .spi = &spi };
        struct refclk_scale bbpll = { .spi = &spi, .phy = &bbpll_phy };
        uint8_t saved_fake_regs[sizeof(fake_regs)];
        bool saved_register_map_enabled = register_map_enabled;
        uint32_t recalculated_rate = 0;
        const uint32_t requested_rate = 737280000;
        const uint32_t reference_rate = 38400000;

        memcpy(saved_fake_regs, fake_regs, sizeof(fake_regs));
        register_map_enabled = true;
        memset(fake_regs, 0, sizeof(fake_regs));
        fake_regs[REG_CH_1_OVERFLOW] = BBPLL_LOCK;
        ret = ad9361_bbpll_set_rate(&bbpll, requested_rate, reference_rate);
        if (ret != 0 ||
            fake_regs[REG_INTEGER_BB_FREQ_WORD] != 19 ||
            fake_regs[REG_FRACT_BB_FREQ_WORD_1] != 0x06 ||
            fake_regs[REG_FRACT_BB_FREQ_WORD_2] != 0x60 ||
            fake_regs[REG_FRACT_BB_FREQ_WORD_3] != 0x00) {
            fprintf(stderr,
                    "BBPLL fractional register order mismatch: ret=%" PRId32
                    " words=%02x:%02x:%02x integer=%u\n",
                    ret, fake_regs[REG_FRACT_BB_FREQ_WORD_1],
                    fake_regs[REG_FRACT_BB_FREQ_WORD_2],
                    fake_regs[REG_FRACT_BB_FREQ_WORD_3],
                    fake_regs[REG_INTEGER_BB_FREQ_WORD]);
            return EXIT_FAILURE;
        }

        ret = ad9361_bbpll_recalc_rate_checked(
            &bbpll, reference_rate, &recalculated_rate);
        if (ret != 0 || recalculated_rate != requested_rate) {
            fprintf(stderr,
                    "BBPLL fractional readback mismatch: ret=%" PRId32
                    " rate=%" PRIu32 " expected=%" PRIu32 "\n",
                    ret, recalculated_rate, requested_rate);
            return EXIT_FAILURE;
        }
        memcpy(fake_regs, saved_fake_regs, sizeof(fake_regs));
        register_map_enabled = saved_register_map_enabled;
    }

    /* The retained BBPLL transaction must not monopolize its caller with
     * the register-programming burst or the lock wait. One poll performs at
     * most one SPI register write/status observation and never sleeps. */
    {
        struct ad9361_rf_phy bbpll_phy = { .spi = &spi, .pdata = &pdata };
        struct no_os_clk refclk = { .rate = 40000000 };
        struct refclk_scale bbpll = { .spi = &spi, .phy = &bbpll_phy };
        struct ad9361_bbpll_rate_transaction transaction = {0};
        struct ad9361_sample_rate_transaction competing_rate_change = {0};
        unsigned previous_writes;
        unsigned previous_reads;
        unsigned previous_delays;
        unsigned polls = 0;

        bbpll_phy.clk_refin = &refclk;
        register_map_enabled = true;
        memset(fake_regs, 0, sizeof(fake_regs));
        fake_regs[REG_CH_1_OVERFLOW] = BBPLL_LOCK;
        fake_time_us = 0;
        previous_writes = write_count;
        previous_delays = delay_count;
        ret = ad9361_bbpll_rate_transaction_start(
            &bbpll, 737280000, refclk.rate, &transaction);
        if (ret != 0 || !transaction.active || write_count != previous_writes) {
            fprintf(stderr,
                    "BBPLL transaction start performed hardware work: "
                    "ret=%" PRId32 " active=%d writes=%u->%u\n",
                    ret, transaction.active, previous_writes, write_count);
            return EXIT_FAILURE;
        }
        if (!ad9361_operation_busy(&bbpll_phy) ||
            ad9361_ensm_force_state_checked(&bbpll_phy, ENSM_STATE_ALERT) !=
                -EBUSY ||
            ad9361_sample_rate_transaction_start(
                &bbpll_phy, 30720000, false, &competing_rate_change) !=
                -EBUSY) {
            fprintf(stderr,
                    "BBPLL transaction did not reserve exclusive PHY ownership\n");
            return EXIT_FAILURE;
        }
        do {
            previous_writes = write_count;
            previous_reads = read_count;
            previous_delays = delay_count;
            ret = ad9361_bbpll_rate_transaction_poll(&bbpll_phy,
                                                       &transaction);
            polls++;
            if (write_count - previous_writes > 1 ||
                read_count - previous_reads > 1 ||
                delay_count != previous_delays) {
                fprintf(stderr,
                        "BBPLL transaction poll exceeded cooperative step: "
                        "writes=%u reads=%u delays=%u\n",
                        write_count - previous_writes,
                        read_count - previous_reads,
                        delay_count - previous_delays);
                return EXIT_FAILURE;
            }
        } while (ret == 1 && polls < 64);
        if (ret != 0 || transaction.active || !transaction.terminal ||
            transaction.terminal_status != 0 ||
            bbpll_phy.status_wait != NULL ||
            ad9361_operation_busy(&bbpll_phy) || polls < 15 ||
            fake_regs[REG_LOOP_FILTER_3] != 0x35 ||
            fake_regs[REG_LOOP_FILTER_3 - 1] != 0x5B ||
            fake_regs[REG_LOOP_FILTER_3 - 2] != 0xE8) {
            fprintf(stderr,
                    "BBPLL transaction sequence mismatch: ret=%" PRId32
                    " active=%d terminal=%d polls=%u filter=%02x:%02x:%02x\n",
                    ret, transaction.active, transaction.terminal, polls,
                    fake_regs[REG_LOOP_FILTER_3],
                    fake_regs[REG_LOOP_FILTER_3 - 1],
                    fake_regs[REG_LOOP_FILTER_3 - 2]);
            return EXIT_FAILURE;
        }

        /* A failed write before calibration launch must stop the retained
         * sequence, poison the PHY, and never issue later launch/cleanup I/O. */
        memset(&transaction, 0, sizeof(transaction));
        bbpll_phy.calibration_state_unknown = false;
        fake_write_counts[REG_SDM_CTRL_1] = 0;
        fail_write_reg = REG_INTEGER_BB_FREQ_WORD;
        if (ad9361_bbpll_rate_transaction_start(
                &bbpll, 737280000, refclk.rate, &transaction) != 0) {
            fprintf(stderr, "BBPLL fault-injection start rejected\n");
            return EXIT_FAILURE;
        }
        do {
            ret = ad9361_bbpll_rate_transaction_poll(&bbpll_phy,
                                                       &transaction);
            polls++;
        } while (ret == 1 && polls < 128);
        fail_write_reg = -1;
        if (ret != -EIO || transaction.active || !transaction.terminal ||
            !ad9361_calibration_state_unknown(&bbpll_phy) ||
            fake_write_counts[REG_SDM_CTRL_1] != 0 ||
            bbpll_phy.status_wait != NULL ||
            ad9361_operation_busy(&bbpll_phy)) {
            fprintf(stderr,
                    "BBPLL write failure did not stop/fail closed: "
                    "ret=%" PRId32 " active=%d terminal=%d poisoned=%d "
                    "launch_writes=%u\n", ret, transaction.active,
                    transaction.terminal,
                    ad9361_calibration_state_unknown(&bbpll_phy),
                    fake_write_counts[REG_SDM_CTRL_1]);
            return EXIT_FAILURE;
        }
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
            struct ad9361_setup_transaction setup_transaction = {0};

            pdata.rx_synth_freq = 18000000000ULL;
            pdata.tx_synth_freq = 1800000000ULL;
            ret = ad9361_setup_transaction_start(
                &phy, &setup_transaction);
            if (ret != -EINVAL || write_count != writes_before ||
                setup_transaction.active ||
                setup_transaction.terminal_status != -EINVAL) {
                fprintf(stderr,
                        "Retained setup accepted invalid RX startup LO: "
                        "ret=%" PRId32 " active=%d terminal=%" PRId32
                        " writes=%u->%u\n",
                        ret, setup_transaction.active,
                        setup_transaction.terminal_status,
                        writes_before, write_count);
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

        /* Retained setup start performs validation and captures state only;
         * the first RFIC mutation belongs to the first poll. */
        {
            struct ad9361_setup_transaction setup_transaction = {0};
            unsigned writes_before = write_count;
            unsigned reads_before = read_count;

            ret = ad9361_setup_transaction_start(&phy, &setup_transaction);
            if (ret != 0 || !setup_transaction.active ||
                setup_transaction.terminal ||
                setup_transaction.stage != AD9361_SETUP_TRANSACTION_PRE_AUXDAC ||
                phy.setup_transaction != &setup_transaction ||
                write_count != writes_before || read_count != reads_before) {
                fprintf(stderr,
                        "Retained setup start performed I/O or failed to reserve PHY: "
                        "ret=%" PRId32 " stage=%u reads=%u->%u writes=%u->%u\n",
                        ret, setup_transaction.stage, reads_before, read_count,
                        writes_before, write_count);
                return EXIT_FAILURE;
            }
            /* This test intentionally stops before poll, so retire only the
             * software reservation; no hardware state has changed. */
            phy.setup_transaction = NULL;
            setup_transaction.active = false;
        }

        /* A failure in the first retained mutation must preserve its errno,
         * poison the partially programmed PHY, and retire the owner. */
        {
            struct ad9361_phy_platform_data fault_pdata = {0};
            struct ad9361_rf_phy fault_phy = {0};
            struct ad9361_setup_transaction setup_transaction = {0};
            int32_t expected_error = -EIO;

            fault_pdata.rx_synth_freq = 1800000000ULL;
            fault_pdata.tx_synth_freq = 1800000000ULL;
            fault_phy.spi = &spi;
            fault_phy.pdata = &fault_pdata;
            fault_phy.clk_refin = &clock_nodes[BB_REFCLK];
            fault_phy.clks[BB_REFCLK] = &clock_nodes[BB_REFCLK];
            fault_phy.ref_clk_scale[BB_REFCLK] =
                &clock_scales[BB_REFCLK];
            if (ad9361_setup_transaction_start(
                    &fault_phy, &setup_transaction) != 0) {
                fprintf(stderr, "Setup fault-injection transaction did not start\n");
                return EXIT_FAILURE;
            }
            fail_write_once_reg = REG_AUXDAC_ENABLE_CTRL;
            fail_write_once_reg_occurrence = 1;
            ret = ad9361_setup_transaction_poll(&fault_phy,
                                                &setup_transaction);
            fail_write_once_reg = -1;
            if (ret != expected_error || !setup_transaction.terminal ||
                setup_transaction.terminal_status != expected_error ||
                fault_phy.setup_transaction != NULL ||
                !ad9361_calibration_state_unknown(&fault_phy)) {
                fprintf(stderr,
                        "Setup prefix failure lost errno/poison/retirement: "
                        "ret=%" PRId32 " terminal=%" PRId32 " unknown=%d\n",
                        ret, setup_transaction.terminal_status,
                        ad9361_calibration_state_unknown(&fault_phy));
                return EXIT_FAILURE;
            }
        }

        /* Drive only the retained pre-clock prefix. Every poll must stay
         * within one complete register helper before ownership reaches the
         * clock-chain child. */
        {
            struct ad9361_phy_platform_data prefix_pdata = pdata;
            struct ad9361_rf_phy prefix_phy = {0};
            struct ad9361_setup_transaction setup_transaction = {0};
            struct no_os_clk prefix_bb_ref = clock_nodes[BB_REFCLK];
            struct refclk_scale prefix_bb_scale =
                clock_scales[BB_REFCLK];
            static const uint32_t prefix_registers[] = {
                REG_AUXDAC_ENABLE_CTRL, REG_AUXDAC_1_WORD,
                REG_AUXDAC_1_CONFIG, REG_AUXDAC_ENABLE_CTRL,
                REG_AUXDAC_2_WORD, REG_AUXDAC_2_CONFIG,
                REG_AUXDAC_ENABLE_CTRL, REG_EXTERNAL_LNA_CTRL,
                REG_AUXDAC1_RX_DELAY, REG_AUXDAC1_TX_DELAY,
                REG_AUXDAC2_RX_DELAY, REG_AUXDAC2_TX_DELAY,
                REG_AUTO_GPO, REG_GPO_FORCE_AND_INIT,
                REG_GPO0_RX_DELAY, REG_GPO0_TX_DELAY,
                REG_GPO1_RX_DELAY, REG_GPO1_TX_DELAY,
                REG_GPO2_RX_DELAY, REG_GPO2_TX_DELAY,
                REG_GPO3_RX_DELAY, REG_GPO3_TX_DELAY,
                REG_EXTERNAL_LNA_CTRL,
            };
            static const uint32_t reference_registers[] = {
                REG_CTRL, REG_BANDGAP_CONFIG0, REG_BANDGAP_CONFIG1,
                REG_DCXO_COARSE_TUNE, REG_DCXO_FINE_TUNE_LOW,
                REG_DCXO_FINE_TUNE_HIGH, REG_REF_DIVIDE_CONFIG_1,
                REG_REF_DIVIDE_CONFIG_2, REG_REF_DIVIDE_CONFIG_2,
                REG_REF_DIVIDE_CONFIG_2, REG_CLOCK_ENABLE,
            };
            uint8_t saved_fake_regs[sizeof(fake_regs)];
            unsigned saved_fake_write_counts[sizeof(fake_write_counts) /
                                             sizeof(fake_write_counts[0])];
            uint16_t saved_write_addr_log[sizeof(spi_write_addr_log) /
                                          sizeof(spi_write_addr_log[0])];
            uint8_t saved_write_value_log[sizeof(spi_write_value_log) /
                                          sizeof(spi_write_value_log[0])];
            unsigned polls = 0;
            unsigned saved_write_count = write_count;
            unsigned saved_read_count = read_count;
            unsigned writes_before;
            uint16_t saved_last_write_addr = last_spi_write_addr;
            unsigned i;

            memcpy(saved_fake_regs, fake_regs, sizeof(fake_regs));
            memcpy(saved_fake_write_counts, fake_write_counts,
                   sizeof(fake_write_counts));
            memcpy(saved_write_addr_log, spi_write_addr_log,
                   sizeof(spi_write_addr_log));
            memcpy(saved_write_value_log, spi_write_value_log,
                   sizeof(spi_write_value_log));
            write_count = 0; /* Keep the bounded trace inside the log array. */
            writes_before = write_count;
            prefix_phy.spi = &spi;
            prefix_phy.pdata = &prefix_pdata;
            prefix_phy.clk_refin = &prefix_bb_ref;
            prefix_phy.clks[BB_REFCLK] = &prefix_bb_ref;
            prefix_phy.ref_clk_scale[BB_REFCLK] = &prefix_bb_scale;
            prefix_bb_scale.phy = &prefix_phy;
            prefix_bb_scale.spi = &spi;
            prefix_bb_scale.source = BB_REFCLK;
            prefix_bb_scale.parent_source = EXT_REF_CLK;

            ret = ad9361_setup_transaction_start(&prefix_phy,
                                                 &setup_transaction);
            if (ret != 0)
                return EXIT_FAILURE;
            while (setup_transaction.stage !=
                       AD9361_SETUP_TRANSACTION_CLOCK_CHAIN_START &&
                   polls++ < 128) {
                unsigned reads_before = read_count;
                unsigned writes_before = write_count;

                ret = ad9361_setup_transaction_poll(&prefix_phy,
                                                    &setup_transaction);
                if (ret != 1 || read_count - reads_before > 1 ||
                    write_count - writes_before > 1) {
                    fprintf(stderr,
                            "Retained pre-clock setup exceeded one register helper per poll: "
                            "ret=%" PRId32 " reads=%u writes=%u stage=%u\n",
                            ret, read_count - reads_before,
                            write_count - writes_before,
                            setup_transaction.stage);
                    return EXIT_FAILURE;
                }
            }
            if (setup_transaction.stage !=
                    AD9361_SETUP_TRANSACTION_CLOCK_CHAIN_START ||
                polls >= 128) {
                fprintf(stderr,
                        "Retained pre-clock setup failed to reach clock-chain phase\n");
                return EXIT_FAILURE;
            }
            for (i = 0; i < sizeof(prefix_registers) /
                            sizeof(prefix_registers[0]); ++i) {
                if (spi_write_addr_log[writes_before + i] !=
                    prefix_registers[i]) {
                    fprintf(stderr,
                            "AuxDAC/GPO retained prefix order changed at write %u\n",
                            i);
                    return EXIT_FAILURE;
                }
            }
            for (i = 0; i < sizeof(reference_registers) /
                            sizeof(reference_registers[0]); ++i) {
                if (spi_write_addr_log[writes_before +
                        sizeof(prefix_registers) /
                        sizeof(prefix_registers[0]) + i] !=
                    reference_registers[i]) {
                    fprintf(stderr,
                            "Analog/reference retained prefix order changed at step %u\n",
                            i);
                    return EXIT_FAILURE;
                }
            }
            i = writes_before +
                sizeof(prefix_registers) / sizeof(prefix_registers[0]) +
                sizeof(reference_registers) / sizeof(reference_registers[0]);
            if (spi_write_addr_log[i] == REG_CLOCK_CTRL)
                ++i;
            if (spi_write_addr_log[i] != REG_FRACT_BB_FREQ_WORD_2 ||
                spi_write_addr_log[i + 1] != REG_FRACT_BB_FREQ_WORD_3) {
                fprintf(stderr, "BBPLL seed writes moved in retained prefix\n");
                return EXIT_FAILURE;
            }
            /* The fake device has no reset/recovery behavior here. This test
             * owns the synthetic PHY, so release its software reservation. */
            prefix_phy.setup_transaction = NULL;
            setup_transaction.active = false;
            memcpy(fake_regs, saved_fake_regs, sizeof(fake_regs));
            memcpy(fake_write_counts, saved_fake_write_counts,
                   sizeof(fake_write_counts));
            memcpy(spi_write_addr_log, saved_write_addr_log,
                   sizeof(spi_write_addr_log));
            memcpy(spi_write_value_log, saved_write_value_log,
                   sizeof(spi_write_value_log));
            write_count = saved_write_count;
            read_count = saved_read_count;
            last_spi_write_addr = saved_last_write_addr;
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

        /* The clock-framework transaction must retain the low-level BBPLL
         * owner through lock, reject stale rate reads while pending, then
         * publish the divider-readback cache only at terminal success. */
        {
            struct ad9361_bbpll_clock_transaction transaction = {0};
            unsigned int polls = 0;

            register_map_enabled = true;
            fake_regs[REG_CH_1_OVERFLOW] = BBPLL_LOCK;
            fake_time_us = 0;
            ret = ad9361_bbpll_clock_transaction_start(
                &phy, bbpll, 1000000000, &transaction);
            if (ret != 0 || !transaction.active ||
                !ad9361_operation_busy(&phy) ||
                clock_nodes[BBPLL_CLK].rate != 0 ||
                clock_nodes[ADC_CLK].rate != 0 ||
                phy.bbpll_initialized) {
                fprintf(stderr,
                        "BBPLL clock transaction did not close cached tree: "
                        "ret=%" PRId32 " active=%d busy=%d bbpll=%" PRIu32
                        " adc=%" PRIu32 " initialized=%d\n",
                        ret, transaction.active, ad9361_operation_busy(&phy),
                        clock_nodes[BBPLL_CLK].rate,
                        clock_nodes[ADC_CLK].rate, phy.bbpll_initialized);
                return EXIT_FAILURE;
            }
            if (clk_get_rate_checked(&phy, bbpll, &clock_nodes[BBPLL_CLK]) !=
                    -EBUSY ||
                ad9361_bbpll_clock_transaction_start(
                    &phy, bbpll, 983040000, &transaction) != -EBUSY) {
                fprintf(stderr,
                        "BBPLL pending owner allowed read/replacement request\n");
                return EXIT_FAILURE;
            }

            do {
                unsigned writes_before = write_count;
                unsigned reads_before = read_count;
                unsigned delays_before = delay_count;

                ret = ad9361_bbpll_clock_transaction_poll(&phy,
                                                           &transaction);
                polls++;
                if (write_count - writes_before > 1 ||
                    read_count - reads_before > 1 ||
                    delay_count != delays_before) {
                    fprintf(stderr,
                            "BBPLL clock poll exceeded one-bus-op slice: "
                            "writes=%u reads=%u delays=%u\n",
                            write_count - writes_before,
                            read_count - reads_before,
                            delay_count - delays_before);
                    return EXIT_FAILURE;
                }
                if (transaction.verify_pending &&
                    !ad9361_operation_busy(&phy)) {
                    fprintf(stderr,
                            "BBPLL owner released before divider readback\n");
                    return EXIT_FAILURE;
                }
            } while (ret == 1 && polls < 64);

            if (ret != 0 || !transaction.terminal || transaction.active ||
                ad9361_operation_busy(&phy) ||
                clock_nodes[BBPLL_CLK].rate != 1000000000 ||
                !phy.bbpll_initialized || polls < 15) {
                fprintf(stderr,
                        "BBPLL clock transaction did not commit readback: "
                        "ret=%" PRId32 " terminal=%d active=%d polls=%u "
                        "rate=%" PRIu32 " initialized=%d\n",
                        ret, transaction.terminal, transaction.active, polls,
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

    /* Exercise the real ADI sample-rate transaction state machine while
     * isolating clock programming and BBF/TXQ completion behind deterministic
     * fakes. This covers preflight preservation, successful commit, verified
     * rollback, and a rollback clock readback mismatch. */
    {
        struct ad9361_rf_phy sample_phy = {
            .spi = &spi,
            .pdata = &pdata,
            .current_bw_valid = true,
            .current_rx_bw_Hz = 18000000,
            .current_tx_bw_Hz = 18000000,
        };
        struct no_os_clk sample_clkrf = { .rate = 30720000 };
        struct refclk_scale sample_rate_scale = {
            .spi = &spi,
            .phy = &sample_phy,
            .source = RX_SAMPL_CLK,
            .parent_source = CLKRF_CLK,
        };
        struct refclk_scale clkrf_scale = {
            .spi = &spi,
            .phy = &sample_phy,
            .source = CLKRF_CLK,
            .parent_source = R1_CLK,
        };
        struct ad9361_sample_rate_transaction transaction = {0};
        struct ad9361_sample_rate_transaction competing_transaction = {0};
        unsigned clock_sets_before;
        uint32_t checked_sample_rate;
        int32_t transaction_status;

        sample_phy.ref_clk_scale[RX_SAMPL_CLK] = &sample_rate_scale;
        sample_phy.ref_clk_scale[CLKRF_CLK] = &clkrf_scale;
        sample_phy.clks[CLKRF_CLK] = &sample_clkrf;
        memset(fake_sample_rate_rx_clks, 0,
               sizeof(fake_sample_rate_rx_clks));
        memset(fake_sample_rate_tx_clks, 0,
               sizeof(fake_sample_rate_tx_clks));
        fake_sample_rate_rx_clks[BBPLL_FREQ] = 983040000;
        fake_sample_rate_rx_clks[ADC_FREQ] = 491520000;
        fake_sample_rate_rx_clks[RX_SAMPL_FREQ] = 30720000;
        fake_sample_rate_tx_clks[BBPLL_FREQ] = 983040000;
        fake_sample_rate_tx_clks[DAC_FREQ] = 491520000;
        fake_sample_rate_tx_clks[TX_SAMPL_FREQ] = 30720000;
        fake_sample_rate_clock_readback_status = 0;
        fake_sample_rate_calculate_status = 0;
        fake_sample_rate_clock_set_count = 0;
        fake_sample_rate_clock_set_attempt_count = 0;
        fake_sample_rate_clock_set_fail_attempt = 0;
        fake_sample_rate_bw_start_count = 0;
        fake_sample_rate_bw_poll_count = 0;
        fake_sample_rate_bw_fail_poll = 0;
        fake_sample_rate_corrupt_rollback_readback = false;
        fake_clock_chain_status = 0;
        fake_sample_rate_transaction_enabled = true;
        register_map_enabled = true;
        fake_regs[REG_RX_ENABLE_FILTER_CTRL] = 0;

        /* A rejected calculation occurs before any clock write and leaves
         * the old known-good bandwidth cache intact. */
        clock_sets_before = fake_sample_rate_clock_set_count;
        fake_sample_rate_calculate_status = -EINVAL;
        transaction_status = ad9361_sample_rate_transaction_start(
            &sample_phy, 15360000, false, &transaction);
        if (transaction_status != -EINVAL || transaction.active ||
            fake_sample_rate_clock_set_count != clock_sets_before ||
            !sample_phy.current_bw_valid) {
            fprintf(stderr,
                    "Sample-rate preflight mutated state: status=%" PRId32
                    " active=%d clock_sets=%u bw_valid=%d\n",
                    transaction_status, transaction.active,
                    fake_sample_rate_clock_set_count,
                    sample_phy.current_bw_valid);
            return EXIT_FAILURE;
        }

        /* Start accepts a retained clock child without touching hardware;
         * later polls complete clock programming then bandwidth/TXQ. */
        fake_sample_rate_calculate_status = 0;
        memset(&transaction, 0, sizeof(transaction));
        transaction_status = ad9361_sample_rate_transaction_start(
            &sample_phy, 15360000, false, &transaction);
        if (transaction_status != 0 || !transaction.active ||
            transaction.stage != AD9361_SAMPLE_RATE_TRANSACTION_NEW_CLOCK ||
            fake_sample_rate_clock_set_count != 0 ||
            fake_sample_rate_bw_start_count != 0 ||
            !ad9361_operation_busy(&sample_phy)) {
            fprintf(stderr,
                    "Sample-rate transaction failed to start: status=%" PRId32
                    " active=%d clock_sets=%u bw_starts=%u\n",
                    transaction_status, transaction.active,
                    fake_sample_rate_clock_set_count,
                    fake_sample_rate_bw_start_count);
            return EXIT_FAILURE;
        }
        transaction_status = ad9361_sample_rate_transaction_poll(
            &sample_phy, &transaction);
        if (transaction_status != 1 ||
            transaction.stage != AD9361_SAMPLE_RATE_TRANSACTION_NEW_BANDWIDTH ||
            sample_phy.sample_rate_transaction != NULL ||
            sample_phy.rf_bandwidth_transaction != &transaction.bandwidth_job ||
            !ad9361_operation_busy(&sample_phy) ||
            ad9361_sample_rate_transaction_start(
                &sample_phy, 30720000, false,
                &competing_transaction) != -EBUSY) {
            fprintf(stderr,
                    "Bandwidth child did not retain sample-rate exclusion\n");
            return EXIT_FAILURE;
        }
        transaction_status = test_sample_rate_poll_to_terminal(
            &sample_phy, &transaction);
        if (transaction_status != 0 || !transaction.terminal ||
            !sample_phy.current_bw_valid || transaction.rollback_complete ||
            ad9361_operation_busy(&sample_phy)) {
            fprintf(stderr,
                    "Sample-rate commit failed: status=%" PRId32
                    " terminal=%d bw_valid=%d rollback=%d\n",
                    transaction_status, transaction.terminal,
                    sample_phy.current_bw_valid,
                    transaction.rollback_complete);
            return EXIT_FAILURE;
        }

        /* A failed new bandwidth stage must restore and read back the old
         * clock plan before the second bandwidth job can validate rollback. */
        memset(&transaction, 0, sizeof(transaction));
        fake_sample_rate_clock_set_count = 0;
        fake_sample_rate_bw_start_count = 0;
        fake_sample_rate_bw_poll_count = 0;
        fake_sample_rate_bw_fail_poll = 1;
        transaction_status = ad9361_sample_rate_transaction_start(
            &sample_phy, 15360000, false, &transaction);
        if (transaction_status != 0 || !transaction.active) {
            fprintf(stderr, "Sample-rate rollback case did not start\n");
            return EXIT_FAILURE;
        }
        transaction_status = test_sample_rate_poll_until_stage(
            &sample_phy, &transaction,
            AD9361_SAMPLE_RATE_TRANSACTION_ROLLBACK_CLOCK);
        if (transaction_status != 1 ||
            transaction.stage != AD9361_SAMPLE_RATE_TRANSACTION_ROLLBACK_CLOCK ||
            sample_phy.sample_rate_transaction != &transaction ||
            !ad9361_operation_busy(&sample_phy) ||
            clk_get_rate_checked(&sample_phy, &sample_rate_scale,
                                 &checked_sample_rate) != -EBUSY ||
            ad9361_sample_rate_transaction_start(
                &sample_phy, 30720000, false,
                &competing_transaction) != -EBUSY) {
            fprintf(stderr,
                    "Sample-rate failure did not retain rollback ownership: "
                    "status=%" PRId32 " stage=%u owner=%d busy=%d\n",
                    transaction_status, transaction.stage,
                    sample_phy.sample_rate_transaction == &transaction,
                    ad9361_operation_busy(&sample_phy));
            return EXIT_FAILURE;
        }
        transaction_status = ad9361_sample_rate_transaction_poll(
            &sample_phy, &transaction);
        if (transaction_status != 1 ||
            !transaction.rollback_clock_started ||
            fake_sample_rate_clock_set_count != 1) {
            fprintf(stderr, "Sample-rate rollback clock child did not start\n");
            return EXIT_FAILURE;
        }
        transaction_status = ad9361_sample_rate_transaction_poll(
            &sample_phy, &transaction);
        if (transaction_status != 1 || fake_sample_rate_clock_set_count != 2 ||
            fake_sample_rate_bw_start_count != 2 ||
            !ad9361_operation_busy(&sample_phy)) {
            fprintf(stderr,
                    "Sample-rate rollback did not restore clocks/start BW: "
                    "status=%" PRId32 " clock_sets=%u bw_starts=%u\n",
                    transaction_status, fake_sample_rate_clock_set_count,
                    fake_sample_rate_bw_start_count);
            return EXIT_FAILURE;
        }
        transaction_status = ad9361_sample_rate_transaction_poll(
            &sample_phy, &transaction);
        if (transaction_status != -EIO || !transaction.rollback_complete ||
            !sample_phy.current_bw_valid ||
            fake_sample_rate_rx_clks[RX_SAMPL_FREQ] != 30720000 ||
            fake_sample_rate_tx_clks[TX_SAMPL_FREQ] != 30720000) {
            fprintf(stderr,
                    "Verified rollback outcome incorrect: status=%" PRId32
                    " rollback=%d bw_valid=%d rx_rate=%u tx_rate=%u\n",
                    transaction_status, transaction.rollback_complete,
                    sample_phy.current_bw_valid,
                    fake_sample_rate_rx_clks[RX_SAMPL_FREQ],
                    fake_sample_rate_tx_clks[TX_SAMPL_FREQ]);
            return EXIT_FAILURE;
        }
        {
            unsigned bw_polls = fake_sample_rate_bw_poll_count;
            unsigned clock_sets = fake_sample_rate_clock_set_count;
            unsigned bw_starts = fake_sample_rate_bw_start_count;

            if (ad9361_sample_rate_transaction_poll(
                    &sample_phy, &transaction) != -EIO ||
                fake_sample_rate_bw_poll_count != bw_polls ||
                fake_sample_rate_clock_set_count != clock_sets ||
                fake_sample_rate_bw_start_count != bw_starts) {
                fprintf(stderr,
                        "Terminal sample-rate poll repeated side effects\n");
                return EXIT_FAILURE;
            }
        }

        /* If the restored clock plan is confirmed but the old-bandwidth
         * recalibration fails, the original operation error is returned
         * while the rollback/cache remain explicitly incomplete. */
        memset(&transaction, 0, sizeof(transaction));
        fake_sample_rate_rx_clks[BBPLL_FREQ] = 983040000;
        fake_sample_rate_rx_clks[ADC_FREQ] = 491520000;
        fake_sample_rate_rx_clks[RX_SAMPL_FREQ] = 30720000;
        fake_sample_rate_tx_clks[BBPLL_FREQ] = 983040000;
        fake_sample_rate_tx_clks[DAC_FREQ] = 491520000;
        fake_sample_rate_tx_clks[TX_SAMPL_FREQ] = 30720000;
        fake_sample_rate_clock_set_count = 0;
        fake_sample_rate_bw_start_count = 0;
        fake_sample_rate_bw_poll_count = 0;
        fake_sample_rate_bw_fail_poll = 3;
        fake_sample_rate_corrupt_rollback_readback = false;
        sample_phy.calibration_state_unknown = false;
        sample_phy.current_bw_valid = true;
        transaction_status = ad9361_sample_rate_transaction_start(
            &sample_phy, 15360000, false, &transaction);
        if (transaction_status != 0) {
            fprintf(stderr, "Sample-rate rollback-bandwidth case did not start\n");
            return EXIT_FAILURE;
        }
        transaction_status = test_sample_rate_poll_to_terminal(
            &sample_phy, &transaction);
        if (transaction_status != -EIO || transaction.rollback_complete ||
            !transaction.terminal || sample_phy.current_bw_valid ||
            fake_sample_rate_bw_start_count != 2) {
            fprintf(stderr,
                    "Failed rollback bandwidth falsely committed: status=%" PRId32
                    " rollback=%d terminal=%d bw_valid=%d bw_starts=%u\n",
                    transaction_status, transaction.rollback_complete,
                    transaction.terminal, sample_phy.current_bw_valid,
                    fake_sample_rate_bw_start_count);
            return EXIT_FAILURE;
        }
        {
            unsigned bw_polls = fake_sample_rate_bw_poll_count;
            unsigned clock_sets = fake_sample_rate_clock_set_count;
            unsigned bw_starts = fake_sample_rate_bw_start_count;

            if (ad9361_sample_rate_transaction_poll(
                    &sample_phy, &transaction) != -EIO ||
                fake_sample_rate_bw_poll_count != bw_polls ||
                fake_sample_rate_clock_set_count != clock_sets ||
                fake_sample_rate_bw_start_count != bw_starts) {
                fprintf(stderr,
                        "Failed terminal sample-rate poll repeated side effects\n");
                return EXIT_FAILURE;
            }
        }

        /* A side-effect-free clock rollback rejection must short-circuit
         * before the old-bandwidth job starts. The observed clock plan stays
         * at the accepted new plan and cache admission remains closed. */
        memset(&transaction, 0, sizeof(transaction));
        fake_sample_rate_rx_clks[BBPLL_FREQ] = 983040000;
        fake_sample_rate_rx_clks[ADC_FREQ] = 491520000;
        fake_sample_rate_rx_clks[RX_SAMPL_FREQ] = 30720000;
        fake_sample_rate_tx_clks[BBPLL_FREQ] = 983040000;
        fake_sample_rate_tx_clks[DAC_FREQ] = 491520000;
        fake_sample_rate_tx_clks[TX_SAMPL_FREQ] = 30720000;
        fake_sample_rate_clock_set_count = 0;
        fake_sample_rate_clock_set_attempt_count = 0;
        fake_sample_rate_clock_set_fail_attempt = 2;
        fake_sample_rate_bw_start_count = 0;
        fake_sample_rate_bw_poll_count = 0;
        fake_sample_rate_bw_fail_poll = 1;
        fake_sample_rate_corrupt_rollback_readback = false;
        sample_phy.calibration_state_unknown = false;
        sample_phy.current_bw_valid = true;
        transaction_status = ad9361_sample_rate_transaction_start(
            &sample_phy, 15360000, false, &transaction);
        if (transaction_status != 0 ||
            test_sample_rate_poll_until_stage(
                &sample_phy, &transaction,
                AD9361_SAMPLE_RATE_TRANSACTION_ROLLBACK_CLOCK) != 1) {
            fprintf(stderr,
                    "Sample-rate clock-rollback fault did not reach rollback\n");
            return EXIT_FAILURE;
        }
        transaction_status = test_sample_rate_poll_to_terminal(
            &sample_phy, &transaction);
        if (transaction_status != -EIO || !transaction.terminal ||
            transaction.rollback_complete || sample_phy.current_bw_valid ||
            fake_sample_rate_clock_set_attempt_count != 2 ||
            fake_sample_rate_clock_set_count != 1 ||
            fake_sample_rate_bw_start_count != 1 ||
            fake_sample_rate_rx_clks[RX_SAMPL_FREQ] != 15360000) {
            fprintf(stderr,
                    "Clock rollback failure did not fail closed: status=%" PRId32
                    " terminal=%d rollback=%d bw_valid=%d attempts=%u"
                    " writes=%u bw_starts=%u observed_rate=%u\n",
                    transaction_status, transaction.terminal,
                    transaction.rollback_complete, sample_phy.current_bw_valid,
                    fake_sample_rate_clock_set_attempt_count,
                    fake_sample_rate_clock_set_count,
                    fake_sample_rate_bw_start_count,
                    fake_sample_rate_rx_clks[RX_SAMPL_FREQ]);
            return EXIT_FAILURE;
        }
        fake_sample_rate_clock_set_fail_attempt = 0;

        /* A failed SPI-backed clock readback after the rollback write is not
         * a preflight rejection: the restored plan cannot be certified, so
         * poison state and do not launch bandwidth restoration. */
        memset(&transaction, 0, sizeof(transaction));
        fake_sample_rate_rx_clks[BBPLL_FREQ] = 983040000;
        fake_sample_rate_rx_clks[ADC_FREQ] = 491520000;
        fake_sample_rate_rx_clks[RX_SAMPL_FREQ] = 30720000;
        fake_sample_rate_tx_clks[BBPLL_FREQ] = 983040000;
        fake_sample_rate_tx_clks[DAC_FREQ] = 491520000;
        fake_sample_rate_tx_clks[TX_SAMPL_FREQ] = 30720000;
        fake_sample_rate_clock_set_count = 0;
        fake_sample_rate_clock_set_attempt_count = 0;
        fake_sample_rate_bw_start_count = 0;
        fake_sample_rate_bw_poll_count = 0;
        fake_sample_rate_bw_fail_poll = 1;
        fake_sample_rate_clock_readback_status = 0;
        fake_sample_rate_corrupt_rollback_readback = false;
        sample_phy.calibration_state_unknown = false;
        sample_phy.current_bw_valid = true;
        transaction_status = ad9361_sample_rate_transaction_start(
            &sample_phy, 15360000, false, &transaction);
        if (transaction_status != 0 ||
            test_sample_rate_poll_until_stage(
                &sample_phy, &transaction,
                AD9361_SAMPLE_RATE_TRANSACTION_ROLLBACK_CLOCK) != 1) {
            fprintf(stderr,
                    "Sample-rate readback-transport case did not reach rollback\n");
            return EXIT_FAILURE;
        }
        fake_sample_rate_clock_readback_status = -EIO;
        transaction_status = test_sample_rate_poll_to_terminal(
            &sample_phy, &transaction);
        if (transaction_status != -EIO || !transaction.terminal ||
            transaction.rollback_complete ||
            !ad9361_calibration_state_unknown(&sample_phy) ||
            sample_phy.current_bw_valid ||
            fake_sample_rate_bw_start_count != 1) {
            fprintf(stderr,
                    "Clock readback transport failure was not fenced: "
                    "status=%" PRId32 " terminal=%d rollback=%d unknown=%d"
                    " bw_valid=%d bw_starts=%u\n",
                    transaction_status, transaction.terminal,
                    transaction.rollback_complete,
                    ad9361_calibration_state_unknown(&sample_phy),
                    sample_phy.current_bw_valid,
                    fake_sample_rate_bw_start_count);
            return EXIT_FAILURE;
        }
        fake_sample_rate_clock_readback_status = 0;

        /* Successful SPI writes do not authorize cache restoration if the
         * clock-chain readback contradicts the saved plan. */
        memset(&transaction, 0, sizeof(transaction));
        fake_sample_rate_clock_set_count = 0;
        fake_sample_rate_bw_start_count = 0;
        fake_sample_rate_bw_poll_count = 0;
        fake_sample_rate_bw_fail_poll = 1;
        fake_sample_rate_corrupt_rollback_readback = true;
        sample_phy.calibration_state_unknown = false;
        sample_phy.current_bw_valid = true;
        transaction_status = ad9361_sample_rate_transaction_start(
            &sample_phy, 15360000, false, &transaction);
        if (transaction_status != 0 ||
            test_sample_rate_poll_until_stage(
                &sample_phy, &transaction,
                AD9361_SAMPLE_RATE_TRANSACTION_ROLLBACK_CLOCK) != 1) {
            fprintf(stderr, "Sample-rate readback-mismatch case did not start\n");
            return EXIT_FAILURE;
        }
        transaction_status = test_sample_rate_poll_to_terminal(
            &sample_phy, &transaction);
        if (transaction_status != -EIO || transaction.rollback_complete ||
            !ad9361_calibration_state_unknown(&sample_phy) ||
            sample_phy.current_bw_valid || fake_sample_rate_bw_start_count != 1) {
            fprintf(stderr,
                    "Clock readback mismatch falsely restored cache: status=%" PRId32
                    " rollback=%d unknown=%d bw_valid=%d bw_starts=%u\n",
                    transaction_status, transaction.rollback_complete,
                    ad9361_calibration_state_unknown(&sample_phy),
                    sample_phy.current_bw_valid,
                    fake_sample_rate_bw_start_count);
            return EXIT_FAILURE;
        }

        /* The synchronous clock-plan wrapper must retain the operation
         * failure when old-bandwidth restoration also fails, and return the
         * latter through its cleanup channel. */
        {
            uint32_t requested_rx[NUM_RX_CLOCKS];
            uint32_t requested_tx[NUM_TX_CLOCKS];
            int32_t cleanup_status = 0;

            fake_sample_rate_rx_clks[BBPLL_FREQ] = 983040000;
            fake_sample_rate_rx_clks[ADC_FREQ] = 491520000;
            fake_sample_rate_rx_clks[RX_SAMPL_FREQ] = 30720000;
            fake_sample_rate_tx_clks[BBPLL_FREQ] = 983040000;
            fake_sample_rate_tx_clks[DAC_FREQ] = 491520000;
            fake_sample_rate_tx_clks[TX_SAMPL_FREQ] = 30720000;
            memcpy(requested_rx, fake_sample_rate_rx_clks,
                   sizeof(requested_rx));
            memcpy(requested_tx, fake_sample_rate_tx_clks,
                   sizeof(requested_tx));
            requested_rx[RX_SAMPL_FREQ] = 15360000;
            requested_tx[TX_SAMPL_FREQ] = 15360000;
            sample_phy.calibration_state_unknown = false;
            sample_phy.current_bw_valid = true;
            sample_phy.current_rx_bw_Hz = 18000000;
            sample_phy.current_tx_bw_Hz = 18000000;
            fake_sample_rate_clock_set_count = 0;
            fake_sample_rate_clock_set_attempt_count = 0;
            fake_sample_rate_clock_set_fail_attempt = 0;
            fake_clock_chain_status = 0;
            fake_clock_chain_cleanup_status = 0;
            fake_bandwidth_update_count = 0;
            fake_bandwidth_update_fail_call = 1;
            fake_bandwidth_update_fail_status = -ETIMEDOUT;
            fake_bandwidth_cleanup_fail_call = 2;
            fake_bandwidth_cleanup_fail_status = -EACCES;

            ret = ad9361_set_trx_path_clks_with_cleanup(
                &sample_phy, requested_rx, requested_tx, &cleanup_status);
            if (ret != -ETIMEDOUT || cleanup_status != -EACCES ||
                !ad9361_calibration_state_unknown(&sample_phy) ||
                sample_phy.current_bw_valid ||
                fake_sample_rate_clock_set_count != 2 ||
                fake_bandwidth_update_count != 2 ||
                fake_sample_rate_rx_clks[RX_SAMPL_FREQ] != 30720000 ||
                fake_sample_rate_tx_clks[TX_SAMPL_FREQ] != 30720000) {
                fprintf(stderr,
                        "Clock-plan rollback lost primary/cleanup outcome: "
                        "ret=%" PRId32 " cleanup=%" PRId32
                        " unknown=%d clock_sets=%u bw_updates=%u\n",
                        ret, cleanup_status,
                        ad9361_calibration_state_unknown(&sample_phy),
                        fake_sample_rate_clock_set_count,
                        fake_bandwidth_update_count);
                return EXIT_FAILURE;
            }
            fake_bandwidth_update_fail_call = 0;
            fake_bandwidth_cleanup_fail_call = 0;
        }

        fake_sample_rate_transaction_enabled = false;
        fake_sample_rate_corrupt_rollback_readback = false;
        fake_sample_rate_bw_fail_poll = 0;
        register_map_enabled = false;
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
            bool saved_register_map_enabled = register_map_enabled;

            /* The fallback path reads the profile bank through SPI; keep the
             * fake register map enabled so it observes the simulated empty
             * bank rather than the generic SPI status byte. */
            register_map_enabled = true;
            fake_regs[REG_RX_FAST_LOCK_PROGRAM_READ] = 0;
            ad9361_fastlock_invalidate_cache(&phy, false);
            frequency = UINT64_C(0xfeedface);
            ret = ad9361_fastlock_sync_current_lo(
                &phy, false, 0, 0, &frequency);
            register_map_enabled = saved_register_map_enabled;
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
		{
			struct ad9361_clock_chain_transaction clock_job = {0};
			unsigned int writes_at_start = write_count;
			unsigned int polls = 0;

			clock_job.phy = &agc_phy;
			clock_job.active = true;
			clock_job.stage = AD9361_CLOCK_CHAIN_TRANSACTION_POST_CHANGE;
			clock_job.post_change_stage =
				AD9361_CLOCK_CHAIN_POST_CHANGE_GC_UPDATE;
			agc_phy.clock_chain_transaction = &clock_job;
			do {
				unsigned int writes_before_step = write_count;

				ret = ad9361_clock_chain_transaction_poll(
					&agc_phy, &clock_job);
				polls++;
				if (ret != 1 || write_count - writes_before_step > 1 ||
				    !ad9361_operation_busy(&agc_phy) || polls > 16) {
					fprintf(stderr,
						"Clock-chain post-change GC did not yield per register\n");
					return EXIT_FAILURE;
				}
			} while (clock_job.post_change_stage ==
				 AD9361_CLOCK_CHAIN_POST_CHANGE_GC_UPDATE);
			if (clock_job.post_change_stage !=
				AD9361_CLOCK_CHAIN_POST_CHANGE_RSSI ||
			    clock_job.post_change_cursor != 0 ||
			    write_count - writes_at_start != 8 ||
			    polls != 9) {
				fprintf(stderr,
					"Clock-chain GC phase lost writes/cursor state\n");
				return EXIT_FAILURE;
			}
			agc_phy.clock_chain_transaction = NULL;
			clock_job.active = false;
		}
		{
			struct ad9361_clock_chain_transaction clock_job = {0};
			unsigned int writes_at_start = write_count;

			agc_phy.calibration_state_unknown = false;
			clock_job.phy = &agc_phy;
			clock_job.active = true;
			clock_job.stage = AD9361_CLOCK_CHAIN_TRANSACTION_POST_CHANGE;
			clock_job.post_change_stage =
				AD9361_CLOCK_CHAIN_POST_CHANGE_GC_UPDATE;
			agc_phy.clock_chain_transaction = &clock_job;
			fail_write_once_reg = REG_AGC_ATTACK_DELAY;
			fail_write_once_reg_occurrence = 1;
			ret = ad9361_clock_chain_transaction_poll(&agc_phy, &clock_job);
			if (ret != -EIO || !clock_job.terminal ||
			    clock_job.terminal_status != -EIO ||
			    !ad9361_calibration_state_unknown(&agc_phy) ||
			    agc_phy.clock_chain_transaction != NULL ||
			    write_count != writes_at_start + 1 ||
			    fail_write_once_reg != -1) {
				fprintf(stderr,
					"Clock-chain post-change error lost failure/owner state\n");
				return EXIT_FAILURE;
			}
		}
        register_map_enabled = true;
        spi_status = 0;
        fail_write_reg = -1;
		{
			uint16_t cursor = 0;
			unsigned int writes_at_start = write_count;
			unsigned int polls = 0;
			int32_t step;

		do {
			unsigned int writes_before_step = write_count;

			step = ad9361_rssi_setup_step(&agc_phy, &rssi, false,
						      &cursor);
			polls++;
			if ((step != 0 && step != 1) ||
			    write_count - writes_before_step > 1 || polls > 10) {
				fprintf(stderr,
					"RSSI setup step exceeded one-register bound\n");
				return EXIT_FAILURE;
			}
		} while (step == 1);
		if (cursor != 9 || write_count - writes_at_start != 9) {
			fprintf(stderr,
				"RSSI setup cursor did not cover its nine ordered writes\n");
			return EXIT_FAILURE;
		}
		}
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
		{
			uint16_t cursor = 0;
			unsigned int writes_at_start = write_count;
			unsigned int polls = 0;
			int32_t step;

		do {
			unsigned int writes_before_step = write_count;

			step = ad9361_auxadc_setup_step(&phy, &auxadc, 983040000,
							&cursor);
			polls++;
			if ((step != 0 && step != 1) ||
			    write_count - writes_before_step > 1 || polls > 7) {
				fprintf(stderr,
					"AuxADC setup step exceeded one-register bound\n");
				return EXIT_FAILURE;
			}
		} while (step == 1);
		if (cursor != 6 || write_count - writes_at_start != 6) {
			fprintf(stderr,
				"AuxADC setup cursor did not cover its six ordered writes\n");
			return EXIT_FAILURE;
		}
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

    /* A failed ENSM restore after programming preloaded factory RSSI tables
     * must survive the retained setup parent as a cleanup result. */
    {
        struct ad9361_phy_platform_data post_pdata = {0};
        struct ad9361_rf_phy post_phy = {0};
        struct ad9361_setup_transaction setup_job = {0};
        int32_t expected_cleanup = -EIO;

        post_pdata.rx2tx2 = true;
        post_pdata.fdd = true;
        post_pdata.rssi_skip_calib = true;
        post_pdata.rssi_lna_err_tbl[0] = 1;
        post_pdata.rssi_ctrl.rssi_unit_is_rx_samples = true;
        post_pdata.rssi_ctrl.rssi_duration = 1;
        post_pdata.txmon_ctrl.tx_mon_duration = 16;
        post_phy.spi = &spi;
        post_phy.pdata = &post_pdata;

        register_map_enabled = true;
        memset(fake_regs, 0, sizeof(fake_regs));
        fake_regs[REG_STATE] = ENSM_STATE_RX;
        state_after_ensm_write = ENSM_STATE_ALERT;
        fail_write_once_reg = REG_ENSM_CONFIG_1;
        /* Initial force-to-ALERT writes twice; fail the first restore write. */
        fail_write_once_reg_occurrence = 3;
        setup_job.phy = &post_phy;
        setup_job.active = true;
        setup_job.stage = AD9361_SETUP_TRANSACTION_CALIBRATION;
        setup_job.calibration_job.phy = &post_phy;
        setup_job.calibration_job.terminal = true;
        setup_job.calibration_job.terminal_status = 0;
        post_phy.setup_transaction = &setup_job;

        ret = ad9361_setup_transaction_poll(&post_phy, &setup_job);
        fail_write_once_reg = -1;
        state_after_ensm_write = -1;
        register_map_enabled = false;
        if (ret != -EIO || !setup_job.terminal ||
            setup_job.terminal_status != -EIO ||
            setup_job.cleanup_status != expected_cleanup ||
            post_phy.setup_transaction != NULL ||
            !ad9361_calibration_state_unknown(&post_phy)) {
            fprintf(stderr,
                    "Post-calibration ENSM cleanup was lost: ret=%" PRId32
                    " terminal=%" PRId32 " cleanup=%" PRId32
                    " owner=%p unknown=%d\n",
                    ret, setup_job.terminal_status,
                    setup_job.cleanup_status,
                    (void *)post_phy.setup_transaction,
                    ad9361_calibration_state_unknown(&post_phy));
            return EXIT_FAILURE;
        }
    }

    /* Setup is not terminal when its final ENSM command is merely written;
     * it remains owned until REG_STATE confirms the requested stable state. */
    {
        struct ad9361_phy_platform_data post_pdata = {0};
        struct ad9361_rf_phy post_phy = {0};
        struct ad9361_setup_transaction setup_job = {0};
        uint32_t delay_us;

        post_pdata.rx2tx2 = true;
        post_pdata.fdd = true;
        post_pdata.rssi_skip_calib = true;
        post_pdata.rssi_ctrl.rssi_unit_is_rx_samples = true;
        post_pdata.rssi_ctrl.rssi_duration = 1;
        post_pdata.txmon_ctrl.tx_mon_duration = 16;
        post_phy.spi = &spi;
        post_phy.pdata = &post_pdata;

        register_map_enabled = true;
        memset(fake_regs, 0, sizeof(fake_regs));
        fake_regs[REG_STATE] = ENSM_STATE_ALERT;
        state_after_ensm_write = ENSM_STATE_ALERT;
        setup_job.phy = &post_phy;
        setup_job.active = true;
        setup_job.stage = AD9361_SETUP_TRANSACTION_CALIBRATION;
        setup_job.calibration_job.phy = &post_phy;
        setup_job.calibration_job.terminal = true;
        setup_job.calibration_job.terminal_status = 0;
        post_phy.setup_transaction = &setup_job;

        ret = ad9361_setup_transaction_poll(&post_phy, &setup_job);
        if (ret != 1 || setup_job.terminal ||
            setup_job.stage !=
                AD9361_SETUP_TRANSACTION_POST_CALIBRATION_WAIT ||
            !setup_job.post_calibration_wait.active ||
            post_phy.status_wait != &setup_job.post_calibration_wait) {
            fprintf(stderr,
                    "Setup reported success before ENSM readback: ret=%" PRId32
                    " stage=%u terminal=%d wait=%p\n",
                    ret, setup_job.stage, setup_job.terminal,
                    (void *)post_phy.status_wait);
            return EXIT_FAILURE;
        }

        /* Model the hardware completing the transition after the command. */
        fake_regs[REG_STATE] = ENSM_STATE_FDD;
        delay_us = ad9361_setup_transaction_poll_delay_us(&setup_job);
        if (delay_us)
            no_os_udelay(delay_us);
        ret = ad9361_setup_transaction_poll(&post_phy, &setup_job);
        state_after_ensm_write = -1;
        register_map_enabled = false;
        if (ret != 0 || !setup_job.terminal ||
            setup_job.terminal_status != 0 ||
            post_phy.status_wait != NULL ||
            setup_job.post_calibration_wait.terminal_status != 0) {
            fprintf(stderr,
                    "Setup did not retire after ENSM readback: ret=%" PRId32
                    " terminal=%" PRId32 " wait=%p\n",
                    ret, setup_job.terminal_status,
                    (void *)post_phy.status_wait);
            return EXIT_FAILURE;
        }

        /* A missing target-state event must fail at the status deadline and
         * retire both nested and parent owners without accepting setup. */
        memset(&setup_job, 0, sizeof(setup_job));
        setup_job.phy = &post_phy;
        setup_job.active = true;
        setup_job.stage = AD9361_SETUP_TRANSACTION_CALIBRATION;
        setup_job.calibration_job.phy = &post_phy;
        setup_job.calibration_job.terminal = true;
        setup_job.calibration_job.terminal_status = 0;
        post_phy.setup_transaction = &setup_job;
        fake_regs[REG_STATE] = ENSM_STATE_ALERT;
        state_after_ensm_write = ENSM_STATE_ALERT;
        fake_time_us += 1000;

        ret = ad9361_setup_transaction_poll(&post_phy, &setup_job);
        if (ret != 1 || !setup_job.post_calibration_wait.active ||
            setup_job.terminal) {
            fprintf(stderr,
                    "Setup did not retain missing ENSM event: ret=%" PRId32
                    " terminal=%d\n", ret, setup_job.terminal);
            return EXIT_FAILURE;
        }
        fake_time_us = setup_job.post_calibration_wait.start_us +
                       setup_job.post_calibration_wait.timeout_us;
        ret = ad9361_setup_transaction_poll(&post_phy, &setup_job);
        state_after_ensm_write = -1;
        register_map_enabled = false;
        if (ret != -ETIMEDOUT || !setup_job.terminal ||
            setup_job.terminal_status != -ETIMEDOUT ||
            post_phy.status_wait != NULL ||
            post_phy.setup_transaction != NULL ||
            !ad9361_calibration_state_unknown(&post_phy)) {
            fprintf(stderr,
                    "Missing ENSM event was not terminal timeout: ret=%" PRId32
                    " terminal=%" PRId32 " wait=%p setup=%p unknown=%d\n",
                    ret, setup_job.terminal_status,
                    (void *)post_phy.status_wait,
                    (void *)post_phy.setup_transaction,
                    ad9361_calibration_state_unknown(&post_phy));
            return EXIT_FAILURE;
        }
    }

    puts("AD9361 calibration, clock, ENSM, AGC, RSSI, and FIR SPI-error propagation: PASS");
    return EXIT_SUCCESS;
}
