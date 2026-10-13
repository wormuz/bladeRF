/* Validate fields read from the NIOS retune completion mailbox. */
#ifndef BACKEND_USB_NIOS_COMPLETION_POLICY_H_
#define BACKEND_USB_NIOS_COMPLETION_POLICY_H_

#include <stdbool.h>
#include <stdint.h>

static inline bool nios_retune_profile_word_valid(uint32_t profile)
{
    return profile <= UINT16_MAX;
}

/* A queue-clear response with CANCEL_PENDING is an intermediate ACK, not a
 * queue-full failure. The caller polls the same clear request until NIOS
 * reports completion or the host watchdog expires. */
static inline bool nios_retune_clear_is_pending(bool queue_clear,
                                                 uint8_t response_flags,
                                                 uint8_t cancel_pending_flag)
{
    return queue_clear && (response_flags & cancel_pending_flag) != 0;
}

#endif
