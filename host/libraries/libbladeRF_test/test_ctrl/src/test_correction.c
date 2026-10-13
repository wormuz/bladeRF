/*
 * This file is part of the bladeRF project:
 *   http://www.github.com/nuand/bladeRF
 *
 * Copyright (C) 2014 Nuand LLC
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include "test_ctrl.h"
#include <string.h>

DECLARE_TEST_CASE(correction);

static void dump_rx_offset_registers(struct bladerf *dev, bool low_band)
{
    uint8_t value;
    uint16_t start = low_band ? 0x17d : 0x174;
    for (uint16_t address = start; address < start + 5; ++address) {
        int status = bladerf_get_rfic_register(dev, address, &value);
        if (status == 0) {
            fprintf(stderr, "[DIAG] AD9361[0x%03x]=0x%02x\n",
                    address, value);
        } else {
            fprintf(stderr, "[DIAG] AD9361[0x%03x] read failed: %s\n",
                    address, bladerf_strerror(status));
        }
    }
    int status = bladerf_get_rfic_register(dev, 0x182, &value);
    if (status == 0) {
        fprintf(stderr, "[DIAG] AD9361[0x182]=0x%02x\n", value);
    }
    const uint16_t diagnostic_registers[] = {0x169, 0x18b, 0x189, 0x1a2,
                                               0x1a3, 0x1a4, 0x1a5};
    for (size_t i = 0; i < ARRAY_SIZE(diagnostic_registers); ++i) {
        int status = bladerf_get_rfic_register(
            dev, diagnostic_registers[i], &value);
        if (status == 0) {
            fprintf(stderr, "[DIAG] AD9361[0x%03x]=0x%02x\n",
                    diagnostic_registers[i], value);
        } else {
            fprintf(stderr, "[DIAG] AD9361[0x%03x] read failed: %s\n",
                    diagnostic_registers[i], bladerf_strerror(status));
        }
    }
}

const struct correction {
    const char *name;
    bladerf_correction type;
    int min;
    int max;
} corrections[] = {
    {
        "I DC offset",
        BLADERF_CORR_LMS_DCOFF_I,
        -2048,
        2048,
    },

    {
        "Q DC offset",
        BLADERF_CORR_LMS_DCOFF_Q,
        -2048,
        2048,
    },

    {
        "Phase",
        BLADERF_CORR_FPGA_PHASE,
        -4096,
        4096,
    },

    {
        "Gain",
        BLADERF_CORR_FPGA_GAIN,
        -4096,
        4096,
    },
};

static int set_and_check(struct bladerf *dev,
                         const struct correction *c,
                         bladerf_channel ch,
                         int16_t val)
{
    int status;
    int16_t readback, scaled_readback, scaled_val;

    status = bladerf_set_correction(dev, ch, c->type, val);
    if (status != 0) {
        PR_ERROR("Failed to set %s correction: %s\n", c->name,
                 bladerf_strerror(status));

        return status;
    }

    status = bladerf_get_correction(dev, ch, c->type, &readback);
    if (status != 0) {
        PR_ERROR("Failed to get %s correction: %s\n", c->name,
                 bladerf_strerror(status));

        return status;
    }

    /* These functions will scale and clamp. Mirror that here so we can check
     * the expected values... */
    switch (c->type) {
        case BLADERF_CORR_LMS_DCOFF_I:
        case BLADERF_CORR_LMS_DCOFF_Q:
            if (!BLADERF_CHANNEL_IS_TX(ch)) {
                if (val <= -2048) {
                    val = -2047;
                }
                scaled_val      = val / 64;
                scaled_readback = val / 64;
            } else {
                scaled_val      = val / 128;
                scaled_readback = val / 128;
            }
            break;

        default:
            scaled_val      = val;
            scaled_readback = readback;
            break;
    }

    if (scaled_val != scaled_readback) {
        PR_ERROR("Correction mismatch, val=%d, readback=%d\n", val, readback);
        return -1;
    }

    return 0;
}

static failure_count sweep_vals(struct bladerf *dev,
                                bladerf_channel ch,
                                bool quiet)
{
    int status;
    int val;
    size_t i;
    failure_count failures = 0;

    for (i = 0; i < ARRAY_SIZE(corrections); i++) {
        PRINT("    %s\n", corrections[i].name);

        for (val = corrections[i].min; val <= corrections[i].max; val++) {
            status = set_and_check(dev, &corrections[i], ch, val);
            if (status != 0) {
                failures++;
            }
        }
    }

    return failures;
}

static failure_count random_vals(struct bladerf *dev,
                                 struct app_params *p,
                                 bladerf_channel ch,
                                 bool quiet)
{
    int status;
    int val, j;
    size_t i;
    const int iterations  = 2500;
    failure_count failures = 0;

    for (i = 0; i < ARRAY_SIZE(corrections); i++) {
        PRINT("    %s\n", corrections[i].name);

        for (j = 0; j < iterations; j++) {
            randval_update(&p->randval_state);
            val = corrections[i].min + (p->randval_state % corrections[i].max);

            if (val < corrections[i].min) {
                val = corrections[i].min;
            } else if (val > corrections[i].max) {
                val = corrections[i].max;
            }

            status = set_and_check(dev, &corrections[i], ch, val);
            if (status != 0) {
                failures++;
            }
        }
    }

    return failures;
}

/* Verify correction readback and packed-field independence for each RX port.
 * A set/get round trip alone cannot catch adjacent fields being overwritten. */
static failure_count test_rx_correction_independence(
    struct bladerf *dev, bladerf_channel ch, bool quiet,
    const char *target_port)
{
    const char *ports[16];
    const char *original_port = NULL;
    const char *selected_port = NULL;
    const char *original_rx1_port = NULL;
    unsigned int i;
    int port_count, status;
    int16_t original_gain = 0, original_phase = 0;
    int16_t original_i = 0, original_q = 0;
    int16_t gain_readback, phase_readback, i_readback, q_readback;
    int16_t original_rx1_i = 0, rx1_i_readback;
    bool have_original_corrections = false;
    bool have_rx1_i_snapshot = false;
    failure_count failures = 0;

    port_count = bladerf_get_rf_ports(dev, ch, NULL, 0);
    if (port_count < 0 || (unsigned int)port_count > ARRAY_SIZE(ports)) {
        PR_ERROR("Unable to enumerate RX ports for correction test\n");
        return 1;
    }
    status = bladerf_get_rf_ports(dev, ch, ports, (unsigned int)port_count);
    if (status < 0) {
        PR_ERROR("Unable to read RX ports for correction test: %s\n",
                 bladerf_strerror(status));
        return 1;
    }
    for (i = 0; i < (unsigned int)port_count; i++) {
        if (strcmp(ports[i], target_port) == 0) {
            selected_port = ports[i];
            break;
        }
    }
    if (selected_port == NULL) {
        PRINT("    %s unavailable; skipping RX correction alias check\n",
              target_port);
        return 0;
    }

    status = bladerf_get_rf_port(dev, ch, &original_port);
    if (status == 0) {
        status = bladerf_set_rf_port(dev, ch, selected_port);
    }
    if (status == 0) {
        status = bladerf_get_correction(dev, ch, BLADERF_CORR_GAIN,
                                        &original_gain);
    }
    if (status == 0) {
        status = bladerf_get_correction(dev, ch, BLADERF_CORR_PHASE,
                                        &original_phase);
    }
    if (status == 0) {
        status = bladerf_get_correction(dev, ch, BLADERF_CORR_DCOFF_I,
                                        &original_i);
    }
    if (status == 0) {
        status = bladerf_get_correction(dev, ch, BLADERF_CORR_DCOFF_Q,
                                        &original_q);
    }
    if (status == 0 && ch == BLADERF_CHANNEL_RX(1)) {
        status = bladerf_get_rf_port(dev, BLADERF_CHANNEL_RX(0),
                                     &original_rx1_port);
        if (status == 0) {
            status = bladerf_set_rf_port(dev, BLADERF_CHANNEL_RX(0),
                                         selected_port);
        }
        if (status == 0) {
            status = bladerf_get_correction(dev, BLADERF_CHANNEL_RX(0),
                                            BLADERF_CORR_DCOFF_I,
                                            &original_rx1_i);
        }
        have_rx1_i_snapshot = status == 0;
    }
    if (status != 0) {
        PR_ERROR("Unable to prepare %s correction independence check: %s\n",
                 target_port, bladerf_strerror(status));
        failures++;
        goto restore_port;
    }
    have_original_corrections = true;

    status = bladerf_set_correction(dev, ch, BLADERF_CORR_GAIN, 128);
    if (status == 0) {
        status = bladerf_set_correction(dev, ch, BLADERF_CORR_PHASE, 192);
    }
    if (status == 0) {
        status = bladerf_get_correction(dev, ch, BLADERF_CORR_GAIN,
                                        &gain_readback);
    }
    if (status == 0) {
        status = bladerf_get_correction(dev, ch, BLADERF_CORR_PHASE,
                                        &phase_readback);
    }
    if (status != 0 || gain_readback != 128 || phase_readback != 192) {
        PR_ERROR("RX%u %s gain/phase corrections aliased or failed "
                 "(status=%s gain=%d phase=%d)\n",
                 ch == BLADERF_CHANNEL_RX(0) ? 1u : 2u,
                 target_port,
                 bladerf_strerror(status),
                 status == 0 ? gain_readback : 0,
                 status == 0 ? phase_readback : 0);
        failures++;
    }

    /* Check signed I values, then set Q and ensure the packed I/Q fields
     * remain independent. */
    status = bladerf_set_correction(dev, ch, BLADERF_CORR_DCOFF_I, 1552);
    if (status == 0) {
        int reverse_status = bladerf_set_correction(
            dev, ch, BLADERF_CORR_DCOFF_I, -1552);
        int16_t reverse_i = 0;
        if (reverse_status == 0) {
            reverse_status = bladerf_get_correction(
                dev, ch, BLADERF_CORR_DCOFF_I, &reverse_i);
        }
        if (reverse_status != 0 || reverse_i != -1552) {
            PR_ERROR("RX%u %s I negative round-trip failed "
                     "(status=%s value=%d)\n",
                     ch == BLADERF_CHANNEL_RX(0) ? 1u : 2u,
                     target_port,
                     bladerf_strerror(reverse_status), reverse_i);
            failures++;
        }
        status = bladerf_set_correction(dev, ch, BLADERF_CORR_DCOFF_I, 1552);
        if (status == 0) {
            status = bladerf_set_correction(dev, ch, BLADERF_CORR_DCOFF_Q, 512);
        }
    }
    if (status == 0) {
        status = bladerf_get_correction(dev, ch, BLADERF_CORR_DCOFF_I,
                                        &i_readback);
    }
    if (status == 0) {
        status = bladerf_get_correction(dev, ch, BLADERF_CORR_DCOFF_Q,
                                        &q_readback);
    }
    if (status == 0 && have_rx1_i_snapshot) {
        status = bladerf_get_correction(dev, BLADERF_CHANNEL_RX(0),
                                        BLADERF_CORR_DCOFF_I,
                                        &rx1_i_readback);
    }
    if (status != 0 || i_readback != 1552 || q_readback != 512) {
        PR_ERROR("RX%u %s I/Q offsets aliased or failed "
                 "(status=%s I=%d Q=%d)\n",
                 ch == BLADERF_CHANNEL_RX(0) ? 1u : 2u,
                 target_port,
                 bladerf_strerror(status),
                 status == 0 ? i_readback : 0,
                 status == 0 ? q_readback : 0);
        failures++;
    }
    if (status == 0 && have_rx1_i_snapshot &&
        rx1_i_readback != original_rx1_i) {
        PR_ERROR("RX2 %s Q offset changed RX1 I offset "
                 "(%d -> %d)\n", target_port, original_rx1_i,
                 rx1_i_readback);
        failures++;
    }
    if (failures != 0) {
        dump_rx_offset_registers(dev, strcmp(target_port, "A_BALANCED") != 0);
    }

    if (have_original_corrections) {
        int restore_status = bladerf_set_correction(
            dev, ch, BLADERF_CORR_GAIN, original_gain);
        if (restore_status == 0) {
            restore_status = bladerf_set_correction(
                dev, ch, BLADERF_CORR_PHASE, original_phase);
        }
        if (restore_status == 0) {
            restore_status = bladerf_set_correction(
                dev, ch, BLADERF_CORR_DCOFF_I, original_i);
        }
        if (restore_status == 0) {
            restore_status = bladerf_set_correction(
                dev, ch, BLADERF_CORR_DCOFF_Q, original_q);
        }
        if (restore_status == 0 && have_rx1_i_snapshot) {
            restore_status = bladerf_set_correction(
                dev, BLADERF_CHANNEL_RX(0), BLADERF_CORR_DCOFF_I,
                original_rx1_i);
        }
        if (restore_status != 0) {
            PR_ERROR("Failed to restore RX correction values: %s\n",
                     bladerf_strerror(restore_status));
            failures++;
        }
    }

restore_port:
    if (original_rx1_port != NULL) {
        int restore_status = bladerf_set_rf_port(
            dev, BLADERF_CHANNEL_RX(0), original_rx1_port);
        if (restore_status != 0) {
            PR_ERROR("Failed to restore RX1 RF port: %s\n",
                     bladerf_strerror(restore_status));
            failures++;
        }
    }
    if (original_port != NULL) {
        int restore_status = bladerf_set_rf_port(dev, ch, original_port);
        if (restore_status != 0) {
            PR_ERROR("Failed to restore RX RF port: %s\n",
                     bladerf_strerror(restore_status));
            failures++;
        }
    }
    return failures;
}

static failure_count test_bladerf2_tx_correction_independence(
    struct bladerf *dev)
{
    static const int16_t values[2][4] = {
        {1024, -1024, 2048, -2048},
        {-1024, 1024, -2048, 2048},
    };
    int16_t original[2][4];
    failure_count failures = 0;
    int status;

    for (unsigned int lane = 0; lane < 2; lane++) {
        bladerf_channel ch = BLADERF_CHANNEL_TX(lane);
        for (size_t i = 0; i < ARRAY_SIZE(corrections); i++) {
            status = bladerf_get_correction(dev, ch, corrections[i].type,
                                            &original[lane][i]);
            if (status != 0) {
                PR_ERROR("Unable to snapshot TX%u %s correction: %s\n",
                         lane + 1, corrections[i].name,
                         bladerf_strerror(status));
                return failures + 1;
            }
        }
    }

    for (unsigned int lane = 0; lane < 2; lane++) {
        bladerf_channel ch = BLADERF_CHANNEL_TX(lane);
        for (size_t i = 0; i < ARRAY_SIZE(corrections); i++) {
            status = bladerf_set_correction(dev, ch, corrections[i].type,
                                            values[lane][i]);
            if (status != 0) {
                PR_ERROR("Failed to set TX%u %s correction: %s\n",
                         lane + 1, corrections[i].name,
                         bladerf_strerror(status));
                failures++;
                break;
            }
        }
    }

    for (unsigned int lane = 0; lane < 2; lane++) {
        bladerf_channel ch = BLADERF_CHANNEL_TX(lane);
        for (size_t i = 0; i < ARRAY_SIZE(corrections); i++) {
            int16_t readback = 0;
            status = bladerf_get_correction(dev, ch, corrections[i].type,
                                            &readback);
            if (status != 0 || readback != values[lane][i]) {
                PR_ERROR("TX%u %s correction mismatch: status=%s "
                         "expected=%d got=%d\n",
                         lane + 1, corrections[i].name,
                         bladerf_strerror(status), values[lane][i],
                         status == 0 ? readback : 0);
                failures++;
            }
        }
    }

    for (unsigned int lane = 0; lane < 2; lane++) {
        bladerf_channel ch = BLADERF_CHANNEL_TX(lane);
        for (size_t i = 0; i < ARRAY_SIZE(corrections); i++) {
            status = bladerf_set_correction(dev, ch, corrections[i].type,
                                            original[lane][i]);
            if (status != 0) {
                PR_ERROR("Failed to restore TX%u %s correction: %s\n",
                         lane + 1, corrections[i].name,
                         bladerf_strerror(status));
                failures++;
            }
        }
    }

    return failures;
}


failure_count test_correction(struct bladerf *dev,
                              struct app_params *p,
                              bool quiet)
{
    failure_count failures = 0;

    const char *board_name = bladerf_get_board_name(dev);
    if (0 != strcmp(board_name, "bladerf1") &&
        0 != strcmp(board_name, "bladerf2")) {
        PRINT("%s: unsupported board, skipping\n", __FUNCTION__);
        return 0;
    }

    if (0 == strcmp(board_name, "bladerf2")) {
        failures += test_rx_correction_independence(
            dev, BLADERF_CHANNEL_RX(0), quiet, "A_BALANCED");
        failures += test_rx_correction_independence(
            dev, BLADERF_CHANNEL_RX(1), quiet, "A_BALANCED");
        failures += test_rx_correction_independence(
            dev, BLADERF_CHANNEL_RX(0), quiet, "B_BALANCED");
        failures += test_rx_correction_independence(
            dev, BLADERF_CHANNEL_RX(1), quiet, "B_BALANCED");
        failures += test_bladerf2_tx_correction_independence(dev);
        return failures;
    }

    PRINT("%s: Sweeping RX corrections...\n", __FUNCTION__);
    failures += sweep_vals(dev, BLADERF_CHANNEL_RX(0), quiet);

    PRINT("%s: Random RX corrections...\n", __FUNCTION__);
    failures += random_vals(dev, p, BLADERF_CHANNEL_RX(0), quiet);

    PRINT("%s: Sweeping TX corrections...\n", __FUNCTION__);
    failures += sweep_vals(dev, BLADERF_CHANNEL_TX(0), quiet);

    PRINT("%s: Random TX corrections...\n", __FUNCTION__);
    failures += random_vals(dev, p, BLADERF_CHANNEL_TX(0), quiet);

    return failures;
}
