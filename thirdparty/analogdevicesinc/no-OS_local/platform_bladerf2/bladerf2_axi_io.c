/*
 * no_os_axi_io implementation for the bladeRF2.
 *
 * The no-OS axi_adc/axi_dac cores expect memory-mapped register access
 * through no_os_axi_io_read/write(base, offset, ...). On this board the ADI
 * AXI interface core lives behind the NIOS (NIOS_PKT_32x32_TARGET_ADI_AXI),
 * so the accessors forward to the backend adi_axi_read/write. The API has no
 * context argument, so the device handle is thread-local and rebound by each
 * host RFIC callback that can reach AXI (including init, FIR reconfiguration,
 * rate changes and digital tuning). Separate handles may operate concurrently,
 * so a process-global pointer would route one device's traffic through
 * another device's backend.
 */
#include <errno.h>
#include <stdio.h>
#include <stdint.h>

#include "board/board.h"

#include "bladerf2_status.h"
#include "no_os_axi_io.h"
#include "log.h"

#if defined(_MSC_VER)
static __declspec(thread) struct bladerf *axi_io_dev;
#else
static __thread struct bladerf *axi_io_dev;
#endif

void bladerf2_axi_io_set_dev(struct bladerf *dev)
{
    axi_io_dev = dev;
}

int32_t no_os_axi_io_read(uint32_t base, uint32_t offset, uint32_t *data)
{
    uint64_t address = (uint64_t)base + offset;

    if (NULL == data) {
        return -EINVAL;
    }
    *data = 0;
    if (NULL == axi_io_dev) {
        return -ENODEV;
    }
    if (address > UINT32_MAX) {
        return -ERANGE;
    }

    int status = axi_io_dev->backend->adi_axi_read(
        axi_io_dev, (uint32_t)address, data);
    if (status < 0) {
        log_error("AD9361 AXI read failed (address=0x%08x, status=%d)\n",
                  (uint32_t)address, status);
        /* Some backends can partially write the output before reporting a
         * transport failure. Never let that value escape as register data. */
        *data = 0;
        return bladerf2_status_to_errno(status);
    }

    return 0;
}

int32_t no_os_axi_io_write(uint32_t base, uint32_t offset, uint32_t data)
{
    uint64_t address = (uint64_t)base + offset;

    if (NULL == axi_io_dev) {
        return -ENODEV;
    }
    if (address > UINT32_MAX) {
        return -ERANGE;
    }

    int status = axi_io_dev->backend->adi_axi_write(
        axi_io_dev, (uint32_t)address, data);
    if (status < 0) {
        log_error("AD9361 AXI write failed (address=0x%08x, value=0x%08x, status=%d)\n",
                  (uint32_t)address, data, status);
        return bladerf2_status_to_errno(status);
    }

    return 0;
}
