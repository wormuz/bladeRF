/* This file is part of the bladeRF project:
 *   http://www.github.com/nuand/bladeRF
 *
 * Copyright (C) 2026 Nuand LLC
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301
 * USA
 */
#ifndef BLADERF2_FASTLOCK_CACHE_POLICY_H_
#define BLADERF2_FASTLOCK_CACHE_POLICY_H_

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "ad9361.h"

/* Profile words the host knows a NIOS fastlock slot holds. */
struct bladerf2_nios_fastlock_words {
    uint8_t words[16];
    bool valid;
};

/* The ADI driver caches the words of each AD9361 fastlock slot when the
 * host stores a profile (ad9361_fastlock_store -> ad9361_fastlock_load) and
 * decodes that cache instead of reading the slot on readback. NIOS uploads
 * its own copy of NIOS slot n into AD9361 slot r before every retune2
 * recall, outside the driver. AD9361 slot r is shared by NIOS slots n,
 * n + 8 and by host-loaded profiles, so after such an upload the driver's
 * cache can describe a different frequency than the bank and the readback
 * fails its 1 kHz check (-EIO).
 *
 * After NIOS has recalled NIOS slot n from AD9361 slot r, the bank holds
 * n's words. Put exactly those words in the driver cache when the host
 * knows them; otherwise drop the cache entry so the driver reads the bank
 * over SPI. */
typedef char bladerf2_fastlock_words_match_driver[
    sizeof(((struct ad9361_fastlock_entry *)0)->profile_data) == 16 ? 1 : -1];

static inline void bladerf2_fastlock_cache_after_nios_recall(
    struct ad9361_fastlock_entry *entry,
    const struct bladerf2_nios_fastlock_words *nios_slot)
{
    if (entry == NULL) {
        return;
    }
    if (nios_slot == NULL || !nios_slot->valid) {
        entry->flags = 0;
        return;
    }
    entry->flags = FASTLOOK_INIT;
    entry->alc_orig = nios_slot->words[15];
    entry->alc_written = nios_slot->words[15];
    memcpy(entry->profile_data, nios_slot->words,
           sizeof(entry->profile_data));
}

#endif
