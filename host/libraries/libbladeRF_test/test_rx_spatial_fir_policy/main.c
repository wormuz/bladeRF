/* Hardware-free checks of the RX spatial FIR host policy
 * (src/board/bladerf2/rx_spatial_fir_policy.h): word index and packing as
 * the fabric reads them (rx_spatial_fir.vhd), identity detection, reset
 * reasons, status decode and commit verification. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "rx_spatial_fir_policy.h"

/* Status word as bladerf_core.vhd composes it. */
static uint32_t status_word(uint8_t version, uint8_t tag, bool force,
                            bool bypass, bool identity, bool both,
                            bool sat1, bool sat2)
{
    return ((uint32_t)version << 28) | ((uint32_t)sat2 << 20) |
           ((uint32_t)sat1 << 19) | ((uint32_t)both << 18) |
           ((uint32_t)identity << 17) | ((uint32_t)bypass << 16) |
           ((uint32_t)tag << 8) | ((uint32_t)force << 7);
}

int main(void)
{
    int16_t coef[2][2][5][2];
    int16_t back[2][2][5][2];
    uint32_t words[BLADERF2_SFIR_NUM_COEF];
    unsigned int i, j, k, m;

    /* Index (i*2 + j)*5 + k, 0..19, every word once. */
    assert(bladerf2_sfir_index(0, 0, 0) == 0);
    assert(bladerf2_sfir_index(0, 1, 4) == 9);
    assert(bladerf2_sfir_index(1, 0, 2) == 12);
    assert(bladerf2_sfir_index(1, 1, 4) == 19);
    assert(BLADERF2_SFIR_NUM_COEF == 20);
    assert(BLADERF_RX_SPATIAL_FIR_ONE == BLADERF2_SFIR_ONE);
    assert(BLADERF_RX_SPATIAL_FIR_TAPS == (int)BLADERF2_SFIR_TAPS);
    assert(BLADERF_RX_SPATIAL_FIR_CENTER == (int)BLADERF2_SFIR_CENTER);

    /* [15:0] real, [31:16] imag, two's complement, including the limits. */
    assert(bladerf2_sfir_pack(16384, 0) == UINT32_C(0x00004000));
    assert(bladerf2_sfir_pack(-1, 1) == UINT32_C(0x0001ffff));
    assert(bladerf2_sfir_pack(INT16_MIN, INT16_MAX) == UINT32_C(0x7fff8000));
    for (i = 0; i < 2; i++) {
        for (j = 0; j < 2; j++) {
            for (k = 0; k < 5; k++) {
                const int v = (int)(i * 1000 + j * 100 + k * 10);
                coef[i][j][k][0] = (int16_t)(v - 20000);
                coef[i][j][k][1] = (int16_t)(-v);
            }
        }
    }
    coef[1][0][3][0] = INT16_MIN;
    coef[0][1][1][1] = INT16_MAX;
    bladerf2_sfir_pack_bank((const int16_t(*)[2][5][2])coef, words);
    assert(words[bladerf2_sfir_index(1, 0, 3)] ==
           bladerf2_sfir_pack(INT16_MIN, coef[1][0][3][1]));
    memset(back, 0x55, sizeof(back));
    bladerf2_sfir_unpack_bank(words, back);
    assert(memcmp(coef, back, sizeof(coef)) == 0);

    /* Identity: h00[2] = h11[2] = 1.0, everything else 0 (fabric rule). */
    bladerf2_sfir_identity_bank(words);
    assert(bladerf2_sfir_bank_is_identity(words));
    for (m = 0; m < BLADERF2_SFIR_NUM_COEF; m++) {
        const bool centre = m == 2 || m == 17;
        assert(words[m] == (centre ? UINT32_C(0x4000) : 0));
    }
    words[2] |= UINT32_C(1) << 16; /* 1.0 + j/16384: not identity */
    assert(!bladerf2_sfir_bank_is_identity(words));
    bladerf2_sfir_identity_bank(words);
    words[bladerf2_sfir_index(0, 1, 2)] = UINT32_C(0x4000); /* swap term */
    assert(!bladerf2_sfir_bank_is_identity(words));
    memset(words, 0, sizeof(words));
    assert(!bladerf2_sfir_bank_is_identity(words));

    /* Reset reasons from design section 4; others leave W in place. */
    assert(bladerf2_sfir_reset_required(BLADERF_RF_INVALIDATE_FREQUENCY));
    assert(bladerf2_sfir_reset_required(BLADERF_RF_INVALIDATE_SAMPLE_RATE));
    assert(bladerf2_sfir_reset_required(BLADERF_RF_INVALIDATE_BANDWIDTH));
    assert(bladerf2_sfir_reset_required(BLADERF_RF_INVALIDATE_GAIN));
    assert(bladerf2_sfir_reset_required(BLADERF_RF_INVALIDATE_GAIN_MODE));
    assert(bladerf2_sfir_reset_required(BLADERF_RF_INVALIDATE_RF_PORT));
    assert(bladerf2_sfir_reset_required(BLADERF_RF_INVALIDATE_CORRECTION));
    assert(bladerf2_sfir_reset_required(BLADERF_RF_INVALIDATE_RX_FIR));
    assert(bladerf2_sfir_reset_required(BLADERF_RF_INVALIDATE_CLOCK));
    assert(bladerf2_sfir_reset_required(BLADERF_RF_INVALIDATE_DEVICE_RESET));
    assert(bladerf2_sfir_reset_required(BLADERF_RF_INVALIDATE_FPGA_RELOAD));
    assert(!bladerf2_sfir_reset_required(BLADERF_RF_INVALIDATE_RX_MUX));
    assert(!bladerf2_sfir_reset_required(BLADERF_RF_INVALIDATE_TRIGGER));
    assert(!bladerf2_sfir_reset_required(BLADERF_RF_INVALIDATE_MODULE));
    assert(!bladerf2_sfir_reset_required(0));
    assert(bladerf2_sfir_reset_required(BLADERF_RF_INVALIDATE_TRIGGER |
                                        BLADERF_RF_INVALIDATE_GAIN));

    /* Commit tags never take the fabric-reset value 0. */
    assert(bladerf2_sfir_tag_for_generation(0) == 1);
    assert(bladerf2_sfir_tag_for_generation(1) == 2);
    assert(bladerf2_sfir_tag_for_generation(254) == 255);
    assert(bladerf2_sfir_tag_for_generation(255) == 1);
    for (m = 0; m < 2000; m++) {
        assert(bladerf2_sfir_tag_for_generation(m) != 0);
        assert(bladerf2_sfir_tag_for_generation(m) !=
               bladerf2_sfir_tag_for_generation(m + 1));
    }

    /* Status decode, bit by bit. */
    {
        const uint32_t w = status_word(1, 0xa5, true, true, false, true,
                                       true, false) |
                           0x40u | 0x13u;
        const struct bladerf2_sfir_status_fields f =
            bladerf2_sfir_status_decode(w);
        assert(f.version == 1 && f.tag == 0xa5 && f.force_identity &&
               f.bypass && !f.identity && f.both_lanes && f.sat_rx1 &&
               !f.sat_rx2 && f.commit_echo && !f.write_echo &&
               f.last_index == 0x13);
        assert(bladerf2_sfir_status_supported(w));
        assert(bladerf2_sfir_status_decode(UINT32_C(1) << 20).sat_rx2);
        assert(bladerf2_sfir_status_decode(UINT32_C(1) << 5).write_echo);
    }
    /* Version 0: PIOs present (or zero) without the block. */
    assert(!bladerf2_sfir_status_supported(0));
    assert(!bladerf2_sfir_status_supported(
        status_word(0, 0, false, true, true, false, false, false)));
    assert(!bladerf2_sfir_status_supported(
        status_word(2, 0, false, true, true, false, false, false)));

    /* Commit verification rejects every mismatch. */
    assert(bladerf2_sfir_commit_verify(
               status_word(1, 7, false, false, false, true, false, false),
               7, false) == 0);
    assert(bladerf2_sfir_commit_verify(
               status_word(1, 7, false, true, true, true, false, false),
               7, true) == 0);
    assert(bladerf2_sfir_commit_verify(
               status_word(1, 6, false, false, false, true, false, false),
               7, false) == BLADERF_ERR_UNEXPECTED);
    assert(bladerf2_sfir_commit_verify(
               status_word(1, 7, false, true, true, true, false, false),
               7, false) == BLADERF_ERR_UNEXPECTED);
    assert(bladerf2_sfir_commit_verify(
               status_word(1, 7, true, true, false, true, false, false),
               7, false) == BLADERF_ERR_UNEXPECTED);
    assert(bladerf2_sfir_commit_verify(
               status_word(0, 7, false, false, false, true, false, false),
               7, false) == BLADERF_ERR_UNSUPPORTED);
    /* A fabric reset after the commit (tag 0, identity) is not the commit. */
    assert(bladerf2_sfir_commit_verify(
               status_word(1, 0, false, true, true, true, false, false),
               7, false) == BLADERF_ERR_UNEXPECTED);

    puts("RX spatial FIR policy: PASS");
    return 0;
}
