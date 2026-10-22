/* Host adapter of the whole-table NIOS gain-table write
 * (NIOS_PKT_16x64_TARGET_GAIN_TABLE_REGION, RX-TRANSITION-1S3-001): row 0 of
 * a driver gain-table load becomes one region request when NIOS's table
 * digest equals ours, the driver's following rows complete locally, and
 * every other case keeps the per-row request. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <libbladeRF.h>

#include "board/bladerf2/rfic_gain_table_load.h"
#include "no_os_spi.h"

extern const struct no_os_spi_platform_ops bladerf2_spi_ops;

#define ROWS 4
static uint8_t tab[ROWS][3] = {
    { 0x01, 0x02, 0x03 }, { 0x11, 0x12, 0x13 },
    { 0x21, 0x22, 0x23 }, { 0x31, 0x32, 0x33 },
};
static const uint8_t lna = 0x80;
static const uint8_t config = 0x1e;

static bool load_active = true;
static int matches_status;     /* 0 or a BLADERF_ERR_* */
static bool matches_result = true;
static unsigned matches_calls, region_calls, row_calls;

bool bladerf2_rfic_gain_table_load_current(struct bladerf *dev,
                                           struct bladerf2_gain_table_load *out)
{
    (void)dev;
    if (!load_active) return false;
    out->tab = tab;
    out->rows = ROWS;
    out->lna = lna;
    out->region = 1;
    out->split = false;
    return true;
}

int nios_ad9361_gain_table_region_matches(struct bladerf *dev, uint8_t region,
                                          bool split, uint8_t rows,
                                          uint8_t (*t)[3], bool *match)
{
    (void)dev;
    assert(region == 1 && !split && rows == ROWS && t == tab);
    matches_calls++;
    *match = matches_status == 0 && matches_result;
    return matches_status;
}

int nios_ad9361_gain_table_region_write(struct bladerf *dev, uint8_t region,
                                        bool split, uint8_t rows, uint8_t l,
                                        uint8_t c)
{
    (void)dev;
    assert(region == 1 && !split && rows == ROWS && l == lna && c == config);
    region_calls++;
    return 0;
}

int nios_ad9361_gain_table_row(struct bladerf *dev, uint16_t row,
                               uint8_t d1, uint8_t d2, uint8_t d3, uint8_t c,
                               uint32_t delay_us)
{
    (void)dev; (void)row; (void)d1; (void)d2; (void)d3; (void)c;
    assert(delay_us == 2);
    row_calls++;
    return 0;
}

int nios_ad9361_spi_write_batch(struct bladerf *dev, const uint16_t *regs,
                                const uint8_t *values, uint8_t count)
{
    (void)dev; (void)regs; (void)values; (void)count;
    return BLADERF_ERR_UNSUPPORTED;
}

int nios_ad9361_spi_update_bits(struct bladerf *dev, uint16_t reg,
                                uint8_t mask, uint8_t value)
{
    (void)dev; (void)reg; (void)mask; (void)value;
    return BLADERF_ERR_UNSUPPORTED;
}

void bladerf2_rx_transition_spi_observe(struct bladerf *dev, bool begin,
                                        int status)
{
    (void)dev; (void)begin; (void)status;
}

void bladerf2_rx_transition_spi_observe_rollback(struct bladerf *dev,
                                                 uint32_t write_count)
{
    (void)dev; (void)write_count;
}

void no_os_mutex_lock(void *m) { (void)m; }
void no_os_mutex_unlock(void *m) { (void)m; }
void no_os_udelay(uint32_t us) { (void)us; }

static struct no_os_spibus_desc bus;

static struct no_os_spi_desc *open_desc(void)
{
    struct no_os_spi_init_param param = { 0 };
    struct no_os_spi_desc *desc = NULL;

    param.platform_ops = &bladerf2_spi_ops;
    param.extra = (void *)0x1;
    assert(bladerf2_spi_ops.init(&desc, &param) == 0);
    desc->bus = &bus;
    return desc;
}

/* One driver load: rows 0..ROWS-1 of `tab` (row `bad` with other data). */
static void load(struct no_os_spi_desc *desc, int bad)
{
    for (uint16_t r = 0; r < ROWS; ++r) {
        const uint8_t d1 = (uint8_t)(tab[r][0] | lna);
        const uint8_t d2 = (uint8_t)(r == bad ? 0xee : tab[r][1]);
        assert(bladerf2_spi_ops.write_gain_table_row(desc, r, d1, d2,
                                                     tab[r][2], config,
                                                     2) == 0);
    }
}

static void reset_counts(void)
{
    matches_calls = region_calls = row_calls = 0;
}

int main(void)
{
    struct no_os_spi_desc *desc;

    /* Supported, digest equal: one region request, no row requests; the
     * digest is checked once per table. */
    desc = open_desc();
    load(desc, -1);
    assert(matches_calls == 1 && region_calls == 1 && row_calls == 0);
    load(desc, -1);
    assert(matches_calls == 1 && region_calls == 2 && row_calls == 0);
    /* A row that differs from the written table is written as asked, and
     * so are the rows after it. */
    reset_counts();
    load(desc, 2);
    assert(region_calls == 1 && row_calls == 2);
    /* No load in progress: per-row. */
    reset_counts();
    load_active = false;
    load(desc, -1);
    assert(region_calls == 0 && row_calls == ROWS);
    load_active = true;
    bladerf2_spi_ops.remove(desc);

    /* NIOS table differs: per-row, digest not asked again. */
    reset_counts();
    matches_result = false;
    desc = open_desc();
    load(desc, -1);
    load(desc, -1);
    assert(matches_calls == 1 && region_calls == 0 && row_calls == 2 * ROWS);
    bladerf2_spi_ops.remove(desc);
    matches_result = true;

    /* Image without the target (FPGA_OP on the digest read): per-row for the
     * rest of the session. */
    reset_counts();
    matches_status = BLADERF_ERR_FPGA_OP;
    desc = open_desc();
    load(desc, -1);
    load(desc, -1);
    assert(matches_calls == 1 && region_calls == 0 && row_calls == 2 * ROWS);
    bladerf2_spi_ops.remove(desc);

    /* Transport failure on the digest read: the error reaches the driver,
     * no fallback write for that row. */
    reset_counts();
    matches_status = BLADERF_ERR_TIMEOUT;
    desc = open_desc();
    assert(bladerf2_spi_ops.write_gain_table_row(
               desc, 0, (uint8_t)(tab[0][0] | lna), tab[0][1], tab[0][2],
               config, 2) < 0);
    assert(row_calls == 0 && region_calls == 0);
    bladerf2_spi_ops.remove(desc);
    matches_status = 0;

    /* The per-register reference path (BLADERF_DISABLE_GAIN_TABLE_REGION)
     * never uses the region request. */
    reset_counts();
    setenv("BLADERF_DISABLE_GAIN_TABLE_REGION", "1", 1);
    desc = open_desc();
    load(desc, -1);
    assert(matches_calls == 0 && region_calls == 0 && row_calls == ROWS);
    bladerf2_spi_ops.remove(desc);
    unsetenv("BLADERF_DISABLE_GAIN_TABLE_REGION");

    puts("bladerf2 SPI gain-table region write: PASS");
    return 0;
}
