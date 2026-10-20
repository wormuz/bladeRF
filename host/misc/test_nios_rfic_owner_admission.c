#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "nios_pkt_8x32.h"

/* While an earlier scheduled retune uploads or recalls a profile, NIOS
 * answers 8x32 packets only if nios_pkt_8x32_rfic_owner_access_safe()
 * admits them (bladeRF_nios.c cooperative dispatch). The live 0.32 sweep
 * failed its second bladerf_rx_sweep_schedule with FPGA_OP because the
 * command-session read was refused during the first request's upload.
 * Every 8x32 read on the schedule/collect path must be admitted; RFIC and
 * RFFE mutations must not. */

static int failures;

static void expect(uint8_t target, bool write, uint8_t addr, bool admitted,
                   const char *what)
{
    if (nios_pkt_8x32_rfic_owner_access_safe(target, write, addr, 0) !=
        admitted) {
        fprintf(stderr, "%s: expected %s\n", what,
                admitted ? "admitted" : "refused");
        failures++;
    }
}

int main(void)
{
    /* bladerf2_schedule_retune_sequenced */
    expect(NIOS_PKT_8x32_TARGET_COMMAND_SESSION_GENERATION, false, 0, true,
           "command session generation read");
    expect(NIOS_PKT_8x32_TARGET_RETUNE2_FAILURES, false,
           NIOS_PKT_8x32_RETUNE2_FAILURES_RX_ADDR, true,
           "RX retune2 failure count read");
    expect(NIOS_PKT_8x32_TARGET_RETUNE2_SUCCESSES, false,
           NIOS_PKT_8x32_RETUNE2_SUCCESSES_RX_ADDR, true,
           "RX retune2 success count read");
    /* bladerf2_rfic_reference_clock_status with the ADF400x PLL enabled */
    expect(NIOS_PKT_8x32_TARGET_RFFE_CSR, false, 0, true, "RFFE CSR read");

    /* Mutations stay fenced. */
    expect(NIOS_PKT_8x32_TARGET_RFFE_CSR, true, 0, false, "RFFE CSR write");
    expect(NIOS_PKT_8x32_TARGET_COMMAND_SESSION_GENERATION, true, 0, false,
           "command session generation write");
    expect(NIOS_PKT_8x32_TARGET_ADF400X, true, 0, false, "ADF400x write");

    if (failures != 0) {
        return 1;
    }
    puts("NIOS RFIC-owner admission of sweep reads: PASS");
    return 0;
}
