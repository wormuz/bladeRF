#include <assert.h>
#include <stdint.h>

#include "nios_axi_address_policy.h"

int main(void)
{
    assert(nios_axi_address_valid(0));
    assert(nios_axi_address_valid(UINT16_MAX));
    assert(!nios_axi_address_valid((uint32_t)UINT16_MAX + 1u));
    assert(!nios_axi_address_valid(UINT32_MAX));
    assert(nios_axi_access_allowed(0, true));
    assert(nios_axi_access_allowed(UINT16_MAX, true));
    assert(!nios_axi_access_allowed(0, false));
    assert(!nios_axi_access_allowed((uint32_t)UINT16_MAX + 1u, true));
    assert(!nios_axi_access_allowed(UINT32_MAX, false));
    return 0;
}
