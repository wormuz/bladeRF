/* Read back the AD9361 RX gain table after a band change (RX-TRANSITION-
 * 1S3-001): proves that the NIOS gain-table row write (one 4-byte streaming
 * write 0x133..0x130 + commit 0x137) programs the same table as the ADI
 * per-register sequence.
 *
 * Usage: [frequency_hz, default 1835000000]
 * Tunes RX1 to 947.5 MHz (table band <= 1.3 GHz) and then to the given
 * frequency, disables RX, reads `rows` rows of the RX1 and RX2 tables
 * (0x130 address, 0x134-0x136 read data, gain-table clock on), prints every
 * row and an FNV-1a digest per receiver.
 * Compare: run once normally and once with BLADERF_DISABLE_GAIN_TABLE_BATCH=1
 * (host per-register ADI writes, no NIOS row command). PASS = equal digests
 * (checked by the caller); the tool itself fails only on I/O errors. */
#include <libbladeRF.h>
#include <bladeRF2.h>

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define RX_CH BLADERF_CHANNEL_RX(0)
#define ROWS 77u
#define REG_ADDRESS 0x130
#define REG_READ1 0x134
#define REG_CONFIG 0x137
#define START_CLOCK (1u << 1)
#define RECEIVER(x) (((x) & 0x3u) << 3)

int main(int argc, char **argv)
{
    const uint64_t target = argc > 1 ? strtoull(argv[1], NULL, 10)
                                     : UINT64_C(1835000000);
    struct bladerf *dev = NULL;
    int s;
    bool ok = false;

    if ((s = bladerf_open(&dev, NULL)) != 0) {
        fprintf(stderr, "open: %s\n", bladerf_strerror(s));
        return 1;
    }
    if ((s = bladerf_set_frequency(dev, RX_CH, UINT64_C(947500000))) != 0 ||
        (s = bladerf_set_frequency(dev, RX_CH, target)) != 0 ||
        (s = bladerf_enable_module(dev, RX_CH, false)) != 0) {
        fprintf(stderr, "tune: %s\n", bladerf_strerror(s));
        goto out;
    }
    for (unsigned rx = 1; rx <= 2; ++rx) {
        uint64_t digest = UINT64_C(1469598103934665603);
        if ((s = bladerf_set_rfic_register(
                 dev, REG_CONFIG, (uint8_t)(START_CLOCK | RECEIVER(rx)))) != 0)
            goto io;
        for (unsigned row = 0; row < ROWS; ++row) {
            uint8_t v[3];
            if ((s = bladerf_set_rfic_register(dev, REG_ADDRESS,
                                               (uint8_t)row)) != 0)
                goto io;
            for (unsigned k = 0; k < 3; ++k) {
                if ((s = bladerf_get_rfic_register(dev, REG_READ1 + k,
                                                   &v[k])) != 0)
                    goto io;
                digest = (digest ^ v[k]) * UINT64_C(1099511628211);
            }
            printf("GT rx=%u row=%u 0x%02x 0x%02x 0x%02x\n", rx, row, v[0],
                   v[1], v[2]);
        }
        printf("GT_DIGEST rx=%u frequency=%" PRIu64 " rows=%u digest=%016"
               PRIx64 "\n", rx, target, ROWS, digest);
    }
    ok = true;
io:
    if (!ok) fprintf(stderr, "register I/O: %s\n", bladerf_strerror(s));
    (void)bladerf_set_rfic_register(dev, REG_CONFIG, 0);
out:
    bladerf_close(dev);
    printf("%s\n", ok ? "DONE" : "FAIL");
    return ok ? 0 : 1;
}
