#include <stdio.h>

#include "rffe_csr_policy.h"

int main(void)
{
    const uint32_t expected = UINT32_C(0x00a51234);
    const uint32_t physical_input_changes =
        expected ^ UINT32_C(0x80080000);
    const uint32_t output_mismatch = expected ^ (1u << 12);

    if ((RFFE_CSR_OUTPUT_MASK & (1u << 19)) != 0 ||
        (RFFE_CSR_OUTPUT_MASK & UINT32_C(0xff000000)) != 0 ||
        !rffe_csr_outputs_match(expected, physical_input_changes,
                                RFFE_CSR_OUTPUT_MASK) ||
        rffe_csr_outputs_match(expected, output_mismatch,
                               RFFE_CSR_OUTPUT_MASK) ||
        !rffe_csr_outputs_match(expected, output_mismatch, 0)) {
        fputs("RFFE output readback policy mismatch\n", stderr);
        return 1;
    }

    return 0;
}
