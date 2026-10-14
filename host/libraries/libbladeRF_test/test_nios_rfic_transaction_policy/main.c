#include <assert.h>
#include <stdint.h>

#include "rfic_transaction_policy.h"

int main(void)
{
    assert(bladerf2_rfic_next_transaction_id(0) == 1);
    assert(bladerf2_rfic_next_transaction_id(1) == 2);
    assert(bladerf2_rfic_next_transaction_id(UINT16_MAX - 1) == UINT16_MAX);
    assert(bladerf2_rfic_next_transaction_id(UINT16_MAX) == 1);

    assert(bladerf2_rfic_orphan_adoptable(1, 7, true, true));
    assert(!bladerf2_rfic_orphan_adoptable(0, 7, true, true));
    assert(!bladerf2_rfic_orphan_adoptable(2, 7, true, true));
    assert(!bladerf2_rfic_orphan_adoptable(1, 0, true, true));
    assert(!bladerf2_rfic_orphan_adoptable(1, 7, false, true));
    assert(!bladerf2_rfic_orphan_adoptable(1, 7, true, false));

    assert(!bladerf2_rfic_deadline_expired(100, 199, 100));
    assert(bladerf2_rfic_deadline_expired(100, 200, 100));
    assert(bladerf2_rfic_deadline_expired(100, 201, 100));
    assert(bladerf2_rfic_deadline_expired(UINT64_MAX - 10, 9, 20));

    assert(bladerf2_rfic_next_poll_delay_us(1000, 20000) == 2000);
    assert(bladerf2_rfic_next_poll_delay_us(5000, 20000) == 10000);
    assert(bladerf2_rfic_next_poll_delay_us(10000, 20000) == 20000);
    assert(bladerf2_rfic_next_poll_delay_us(20000, 20000) == 20000);
    assert(bladerf2_rfic_next_poll_delay_us(30000, 20000) == 20000);
    assert(bladerf2_rfic_next_poll_delay_us(0, 20000) == 20000);
    assert(bladerf2_rfic_next_poll_delay_us(1000, 0) == 0);
    return 0;
}
