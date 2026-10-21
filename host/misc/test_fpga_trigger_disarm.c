/* fpga_trigger_disarm_all() clears both trigger control registers, attempts
 * both when the first write fails, and reports the first error. */
#include "driver/fpga_trigger.h"
#include "backend/backend.h"

#include <stdio.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

static int writes;
static bladerf_channel written_ch[4];
static uint8_t written_val[4];
static int fail_channel_index = -1;

static int fake_write_trigger(struct bladerf *dev, bladerf_channel ch,
                              bladerf_trigger_signal sig, uint8_t val)
{
    (void)dev; (void)sig;
    if (writes < 4) {
        written_ch[writes] = ch;
        written_val[writes] = val;
    }
    return writes++ == fail_channel_index ? BLADERF_ERR_IO : 0;
}

/* The driver only calls dev->backend->write_trigger. */
int main(void)
{
    struct backend_fns backend;
    struct bladerf dev;

    memset(&backend, 0, sizeof(backend));
    memset(&dev, 0, sizeof(dev));
    backend.write_trigger = fake_write_trigger;
    dev.backend = &backend;

    CHECK(fpga_trigger_disarm_all(&dev) == 0);
    CHECK(writes == 2);
    CHECK(written_ch[0] == BLADERF_CHANNEL_RX(0) && written_val[0] == 0);
    CHECK(written_ch[1] == BLADERF_CHANNEL_TX(0) && written_val[1] == 0);

    writes = 0;
    fail_channel_index = 0;
    CHECK(fpga_trigger_disarm_all(&dev) == BLADERF_ERR_IO);
    CHECK(writes == 2);

    puts("fpga_trigger_disarm_all: PASS");
    return 0;
}
