/* This file is part of the bladeRF project:
 *   http://www.github.com/nuand/bladeRF
 *
 * Copyright (c) 2015 Nuand LLC
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <stdint.h>
#include <stdbool.h>
#include "pkt_handler.h"
#include "pkt_8x64.h"
#include "devices.h"
#include "debug.h"

static inline bool perform_write(uint8_t id, uint8_t addr, uint64_t data)
{
    switch (id) {
        case NIOS_PKT_8x64_TARGET_TIMESTAMP:
            DBG("Invalid write access to timestamp: 0x%x\n", addr);
            return false;

        /* Add user customizations here

        case NIOS_PKT_8x64_TARGET_USR1:
        ...
        case NIOS_PKT_8x64_TARGET_USR128:

        */

        default:
            DBG("Unknown ID: 0x%x\n", id);
            return false;
    }
}

static inline bool read_timestamp(uint8_t addr, uint64_t *data)
{
    switch (addr) {
        case NIOS_PKT_8x64_TIMESTAMP_RX:
            return time_tamer_read(BLADERF_MODULE_RX, data);

        case NIOS_PKT_8x64_TIMESTAMP_TX:
            return time_tamer_read(BLADERF_MODULE_TX, data);

        default:
            DBG("Invalid addr: 0x%x\n", addr);
            return false;
    }

    return true;
}

static inline bool perform_read(uint8_t id, uint8_t addr, uint64_t *data)
{
    bool success;

    switch (id) {
        case NIOS_PKT_8x64_TARGET_TIMESTAMP:
            success = read_timestamp(addr, data);
            break;

#ifdef BOARD_BLADERF_MICRO
        case NIOS_PKT_8x64_TARGET_RFIC_TIMING:
            success = rfic_command_init_timing_read(addr, data);
            break;
#endif

        /* Add user customizations here

        case NIOS_PKT_8x64_TARGET_USR1:
        ...
        case NIOS_PKT_8x64_TARGET_USR128:

        */

        default:
            DBG("Invalid ID: 0x%x\n", id);
            success = false;

    }

    return success;
}

void pkt_8x64(struct pkt_buf *b)
{
    uint8_t id;
    uint8_t addr;
    uint64_t data;
    bool    is_write;
    bool    success;

    nios_pkt_8x64_unpack(b->req, &id, &is_write, &addr, &data);

    if (is_write) {
        success = perform_write(id, addr, data);
    } else {
        success = perform_read(id, addr, &data);
    }

    nios_pkt_8x64_resp_pack(b->resp, id, is_write, addr, data, success);
}
