/*
 * This file is part of the bladeRF project:
 *   http://www.github.com/nuand/bladeRF
 *
 * Copyright (C) 2026 Nuand LLC
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
#include <assert.h>
#include <string.h>
#include <stdint.h>

#include "host_config.h"
#include "metadata.h"

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

int main(void)
{
    const uint64_t near_wrap = UINT64_MAX - 2;

    /* TX BURST_START / UPDATE_TIMESTAMP: a small forward distance across
     * uint64 wrap is future, while the preceding tick is in the past. */
    CHECK(metadata_timestamp_at_or_after(1, near_wrap));
    CHECK(!metadata_timestamp_at_or_after(UINT64_MAX - 3, near_wrap));

    /* Equality is admissible. Zero is a valid timestamp, not an unset value. */
    CHECK(metadata_timestamp_at_or_after(0, 0));
    CHECK(metadata_timestamp_at_or_after(0, UINT64_MAX));

    /* The opposite direction across wrap is correctly classified as past. */
    CHECK(!metadata_timestamp_at_or_after(UINT64_MAX, 0));

    /* Exactly half-range has no unambiguous ordering and must fail closed. */
    CHECK(!metadata_timestamp_at_or_after(UINT64_C(1) << 63, 0));

    CHECK(metadata_tx_start_timestamp_valid(
        BLADERF_META_FLAG_TX_BURST_START, 101, 100));
    CHECK(!metadata_tx_start_timestamp_valid(
        BLADERF_META_FLAG_TX_BURST_START, 99, 100));
    CHECK(metadata_tx_start_timestamp_valid(
        BLADERF_META_FLAG_TX_BURST_START | BLADERF_META_FLAG_TX_NOW,
        99, 100));
    CHECK(metadata_tx_start_timestamp_valid(
        BLADERF_META_FLAG_TX_BURST_END, 99, 100));
    CHECK(metadata_tx_start_timestamp_valid(
        BLADERF_META_FLAG_TX_BURST_START, 0, UINT64_MAX));
    CHECK(!metadata_tx_start_timestamp_valid(
        BLADERF_META_FLAG_TX_BURST_START, UINT64_MAX, 0));

    return 0;
}
