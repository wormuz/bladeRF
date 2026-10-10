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
    return 0;
}
