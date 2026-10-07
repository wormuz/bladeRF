#include <assert.h>
#include <string.h>

#include "host_config.h"
#include "board/board.h"

struct mock_state {
    int invalidate_status;
    int sample_rate_status;
    int rational_rate_status;
    int bandwidth_status;
    unsigned int invalidate_calls;
    unsigned int sample_rate_calls;
    unsigned int rational_rate_calls;
    unsigned int bandwidth_calls;
    unsigned int complete_calls;
    bladerf_channel invalidated_channel;
    bladerf_channel sample_rate_channel;
    bladerf_channel rational_rate_channel;
    bladerf_channel bandwidth_channel;
    bladerf_channel completed_channel;
    uint32_t invalidate_reason;
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

static const struct board_fns mock_board = {
    .name = "bladerf2",
    .invalidate_rx_data = mock_invalidate_rx,
    .rx_reconfigure_complete = mock_reconfigure_complete,
    .set_sample_rate = mock_set_sample_rate,
    .set_rational_sample_rate = mock_set_rational_sample_rate,
    .set_bandwidth = mock_set_bandwidth,
};

int main(void)
{
    struct bladerf dev;
    struct mock_state state = {0};
    const bladerf_channel tx0 = BLADERF_CHANNEL_TX(0);
    const bladerf_channel rx0 = BLADERF_CHANNEL_RX(0);
    const bladerf_channel rx2 = BLADERF_CHANNEL_RX(1);
    struct bladerf_rational_rate rational = {
        .integer = 2000000, .num = 1, .den = 3,
    };

    memset(&dev, 0, sizeof(dev));
    dev.board = &mock_board;
    dev.board_data = &state;
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

    /* Failed RX fencing blocks both shared TX configuration paths before the
     * board setter runs. */
    state.invalidate_status = BLADERF_ERR_WOULD_BLOCK;
    assert(bladerf_set_bandwidth(&dev, tx0, 1400000, NULL) ==
           BLADERF_ERR_WOULD_BLOCK);
    assert(state.invalidate_calls == 7 && state.bandwidth_calls == 3);
    assert(bladerf_set_sample_rate(&dev, tx0, 3000000, NULL) ==
           BLADERF_ERR_WOULD_BLOCK);
    assert(state.invalidate_calls == 8 && state.sample_rate_calls == 2);
    assert(state.complete_calls == 6);

    assert(MUTEX_DESTROY(&dev.lock) == 0);
    return 0;
}
