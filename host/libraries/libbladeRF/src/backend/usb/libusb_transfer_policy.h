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

#endif
