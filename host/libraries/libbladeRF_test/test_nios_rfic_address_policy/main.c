#include <assert.h>
#include <stdint.h>

#include "rfic_channel_validation.h"

int main(void)
{
    assert(_rfic_address_fields_valid(0x0000));
    assert(_rfic_address_fields_valid(0x0f0b));
    assert(_rfic_address_fields_valid(0x0f00));
    assert(!_rfic_address_fields_valid(0x0010));
    assert(!_rfic_address_fields_valid(0x1000));
    assert(!_rfic_address_fields_valid(0xffff));
    return 0;
}
