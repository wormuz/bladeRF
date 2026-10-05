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

int32_t no_os_spi_write_and_read(struct no_os_spi_desc *desc, uint8_t *data,
                                 uint16_t bytes_number)
{
    uint16_t cmd = ((uint16_t)data[0] << 8) | data[1];
    uint16_t addr = cmd & 0x0fff;
    bool is_write = (cmd & AD_WRITE) != 0;
    (void)desc;

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
    }
    return 0;
}

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

int main(void)
{
    if (test_mid_table_write_failure() != 0 ||
        test_failed_read_before_table_programming() != 0 ||
        test_gain_index_write_failure_invalidates_cache() != 0 ||
        test_retry_reloads_complete_table() != 0) {
        return EXIT_FAILURE;
    }

    puts("PASS: SPI failures are propagated, table cache is invalidated, and retry loads all rows");
    return EXIT_SUCCESS;
}
