/* Host platform for the real no-OS AD9361 driver: a flat 10-bit register file
 * with just enough chip behaviour for the retained constructor to run from
 * RESETB to "successfully initialized" without any driver-side fake.
 *
 *  - every register reads 0xFF (lock/valid/done status bits are set) except
 *    PRODUCT_ID, which carries the AD9361 identifier;
 *  - writes read back as written; CALIBRATION_CTRL self-clears (calibration
 *    done) and a write to ENSM_CONFIG_1 moves REG_STATE like the FDD part;
 *  - GPIO/SPI descriptors and the AXI cores are inert allocations. */
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
#include "../../thirdparty/analogdevicesinc/no-OS/drivers/rf-transceiver/ad9361/ad9361_api.c"

const struct no_os_spi_platform_ops bladerf2_spi_ops;
const struct no_os_gpio_platform_ops bladerf2_gpio_ops;
#include "../../fpga_common/src/ad936x_params.c"

#define MAX_WRITES 4096

static uint8_t regs[1024];
static uint64_t now_us;
static int live_allocs;
static int cur_init_stage = -1, cur_setup_stage = -1;
static int inject_setup_stage = -1; /* fail the first SPI write in this setup stage */
static bool injected;
static unsigned write_count;
static struct { uint16_t reg; int init_stage, setup_stage; } write_log[MAX_WRITES];

static void *counted_calloc(size_t a, size_t b)
{
    void *p = calloc(a, b);
    if (p)
        live_allocs++;
    return p;
}
void *no_os_malloc(size_t n) { return counted_calloc(1, n); }
void *no_os_calloc(size_t a, size_t b) { return counted_calloc(a, b); }
void no_os_free(void *p) { if (p) { live_allocs--; free(p); } }
int32_t no_os_gpio_get(struct no_os_gpio_desc **d, const struct no_os_gpio_init_param *p)
{ (void)p; *d = counted_calloc(1, 64); return *d ? 0 : -ENOMEM; }
int32_t no_os_gpio_get_optional(struct no_os_gpio_desc **d, const struct no_os_gpio_init_param *p)
{ (void)p; *d = NULL; return 0; }
int32_t no_os_gpio_remove(struct no_os_gpio_desc *d) { no_os_free(d); return 0; }
int32_t no_os_gpio_direction_output(struct no_os_gpio_desc *d, uint8_t v) { (void)d; (void)v; return 0; }
int32_t no_os_gpio_direction_input(struct no_os_gpio_desc *d) { (void)d; return 0; }
int32_t no_os_gpio_set_value(struct no_os_gpio_desc *d, uint8_t v) { (void)d; (void)v; return 0; }
int32_t no_os_spi_init(struct no_os_spi_desc **d, const struct no_os_spi_init_param *p)
{ (void)p; *d = counted_calloc(1, 64); return *d ? 0 : -ENOMEM; }
int32_t no_os_spi_remove(struct no_os_spi_desc *d) { no_os_free(d); return 0; }
uint64_t no_os_do_div(uint64_t *n, uint64_t b) { uint64_t r = *n % b; *n /= b; return r; }
struct no_os_time no_os_get_time(void)
{
    struct no_os_time t;
    t.s = (unsigned)(now_us / 1000000);
    t.us = (unsigned)(now_us % 1000000);
    return t;
}
void no_os_udelay(uint32_t u) { now_us += u; }
void no_os_mdelay(uint32_t m) { now_us += (uint64_t)m * 1000; }

static uint32_t axi_regs[0x5000 / 4];
int32_t no_os_axi_io_read(uint32_t b, uint32_t o, uint32_t *d)
{ (void)b; *d = axi_regs[o / 4] ? axi_regs[o / 4] : 1; return 0; }
int32_t no_os_axi_io_write(uint32_t b, uint32_t o, uint32_t d)
{ (void)b; axi_regs[o / 4] = d; return 0; }

int32_t no_os_spi_write_and_read(struct no_os_spi_desc *d, uint8_t *data, uint16_t n)
{
    uint16_t cmd = ((uint16_t)data[0] << 8) | data[1], reg = cmd & 0x3ff, i;

    (void)d;
    if (cmd & AD_WRITE) {
        if (cur_setup_stage == inject_setup_stage && cur_init_stage == AD9361_INIT_TXN_SETUP_POLL && !injected) {
            injected = true;
            return -EIO;
        }
        if (write_count < MAX_WRITES) {
            write_log[write_count].reg = reg;
            write_log[write_count].init_stage = cur_init_stage;
            write_log[write_count].setup_stage = cur_setup_stage;
        }
        write_count++;
        regs[reg] = data[2];
        if (reg == REG_CALIBRATION_CTRL)
            regs[reg] = 0;
        if (reg == REG_ENSM_CONFIG_1) {
            uint8_t v = data[2];
            if (v & FORCE_ALERT_STATE)
                regs[REG_STATE] = ENSM_STATE_ALERT;
            else if (v & (FORCE_TX_ON | FORCE_RX_ON))
                regs[REG_STATE] = ENSM_STATE_FDD;
            else if (v & TO_ALERT)
                regs[REG_STATE] = ENSM_STATE_ALERT;
            else
                regs[REG_STATE] = ENSM_STATE_FDD;
        }
        return 0;
    }
    for (i = 0; i < n - 2; i++)
        data[2 + i] = regs[reg - i];
    return 0;
}

static void platform_reset(void)
{
    memset(regs, 0xff, sizeof(regs));
    memset(axi_regs, 0, sizeof(axi_regs));
    regs[REG_PRODUCT_ID] = PRODUCT_ID_9361;
    now_us = 0;
    write_count = 0;
    injected = false;
    inject_setup_stage = -1;
    cur_init_stage = cur_setup_stage = -1;
}
