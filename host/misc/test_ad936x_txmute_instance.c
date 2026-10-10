/* Regression: TX mute state and restore attenuation belong to each RFIC. */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>

#include "../../fpga_common/src/ad936x_helpers.c"

static struct ad9361_rf_phy *fake_phys[2];
static uint32_t fake_attenuation[2][2];
static int fail_next_write;

static unsigned phy_index(struct ad9361_rf_phy *phy)
{
    assert(phy == fake_phys[0] || phy == fake_phys[1]);
    return phy == fake_phys[0] ? 0u : 1u;
}

int32_t ad9361_get_tx_attenuation(struct ad9361_rf_phy *phy, uint8_t ch,
                                  uint32_t *attenuation_mdb)
{
    *attenuation_mdb = fake_attenuation[phy_index(phy)][ch];
    return 0;
}

int32_t ad9361_set_tx_attenuation(struct ad9361_rf_phy *phy, uint8_t ch,
                                  uint32_t attenuation_mdb)
{
    if (fail_next_write) {
        fail_next_write = 0;
        return -EIO;
    }
    fake_attenuation[phy_index(phy)][ch] = attenuation_mdb;
    return 0;
}

int errno_ad9361_to_bladerf(int err)
{
    return err < 0 ? BLADERF_ERR_IO : 0;
}

int main(void)
{
    struct ad9361_rf_phy first = { 0 };
    struct ad9361_rf_phy second = { 0 };
    bool muted;

    fake_phys[0] = &first;
    fake_phys[1] = &second;
    first.tx1_atten_cached = 20000;
    second.tx1_atten_cached = 30000;
    fake_attenuation[0][0] = 20000;
    fake_attenuation[1][0] = 30000;

    assert(txmute_set(&first, BLADERF_CHANNEL_TX(0), true) == 0);
    assert(txmute_get(&second, BLADERF_CHANNEL_TX(0), &muted) == 0);
    assert(!muted);
    assert(txmute_set(&second, BLADERF_CHANNEL_TX(0), true) == 0);
    assert(fake_attenuation[0][0] == 89750);
    assert(fake_attenuation[1][0] == 89750);

    assert(txmute_set(&second, BLADERF_CHANNEL_TX(0), false) == 0);
    assert(fake_attenuation[1][0] == 30000);
    assert(txmute_set(&first, BLADERF_CHANNEL_TX(0), false) == 0);
    assert(fake_attenuation[0][0] == 20000);

    fail_next_write = 1;
    assert(txmute_set(&first, BLADERF_CHANNEL_TX(1), true) != 0);
    assert(txmute_get(&first, BLADERF_CHANNEL_TX(1), &muted) == 0);
    assert(!muted);
    assert(txmute_set(&first, BLADERF_CHANNEL_TX(1), true) == 0);
    assert(txmute_get(&first, BLADERF_CHANNEL_TX(1), &muted) == 0);
    assert(muted);

    assert(txmute_get(&first, BLADERF_CHANNEL_RX(0), &muted) ==
           BLADERF_ERR_INVAL);
    assert(txmute_get(NULL, BLADERF_CHANNEL_TX(0), &muted) ==
           BLADERF_ERR_INVAL);
    assert(txmute_get(&first, BLADERF_CHANNEL_TX(0), NULL) ==
           BLADERF_ERR_INVAL);

    puts("AD936x TX mute instance isolation: PASS");
    return 0;
}
