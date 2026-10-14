#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "../libraries/libbladeRF/src/backend/usb/libusb_transfer_policy.h"

int main(void)
{
    assert(libusb_transfer_index_valid(0, 1));
    assert(libusb_transfer_index_valid(7, 8));
    assert(!libusb_transfer_index_valid(8, 8));
    assert(!libusb_transfer_index_valid(SIZE_MAX, 8));
    assert(!libusb_transfer_index_valid(0, 0));

    /* Only a completed TX transfer can bridge data across an RF admission
     * generation. RX and failed/cancelled TX transfers use their own paths. */
    assert(libusb_tx_completion_generation_is_stale(true, true, false));
    assert(!libusb_tx_completion_generation_is_stale(true, true, true));
    assert(!libusb_tx_completion_generation_is_stale(false, true, false));
    assert(!libusb_tx_completion_generation_is_stale(true, false, false));
    return 0;
}
