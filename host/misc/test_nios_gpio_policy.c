#include <stdio.h>

#include "nios_gpio_policy.h"

int main(void)
{
    const uint32_t expected = UINT32_C(0x80402080);
    const uint32_t input_changes = expected ^ UINT32_C(0x38000001);
    const uint32_t output_mismatch = expected ^ (1u << 18);

    if (!nios_gpio_outputs_match(expected, input_changes) ||
        nios_gpio_outputs_match(expected, output_mismatch)) {
        fputs("NIOS control GPIO readback policy mismatch\n", stderr);
        return 1;
    }

    return 0;
}
