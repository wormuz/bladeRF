#include <assert.h>
#include <stdint.h>

#include "ad9361_tx_quad_cal_policy.h"

int main(void)
{
    uint32_t mode = UINT32_MAX;
	uint32_t restore_rx_bw = 0;
	uint32_t restore_tx_bw = 0;
    uint64_t history[AD9361_TX_QUAD_PORT_COUNT]
                    [AD9361_TX_QUAD_MODE_COUNT] = {{0}};
    bool valid[AD9361_TX_QUAD_PORT_COUNT]
              [AD9361_TX_QUAD_MODE_COUNT] = {{false}};
    const uint64_t threshold = UINT64_C(100000000);

    assert(ad9361_tx_quad_mode_index(false, 1, &mode));
    assert(mode == AD9361_TX_QUAD_MODE_TX1);
    assert(ad9361_tx_quad_mode_index(false, 2, &mode));
    assert(mode == AD9361_TX_QUAD_MODE_TX2);
    assert(ad9361_tx_quad_mode_index(true, 1, &mode));
    assert(mode == AD9361_TX_QUAD_MODE_2X2);
    assert(!ad9361_tx_quad_mode_index(false, 0, &mode));

	/* A pending outer bandwidth transaction restores its requested target,
	 * even though the committed cache still contains the previous bandwidth. */
	assert(ad9361_tx_quad_restore_bandwidth_target(
		UINT32_C(56000000), UINT32_C(44000000),
		&restore_rx_bw, &restore_tx_bw));
	assert(restore_rx_bw == UINT32_C(56000000));
	assert(restore_tx_bw == UINT32_C(44000000));
	assert(!ad9361_tx_quad_restore_bandwidth_target(
		0, UINT32_C(44000000), &restore_rx_bw, &restore_tx_bw));
	assert(!ad9361_tx_quad_restore_bandwidth_target(
		UINT32_C(56000000), UINT32_C(44000000), NULL, &restore_tx_bw));

    /* First use of either output/mode requires calibration even if another
     * output was calibrated at exactly the same LO. */
    history[0][AD9361_TX_QUAD_MODE_2X2] = UINT64_C(2400000000);
    valid[0][AD9361_TX_QUAD_MODE_2X2] = true;
    assert(!ad9361_tx_quad_calibration_required(
        valid[0][AD9361_TX_QUAD_MODE_2X2],
        history[0][AD9361_TX_QUAD_MODE_2X2], UINT64_C(2450000000),
        threshold));
    assert(ad9361_tx_quad_calibration_required(
        valid[1][AD9361_TX_QUAD_MODE_2X2],
        history[1][AD9361_TX_QUAD_MODE_2X2], UINT64_C(2450000000),
        threshold));
    assert(ad9361_tx_quad_calibration_required(
        valid[0][AD9361_TX_QUAD_MODE_TX2],
        history[0][AD9361_TX_QUAD_MODE_TX2], UINT64_C(2450000000),
        threshold));

    /* Preserve ADI's strict >100 MHz refresh boundary. */
    assert(!ad9361_tx_quad_calibration_required(
        true, UINT64_C(2400000000), UINT64_C(2500000000), threshold));
    assert(ad9361_tx_quad_calibration_required(
        true, UINT64_C(2400000000), UINT64_C(2500000001), threshold));
    assert(ad9361_tx_quad_calibration_required(
        false, UINT64_C(2400000000), UINT64_C(2400000000), threshold));
    return 0;
}
