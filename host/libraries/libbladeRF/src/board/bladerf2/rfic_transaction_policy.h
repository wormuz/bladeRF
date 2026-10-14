/* This file is part of the bladeRF project:
 *   http://www.github.com/nuand/bladeRF
 *
 * Copyright (c) 2026 Nuand LLC
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 */

#ifndef BLADERF2_RFIC_TRANSACTION_POLICY_H_
#define BLADERF2_RFIC_TRANSACTION_POLICY_H_

#include <stdbool.h>
#include <stdint.h>

/* Transaction IDs are selected from the firmware's last-retired ID rather
 * than the 32-bit retirement sequence. This survives host-process restart and
 * avoids ID aliasing at sequence wrap; zero remains reserved for legacy
 * untagged writes. */
static inline uint16_t bladerf2_rfic_next_transaction_id(uint16_t last_id)
{
    return last_id == UINT16_MAX ? 1u : (uint16_t)(last_id + 1u);
}

/* After reopen, adopt only an unambiguous single tagged queue head whose
 * STATUS and transaction snapshot agree. Anything else remains fail-closed. */
static inline bool bladerf2_rfic_orphan_adoptable(
    uint8_t queue_length, uint16_t active_id, bool active_command_valid,
    bool sequence_snapshot_matches)
{
    return queue_length == 1 && active_id != 0 && active_command_valid &&
           sequence_snapshot_matches;
}

/* A caller deadline is a failure-only deadman for a transaction that remains
 * active. It must never be interpreted as retirement or success. Unsigned
 * subtraction keeps elapsed-time checks valid across uint64_t wrap. */
static inline bool bladerf2_rfic_deadline_expired(uint64_t started_ns,
                                                  uint64_t now_ns,
                                                  uint64_t budget_ns)
{
    return (uint64_t)(now_ns - started_ns) >= budget_ns;
}

/* Polling is only the transport mechanism for observing the firmware's
 * retirement event. Start promptly, then reduce USB/NIOS traffic while a
 * bounded calibration callback owns the single NIOS execution context. */
static inline uint32_t bladerf2_rfic_next_poll_delay_us(uint32_t current_us,
                                                        uint32_t maximum_us)
{
    if (current_us == 0 || maximum_us == 0 || current_us >= maximum_us) {
        return maximum_us;
    }
    return current_us > maximum_us / 2u ? maximum_us : current_us * 2u;
}

#endif
