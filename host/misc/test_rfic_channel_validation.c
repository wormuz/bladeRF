/* Exercise the exact channel predicate used by the NIOS RFIC dispatcher. */
#include <stdio.h>

#include "rfic_channel_validation.h"

int main(void)
{
    static const bladerf_channel valid[] = {
        BLADERF_CHANNEL_RX(0),
        BLADERF_CHANNEL_TX(0),
        BLADERF_CHANNEL_RX(1),
        BLADERF_CHANNEL_TX(1),
        RFIC_SYSTEM_CHANNEL,
    };
    static const bladerf_channel invalid[] = {
        BLADERF_CHANNEL_INVALID,
        4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
        0x10f,
    };

    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); ++i) {
        if (!_rfic_chan_is_valid(valid[i])) {
            fprintf(stderr, "valid RFIC channel %d rejected\n", valid[i]);
            return 1;
        }
    }

    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        if (_rfic_chan_is_valid(invalid[i])) {
            fprintf(stderr, "invalid RFIC channel %d accepted\n", invalid[i]);
            return 1;
        }
    }

    if (_rfic_chan_is_system(BLADERF_CHANNEL_RX(0)) ||
        !_rfic_chan_is_system(RFIC_SYSTEM_CHANNEL)) {
        fprintf(stderr, "RFIC system-channel classification failed\n");
        return 1;
    }

    return 0;
}
