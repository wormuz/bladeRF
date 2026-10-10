#ifndef RFFE_CSR_POLICY_H_
#define RFFE_CSR_POLICY_H_

#include <stdbool.h>
#include <stdint.h>

/* GPIO_RFFE_0 loops output bits into the input image. Bit 19 and bits 24..31
 * are physical inputs and must not participate in a write verification. */
#define RFFE_CSR_OUTPUT_MASK UINT32_C(0x00f7ffff)

static inline bool rffe_csr_outputs_match(uint32_t expected,
                                         uint32_t observed,
                                         uint32_t verify_mask)
{
    return (observed & verify_mask) == (expected & verify_mask);
}

#endif
