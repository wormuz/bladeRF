/* Regression test: calibration status reads must propagate SPI errors. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../thirdparty/analogdevicesinc/no-OS/drivers/rf-transceiver/ad9361/ad9361.c"

static unsigned read_count;
static unsigned delay_count;
static int32_t spi_status;

uint32_t find_first_bit(uint32_t word)
{
    uint32_t bit = 0;
    while (bit < 32 && !(word & (1u << bit))) ++bit;
    return bit;
}

void *no_os_malloc(size_t size) { return malloc(size); }
void no_os_free(void *ptr) { free(ptr); }
void no_os_udelay(uint32_t usecs) { (void)usecs; ++delay_count; }

int32_t no_os_spi_write_and_read(struct no_os_spi_desc *desc, uint8_t *data,
                                 uint16_t bytes_number)
{
    (void)desc;
    if (bytes_number < 3 || (data[0] & (AD_WRITE >> 8)) != 0)
        return -EINVAL;
    ++read_count;
    if (spi_status < 0) return spi_status;
    data[2] = (uint8_t)spi_status;
    return 0;
}

int main(void)
{
    struct ad9361_rf_phy phy = {0};
    struct no_os_spi_desc spi = {0};
    phy.spi = &spi;

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

    puts("AD9361 calibration SPI-error propagation: PASS");
    return EXIT_SUCCESS;
}
