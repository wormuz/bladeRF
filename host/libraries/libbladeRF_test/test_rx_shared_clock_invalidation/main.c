#include <assert.h>
#include <string.h>

#include "host_config.h"
#include "board/board.h"

struct mock_state {
    int invalidate_status;
    int sample_rate_status;
    int rational_rate_status;
    unsigned int invalidate_calls;
    unsigned int sample_rate_calls;
    unsigned int rational_rate_calls;
    unsigned int complete_calls;
    bladerf_channel invalidated_channel;
    bladerf_channel sample_rate_channel;
    bladerf_channel rational_rate_channel;
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

static const struct board_fns mock_board = {
    .name = "bladerf2",
    .invalidate_rx_data = mock_invalidate_rx,
    .rx_reconfigure_complete = mock_reconfigure_complete,
    .set_sample_rate = mock_set_sample_rate,
    .set_rational_sample_rate = mock_set_rational_sample_rate,
};

int main(void)
{
    struct bladerf dev;
    struct mock_state state = {0};
    const bladerf_channel tx0 = BLADERF_CHANNEL_TX(0);
    const bladerf_channel rx0 = BLADERF_CHANNEL_RX(0);
    const bladerf_channel rx1 = BLADERF_CHANNEL_RX(1);
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

    /* An RX1 request keeps its own channel identity instead of being
     * normalized to RX0. */
    assert(bladerf_set_sample_rate(&dev, rx1, 1000000, NULL) == 0);
    assert(state.invalidated_channel == rx1 &&
           state.sample_rate_channel == rx1 &&
           state.completed_channel == rx1);
    assert(state.complete_calls == 3);

    /* A failed RX fence prevents the TX clock-chain write entirely. */
    state.invalidate_status = BLADERF_ERR_WOULD_BLOCK;
    assert(bladerf_set_sample_rate(&dev, tx0, 3000000, NULL) ==
           BLADERF_ERR_WOULD_BLOCK);
    assert(state.invalidate_calls == 4 && state.sample_rate_calls == 2);
    assert(state.complete_calls == 3);

    assert(MUTEX_DESTROY(&dev.lock) == 0);
    return 0;
}
