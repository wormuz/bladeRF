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

#include "devices.h"

#include "no_os_axi_io.h"

int32_t no_os_axi_io_read(uint32_t base, uint32_t offset, uint32_t *data)
{
    if (NULL == data) {
        return -1;
    }

    *data = adi_axi_read((uint16_t)(base + offset));

    return 0;
}

int32_t no_os_axi_io_write(uint32_t base, uint32_t offset, uint32_t data)
{
    adi_axi_write((uint16_t)(base + offset), data);

    return 0;
}
