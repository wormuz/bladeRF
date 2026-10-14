/* Bounds policy shared by the libusb completion path and its regression. */
#ifndef LIBBLADERF_LIBUSB_TRANSFER_POLICY_H_
#define LIBBLADERF_LIBUSB_TRANSFER_POLICY_H_

#include <stdbool.h>
#include <stddef.h>

static inline bool libusb_transfer_index_valid(size_t index,
                                              size_t transfer_count)
{
    return index < transfer_count;
}

/* A completed TX payload belongs to the RF admission generation captured
 * when that transfer was submitted. Never let its callback silently adopt a
 * newer generation after the payload has already crossed USB. */
static inline bool libusb_tx_completion_generation_is_stale(
    bool is_tx, bool completed, bool submitted_generation_is_current)
{
    return is_tx && completed && !submitted_generation_is_current;
}

#endif
