/*
 * This file is part of the bladeRF project:
 *   http://www.github.com/nuand/bladeRF
 *
 * Copyright (C) 2014 Nuand LLC
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef STREAMING_FORMAT_H_
#define STREAMING_FORMAT_H_

#include <stdint.h>

#include "rel_assert.h"

/*
 * Convert SC8Q8 samples to bytes
 */
static inline size_t sc8q7_to_bytes(size_t n_samples)
{
    const size_t sample_size = 2 * sizeof(int8_t);
    assert(n_samples <= (SIZE_MAX / sample_size));
    return n_samples * sample_size;
}

/*
 * Convert bytes to SC8Q8 samples
 */
static inline size_t bytes_to_sc8q7(size_t n_bytes)
{
    const size_t sample_size = 2 * sizeof(int8_t);
    assert((n_bytes % sample_size) == 0);
    return n_bytes / sample_size;
}

/*
 * Convert SC16Q11 samples to bytes
 */
static inline size_t sc16q11_to_bytes(size_t n_samples)
{
    const size_t sample_size = 2 * sizeof(int16_t);
    assert(n_samples <= (SIZE_MAX / sample_size));
    return n_samples * sample_size;
}

/*
 * Convert SC16Q11 packed (SC12Q11) samples to bytes
 * I and Q are packed into 3 bytes. I and Q are 12 bits each.
 * That makes a sample 24 bits = 3 bytes.
 */
static inline size_t sc16q11_packed_to_bytes(size_t n_samples)
{
    const size_t sample_size = 3*sizeof(int8_t);
    assert(n_samples <= (SIZE_MAX / sample_size));
    return n_samples * sample_size;
}

/* Pack signed SC16 Q11 I/Q pairs into the little-endian 12-bit wire format.
 * Process one complex sample per iteration so odd caller write sizes are
 * safe, and use byte stores to avoid alignment and signed-shift assumptions.
 */
static inline void sc16q11_pack_samples(const int16_t *src, uint8_t *dst,
                                        size_t n_samples)
{
    size_t i;

    for (i = 0; i < n_samples; ++i) {
        const uint16_t i_sample = (uint16_t)src[2 * i] & 0x0fff;
        const uint16_t q_sample = (uint16_t)src[2 * i + 1] & 0x0fff;
        uint8_t *packed_sample = dst + 3 * i;

        packed_sample[0] = (uint8_t)i_sample;
        packed_sample[1] =
            (uint8_t)((i_sample >> 8) | ((q_sample & 0x0f) << 4));
        packed_sample[2] = (uint8_t)(q_sample >> 4);
    }
}

/*
 * Convert bytes to SC16Q11 samples
 */
static inline size_t bytes_to_sc16q11(size_t n_bytes)
{
    const size_t sample_size = 2 * sizeof(int16_t);
    assert((n_bytes % sample_size) == 0);
    return n_bytes / sample_size;
}

/* Covert samples to bytes based upon the provided format */
static inline size_t samples_to_bytes(bladerf_format format, size_t n)
{
    switch (format) {
        case BLADERF_FORMAT_SC8_Q7:
        case BLADERF_FORMAT_SC8_Q7_META:
            return sc8q7_to_bytes(n);

        case BLADERF_FORMAT_SC16_Q11_PACKED:
            return sc16q11_packed_to_bytes(n);

        case BLADERF_FORMAT_SC16_Q11:
        case BLADERF_FORMAT_SC16_Q11_META:
            return sc16q11_to_bytes(n);

        case BLADERF_FORMAT_PACKET_META:
            return n*4;

        default:
            assert(!"Invalid format");
            return 0;
    }
}

/* Convert bytes to samples based upon the provided format */
static inline size_t bytes_to_samples(bladerf_format format, size_t n)
{
    switch (format) {
        case BLADERF_FORMAT_SC8_Q7:
        case BLADERF_FORMAT_SC8_Q7_META:
            return bytes_to_sc8q7(n);

        case BLADERF_FORMAT_SC16_Q11_PACKED:
        case BLADERF_FORMAT_SC16_Q11:
        case BLADERF_FORMAT_SC16_Q11_META:
            return bytes_to_sc16q11(n);

        case BLADERF_FORMAT_PACKET_META:
            return (n+3)/4;

        default:
            assert(!"Invalid format");
            return 0;
    }
}

#endif
