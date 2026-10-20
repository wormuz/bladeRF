/* bladerf2_nios_ram_usage_consistent(): a field set read across a NIOS stack
 * or heap change is rejected. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "nios_ram_policy.h"

int main(void)
{
    const uint32_t steady[4] = { 2700, 2504, 1040, 6244 };
    const uint32_t deeper_between_reads[4] = { 2700, 2504, 1104, 6244 };
    const uint32_t wraps[4] = { UINT32_C(0xffffffff), 1, 0, 0 };

    if (!bladerf2_nios_ram_usage_consistent(steady) ||
        bladerf2_nios_ram_usage_consistent(deeper_between_reads) ||
        bladerf2_nios_ram_usage_consistent(wraps)) {
        fputs("NIOS RAM usage consistency check mismatch\n", stderr);
        return 1;
    }
    puts("NIOS RAM usage consistency: PASS");
    return 0;
}
