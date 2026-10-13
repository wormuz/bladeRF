#include <libbladeRF.h>
#include <bladeRF2.h>

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

extern void bladerf2_axi_io_set_dev(struct bladerf *dev);
extern int32_t no_os_axi_io_read(uint32_t base, uint32_t offset,
                                 uint32_t *data);

#define RX_CLOCK_MONITOR_BASE 0x00000000u
#define RX_CLOCK_MONITOR_RATE 0x54u
#define UP_CLOCK_HZ 80000000.0
#define BBPLL_LOCK_REG 0x05eu
#define BBPLL_CTRL_REG 0x03fu
#define BBPLL_VCO_PROGRAM_1 0x04cu
#define BBPLL_VCO_PROGRAM_2 0x04du

static int read_reg(struct bladerf *dev, uint16_t address, uint8_t *value)
{
    int status = bladerf_get_rfic_register(dev, address, value);
    if (status != 0) {
        fprintf(stderr, "read RFIC 0x%03x: %s\n", address,
                bladerf_strerror(status));
    }
    return status;
}

static int write_reg(struct bladerf *dev, uint16_t address, uint8_t value)
{
    int status = bladerf_set_rfic_register(dev, address, value);
    if (status != 0) {
        fprintf(stderr, "write RFIC 0x%03x=0x%02x: %s\n", address, value,
                bladerf_strerror(status));
    }
    return status;
}

static int read_fraction_and_clock(struct bladerf *dev, const char *phase)
{
    uint8_t reg41, reg42, reg43, reg44, lock;
    uint32_t monitor_count;
    int32_t status;

    if (read_reg(dev, 0x041, &reg41) != 0 ||
        read_reg(dev, 0x042, &reg42) != 0 ||
        read_reg(dev, 0x043, &reg43) != 0 ||
        read_reg(dev, 0x044, &reg44) != 0 ||
        read_reg(dev, BBPLL_LOCK_REG, &lock) != 0) {
        return -1;
    }

    status = no_os_axi_io_read(RX_CLOCK_MONITOR_BASE,
                               RX_CLOCK_MONITOR_RATE, &monitor_count);
    if (status < 0) {
        fprintf(stderr, "read RX clock monitor: %d\n", status);
        return -1;
    }

    printf("%s bbpll_regs=0x41:%02x,0x42:%02x,0x43:%02x,0x44:%02x "
           "lock=%u clock_count=%" PRIu32 " sample_equiv_hz=%.3f\n",
           phase, reg41, reg42, reg43, reg44, (lock >> 7) & 1u,
           monitor_count,
           (double)monitor_count * UP_CLOCK_HZ / 65536.0 / 2.0);
    return 0;
}

static int wait_for_bbpll_lock(struct bladerf *dev)
{
    const struct timespec pause = {.tv_sec = 0, .tv_nsec = 10000000};

    for (unsigned int i = 0; i < 200; ++i) {
        uint8_t lock;
        if (read_reg(dev, BBPLL_LOCK_REG, &lock) != 0) {
            return -1;
        }
        if ((lock & 0x80u) != 0) {
            return 0;
        }
        (void)nanosleep(&pause, NULL);
    }

    fprintf(stderr, "BBPLL did not report lock after corrected word write\n");
    return -1;
}

static void wait_for_monitor_settle(void)
{
    const struct timespec pause = {.tv_sec = 0, .tv_nsec = 250000000};
    (void)nanosleep(&pause, NULL);
}

int main(void)
{
    struct bladerf *dev = NULL;
    bladerf_sample_rate actual_rate = 0;
    uint8_t control;
    int status = bladerf_open(&dev, NULL);

    if (status != 0) {
        fprintf(stderr, "bladerf_open: %s\n", bladerf_strerror(status));
        return EXIT_FAILURE;
    }

    status = bladerf_set_sample_rate(dev, BLADERF_CHANNEL_RX(0),
                                     23040000, &actual_rate);
    if (status != 0 || actual_rate != 23040000) {
        fprintf(stderr, "set RX rate: status=%s actual=%" PRIu64 "\n",
                bladerf_strerror(status), (uint64_t)actual_rate);
        goto fail;
    }

    bladerf2_axi_io_set_dev(dev);
    if (read_fraction_and_clock(dev, "before") != 0) {
        goto fail_unbind;
    }

    /* UG-672 Table 25 maps 0x041[4:0] to fractional bits [20:16],
     * 0x042 to [15:8], and 0x043 to [7:0]. 23.04 Msps requires
     * BBPLL=737.28 MHz, hence integer 19 and fraction 0x066000. */
    if (write_reg(dev, 0x041, 0x06) != 0 ||
        write_reg(dev, 0x042, 0x60) != 0 ||
        write_reg(dev, 0x043, 0x00) != 0) {
        goto fail_unbind;
    }

    if (read_reg(dev, BBPLL_CTRL_REG, &control) != 0 ||
        write_reg(dev, BBPLL_CTRL_REG, (uint8_t)(control | 0x04u)) != 0 ||
        write_reg(dev, BBPLL_CTRL_REG, (uint8_t)(control & ~0x04u)) != 0 ||
        write_reg(dev, BBPLL_VCO_PROGRAM_1, 0x86) != 0 ||
        write_reg(dev, BBPLL_VCO_PROGRAM_2, 0x01) != 0 ||
        write_reg(dev, BBPLL_VCO_PROGRAM_2, 0x05) != 0 ||
        wait_for_bbpll_lock(dev) != 0) {
        goto fail_unbind;
    }

    /* Let the asynchronous ADI clock-monitor counter complete several
     * reference windows after the BBPLL reprogramming. */
    wait_for_monitor_settle();
    if (read_fraction_and_clock(dev, "after") != 0) {
        goto fail_unbind;
    }

    bladerf2_axi_io_set_dev(NULL);
    bladerf_close(dev);
    return EXIT_SUCCESS;

fail_unbind:
    bladerf2_axi_io_set_dev(NULL);
fail:
    bladerf_close(dev);
    return EXIT_FAILURE;
}
