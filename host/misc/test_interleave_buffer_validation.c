/* Regression coverage for malformed RX_X2/TX_X2 interleave buffers. */
#include <stdio.h>

#include <libbladeRF.h>
#include "helpers/interleave.h"

int main(void)
{
    int16_t iq[] = {1, 2, 3, 4, 5, 6, 7, 8};
    const int16_t expected[] = {1, 2, 5, 6, 3, 4, 7, 8};
    int8_t short_meta[16] = {0};

    if (_interleave_interleave_buf(BLADERF_RX_X2,
                                   BLADERF_FORMAT_SC16_Q11, 3, iq) !=
        BLADERF_ERR_INVAL) {
        fprintf(stderr, "odd X2 sample count was accepted\n");
        return 1;
    }

    if (_interleave_interleave_buf(BLADERF_RX_X2,
                                   BLADERF_FORMAT_SC8_Q7_META, 2,
                                   short_meta) != BLADERF_ERR_INVAL) {
        fprintf(stderr, "undersized X2 META buffer was accepted\n");
        return 1;
    }

    if (_interleave_interleave_buf(BLADERF_RX_X2,
                                   BLADERF_FORMAT_SC16_Q11, 4, NULL) !=
        BLADERF_ERR_INVAL) {
        fprintf(stderr, "NULL sample buffer was accepted\n");
        return 1;
    }

    if (_interleave_interleave_buf(BLADERF_RX_X2,
                                   BLADERF_FORMAT_SC16_Q11, 4, iq) != 0) {
        fprintf(stderr, "valid X2 interleave failed\n");
        return 1;
    }
    for (size_t i = 0; i < sizeof(iq) / sizeof(iq[0]); ++i) {
        if (iq[i] != expected[i]) {
            fprintf(stderr, "interleave output mismatch at %zu\n", i);
            return 1;
        }
    }

    if (_interleave_deinterleave_buf(BLADERF_TX_X2,
                                     BLADERF_FORMAT_SC16_Q11, 4, iq) != 0) {
        fprintf(stderr, "valid X2 deinterleave failed\n");
        return 1;
    }
    if (iq[0] != 1 || iq[1] != 2 || iq[2] != 3 || iq[3] != 4 ||
        iq[4] != 5 || iq[5] != 6 || iq[6] != 7 || iq[7] != 8) {
        fprintf(stderr, "deinterleave did not restore channel-major input\n");
        return 1;
    }

    return 0;
}
