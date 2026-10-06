#include <assert.h>
#include <string.h>

#include "host_config.h"
#include "board/board.h"

struct mock_state {
    int frequency_status;
    bladerf_frequency frequency;
    unsigned int set_gain_calls;
};

static int mock_get_gain_mode(struct bladerf *dev, bladerf_channel ch,
                              bladerf_gain_mode *mode)
{
    (void)dev;
    (void)ch;
    *mode = BLADERF_GAIN_MGC;
    return 0;
}

static int mock_get_frequency(struct bladerf *dev, bladerf_channel ch,
                              bladerf_frequency *frequency)
{
    struct mock_state *state = dev->board_data;
    (void)ch;
    *frequency = state->frequency;
    return state->frequency_status;
}

static int mock_set_gain(struct bladerf *dev, bladerf_channel ch,
                         bladerf_gain gain)
{
    struct mock_state *state = dev->board_data;
    (void)ch;
    (void)gain;
    state->set_gain_calls++;
    return 0;
}

static const struct board_fns mock_board = {
    .get_gain_mode = mock_get_gain_mode,
    .get_frequency = mock_get_frequency,
    .set_gain = mock_set_gain,
};

int main(void)
{
    struct bladerf dev;
    struct mock_state state = {
        .frequency_status = BLADERF_ERR_IO,
        .frequency = 0,
        .set_gain_calls = 0,
    };
    const bladerf_channel ch = BLADERF_CHANNEL_RX(0);
    const bladerf_gain initial_target = 17;
    int status;

    memset(&dev, 0, sizeof(dev));
    dev.board = &mock_board;
    dev.board_data = &state;
    dev.gain_tbls[ch].enabled = true;
    dev.gain_tbls[ch].gain_target = initial_target;
    assert(MUTEX_INIT(&dev.lock) == 0);

    status = bladerf_set_gain(&dev, ch, 30);
    assert(status == BLADERF_ERR_IO);
    assert(state.set_gain_calls == 0);
    assert(dev.gain_tbls[ch].gain_target == initial_target);

    /* A successful frequency read followed by an empty calibration table
     * makes get_gain_correction() fail. That error must also be propagated. */
    state.frequency_status = 0;
    state.frequency = 1000000000;
    status = bladerf_set_gain(&dev, ch, 31);
    assert(status == BLADERF_ERR_UNEXPECTED);
    assert(state.set_gain_calls == 0);
    assert(dev.gain_tbls[ch].gain_target == initial_target);

    assert(MUTEX_DESTROY(&dev.lock) == 0);
    return 0;
}
