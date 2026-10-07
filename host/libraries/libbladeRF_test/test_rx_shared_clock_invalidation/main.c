#include <assert.h>
#include <string.h>

#include "host_config.h"
#include "board/board.h"
#include "backend/backend.h"

struct mock_state {
    int invalidate_status;
    int sample_rate_status;
    int rational_rate_status;
    int bandwidth_status;
    unsigned int invalidate_calls;
    unsigned int sample_rate_calls;
    unsigned int rational_rate_calls;
    unsigned int bandwidth_calls;
    unsigned int config_gpio_write_calls;
    unsigned int wishbone_write_calls;
    unsigned int complete_calls;
    bladerf_channel invalidated_channel;
    bladerf_channel sample_rate_channel;
    bladerf_channel rational_rate_channel;
    bladerf_channel bandwidth_channel;
    bladerf_channel completed_channel;
    uint32_t invalidate_reason;
    uint32_t config_gpio_value;
    uint32_t wishbone_address;
    uint32_t wishbone_value;
    uint32_t expansion_gpio_value;
    uint8_t lms_5a;
    unsigned int expansion_gpio_write_calls;
    unsigned int expansion_gpio_dir_write_calls;
    unsigned int lms_write_calls;
};

struct fake_xb200_data {
    bladerf_xb200_filter auto_filter[2];
};

static int mock_invalidate_rx(struct bladerf *dev, bladerf_channel ch,
                              uint32_t reason)
{
    struct mock_state *state = dev->board_data;
    state->invalidate_calls++;
    state->invalidated_channel = ch;
    state->invalidate_reason = reason;
    return state->invalidate_status;
}

static void mock_reconfigure_complete(struct bladerf *dev,
                                      bladerf_channel ch)
{
    struct mock_state *state = dev->board_data;
    state->complete_calls++;
    state->completed_channel = ch;
}

static int mock_set_sample_rate(struct bladerf *dev, bladerf_channel ch,
                                bladerf_sample_rate rate,
                                bladerf_sample_rate *actual)
{
    struct mock_state *state = dev->board_data;
    (void)rate;
    state->sample_rate_calls++;
    state->sample_rate_channel = ch;
    if (actual != NULL) {
        *actual = rate;
    }
    return state->sample_rate_status;
}

static int mock_set_rational_sample_rate(
    struct bladerf *dev, bladerf_channel ch,
    struct bladerf_rational_rate *rate,
    struct bladerf_rational_rate *actual)
{
    struct mock_state *state = dev->board_data;
    state->rational_rate_calls++;
    state->rational_rate_channel = ch;
    if (actual != NULL) {
        *actual = *rate;
    }
    return state->rational_rate_status;
}

static int mock_set_bandwidth(struct bladerf *dev, bladerf_channel ch,
                              bladerf_bandwidth bandwidth,
                              bladerf_bandwidth *actual)
{
    struct mock_state *state = dev->board_data;
    state->bandwidth_calls++;
    state->bandwidth_channel = ch;
    if (actual != NULL) {
        *actual = bandwidth;
    }
    return state->bandwidth_status;
}

static int mock_config_gpio_write(struct bladerf *dev, uint32_t value)
{
    struct mock_state *state = dev->board_data;
    state->config_gpio_write_calls++;
    state->config_gpio_value = value;
    return 0;
}

static int mock_wishbone_write(struct bladerf *dev, uint32_t address,
                               uint32_t value)
{
    struct mock_state *state = dev->board_data;
    state->wishbone_write_calls++;
    state->wishbone_address = address;
    state->wishbone_value = value;
    return 0;
}

static int mock_expansion_gpio_read(struct bladerf *dev, uint32_t *value)
{
    struct mock_state *state = dev->board_data;
    *value = state->expansion_gpio_value;
    return 0;
}

static int mock_expansion_gpio_write(struct bladerf *dev, uint32_t mask,
                                     uint32_t value)
{
    struct mock_state *state = dev->board_data;
    state->expansion_gpio_value =
        (state->expansion_gpio_value & ~mask) | (value & mask);
    state->expansion_gpio_write_calls++;
    return 0;
}

static int mock_expansion_gpio_dir_write(struct bladerf *dev, uint32_t mask,
                                         uint32_t value)
{
    struct mock_state *state = dev->board_data;
    (void)mask;
    (void)value;
    state->expansion_gpio_dir_write_calls++;
    return 0;
}

static int mock_expansion_gpio_dir_read(struct bladerf *dev, uint32_t *value)
{
    (void)dev;
    *value = 0;
    return 0;
}

static int mock_lms_read(struct bladerf *dev, uint8_t address, uint8_t *value)
{
    struct mock_state *state = dev->board_data;
    assert(address == 0x5a);
    *value = state->lms_5a;
    return 0;
}

static int mock_lms_write(struct bladerf *dev, uint8_t address, uint8_t value)
{
    struct mock_state *state = dev->board_data;
    assert(address == 0x5a);
    state->lms_5a = value;
    state->lms_write_calls++;
    return 0;
}

static const struct board_fns mock_board = {
    .name = "bladerf2",
    .invalidate_rx_data = mock_invalidate_rx,
    .rx_reconfigure_complete = mock_reconfigure_complete,
    .set_sample_rate = mock_set_sample_rate,
    .set_rational_sample_rate = mock_set_rational_sample_rate,
    .set_bandwidth = mock_set_bandwidth,
    .config_gpio_write = mock_config_gpio_write,
    .wishbone_master_write = mock_wishbone_write,
};

static const struct backend_fns mock_backend = {
    .expansion_gpio_read = mock_expansion_gpio_read,
    .expansion_gpio_write = mock_expansion_gpio_write,
    .expansion_gpio_dir_write = mock_expansion_gpio_dir_write,
    .expansion_gpio_dir_read = mock_expansion_gpio_dir_read,
    .lms_read = mock_lms_read,
    .lms_write = mock_lms_write,
};

int main(void)
{
    struct bladerf dev;
    struct mock_state state = {0};
    struct fake_xb200_data xb200_data = {{
        BLADERF_XB200_50M, BLADERF_XB200_50M,
    }};
    const bladerf_channel tx0 = BLADERF_CHANNEL_TX(0);
    const bladerf_channel rx0 = BLADERF_CHANNEL_RX(0);
    const bladerf_channel rx2 = BLADERF_CHANNEL_RX(1);
    struct bladerf_rational_rate rational = {
        .integer = 2000000, .num = 1, .den = 3,
    };

    memset(&dev, 0, sizeof(dev));
    dev.board = &mock_board;
    dev.backend = &mock_backend;
    dev.board_data = &state;
    dev.xb_data = &xb200_data;
    assert(MUTEX_INIT(&dev.lock) == 0);

    /* AD9361's TX samplerate API updates both clock chains. Fence RX before
     * calling the TX setter and release the RX reservation afterward. */
    assert(bladerf_set_sample_rate(&dev, tx0, 2000000, NULL) == 0);
    assert(state.invalidate_calls == 1 && state.sample_rate_calls == 1);
    assert(state.invalidated_channel == rx0 &&
           state.sample_rate_channel == tx0 &&
           state.completed_channel == rx0);
    assert(state.invalidate_reason == BLADERF_RF_INVALIDATE_SAMPLE_RATE);
    assert(state.complete_calls == 1);

    /* Rational TX rates use the same shared-clock fence, including when the
     * AD9361 operation reports an error. */
    state.rational_rate_status = BLADERF_ERR_IO;
    assert(bladerf_set_rational_sample_rate(
               &dev, tx0, &rational, NULL) == BLADERF_ERR_IO);
    assert(state.invalidate_calls == 2 && state.rational_rate_calls == 1);
    assert(state.invalidated_channel == rx0 &&
           state.rational_rate_channel == tx0 &&
           state.completed_channel == rx0);
    assert(state.complete_calls == 2);

    /* An RX2 request keeps its own channel identity instead of being
     * normalized to RX0. */
    assert(bladerf_set_sample_rate(&dev, rx2, 1000000, NULL) == 0);
    assert(state.invalidated_channel == rx2 &&
           state.sample_rate_channel == rx2 &&
           state.completed_channel == rx2);
    assert(state.complete_calls == 3);

    /* ADI's TX bandwidth path calls update_rf_bandwidth(), which also reruns
     * RX analog filter, TIA, and ADC setup. It shares the same RX fence. */
    assert(bladerf_set_bandwidth(&dev, tx0, 1000000, NULL) == 0);
    assert(state.invalidated_channel == rx0 &&
           state.bandwidth_channel == tx0 &&
           state.completed_channel == rx0);
    assert(state.invalidate_reason == BLADERF_RF_INVALIDATE_BANDWIDTH);
    assert(state.bandwidth_calls == 1 && state.complete_calls == 4);

    state.bandwidth_status = BLADERF_ERR_IO;
    assert(bladerf_set_bandwidth(&dev, tx0, 1200000, NULL) == BLADERF_ERR_IO);
    assert(state.bandwidth_calls == 2 && state.complete_calls == 5);

    /* An RX2 bandwidth request must retain RX2 identity through fence and
     * completion rather than being collapsed onto the RX1 shared clock. */
    state.bandwidth_status = 0;
    assert(bladerf_set_bandwidth(&dev, rx2, 1200000, NULL) == 0);
    assert(state.invalidated_channel == rx2 &&
           state.bandwidth_channel == rx2 &&
           state.completed_channel == rx2);
    assert(state.bandwidth_calls == 3 && state.complete_calls == 6);

    /* Raw configuration GPIO writes can change RX mux or clock selection.
     * Fence the shared RX certificate even though the generic API cannot
     * identify which register bits the caller intended to alter. */
    assert(bladerf_config_gpio_write(&dev, 0x12345678) == 0);
    assert(state.invalidate_calls == 7 && state.config_gpio_write_calls == 1);
    assert(state.invalidate_reason == BLADERF_RF_INVALIDATE_CONFIG_GPIO &&
           state.invalidated_channel == rx0 && state.completed_channel == rx0);
    assert(state.config_gpio_value == 0x12345678 && state.complete_calls == 7);

    /* Arbitrary Wishbone writes can touch the FPGA RX admission or metadata
     * path, so they use a distinct fail-closed reason. */
    assert(bladerf_wishbone_master_write(&dev, 0x100, 0xa5a55a5a) == 0);
    assert(state.invalidate_calls == 8 && state.wishbone_write_calls == 1);
    assert(state.invalidate_reason == BLADERF_RF_INVALIDATE_WISHBONE &&
           state.invalidated_channel == rx0 && state.completed_channel == rx0);
    assert(state.wishbone_address == 0x100 &&
           state.wishbone_value == 0xa5a55a5a && state.complete_calls == 8);

    /* OVERSAMPLE changes the sample-rate and stream-format interpretation.
     * Fence before changing the public feature policy, including disable. */
    assert(bladerf_enable_feature(&dev, BLADERF_FEATURE_OVERSAMPLE, true) == 0);
    assert(dev.feature == BLADERF_FEATURE_OVERSAMPLE &&
           state.invalidate_reason == BLADERF_RF_INVALIDATE_FEATURE &&
           state.invalidated_channel == rx0 && state.completed_channel == rx0);
    assert(state.invalidate_calls == 9 && state.complete_calls == 9);
    assert(bladerf_enable_feature(&dev, BLADERF_FEATURE_DEFAULT, false) == 0);
    assert(dev.feature == BLADERF_FEATURE_DEFAULT &&
           state.invalidate_reason == BLADERF_RF_INVALIDATE_FEATURE);
    assert(state.invalidate_calls == 10 && state.complete_calls == 10);

    /* Failed RX fencing blocks both shared TX configuration paths before the
     * board setter runs. */
    state.invalidate_status = BLADERF_ERR_WOULD_BLOCK;
    assert(bladerf_set_bandwidth(&dev, tx0, 1400000, NULL) ==
           BLADERF_ERR_WOULD_BLOCK);
    assert(state.invalidate_calls == 11 && state.bandwidth_calls == 3);
    assert(bladerf_set_sample_rate(&dev, tx0, 3000000, NULL) ==
           BLADERF_ERR_WOULD_BLOCK);
    assert(state.invalidate_calls == 12 && state.sample_rate_calls == 2);
    assert(bladerf_config_gpio_write(&dev, 0) == BLADERF_ERR_WOULD_BLOCK);
    assert(bladerf_wishbone_master_write(&dev, 0, 0) ==
           BLADERF_ERR_WOULD_BLOCK);
    assert(state.invalidate_calls == 14 &&
           state.config_gpio_write_calls == 1 && state.wishbone_write_calls == 1);
    assert(bladerf_enable_feature(&dev, BLADERF_FEATURE_OVERSAMPLE, true) ==
           BLADERF_ERR_WOULD_BLOCK);
    assert(dev.feature == BLADERF_FEATURE_DEFAULT && state.invalidate_calls == 15);
    assert(state.complete_calls == 10);

    /* XB-200 RX filter changes alter the RF path. Fence the common RX epoch
     * first and publish the dedicated reason before touching the expansion. */
    state.invalidate_status = 0;
    assert(bladerf_xb200_set_filterbank(
               &dev, rx0, BLADERF_XB200_144M) == 0);
    assert(state.invalidate_calls == 16 && state.complete_calls == 11);
    assert(state.invalidate_reason == BLADERF_RF_INVALIDATE_RF_PORT &&
           state.invalidated_channel == rx0 && state.completed_channel == rx0);
    assert(state.expansion_gpio_write_calls == 1);

    /* The RX bypass/mixer switch also changes the external signal path and
     * uses both LMS and expansion GPIO controls. */
    state.expansion_gpio_value |= 0x0800;
    assert(bladerf_xb200_set_path(
               &dev, rx0, BLADERF_XB200_MIX) == 0);
    assert(state.invalidate_calls == 17 && state.complete_calls == 12);
    assert(state.invalidate_reason == BLADERF_RF_INVALIDATE_RF_PORT &&
           state.invalidated_channel == rx0 && state.completed_channel == rx0);
    assert(state.lms_write_calls == 1 &&
           state.expansion_gpio_write_calls == 2);

    /* XB-200 RX path changes also program LMS and expansion GPIO; a failed
     * epoch fence must stop before either write. */
    state.invalidate_status = BLADERF_ERR_WOULD_BLOCK;
    const unsigned int lms_writes_before = state.lms_write_calls;
    const unsigned int gpio_writes_before = state.expansion_gpio_write_calls;
    assert(bladerf_xb200_set_path(
               &dev, rx0, BLADERF_XB200_MIX) == BLADERF_ERR_WOULD_BLOCK);
    assert(state.invalidate_calls == 18 && state.complete_calls == 12);
    assert(state.lms_write_calls == lms_writes_before &&
           state.expansion_gpio_write_calls == gpio_writes_before);

    /* TX-only XB-200 filter changes do not invalidate the RX epoch. */
    state.invalidate_status = 0;
    assert(bladerf_xb200_set_filterbank(
               &dev, tx0, BLADERF_XB200_144M) == 0);
    assert(state.invalidate_calls == 18 && state.complete_calls == 12);

    /* XB-300 TRX antenna routing affects RX regardless of the selected mode. */
    assert(bladerf_xb300_set_trx(&dev, BLADERF_XB300_TRX_RX) == 0);
    assert(state.invalidate_calls == 19 && state.complete_calls == 13);
    assert(state.invalidate_reason == BLADERF_RF_INVALIDATE_RF_PORT &&
           state.invalidated_channel == rx0 && state.completed_channel == rx0);

    /* Its LNA affects RX gain; PA/AUX controls are TX-only and preserve RX. */
    assert(bladerf_xb300_set_amplifier_enable(
               &dev, BLADERF_XB300_AMP_LNA, true) == 0);
    assert(state.invalidate_calls == 20 && state.complete_calls == 14);
    assert(state.invalidate_reason == BLADERF_RF_INVALIDATE_GAIN &&
           state.invalidated_channel == rx0 && state.completed_channel == rx0);
    assert(bladerf_xb300_set_amplifier_enable(
               &dev, BLADERF_XB300_AMP_PA, true) == 0);
    assert(bladerf_xb300_set_amplifier_enable(
               &dev, BLADERF_XB300_AMP_PA_AUX, true) == 0);
    assert(state.invalidate_calls == 20 && state.complete_calls == 14);

    /* A failed LNA fence prevents the GPIO write and releases no reservation. */
    state.invalidate_status = BLADERF_ERR_WOULD_BLOCK;
    const unsigned int gpio_writes_before_xb300 =
        state.expansion_gpio_write_calls;
    assert(bladerf_xb300_set_amplifier_enable(
               &dev, BLADERF_XB300_AMP_LNA, false) ==
           BLADERF_ERR_WOULD_BLOCK);
    assert(state.invalidate_calls == 21 && state.complete_calls == 14);
    assert(state.expansion_gpio_write_calls == gpio_writes_before_xb300);

    /* Raw expansion GPIO is an opaque external path control. Its public
     * write/direction APIs must revoke RX validity before changing pins. */
    state.invalidate_status = 0;
    assert(bladerf_expansion_gpio_masked_write(&dev, 0x10, 0x10) == 0);
    assert(state.invalidate_calls == 22 && state.complete_calls == 15);
    assert(state.invalidate_reason == BLADERF_RF_INVALIDATE_RF_PORT &&
           state.invalidated_channel == rx0 && state.completed_channel == rx0);
    assert(bladerf_expansion_gpio_dir_masked_write(&dev, 0x10, 0x10) == 0);
    assert(state.invalidate_calls == 23 && state.complete_calls == 16 &&
           state.expansion_gpio_dir_write_calls == 1);
    assert(bladerf_expansion_gpio_write(&dev, 0x1234) == 0);
    assert(state.invalidate_calls == 24 && state.complete_calls == 17);
    assert(bladerf_expansion_gpio_dir_write(&dev, 0x1234) == 0);
    assert(state.invalidate_calls == 25 && state.complete_calls == 18 &&
           state.expansion_gpio_dir_write_calls == 2);
    assert(bladerf_expansion_gpio_masked_write(&dev, 0, 0) == 0);
    assert(state.invalidate_calls == 25 && state.complete_calls == 18);

    /* A rejected fence must stop the opaque GPIO mutation as well. */
    state.invalidate_status = BLADERF_ERR_WOULD_BLOCK;
    const unsigned int expansion_writes_before_raw_gpio =
        state.expansion_gpio_write_calls;
    assert(bladerf_expansion_gpio_masked_write(&dev, 0x20, 0x20) ==
           BLADERF_ERR_WOULD_BLOCK);
    assert(state.invalidate_calls == 26 && state.complete_calls == 18 &&
           state.expansion_gpio_write_calls == expansion_writes_before_raw_gpio);

    assert(MUTEX_DESTROY(&dev.lock) == 0);
    return 0;
}
