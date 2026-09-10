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
#include "nios_pkt_8x32.h"
#include "pkt_8x32.h"
#include "devices.h"
#include "debug.h"

#ifdef BOARD_BLADERF_MICRO
/* Page base for pre-trigger ring reads. See the PRETRIG_READ case below:
 * the packet carries an 8-bit address and the ring is 4096 deep, so the
 * host writes a base and then reads 256 entries relative to it.
 *
 * File-scope state in a packet handler is worth being uneasy about, but the
 * alternative is a wider address field in a vendor packet format we keep
 * compatible. It is only read by PRETRIG_READ, and a stale base yields
 * samples from the wrong part of a frozen ring -- not a hang, and visible
 * because the host wrote the base it expects. */
static uint16_t pretrig_base = 0;
#endif  // BOARD_BLADERF_MICRO

static inline bool perform_read(uint8_t id, uint8_t addr, uint32_t *data)
{
    switch (id) {
        case NIOS_PKT_8x32_TARGET_VERSION:
            *data = fpga_version();
            break;

        case NIOS_PKT_8x32_TARGET_CONTROL:
            *data = control_reg_read();
            break;

        case NIOS_PKT_8x32_TARGET_ADF4351:
            DBG("Illegal read from ADF4351.\n");
            *data = 0x00;
            return false;

        case NIOS_PKT_8x32_TARGET_RFFE_CSR:
            *data = rffe_csr_read();
            break;

#ifdef BOARD_BLADERF_MICRO
        case NIOS_PKT_8x32_TARGET_ADF400X:
            *data = adf400x_spi_read(addr);
            break;
#endif  // BOARD_BLADERF_MICRO

#ifdef BOARD_BLADERF_MICRO
        case NIOS_PKT_8x32_TARGET_FASTLOCK:
            DBG("Read from AD9361 fast lock not supported.\n");
            *data = 0x00;
            return false;
#endif  // BOARD_BLADERF_MICRO

#ifdef BOARD_BLADERF_MICRO
        /* Which USB speed the GPIF buffer geometry was latched at, plus a
         * sticky bit for "the host changed it since". Reads 0 until the
         * Qsys PIO exists -- see rf_link_status_read(). */
        case NIOS_PKT_8x32_TARGET_RF_LINK_STATUS:
            *data = rf_link_status_read();
            break;
#endif  // BOARD_BLADERF_MICRO

#ifdef BOARD_BLADERF_MICRO
        /* Dwell summary and pre-trigger ring state. Reads 0 on revisions
         * that do not instantiate the PIOs, which is the honest answer
         * there: nothing frozen, nothing triggered. */
        case NIOS_PKT_8x32_TARGET_DWELL_STATUS:
            *data = dwell_status_read();
            break;

        /* One word of the latched dwell summary; addr is the word index,
         * 15 being the generation counter.
         *
         * The consistency check belongs to the caller, not here: it needs
         * the generation before and after the whole record, and this
         * protocol carries one word per packet. libbladeRF's
         * nios_dwell_summary_read does it. */
        case NIOS_PKT_8x32_TARGET_DWELL_READOUT:
            *data = dwell_word_read(addr);
            break;

        /* One entry of the pre-trigger ring.
         *
         * The packet's addr field is 8 bits (nios_pkt_8x32.h:98) and the
         * ring is 4096 deep, so addr alone reaches 1/16 of it. Widening the
         * field is not available: this is the vendor packet format and we
         * keep it compatible.
         *
         * So the index is split. A write to this target sets the base --
         * any 32-bit value, of which the low 12 bits are used -- and a read
         * returns base + addr. The host writes a base every 256 entries and
         * reads 256 words between them.
         *
         * addr is a RAW ring index, not an offset from the oldest sample.
         * The caller converts using oldest_index from DWELL_STATUS; the
         * ring wrapped, so reading 0..N-1 in order gives a rotated capture
         * that looks like a burst starting in the middle. */
        case NIOS_PKT_8x32_TARGET_PRETRIG_READ:
            *data = pretrig_read((uint16_t) (pretrig_base + addr));
            break;
#endif  // BOARD_BLADERF_MICRO

        default:
            DBG("Invalid id: 0x%x\n", id);
            *data = 0x00;
            return false;
    }

    return true;
}

static inline bool perform_write(uint8_t id, uint8_t addr, uint32_t data)
{
    switch (id) {
        case NIOS_PKT_8x32_TARGET_VERSION:
            DBG("Invalid write to version register.\n");
            return false;

        case NIOS_PKT_8x32_TARGET_CONTROL:
            control_reg_write(data);
            break;

        case NIOS_PKT_8x32_TARGET_ADF4351:
            adf4351_write(data);
            break;

        case NIOS_PKT_8x32_TARGET_RFFE_CSR:
            rffe_csr_write(data);
            break;

#ifdef BOARD_BLADERF_MICRO
        case NIOS_PKT_8x32_TARGET_ADF400X:
            adf400x_spi_write(data);
            break;
#endif  // BOARD_BLADERF_MICRO

#ifdef BOARD_BLADERF_MICRO
        case NIOS_PKT_8x32_TARGET_FASTLOCK:
            adi_fastlock_save( (addr == 1), (data >> 16), (data & 0xff));
            break;
#endif  // BOARD_BLADERF_MICRO

#ifdef BOARD_BLADERF_MICRO
        /* RF link control. addr carries the command, not a register offset,
         * so the host never has to know that bits 1..3 of the underlying
         * word are toggles -- it says "start", the firmware flips the bit.
         * Encoding the toggle state on the host would mean two writers
         * racing over one shadow. */
        /* Set the page base for subsequent ring reads. Masked to the ring
         * depth here rather than trusted: a base past the end would other-
         * wise alias to a valid entry and return a plausible wrong sample. */
        case NIOS_PKT_8x32_TARGET_PRETRIG_READ:
            pretrig_base = (uint16_t) (data & (PRETRIG_DEPTH - 1u));
            break;

        case NIOS_PKT_8x32_TARGET_RF_LINK_CFG:
            switch (addr) {
                case NIOS_PKT_8x32_RF_LINK_CMD_SET_SPEED:
                    rf_link_cfg_set_speed(data != 0);
                    break;
                case NIOS_PKT_8x32_RF_LINK_CMD_SET_TAG:
                    rf_link_cfg_set_epoch_tag((uint8_t)data);
                    break;
                case NIOS_PKT_8x32_RF_LINK_CMD_START:
                    rf_link_cfg_start();
                    break;
                case NIOS_PKT_8x32_RF_LINK_CMD_STOP:
                    rf_link_cfg_stop();
                    break;
                case NIOS_PKT_8x32_RF_LINK_CMD_CLEAR_FAULTS:
                    rf_link_cfg_clear_faults();
                    break;
                default:
                    DBG("Invalid RF link command: 0x%x\n", addr);
                    return false;
            }
            break;
#endif  // BOARD_BLADERF_MICRO

        default:
            DBG("Invalid id: 0x%x\n", id);
            return false;
    }

    return true;
}

void pkt_8x32(struct pkt_buf *b)
{
    uint8_t id;
    uint8_t addr;
    uint32_t data;
    bool is_write;
    bool success;

    nios_pkt_8x32_unpack(b->req, &id, &is_write, &addr, &data);

    if (is_write) {
        success = perform_write(id, addr, data);
    } else {
        success = perform_read(id, addr, &data);
    }

    if (!success && is_write) {
        DBG("Failed to write 0x%08x to 0x%02x\n", data, addr);
    }

    if (!success && !is_write) {
        DBG("Failed to read from 0x%02x\n", addr);
    }

    nios_pkt_8x32_resp_pack(b->resp, id, is_write, addr, data, success);
}
