/* Read-only probe: is the NIOS RFIC command queue alive?
 *
 * Answers one question without ever calling bladerf_set_tuning_mode(), which
 * is what wedged the board on 2026-09-19. Reads BLADERF_RFIC_COMMAND_STATUS
 * over pkt_16x64/TARGET_RFIC and prints the raw 64-bit word.
 *
 * Expected readings (rfic_fpga.c:84-97, devices_rfic.c:189-223):
 *   0xDEADBEEF0B4DF00D  handler alive, RFIC not initialized on NIOS.
 *                       Normal in TUNING_MODE_HOST, and already proves the
 *                       16x64 bus works and polling alone does not wedge it.
 *   bit 0 set           rfic_initialized
 *   bits 8-15           write_queue_length
 *
 * ponytail: no argument parsing, no loop options. One read, one line.
 */
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>

#include <libbladeRF.h>

#include "board/board.h"
#include "backend/backend.h"

/* Literal rather than including bladerf2_common.h, which drags in the whole
 * ADI ad9361_api tree. Value read from fpga_common/include/bladerf2_common.h:205
 * (BLADERF_RFIC_COMMAND_STATUS = 0x00). */
#define RFIC_COMMAND_STATUS 0x00

#define RFIC_ADDRESS(cmd, ch) ((cmd & 0xFF) + ((ch & 0xF) << 8))

int main(int argc, char *argv[])
{
    struct bladerf *dev = NULL;
    uint64_t sreg       = 0;
    uint16_t addr;
    int status;

    status = bladerf_open(&dev, argc > 1 ? argv[1] : NULL);
    if (status < 0) {
        fprintf(stderr, "open: %s\n", bladerf_strerror(status));
        return 1;
    }

    addr = RFIC_ADDRESS(RFIC_COMMAND_STATUS, BLADERF_CHANNEL_INVALID);

    status = dev->backend->rfic_command_read(dev, addr, &sreg);
    if (status < 0) {
        fprintf(stderr, "rfic_command_read: %s\n", bladerf_strerror(status));
        bladerf_close(dev);
        return 2;
    }

    printf("raw=0x%016" PRIx64 " initialized=%u wqsuccess=%u stage=%u "
           "write_queue_length=%u\n",
           sreg, (unsigned)(sreg & 0x1), (unsigned)((sreg >> 1) & 0x1),
           (unsigned)((sreg >> 2) & 0x3F), (unsigned)((sreg >> 8) & 0xFF));

    bladerf_close(dev);
    return 0;
}
