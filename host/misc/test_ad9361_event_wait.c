/* Reuse the fake SPI/time platform; exercise production wait implementations. */
#define main ad9361_full_regression_main
#include "test_ad9361_calibration_read_error.c"
#undef main
#include <assert.h>

int main(void)
{
    struct no_os_spi_desc spi = {0};
    struct ad9361_rf_phy phy = { .spi = &spi };
    struct ad9361_rf_phy contender = { .spi = &spi };
    struct ad9361_status_wait status = {0};
    struct ad9361_calibration_wait calibration = {0};
    unsigned i;

    register_map_enabled = true;
    fake_calibration_autocomplete = false;
    fake_time_us = 0;
    memset(fake_regs, 0, sizeof(fake_regs));
    assert(ad9361_status_wait_start(&phy, REG_STATE, 1, 1,
                                    100, 10, &status) == 0);
    for (i = 0; i < 64; ++i) {
        fake_time_us += 1000000000ULL;
        assert(ad9361_status_wait_poll(&phy, &status) == 1);
        assert(status.active && !status.terminal);
        assert(phy.status_wait == &status);
        assert(!ad9361_calibration_state_unknown(&phy));
    }
    assert(ad9361_status_wait_start(&contender, REG_STATE, 1, 1,
                                    100, 10, &status) == -EBUSY);
    fake_regs[REG_STATE] = 1;
    fake_time_us += 10;
    assert(ad9361_status_wait_poll(&phy, &status) == 0);
    assert(status.terminal && !status.active && !phy.status_wait);

    assert(ad9361_calibration_wait_start(&phy, BBDC_CAL, 100,
                                         &calibration) == 0);
    for (i = 0; i < 64; ++i) {
        fake_time_us += 1000000000ULL;
        assert(ad9361_calibration_wait_poll(&phy, &calibration) == 1);
        assert(calibration.active && !calibration.terminal);
        assert(phy.calibration_wait == &calibration);
        assert(!ad9361_calibration_state_unknown(&phy));
    }
    fake_regs[REG_CALIBRATION_CTRL] = 0;
    fake_time_us += calibration.poll_delay_us;
    assert(ad9361_calibration_wait_poll(&phy, &calibration) == 0);
    assert(calibration.terminal && !calibration.active && !phy.calibration_wait);

    memset(&status, 0, sizeof(status));
    assert(ad9361_status_wait_start(&phy, REG_STATE, 1, 1,
                                    100, 10, &status) == 0);
    fail_read_reg = REG_STATE;
    assert(ad9361_status_wait_poll(&phy, &status) == -EIO);
    assert(ad9361_calibration_state_unknown(&phy) && !phy.status_wait);
    assert(ad9361_status_wait_start(&phy, REG_STATE, 1, 1,
                                    100, 10, &(struct ad9361_status_wait){0}) == -EIO);
    fail_read_reg = -1;

    phy.calibration_state_unknown = false;
    memset(&calibration, 0, sizeof(calibration));
    assert(ad9361_calibration_wait_start(&phy, BBDC_CAL, 100,
                                         &calibration) == 0);
    fail_read_reg = REG_CALIBRATION_CTRL;
    assert(ad9361_calibration_wait_poll(&phy, &calibration) == -EIO);
    assert(phy.calibration_wait_quarantined && !phy.calibration_wait);
    assert(ad9361_calibration_state_unknown(&phy));
    puts("AD9361 event waits: pending ownership, late completion and SPI fault: PASS");
    return EXIT_SUCCESS;
}
