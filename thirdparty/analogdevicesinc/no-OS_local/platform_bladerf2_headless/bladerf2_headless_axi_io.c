/*
 * no_os_axi_io implementation for the bladeRF2 Nios softcore.
 *
 * The no-OS axi_adc/axi_dac cores expect memory-mapped register access
 * through no_os_axi_io_read/write(base, offset, ...). On the Nios build this
 * is a direct memory-mapped read/write over the Avalon bus, already
 * implemented as adi_axi_read/adi_axi_write in devices.c (common bladeRF_nios
 * app code, IORD_32DIRECT against AXI_AD9361_0_BASE). This file only adapts
 * the no-OS signature to that existing primitive - unlike the host variant
 * (bladerf2_axi_io.c), there is no USB backend or device handle involved.
 */
#include <stdint.h>
#include <errno.h>

#include "devices.h"

#include "no_os_axi_io.h"

int32_t no_os_axi_io_read(uint32_t base, uint32_t offset, uint32_t *data)
{
    uint32_t value;

    if (NULL == data) {
        return -EINVAL;
    }

    value = adi_axi_read((uint16_t)(base + offset));
    if (value == UINT32_C(0xDEADDEAD)) {
        return -EIO;
    }

    *data = value;

    return 0;
}

int32_t no_os_axi_io_write(uint32_t base, uint32_t offset, uint32_t data)
{
    adi_axi_write((uint16_t)(base + offset), data);

    /* The current ADI up_axi write timeout is returned as AXI OKAY, so this
     * callback cannot distinguish it. A readback/status check is required by
     * callers that need to certify writes. */
    return 0;
}
