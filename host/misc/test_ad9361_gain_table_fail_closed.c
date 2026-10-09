/*
 * Fault-injection test for the Nuand-patched AD9361 gain-table loader.
 *
 * The ADI loader is static, so this focused harness includes ad9361.c and
 * supplies a fake SPI transport. Linker section GC discards unrelated driver
 * entry points. Run via run_ad9361_gain_table_fail_closed.sh.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../thirdparty/analogdevicesinc/no-OS/drivers/rf-transceiver/ad9361/ad9361.c"

static int fail_write_reg = -1;
static int fail_read_reg = -1;
static unsigned fail_after_commits;
static unsigned table_commits;
static bool cleanup_clock_stop_seen;
static bool read_state_alert;
static unsigned spi_write_calls;
static const uint8_t (*expected_table)[3];
static unsigned batch_row_calls;
static unsigned batch_commits;
static unsigned batch_fail_row;
static bool batch_bad_payload;
static bool batch_bad_delay;

uint32_t find_first_bit(uint32_t word)
{
    uint32_t bit = 0;
    while (bit < 32 && !(word & (1u << bit))) {
        ++bit;
    }
    return bit;
}

void *no_os_malloc(size_t size)
{
    return malloc(size);
}

void no_os_free(void *ptr)
{
    free(ptr);
}

void no_os_udelay(uint32_t us)
{
    (void)us;
}

void no_os_mdelay(uint32_t ms)
{
    (void)ms;
}

int32_t ilog2(int32_t value)
{
    int32_t log = 0;
    while (value > 1) {
        value >>= 1;
        ++log;
    }
    return log;
}

int32_t no_os_spi_write_and_read(struct no_os_spi_desc *desc, uint8_t *data,
                                 uint16_t bytes_number)
{
    uint16_t cmd = ((uint16_t)data[0] << 8) | data[1];
    uint16_t addr = cmd & 0x0fff;
    bool is_write = (cmd & AD_WRITE) != 0;
    (void)desc;

    if (is_write) {
        ++spi_write_calls;
    }

    if (is_write && addr == REG_GAIN_TABLE_CONFIG) {
        if (bytes_number >= 3 && (data[2] & WRITE_GAIN_TABLE)) {
            ++table_commits;
        } else if (bytes_number >= 3 && data[2] == 0) {
            cleanup_clock_stop_seen = true;
        }
    }

    if ((is_write && (int)addr == fail_write_reg &&
         table_commits >= fail_after_commits) ||
        (!is_write && (int)addr == fail_read_reg)) {
        fail_write_reg = -1;
        fail_read_reg = -1;
        return -EIO;
    }

    if (!is_write && bytes_number > 2) {
        memset(data + 2, 0, bytes_number - 2);
        if (read_state_alert && addr == REG_STATE) {
            data[2] = ENSM_STATE_ALERT;
        }
    }
    return 0;
}

uint32_t clk_get_rate(struct ad9361_rf_phy *phy,
                      struct refclk_scale *clk_priv)
{
    (void)phy;
    (void)clk_priv;
    return 1000000000U;
}

static int32_t fake_write_gain_table_row(struct no_os_spi_desc *desc,
                                         uint16_t row, uint8_t data1,
                                         uint8_t data2, uint8_t data3,
                                         uint8_t config, uint32_t delay_us)
{
    (void)desc;
    if (row != batch_row_calls || row >= SIZE_FULL_TABLE ||
        data1 != expected_table[row][0] || data2 != expected_table[row][1] ||
        data3 != expected_table[row][2] ||
        config != (START_GAIN_TABLE_CLOCK | WRITE_GAIN_TABLE |
                   RECEIVER_SELECT(GT_RX1 + GT_RX2))) {
        batch_bad_payload = true;
    }
    if (delay_us != 2) {
        batch_bad_delay = true;
    }
    ++batch_row_calls;
    if (row == batch_fail_row) {
        return -EIO;
    }
    ++batch_commits;
    return 0;
}

static const struct no_os_spi_platform_ops batch_spi_ops = {
    .write_gain_table_row = fake_write_gain_table_row,
};

static void setup_phy(struct ad9361_rf_phy *phy,
                      struct ad9361_phy_platform_data *pdata,
                      struct no_os_spi_desc *spi)
{
    memset(phy, 0, sizeof(*phy));
    memset(pdata, 0, sizeof(*pdata));
    memset(spi, 0, sizeof(*spi));
    phy->spi = spi;
    phy->pdata = pdata;
    phy->gt_info = ad9361_adi_gt_info;
    phy->current_table = TBL_200_1300_MHZ;
    fail_after_commits = 0;
    table_commits = 0;
    cleanup_clock_stop_seen = false;
    read_state_alert = false;
    spi_write_calls = 0;
    expected_table = NULL;
    batch_row_calls = 0;
    batch_commits = 0;
    batch_fail_row = UINT_MAX;
    batch_bad_payload = false;
    batch_bad_delay = false;
}

static int test_rssi_setup_propagates_each_write_error(void)
{
    static const int regs[] = {
        REG_MEASURE_DURATION_01, REG_MEASURE_DURATION_23,
        REG_RSSI_WEIGHT_0, REG_RSSI_WEIGHT_1, REG_RSSI_WEIGHT_2,
        REG_RSSI_WEIGHT_3, REG_RSSI_DELAY, REG_RSSI_WAIT_TIME,
        REG_RSSI_CONFIG,
    };
    struct ad9361_rf_phy phy;
    struct ad9361_phy_platform_data pdata;
    struct no_os_spi_desc spi;
    struct rssi_control ctrl = {
        .restart_mode = SPI_WRITE_TO_REGISTER,
        .rssi_unit_is_rx_samples = true,
        .rssi_duration = 1,
    };
    unsigned i;

    for (i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        setup_phy(&phy, &pdata, &spi);
        fail_write_reg = regs[i];
        if (ad9361_rssi_setup(&phy, &ctrl, false) != -EIO ||
            spi_write_calls != i + 1) {
            fprintf(stderr,
                    "RSSI setup write failure reg=0x%02x calls=%u expected=%u\n",
                    regs[i], spi_write_calls, i + 1);
            return -1;
        }
    }

    setup_phy(&phy, &pdata, &spi);
    if (ad9361_rssi_setup(&phy, &ctrl, false) != 0 || spi_write_calls != 9) {
        fprintf(stderr, "RSSI setup success: writes=%u expected=9\n",
                spi_write_calls);
        return -1;
    }

    setup_phy(&phy, &pdata, &spi);
    ctrl.rssi_duration = 0;
    if (ad9361_rssi_setup(&phy, &ctrl, false) != -ERANGE ||
        spi_write_calls != 0) {
        fprintf(stderr, "zero RSSI duration was not rejected before SPI\n");
        return -1;
    }
    return 0;
}

static int test_auxadc_setup_propagates_each_write_error(void)
{
    static const int regs[] = {
        REG_TEMP_OFFSET, REG_START_TEMP_READING, REG_TEMP_SENSE2,
        REG_TEMP_SENSOR_CONFIG, REG_AUXADC_CLOCK_DIVIDER,
        REG_AUXADC_CONFIG,
    };
    struct ad9361_rf_phy phy;
    struct ad9361_phy_platform_data pdata;
    struct no_os_spi_desc spi;
    struct auxadc_control ctrl = {
        .temp_time_inteval_ms = 5,
        .temp_sensor_decimation = 256,
        .auxadc_clock_rate = 1000000,
        .auxadc_decimation = 256,
    };
    unsigned i;

    for (i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        setup_phy(&phy, &pdata, &spi);
        fail_write_reg = regs[i];
        if (ad9361_auxadc_setup(&phy, &ctrl, 983040000U) != -EIO ||
            spi_write_calls != i + 1) {
            fprintf(stderr,
                    "AuxADC setup write failure reg=0x%02x calls=%u expected=%u\n",
                    regs[i], spi_write_calls, i + 1);
            return -1;
        }
    }

    setup_phy(&phy, &pdata, &spi);
    ctrl.auxadc_clock_rate = 0;
    if (ad9361_auxadc_setup(&phy, &ctrl, 983040000U) != -EINVAL ||
        spi_write_calls != 0) {
        fprintf(stderr, "zero AuxADC clock rate was not rejected before SPI\n");
        return -1;
    }
    return 0;
}

static int test_rssi_calibration_and_table_loaders_fail_closed(void)
{
    struct ad9361_rf_phy phy;
    struct ad9361_phy_platform_data pdata;
    struct no_os_spi_desc spi;

    setup_phy(&phy, &pdata, &spi);
    read_state_alert = true;
    fail_write_reg = REG_MAX_MIXER_CALIBRATION_GAIN_INDEX;
    if (ad9361_rssi_gain_step_calib(&phy) != -EIO || spi_write_calls != 3) {
        fprintf(stderr,
                "RSSI gain calibration failure status/calls mismatch: calls=%u\n",
                spi_write_calls);
        return -1;
    }

    setup_phy(&phy, &pdata, &spi);
    fail_write_reg = REG_LNA_GAIN;
    if (ad9361_rssi_program_lna_gain(&phy) != -EIO || spi_write_calls != 1) {
        fprintf(stderr, "RSSI LNA table failure was not propagated\n");
        return -1;
    }

    setup_phy(&phy, &pdata, &spi);
    fail_write_reg = REG_CONFIG;
    if (ad9361_rssi_write_err_tbl(&phy) != -EIO || spi_write_calls != 2) {
        fprintf(stderr, "RSSI error table failure was not propagated\n");
        return -1;
    }
    return 0;
}

static int test_mid_table_write_failure(void)
{
    struct ad9361_rf_phy phy;
    struct ad9361_phy_platform_data pdata;
    struct no_os_spi_desc spi;

    setup_phy(&phy, &pdata, &spi);
    fail_write_reg = REG_GAIN_TABLE_WRITE_DATA2;
    fail_after_commits = 1;

    int status = ad9361_load_gt(&phy, 1301000000ULL, GT_RX1 + GT_RX2);
    if (status != -EIO || phy.current_table != NO_GAIN_TABLE ||
        !cleanup_clock_stop_seen || table_commits != 1) {
        fprintf(stderr,
                "mid-table failure: status=%d current_table=%u commits=%u cleanup=%d\n",
                status, phy.current_table, table_commits,
                cleanup_clock_stop_seen);
        return -1;
    }
    return 0;
}

static int test_failed_read_before_table_programming(void)
{
    struct ad9361_rf_phy phy;
    struct ad9361_phy_platform_data pdata;
    struct no_os_spi_desc spi;

    setup_phy(&phy, &pdata, &spi);
    fail_read_reg = REG_RX1_MANUAL_LMT_FULL_GAIN;

    int status = ad9361_load_gt(&phy, 1301000000ULL, GT_RX1 + GT_RX2);
    if (status != -EIO || phy.current_table != NO_GAIN_TABLE ||
        table_commits != 0) {
        fprintf(stderr,
                "gain-index read failure: status=%d current_table=%u commits=%u\n",
                status, phy.current_table, table_commits);
        return -1;
    }
    return 0;
}

static int test_gain_index_write_failure_invalidates_cache(void)
{
    struct ad9361_rf_phy phy;
    struct ad9361_phy_platform_data pdata;
    struct no_os_spi_desc spi;

    setup_phy(&phy, &pdata, &spi);
    fail_write_reg = REG_RX1_MANUAL_LMT_FULL_GAIN;

    int status = ad9361_load_gt(&phy, 1301000000ULL, GT_RX1 + GT_RX2);
    if (status != -EIO || phy.current_table != NO_GAIN_TABLE ||
        table_commits != SIZE_FULL_TABLE || !cleanup_clock_stop_seen) {
        fprintf(stderr,
                "gain-index write failure: status=%d current_table=%u commits=%u cleanup=%d\n",
                status, phy.current_table, table_commits,
                cleanup_clock_stop_seen);
        return -1;
    }
    return 0;
}

static int test_retry_reloads_complete_table(void)
{
    struct ad9361_rf_phy phy;
    struct ad9361_phy_platform_data pdata;
    struct no_os_spi_desc spi;

    setup_phy(&phy, &pdata, &spi);
    phy.current_table = NO_GAIN_TABLE;
    if (ad9361_load_gt(&phy, 1301000000ULL, GT_RX1 + GT_RX2) != 0 ||
        phy.current_table != TBL_1300_4000_MHZ ||
        table_commits != SIZE_FULL_TABLE) {
        fprintf(stderr,
                "full reload: current_table=%u commits=%u expected=%u\n",
                phy.current_table, table_commits, SIZE_FULL_TABLE);
        return -1;
    }
    return 0;
}

static int test_batched_rows_preserve_payload_and_delay(void)
{
    struct ad9361_rf_phy phy;
    struct ad9361_phy_platform_data pdata;
    struct no_os_spi_desc spi;

    setup_phy(&phy, &pdata, &spi);
    expected_table = phy.gt_info[TBL_1300_4000_MHZ].tab;
    spi.platform_ops = &batch_spi_ops;

    int status = ad9361_load_gt(&phy, 1301000000ULL, GT_RX1 + GT_RX2);
    if (status != 0 || phy.current_table != TBL_1300_4000_MHZ ||
        batch_row_calls != SIZE_FULL_TABLE ||
        batch_commits != SIZE_FULL_TABLE || batch_bad_payload ||
        batch_bad_delay) {
        fprintf(stderr,
                "batched rows: status=%d table=%u calls=%u commits=%u bad_payload=%d bad_delay=%d\n",
                status, phy.current_table, batch_row_calls, batch_commits,
                batch_bad_payload, batch_bad_delay);
        return -1;
    }
    return 0;
}

static int test_batched_row_failure_invalidates_cache(void)
{
    struct ad9361_rf_phy phy;
    struct ad9361_phy_platform_data pdata;
    struct no_os_spi_desc spi;

    setup_phy(&phy, &pdata, &spi);
    expected_table = phy.gt_info[TBL_1300_4000_MHZ].tab;
    batch_fail_row = 7;
    spi.platform_ops = &batch_spi_ops;

    int status = ad9361_load_gt(&phy, 1301000000ULL, GT_RX1 + GT_RX2);
    if (status != -EIO || phy.current_table != NO_GAIN_TABLE ||
        batch_row_calls != 8 || batch_commits != 7 ||
        !cleanup_clock_stop_seen) {
        fprintf(stderr,
                "batched row failure: status=%d table=%u calls=%u commits=%u cleanup=%d\n",
                status, phy.current_table, batch_row_calls, batch_commits,
                cleanup_clock_stop_seen);
        return -1;
    }
    return 0;
}

int main(void)
{
    if (test_rssi_setup_propagates_each_write_error() != 0 ||
        test_auxadc_setup_propagates_each_write_error() != 0 ||
        test_rssi_calibration_and_table_loaders_fail_closed() != 0 ||
        test_mid_table_write_failure() != 0 ||
        test_failed_read_before_table_programming() != 0 ||
        test_gain_index_write_failure_invalidates_cache() != 0 ||
        test_retry_reloads_complete_table() != 0 ||
        test_batched_rows_preserve_payload_and_delay() != 0 ||
        test_batched_row_failure_invalidates_cache() != 0) {
        return EXIT_FAILURE;
    }

    puts("PASS: AD9361 RSSI/AuxADC and gain-table SPI failures propagate; retry reloads all rows");
    return EXIT_SUCCESS;
}
