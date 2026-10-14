#ifndef NIOS_AXI_ADDRESS_POLICY_H_
#define NIOS_AXI_ADDRESS_POLICY_H_

#include <stdbool.h>
#include <stdint.h>

/* The AD9361 Avalon adapter accepts a 16-bit byte offset. Reject wider
 * protocol addresses before narrowing so malformed requests cannot alias a
 * different register. */
static inline bool nios_axi_address_valid(uint32_t address)
{
    return address <= UINT16_MAX;
}

/* The FPGA top level warns that losing the AD9361 interface clock can stall
 * its register-access path. Check the independently sampled watchdog bit
 * before issuing MMIO; the guard also rejects malformed addresses. */
static inline bool nios_axi_access_allowed(uint32_t address,
                                          bool if_clock_alive)
{
    return nios_axi_address_valid(address) && if_clock_alive;
}

#endif
