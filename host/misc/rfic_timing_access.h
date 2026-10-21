/* Host access to the NIOS RFIC timing target (NIOS_PKT_8x64_TARGET_RFIC_TIMING):
 * init_timing counters (cold INIT) and the host-armed operation window. Needs
 * the libbladeRF internal include paths (see run_rfic_init_timing_live.sh). */
#ifndef RFIC_TIMING_ACCESS_H_
#define RFIC_TIMING_ACCESS_H_

#include <libbladeRF.h>
#include "backend/usb/usb.h"
#include "board/board.h"
#include "nios_pkt_8x64.h"

#include <stdbool.h>
#include <stdint.h>

static int rfic_timing_xfer(struct bladerf *dev, bool is_write, uint8_t addr,
                            uint64_t *value)
{
    struct bladerf_usb *usb = dev->backend_data;
    uint8_t packet[NIOS_PKT_8x64_LEN];
    uint32_t transferred = 0;
    bool transferred_valid = false;
    bool success = false;
    uint8_t target = 0;
    uint8_t response_addr = 0;
    int status;

    nios_pkt_8x64_pack(packet, NIOS_PKT_8x64_TARGET_RFIC_TIMING, is_write,
                       addr, is_write ? *value : 0);
    status = usb->fn->bulk_transfer(usb->driver, PERIPHERAL_EP_OUT, packet,
                                    sizeof(packet), 0, &transferred,
                                    &transferred_valid);
    if (status != 0 || !transferred_valid || transferred != sizeof(packet))
        return status != 0 ? status : BLADERF_ERR_UNEXPECTED;

    transferred = 0;
    transferred_valid = false;
    status = usb->fn->bulk_transfer(usb->driver, PERIPHERAL_EP_IN, packet,
                                    sizeof(packet), 0, &transferred,
                                    &transferred_valid);
    if (status != 0 || !transferred_valid || transferred != sizeof(packet))
        return status != 0 ? status : BLADERF_ERR_UNEXPECTED;

    nios_pkt_8x64_resp_unpack(packet, &target, NULL, &response_addr, value,
                              &success);
    if (!success || target != NIOS_PKT_8x64_TARGET_RFIC_TIMING ||
        response_addr != addr)
        return BLADERF_ERR_FPGA_OP;
    return 0;
}

static int read_metric(struct bladerf *dev, uint8_t addr, uint64_t *value)
{
    return rfic_timing_xfer(dev, false, addr, value);
}

static int write_metric(struct bladerf *dev, uint8_t addr, uint64_t value)
{
    return rfic_timing_xfer(dev, true, addr, &value);
}

#endif
