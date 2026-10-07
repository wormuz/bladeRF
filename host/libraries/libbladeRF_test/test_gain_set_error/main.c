#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "host_config.h"
#include "board/board.h"

struct mock_state {
    int gain_mode_status;
    int restore_gain_mode_status;
    int frequency_status;
    int invalidate_status;
    bladerf_gain_mode current_gain_mode;
    bladerf_frequency frequency;
    unsigned int set_gain_calls;
    unsigned int set_frequency_calls;
    unsigned int invalidate_calls;
    unsigned int complete_calls;
    bladerf_gain last_gain;
};

static int mock_get_gain_mode(struct bladerf *dev, bladerf_channel ch,
                              bladerf_gain_mode *mode)
{
    (void)dev;
    (void)ch;
    struct mock_state *state = dev->board_data;
    *mode = state->current_gain_mode;
    return state->gain_mode_status;
}

static int mock_set_gain_mode(struct bladerf *dev, bladerf_channel ch,
                              bladerf_gain_mode mode)
{
    struct mock_state *state = dev->board_data;
    (void)ch;
    if (mode != BLADERF_GAIN_MGC && state->restore_gain_mode_status != 0) {
        return state->restore_gain_mode_status;
    }
    state->current_gain_mode = mode;
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
    state->set_gain_calls++;
    state->last_gain = gain;
    return 0;
}

static int mock_set_frequency(struct bladerf *dev, bladerf_channel ch,
                              bladerf_frequency frequency)
{
    struct mock_state *state = dev->board_data;
    (void)ch;
    state->set_frequency_calls++;
    state->frequency = frequency;
    return 0;
}

static int mock_get_gain_range(struct bladerf *dev, bladerf_channel ch,
                               const struct bladerf_range **range)
{
    static const struct bladerf_range supported = {
        .min = 0, .max = 30, .step = 1, .scale = 1.0f,
    };
    (void)dev;
    (void)ch;
    *range = &supported;
    return 0;
}

static int mock_invalidate_rx_data(struct bladerf *dev, bladerf_channel ch,
                                  uint32_t reason)
{
    struct mock_state *state = dev->board_data;
    (void)ch;
    (void)reason;
    state->invalidate_calls++;
    return state->invalidate_status;
}

static void mock_rx_reconfigure_complete(struct bladerf *dev,
                                         bladerf_channel ch)
{
    struct mock_state *state = dev->board_data;
    (void)ch;
    state->complete_calls++;
}

static const struct board_fns mock_board = {
    .get_gain_mode = mock_get_gain_mode,
    .set_gain_mode = mock_set_gain_mode,
    .get_frequency = mock_get_frequency,
    .set_gain = mock_set_gain,
    .set_frequency = mock_set_frequency,
    .get_gain_range = mock_get_gain_range,
    .invalidate_rx_data = mock_invalidate_rx_data,
    .rx_reconfigure_complete = mock_rx_reconfigure_complete,
};

int main(void)
{
    struct bladerf dev;
    struct mock_state state = {
        .gain_mode_status = 0,
        .restore_gain_mode_status = 0,
        .frequency_status = BLADERF_ERR_IO,
        .invalidate_status = 0,
        .current_gain_mode = BLADERF_GAIN_MGC,
        .frequency = 0,
        .set_gain_calls = 0,
        .set_frequency_calls = 0,
        .invalidate_calls = 0,
        .complete_calls = 0,
        .last_gain = 0,
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
    assert(state.invalidate_calls == 1 && state.complete_calls == 1);

    /* A failed gain-mode read must not produce a successful target query. */
    dev.gain_tbls[ch].state = BLADERF_GAIN_CAL_LOADED;
    state.gain_mode_status = BLADERF_ERR_IO;
    {
        bladerf_gain target = -123;
        status = bladerf_get_gain_target(&dev, ch, &target);
        assert(status == BLADERF_ERR_IO);
        assert(target == -123);
    }
    state.gain_mode_status = 0;

    /* A successful frequency read followed by an empty calibration table
     * makes get_gain_correction() fail. That error must also be propagated. */
    state.frequency_status = 0;
    state.frequency = 1000000000;
    status = bladerf_set_gain(&dev, ch, 31);
    assert(status == BLADERF_ERR_UNEXPECTED);
    assert(state.set_gain_calls == 0);
    assert(dev.gain_tbls[ch].gain_target == initial_target);

    /* Enabling calibration is one fenced transaction. If gain correction
     * fails, the software policy flag must roll back with the hardware write. */
    state.frequency_status = BLADERF_ERR_IO;
    dev.gain_tbls[ch].enabled = false;
    status = bladerf_enable_gain_calibration(&dev, ch, true);
    assert(status == BLADERF_ERR_IO);
    assert(!dev.gain_tbls[ch].enabled);
    assert(dev.gain_tbls[ch].gain_target == initial_target);
    assert(state.set_gain_calls == 0);
    assert(state.invalidate_calls == 3 && state.complete_calls == 3);

    /* A failed fence leaves calibration policy untouched and does not start
     * the RFIC gain operation. */
    state.invalidate_status = BLADERF_ERR_WOULD_BLOCK;
    status = bladerf_enable_gain_calibration(&dev, ch, true);
    assert(status == BLADERF_ERR_WOULD_BLOCK);
    assert(!dev.gain_tbls[ch].enabled);
    assert(state.set_gain_calls == 0);
    assert(state.invalidate_calls == 4 && state.complete_calls == 3);

    /* Successful application uses the corrected physical gain and commits
     * the matching policy flag while holding dev->lock. */
    state.invalidate_status = 0;
    state.frequency_status = 0;
    state.frequency = 1000000000;
    dev.gain_tbls[ch].entries = calloc(1, sizeof(*dev.gain_tbls[ch].entries));
    assert(dev.gain_tbls[ch].entries != NULL);
    dev.gain_tbls[ch].n_entries = 1;
    dev.gain_tbls[ch].entries[0].freq = state.frequency;
    dev.gain_tbls[ch].entries[0].gain_corr = 2.0;
    status = bladerf_enable_gain_calibration(&dev, ch, true);
    assert(status == 0);
    assert(dev.gain_tbls[ch].enabled);
    assert(state.set_gain_calls == 1 && state.last_gain == 15);
    assert(state.invalidate_calls == 5 && state.complete_calls == 4);

    /* If gain programming succeeds but restoring AGC fails, keep the
     * calibration policy consistent with the gain that was already applied. */
    state.current_gain_mode = BLADERF_GAIN_DEFAULT;
    state.restore_gain_mode_status = BLADERF_ERR_IO;
    status = bladerf_enable_gain_calibration(&dev, ch, false);
    assert(status == BLADERF_ERR_IO);
    assert(!dev.gain_tbls[ch].enabled);
    assert(state.set_gain_calls == 2 && state.last_gain == initial_target);
    assert(state.invalidate_calls == 6 && state.complete_calls == 5);

    /* Frequency-driven correction must borrow MGC while applying the gain,
     * clamp to the gain range, then restore AGC before the RX setter
     * reservation is released. */
    dev.gain_tbls[ch].enabled = true;
    dev.gain_tbls[ch].entries[0].gain_corr = -20.0;
    state.current_gain_mode = BLADERF_GAIN_DEFAULT;
    state.restore_gain_mode_status = 0;
    status = bladerf_set_frequency(&dev, ch, state.frequency);
    assert(status == 0);
    assert(state.set_frequency_calls == 1);
    assert(state.current_gain_mode == BLADERF_GAIN_DEFAULT);
    assert(state.set_gain_calls == 3 && state.last_gain == 30);
    assert(state.invalidate_calls == 7 && state.complete_calls == 6);

    free(dev.gain_tbls[ch].entries);
    assert(MUTEX_DESTROY(&dev.lock) == 0);
    return 0;
}
