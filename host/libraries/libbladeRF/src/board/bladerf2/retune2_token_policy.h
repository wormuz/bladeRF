/* Host request tokens for RETUNE2 identify one accepted command together with
 * the FPGA session generation. Token 0 means "untracked" on the wire, so the
 * 16-bit counter skips it when it wraps. */
#ifndef BLADERF2_RETUNE2_TOKEN_POLICY_H_
#define BLADERF2_RETUNE2_TOKEN_POLICY_H_

#include <stdint.h>

static inline uint16_t bladerf2_retune2_token_next(uint16_t *counter)
{
    uint16_t token = ++*counter;

    if (token == 0) {
        token = ++*counter;
    }
    return token;
}

#endif
