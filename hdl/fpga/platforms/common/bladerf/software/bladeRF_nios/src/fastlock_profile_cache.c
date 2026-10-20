/* This file is part of the bladeRF project:
 *   http://www.github.com/nuand/bladeRF
 *
 * Copyright (c) 2026 Nuand LLC
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
#include <string.h>

#include "devices.h"
#include "nios_pkt_16x64.h"
#include "pkt_retune2.h"
#include "fastlock_profile_cache.h"

#if defined(BOARD_BLADERF_MICRO) || defined(BLADERF_NIOS_PC_SIMULATION)

/* One host writer stages one profile at a time. The halves stay here until
 * COMMIT, so a host that dies between packets leaves the cache untouched. */
static struct {
    bool tx;
    uint8_t slot;
    uint8_t halves;
    uint8_t words[NIOS_PKT_16x64_FASTLOCK_PROFILE_LEN];
} stage;

static fastlock_profile *slot_profile(bool tx, uint8_t slot)
{
    if (slot >= NUM_BBP_FASTLOCK_PROFILES) {
        return NULL;
    }
    return tx ? &fastlocks_tx[slot] : &fastlocks_rx[slot];
}

static uint8_t commit(bool tx, uint8_t slot)
{
    const bladerf_module module = tx ? BLADERF_MODULE_TX : BLADERF_MODULE_RX;
    const bool staged = stage.halves == 0x3u && stage.tx == tx &&
                        stage.slot == slot;
    fastlock_profile *p = slot_profile(tx, slot);
    enum pkt_retune2_slot_use use;

    stage.halves = 0;
    if (!staged) {
        return NIOS_PKT_16x64_FASTLOCK_PROFILE_ERR_NOT_STAGED;
    }

    use = pkt_retune2_profile_slot_use(module, slot);
    if (use == PKT_RETUNE2_SLOT_LOADING) {
        return NIOS_PKT_16x64_FASTLOCK_PROFILE_ERR_LOADING;
    }
    if (use != PKT_RETUNE2_SLOT_FREE) {
        return NIOS_PKT_16x64_FASTLOCK_PROFILE_ERR_QUEUED;
    }
    /* No RFIC-ownership check: COMMIT only writes NIOS RAM, and the one
     * RFIC operation that reads this slot afterwards (its retune2 entry's
     * upload) is excluded above. A sweep keeps uploads or recalls of other
     * slots running almost continuously, so refusing on any RFIC owner made
     * uploads during a session impossible. ERR_RFIC_OWNED is no longer
     * returned. */

    /* The RFFE slot named by profile_num still holds this entry's previous
     * words. BBP is the only state that is true for the new words: retune2
     * reuses an RFFE copy only for BBP_RFFE/RFFE, so the next request for
     * this slot uploads before it recalls. No other entry's residency
     * changes, because no RFFE slot was written. */
    p->state = FASTLOCK_STATE_BBP;
    memcpy(p->profile_data, stage.words, sizeof(p->profile_data));
    return NIOS_PKT_16x64_FASTLOCK_PROFILE_OK;
}

uint8_t fastlock_profile_cache_write(uint16_t addr, uint64_t data)
{
    bool tx;
    uint8_t slot;
    uint8_t op;

    if (!nios_pkt_16x64_fastlock_profile_addr_unpack(addr, &tx, &slot, &op) ||
        slot >= NUM_BBP_FASTLOCK_PROFILES) {
        stage.halves = 0;
        return NIOS_PKT_16x64_FASTLOCK_PROFILE_ERR_INVALID;
    }

    if (op == NIOS_PKT_16x64_FASTLOCK_PROFILE_OP_COMMIT) {
        if (data != 0) {
            stage.halves = 0;
            return NIOS_PKT_16x64_FASTLOCK_PROFILE_ERR_INVALID;
        }
        return commit(tx, slot);
    }

    if (stage.halves != 0 && (stage.tx != tx || stage.slot != slot)) {
        stage.halves = 0;
    }
    stage.tx = tx;
    stage.slot = slot;
    nios_pkt_16x64_fastlock_profile_half_unpack(data, &stage.words[8u * op]);
    stage.halves |= (uint8_t)(1u << op);
    return NIOS_PKT_16x64_FASTLOCK_PROFILE_OK;
}

uint8_t fastlock_profile_cache_read(uint16_t addr, uint64_t *data)
{
    bool tx;
    uint8_t slot;
    uint8_t op;
    const fastlock_profile *p;

    if (data == NULL ||
        !nios_pkt_16x64_fastlock_profile_addr_unpack(addr, &tx, &slot, &op) ||
        slot >= NUM_BBP_FASTLOCK_PROFILES) {
        return NIOS_PKT_16x64_FASTLOCK_PROFILE_ERR_INVALID;
    }

    p = slot_profile(tx, slot);
    if (op == NIOS_PKT_16x64_FASTLOCK_PROFILE_OP_COMMIT) {
        *data = (uint64_t)p->state | ((uint64_t)p->profile_num << 8);
        return NIOS_PKT_16x64_FASTLOCK_PROFILE_OK;
    }
    if (p->state == FASTLOCK_STATE_INVALID) {
        return NIOS_PKT_16x64_FASTLOCK_PROFILE_ERR_EMPTY;
    }
    *data = nios_pkt_16x64_fastlock_profile_half_pack(&p->profile_data[8u * op]);
    return NIOS_PKT_16x64_FASTLOCK_PROFILE_OK;
}

#endif  // BOARD_BLADERF_MICRO || BLADERF_NIOS_PC_SIMULATION
