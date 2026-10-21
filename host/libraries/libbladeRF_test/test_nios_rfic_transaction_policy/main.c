#include <assert.h>
#include <stdint.h>

#include "nios_pkt_8x32.h"
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

    /* Boot nonce: only a recorded value that changed is a reboot. */
    assert(!bladerf2_rfic_boot_nonce_rebooted(false, 0, 0x1234));
    assert(!bladerf2_rfic_boot_nonce_rebooted(true, 0x1234, 0x1234));
    assert(bladerf2_rfic_boot_nonce_rebooted(true, 0x1234, 0));
    assert(bladerf2_rfic_boot_nonce_rebooted(true, 0x1234, 0x4321));
    for (uint64_t seed = 0; seed < 4096; ++seed) {
        assert(bladerf2_rfic_boot_nonce_from_seed(seed) != 0);
    }
    assert(bladerf2_rfic_boot_nonce_from_seed(1) !=
           bladerf2_rfic_boot_nonce_from_seed(2));

    /* NIOS store rule: write-once per boot, 0 refused. */
    {
        uint32_t nonce = 0;
        assert(!nios_pkt_8x32_boot_nonce_store(&nonce, 0));
        assert(nonce == 0);
        assert(nios_pkt_8x32_boot_nonce_store(&nonce, 0xabcd));
        assert(nonce == 0xabcd);
        assert(nios_pkt_8x32_boot_nonce_store(&nonce, 0xabcd));
        assert(!nios_pkt_8x32_boot_nonce_store(&nonce, 0x1111));
        assert(nonce == 0xabcd);
    }
    assert(nios_pkt_8x32_rfic_owner_access_safe(
        NIOS_PKT_8x32_TARGET_BOOT_NONCE, false, 0, 0));
    assert(nios_pkt_8x32_rfic_owner_access_safe(
        NIOS_PKT_8x32_TARGET_BOOT_NONCE, true, 0, 0x1234));
    assert(!nios_pkt_8x32_rfic_owner_access_safe(
        NIOS_PKT_8x32_TARGET_BOOT_NONCE, false, 1, 0));
    return 0;
}
