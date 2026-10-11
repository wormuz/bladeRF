/* Focused test for matching FPGA RX epoch status to the armed epoch. */
#include <assert.h>
#include <stdint.h>

#include "nios_pkt_8x32.h"

static uint32_t status_word(uint8_t state, uint8_t epoch_id)
{
    return ((uint32_t)state << NIOS_PKT_8x32_RX_EPOCH_STATUS_STATE_SHIFT) |
           ((uint32_t)epoch_id << NIOS_PKT_8x32_RX_EPOCH_STATUS_EPOCH_ID_SHIFT);
}

static uint32_t fenced_status_word(uint8_t state, uint8_t epoch_id)
{
    return status_word(state, epoch_id) |
           NIOS_PKT_8x32_RX_EPOCH_STATUS_DISCARD_ACTIVE;
}

int main(void)
{
    assert(nios_pkt_8x32_rx_epoch_status_is_active(
        status_word(NIOS_PKT_8x32_RX_EPOCH_STATE_ACTIVE_NEW, 0), 0));
    assert(nios_pkt_8x32_rx_epoch_status_is_active(
        status_word(NIOS_PKT_8x32_RX_EPOCH_STATE_ACTIVE, 0xff), 0xff));

    /* The state register trails the immediate ARM/ABORT fence by a clock.
     * Matching ACTIVE state and epoch are not success while discard is set. */
    assert(!nios_pkt_8x32_rx_epoch_status_is_active(
        fenced_status_word(NIOS_PKT_8x32_RX_EPOCH_STATE_ACTIVE_NEW, 0x21),
        0x21));
    assert(!nios_pkt_8x32_rx_epoch_status_is_active(
        fenced_status_word(NIOS_PKT_8x32_RX_EPOCH_STATE_ACTIVE, 0x21),
        0x21));

    /* A stale active status must never validate a newly armed epoch. */
    assert(!nios_pkt_8x32_rx_epoch_status_is_active(
        status_word(NIOS_PKT_8x32_RX_EPOCH_STATE_ACTIVE, 0x17), 0x18));
    assert(!nios_pkt_8x32_rx_epoch_status_is_active(
        status_word(NIOS_PKT_8x32_RX_EPOCH_STATE_ACTIVE_NEW, 0x17), 0x18));

    /* Matching IDs in nonterminal states still do not establish validity. */
    assert(!nios_pkt_8x32_rx_epoch_status_is_active(
        status_word(NIOS_PKT_8x32_RX_EPOCH_STATE_PENDING, 0x18), 0x18));
    assert(!nios_pkt_8x32_rx_epoch_status_is_active(
        status_word(NIOS_PKT_8x32_RX_EPOCH_STATE_ERROR, 0x18), 0x18));
    return 0;
}
