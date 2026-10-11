/* Validate fields read from the NIOS retune completion mailbox. */
#ifndef BACKEND_USB_NIOS_COMPLETION_POLICY_H_
#define BACKEND_USB_NIOS_COMPLETION_POLICY_H_

#include <stdbool.h>
#include <stdint.h>

static inline bool nios_retune_profile_word_valid(uint32_t profile)
{
    return profile <= UINT16_MAX;
}

#endif
