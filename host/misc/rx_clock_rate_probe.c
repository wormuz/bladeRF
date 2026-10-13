#define _GNU_SOURCE
#include <libbladeRF.h>
#include <bladeRF2.h>

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

/* Internal ADI no-OS AXI accessors exported by the matching libbladeRF.
 * The context setter is thread-local and must remain bound on this thread. */
extern void bladerf2_axi_io_set_dev(struct bladerf *dev);
extern int32_t no_os_axi_io_read(uint32_t base, uint32_t offset,
                                 uint32_t *data);

/* NIOS ADI_AXI packet addresses are 16-bit offsets inside this peripheral
 * window, not absolute Qsys CPU addresses. 0x54 is AXI_ADC_REG_CLK_FREQ /
 * up_adc_clk_count in ADI up_adc_common. */
#ifndef AXI_AD9361_0_BASE
#define AXI_AD9361_0_BASE 0x00000000u
#endif
#define AXI_ADC_REG_CLK_FREQ 0x54u
#define AXI_AD9361_TX_OFFSET 0x4000u
#define NIOS_UP_CLOCK_HZ 80000000.0

static uint64_t monotonic_raw_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0) {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int read_clock_monitor(uint32_t base, uint32_t *count)
{
    return no_os_axi_io_read(base, AXI_ADC_REG_CLK_FREQ, count);
}

static int read_timestamp(struct bladerf *dev, bladerf_direction direction,
                          bladerf_timestamp *timestamp)
{
    return bladerf_get_timestamp(dev, direction, timestamp);
}

int main(int argc, char **argv)
{
    const unsigned int intervals = argc > 1 ? (unsigned int)strtoul(argv[1], NULL, 10) : 5u;
    const unsigned int interval_ms = argc > 2 ? (unsigned int)strtoul(argv[2], NULL, 10) : 1000u;
    const bladerf_sample_rate requested_rx_rate = argc > 3
        ? (bladerf_sample_rate)strtoull(argv[3], NULL, 10) : 23040000u;
    /* Zero leaves TX untouched, isolating the shared AD9361 BBPLL path. */
    const bladerf_sample_rate requested_tx_rate = argc > 4
        ? (bladerf_sample_rate)strtoull(argv[4], NULL, 10) : 0u;
    const uint32_t axi_base = argc > 5
        ? (uint32_t)strtoul(argv[5], NULL, 0) : AXI_AD9361_0_BASE;
    struct bladerf *dev = NULL;
    bladerf_sample_rate rx_rate = 0, tx_rate = 0;
    bladerf_timestamp rx_prev = 0, tx_prev = 0;
    uint64_t host_prev = 0;
    uint32_t core_version = 0, core_id = 0, rx_clk = 0, tx_clk = 0;
    static const uint16_t clock_reg_addrs[] = {
        0x003u, 0x00au, 0x045u, 0x041u, 0x042u, 0x043u, 0x044u,
        0x2abu, 0x2acu
    };
    uint8_t clock_regs[sizeof(clock_reg_addrs) / sizeof(clock_reg_addrs[0])] = {0};
    int status;
    int ret = EXIT_FAILURE;

    if (intervals == 0 || interval_ms == 0) {
        fprintf(stderr, "usage: %s [intervals=5] [interval_ms=1000]"
                        " [rx_rate_hz=23040000] [tx_rate_hz=0=unchanged]"
                        " [axi_base=0]\n", argv[0]);
        return EXIT_FAILURE;
    }

    status = bladerf_open(&dev, NULL);
    if (status < 0) {
        fprintf(stderr, "bladerf_open: %s\n", bladerf_strerror(status));
        return EXIT_FAILURE;
    }
    status = bladerf_set_sample_rate(dev, BLADERF_CHANNEL_RX(0),
                                     requested_rx_rate, NULL);
    if (status < 0) {
        fprintf(stderr, "RX sample-rate set: %s\n", bladerf_strerror(status));
        goto out;
    }
    if (requested_tx_rate != 0) {
        status = bladerf_set_sample_rate(dev, BLADERF_CHANNEL_TX(0),
                                         requested_tx_rate, NULL);
        if (status < 0) {
            fprintf(stderr, "TX sample-rate set: %s\n", bladerf_strerror(status));
            goto out;
        }
    }
    status = bladerf_get_sample_rate(dev, BLADERF_CHANNEL_RX(0), &rx_rate);
    if (status < 0) {
        fprintf(stderr, "RX sample-rate read: %s\n", bladerf_strerror(status));
        goto out;
    }
    status = bladerf_get_sample_rate(dev, BLADERF_CHANNEL_TX(0), &tx_rate);
    if (status < 0) {
        fprintf(stderr, "TX sample-rate read: %s\n", bladerf_strerror(status));
        goto out;
    }

    bladerf_log_set_verbosity(BLADERF_LOG_LEVEL_DEBUG);
    bladerf2_axi_io_set_dev(dev);
    status = no_os_axi_io_read(axi_base, 0x00u, &core_version);
    if (status < 0) {
        fprintf(stderr, "ADI AXI core-version read: %d\n", status);
        goto out_unbind;
    }
    status = no_os_axi_io_read(axi_base, 0x04u, &core_id);
    if (status < 0) {
        fprintf(stderr, "ADI AXI core-ID read: %d\n", status);
        goto out_unbind;
    }
    status = read_clock_monitor(axi_base, &rx_clk);
    if (status < 0) {
        fprintf(stderr, "RX ADI clock monitor read: %d\n", status);
        goto out_unbind;
    }
    status = read_clock_monitor(axi_base + AXI_AD9361_TX_OFFSET, &tx_clk);
    if (status < 0) {
        fprintf(stderr, "TX ADI clock monitor read: %d\n", status);
        goto out_unbind;
    }
    for (unsigned int i = 0;
         i < sizeof(clock_reg_addrs) / sizeof(clock_reg_addrs[0]); i++) {
        status = bladerf_get_rfic_register(dev, clock_reg_addrs[i],
                                           &clock_regs[i]);
        if (status < 0) {
            fprintf(stderr, "AD9361 clock register 0x%02x read: %s\n",
                    clock_reg_addrs[i], bladerf_strerror(status));
            goto out_unbind;
        }
    }
    status = read_timestamp(dev, BLADERF_RX, &rx_prev);
    if (status < 0) {
        fprintf(stderr, "RX timestamp read: %s\n", bladerf_strerror(status));
        goto out_unbind;
    }
    status = read_timestamp(dev, BLADERF_TX, &tx_prev);
    if (status < 0) {
        fprintf(stderr, "TX timestamp read: %s\n", bladerf_strerror(status));
        goto out_unbind;
    }
    host_prev = monotonic_raw_ns();

    printf("requested_readback_rx_hz=%" PRIu64 " tx_hz=%" PRIu64
           " axi_base=0x%08" PRIx32 " adi_core_version=0x%08" PRIx32
           " adi_core_id=0x%08" PRIx32 "\n",
           (uint64_t)rx_rate, (uint64_t)tx_rate, axi_base,
           core_version, core_id);
    printf("ADI_clock_monitor_rx_count=%" PRIu32
           " tx_count=%" PRIu32 " (counts per 65536 up_clk cycles)"
           " rx_sample_rate_from_monitor=%.3f tx_sample_rate_from_monitor=%.3f\n",
           rx_clk, tx_clk,
           (double)rx_clk * NIOS_UP_CLOCK_HZ / 65536.0 / 2.0,
           (double)tx_clk * NIOS_UP_CLOCK_HZ / 65536.0 / 2.0);
    printf("AD9361_clock_registers:");
    for (unsigned int i = 0;
         i < sizeof(clock_reg_addrs) / sizeof(clock_reg_addrs[0]); i++) {
        printf(" 0x%03x=0x%02x", clock_reg_addrs[i], clock_regs[i]);
    }
    putchar('\n');

    for (unsigned int i = 0; i < intervals; i++) {
        bladerf_timestamp rx_now, tx_now;
        uint64_t host_now;
        double dt, rx_hz, tx_hz;

        usleep((useconds_t)interval_ms * 1000u);
        status = read_timestamp(dev, BLADERF_RX, &rx_now);
        if (status < 0) {
            fprintf(stderr, "RX timestamp read: %s\n", bladerf_strerror(status));
            goto out_unbind;
        }
        status = read_timestamp(dev, BLADERF_TX, &tx_now);
        if (status < 0) {
            fprintf(stderr, "TX timestamp read: %s\n", bladerf_strerror(status));
            goto out_unbind;
        }
        status = read_clock_monitor(axi_base, &rx_clk);
        if (status < 0) {
            fprintf(stderr, "RX ADI clock monitor read: %d\n", status);
            goto out_unbind;
        }
        status = read_clock_monitor(axi_base + AXI_AD9361_TX_OFFSET, &tx_clk);
        if (status < 0) {
            fprintf(stderr, "TX ADI clock monitor read: %d\n", status);
            goto out_unbind;
        }
        host_now = monotonic_raw_ns();
        dt = (double)(host_now - host_prev) / 1e9;
        rx_hz = (double)(rx_now - rx_prev) / dt;
        tx_hz = (double)(tx_now - tx_prev) / dt;
        printf("interval=%u host_s=%.9f rx_timestamp_hz=%.3f"
               " tx_timestamp_hz=%.3f rx_clock_count=%" PRIu32
               " tx_clock_count=%" PRIu32
               " rx_sample_rate_from_monitor=%.3f"
               " tx_sample_rate_from_monitor=%.3f\n",
               i, dt, rx_hz, tx_hz, rx_clk, tx_clk,
               (double)rx_clk * NIOS_UP_CLOCK_HZ / 65536.0 / 2.0,
               (double)tx_clk * NIOS_UP_CLOCK_HZ / 65536.0 / 2.0);
        fflush(stdout);
        rx_prev = rx_now;
        tx_prev = tx_now;
        host_prev = host_now;
    }
    ret = EXIT_SUCCESS;

out_unbind:
    bladerf2_axi_io_set_dev(NULL);
out:
    bladerf_close(dev);
    return ret;
}
