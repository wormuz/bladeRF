/*
 * This file is part of the bladeRF project.
 * Copyright (C) 2026 bladeRF project contributors
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

/* RX spatial FIR (doc/development/rx-spatial-mimo-fir-design.md): pure
 * packing, identity, reset-reason and status-word rules shared by bladerf2.c
 * and its hardware-free unit test. */

#ifndef BLADERF2_RX_SPATIAL_FIR_POLICY_H_
#define BLADERF2_RX_SPATIAL_FIR_POLICY_H_

#include <stdbool.h>
#include <stdint.h>

#include <libbladeRF.h>

#include "nios_pkt_8x32.h"

#define BLADERF2_SFIR_LANES    2u
#define BLADERF2_SFIR_TAPS     5u
#define BLADERF2_SFIR_CENTER   2u
#define BLADERF2_SFIR_NUM_COEF NIOS_PKT_8x32_SFIR_NUM_COEF
#define BLADERF2_SFIR_ONE      16384 /* Q2.14 */

/* Word index used by the fabric: output lane i, input lane j, tap k. */
static inline unsigned int bladerf2_sfir_index(unsigned int i, unsigned int j,
                                               unsigned int k)
{
    return (i * BLADERF2_SFIR_LANES + j) * BLADERF2_SFIR_TAPS + k;
}

/* [15:0] real, [31:16] imag, two's complement Q2.14. */
static inline uint32_t bladerf2_sfir_pack(int16_t re, int16_t im)
{
    return (uint32_t)(uint16_t)re | ((uint32_t)(uint16_t)im << 16);
}

static inline void bladerf2_sfir_unpack(uint32_t word, int16_t *re,
                                        int16_t *im)
{
    *re = (int16_t)(uint16_t)(word & 0xFFFFu);
    *im = (int16_t)(uint16_t)(word >> 16);
}

static inline void bladerf2_sfir_pack_bank(const int16_t coef[2][2][5][2],
                                           uint32_t words[])
{
    unsigned int i, j, k;
    for (i = 0; i < BLADERF2_SFIR_LANES; i++) {
        for (j = 0; j < BLADERF2_SFIR_LANES; j++) {
            for (k = 0; k < BLADERF2_SFIR_TAPS; k++) {
                words[bladerf2_sfir_index(i, j, k)] =
                    bladerf2_sfir_pack(coef[i][j][k][0], coef[i][j][k][1]);
            }
        }
    }
}

static inline void bladerf2_sfir_unpack_bank(const uint32_t words[],
                                             int16_t coef[2][2][5][2])
{
    unsigned int i, j, k;
    for (i = 0; i < BLADERF2_SFIR_LANES; i++) {
        for (j = 0; j < BLADERF2_SFIR_LANES; j++) {
            for (k = 0; k < BLADERF2_SFIR_TAPS; k++) {
                bladerf2_sfir_unpack(words[bladerf2_sfir_index(i, j, k)],
                                     &coef[i][j][k][0], &coef[i][j][k][1]);
            }
        }
    }
}

/* Identity word as the fabric defines it: h00[2] = h11[2] = 1.0 + 0j. */
static inline uint32_t bladerf2_sfir_identity_word(unsigned int idx)
{
    if (idx == bladerf2_sfir_index(0, 0, BLADERF2_SFIR_CENTER) ||
        idx == bladerf2_sfir_index(1, 1, BLADERF2_SFIR_CENTER)) {
        return bladerf2_sfir_pack(BLADERF2_SFIR_ONE, 0);
    }
    return 0;
}

static inline void bladerf2_sfir_identity_bank(uint32_t words[])
{
    unsigned int m;
    for (m = 0; m < BLADERF2_SFIR_NUM_COEF; m++) {
        words[m] = bladerf2_sfir_identity_word(m);
    }
}

static inline bool bladerf2_sfir_bank_is_identity(const uint32_t words[])
{
    unsigned int m;
    for (m = 0; m < BLADERF2_SFIR_NUM_COEF; m++) {
        if (words[m] != bladerf2_sfir_identity_word(m)) {
            return false;
        }
    }
    return true;
}

/* Taps are in samples and W is estimated per gain/LO/filter state, so every
 * reason that changes one of those invalidates W (design section 4). */
#define BLADERF2_SFIR_RESET_MASK                                         \
    (BLADERF_RF_INVALIDATE_FREQUENCY | BLADERF_RF_INVALIDATE_SAMPLE_RATE | \
     BLADERF_RF_INVALIDATE_BANDWIDTH | BLADERF_RF_INVALIDATE_GAIN |       \
     BLADERF_RF_INVALIDATE_GAIN_MODE | BLADERF_RF_INVALIDATE_RF_PORT |    \
     BLADERF_RF_INVALIDATE_CORRECTION | BLADERF_RF_INVALIDATE_RX_FIR |    \
     BLADERF_RF_INVALIDATE_CLOCK | BLADERF_RF_INVALIDATE_DEVICE_RESET |   \
     BLADERF_RF_INVALIDATE_FPGA_RELOAD)

static inline bool bladerf2_sfir_reset_required(uint32_t reason)
{
    return (reason & BLADERF2_SFIR_RESET_MASK) != 0;
}

/* Commit tags are 8 bits; 0 is what a fabric reset reports, so the host
 * never uses it for a commit. */
static inline uint8_t bladerf2_sfir_tag_for_generation(uint32_t generation)
{
    uint8_t tag = (uint8_t)(generation % 255u);
    return (uint8_t)(tag + 1u);
}

struct bladerf2_sfir_status_fields {
    uint8_t version;
    uint8_t last_index;
    bool write_echo;
    bool commit_echo;
    bool force_identity;
    uint8_t tag;
    bool bypass;
    bool identity;
    bool both_lanes;
    bool sat_rx1;
    bool sat_rx2;
};

static inline struct bladerf2_sfir_status_fields
bladerf2_sfir_status_decode(uint32_t word)
{
    struct bladerf2_sfir_status_fields f;
    f.version = (uint8_t)((word >> NIOS_PKT_8x32_SFIR_STATUS_VERSION_SHIFT) &
                          NIOS_PKT_8x32_SFIR_STATUS_VERSION_MASK);
    f.last_index = (uint8_t)(word & NIOS_PKT_8x32_SFIR_STATUS_IDX_MASK);
    f.write_echo = (word & NIOS_PKT_8x32_SFIR_STATUS_WR_ECHO) != 0;
    f.commit_echo = (word & NIOS_PKT_8x32_SFIR_STATUS_COMMIT_ECHO) != 0;
    f.force_identity = (word & NIOS_PKT_8x32_SFIR_STATUS_FORCE) != 0;
    f.tag = (uint8_t)((word >> NIOS_PKT_8x32_SFIR_STATUS_TAG_SHIFT) &
                      NIOS_PKT_8x32_SFIR_STATUS_TAG_MASK);
    f.bypass = (word & NIOS_PKT_8x32_SFIR_STATUS_BYPASS) != 0;
    f.identity = (word & NIOS_PKT_8x32_SFIR_STATUS_IDENTITY) != 0;
    f.both_lanes = (word & NIOS_PKT_8x32_SFIR_STATUS_BOTH_LANES) != 0;
    f.sat_rx1 = (word & NIOS_PKT_8x32_SFIR_STATUS_SAT_RX1) != 0;
    f.sat_rx2 = (word & NIOS_PKT_8x32_SFIR_STATUS_SAT_RX2) != 0;
    return f;
}

/* Version 0 is gateware that has the PIOs but not the block (or reads 0). */
static inline bool bladerf2_sfir_status_supported(uint32_t word)
{
    return bladerf2_sfir_status_decode(word).version ==
           NIOS_PKT_8x32_SFIR_VERSION;
}

/* Under the RX epoch contract a commit or identity reset is an RX
 * transaction, and with the FIR in the chain the gate sees COMPLETE only
 * after TAPS-1 ADC samples. With RX disabled none arrive: the transaction
 * would wait out its timeout and fall back to forced identity. Refuse it
 * before touching the fabric instead. */
static inline int bladerf2_sfir_fence_ready(bool contract_enabled,
                                            bool rx_enabled)
{
    return (contract_enabled && !rx_enabled) ? BLADERF_ERR_NOT_INIT : 0;
}

/* After NIOS acknowledged a commit: the fabric must report this tag, the
 * identity bit that matches the bank, and no force_identity level. */
static inline int bladerf2_sfir_commit_verify(uint32_t word, uint8_t tag,
                                              bool bank_identity)
{
    const struct bladerf2_sfir_status_fields f =
        bladerf2_sfir_status_decode(word);

    if (f.version != NIOS_PKT_8x32_SFIR_VERSION) {
        return BLADERF_ERR_UNSUPPORTED;
    }
    if (f.tag != tag || f.identity != bank_identity || f.force_identity) {
        return BLADERF_ERR_UNEXPECTED;
    }
    return 0;
}

#endif
