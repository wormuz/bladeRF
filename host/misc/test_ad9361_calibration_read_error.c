/* Regression test: calibration status reads must propagate SPI errors. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../thirdparty/analogdevicesinc/no-OS/drivers/rf-transceiver/ad9361/ad9361.c"

static unsigned read_count;
static unsigned delay_count;
static int32_t spi_status;
static bool register_map_enabled;
static int32_t fail_read_reg = -1;
static int32_t fail_write_reg = -1;
static int32_t state_after_ensm_write = -1;
static uint8_t fake_regs[1024];

uint32_t find_first_bit(uint32_t word)
{
    uint32_t bit = 0;
    while (bit < 32 && !(word & (1u << bit))) ++bit;
    return bit;
}

void *no_os_malloc(size_t size) { return malloc(size); }
void no_os_free(void *ptr) { free(ptr); }
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

    puts("AD9361 calibration and ENSM SPI-error propagation: PASS");
    return EXIT_SUCCESS;
}
