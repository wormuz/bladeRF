/* Hardware regression for scheduled async TX admission.
 *
 * The test uses the AD9361 BIST loopback, so an unguarded past-dated packet
 * cannot radiate. Current libbladeRF must reject that packet before USB
 * submission with BLADERF_ERR_TIME_PAST.
 */
#include <stdbool.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libbladeRF.h>

#include "host_config.h"
#include "metadata.h"

#define TX_FREQUENCY_HZ UINT64_C(925000000)
#define TX_SAMPLE_RATE_HZ UINT32_C(10000000)
#define TX_BANDWIDTH_HZ UINT32_C(5000000)
#define TX_BUFFER_SAMPLES 2048u
#define TX_PACKET_PAYLOAD_SAMPLES 4u
#define TX_PACKET_DWORDS 8u
#define TX_NUM_BUFFERS 1u
#define TX_NUM_TRANSFERS 1u

struct test_state {
    void **buffers;
    unsigned int callback_count;
    bladerf_timestamp scheduled_timestamp;
};

static void *tx_callback(struct bladerf *dev, struct bladerf_stream *stream,
                         struct bladerf_metadata *metadata, void *samples,
                         size_t num_samples, void *user_data)
{
    struct test_state *state = user_data;
    uint8_t *buffer;
    int16_t tone[TX_PACKET_PAYLOAD_SAMPLES * 2u] = {
        1200, 0, 0, 1200, -1200, 0, 0, -1200
    };
    (void)dev;
    (void)stream;
    (void)num_samples;

    if (state->callback_count++ != 0 || samples != NULL) {
        return BLADERF_STREAM_SHUTDOWN;
    }

    buffer = state->buffers[0];
    memset(buffer, 0, TX_BUFFER_SAMPLES * sizeof(int32_t));
    memcpy(buffer + METADATA_HEADER_SIZE, tone, sizeof(tone));
    metadata_set_packet(buffer, state->scheduled_timestamp,
                        BLADERF_META_FLAG_TX_BURST_START |
                            BLADERF_META_FLAG_TX_BURST_END,
                        TX_PACKET_DWORDS -
                            METADATA_HEADER_SIZE / sizeof(uint32_t),
                        0, 0);
    metadata->actual_count = TX_PACKET_DWORDS;
    return buffer;
}

int main(void)
{
    struct bladerf *dev = NULL;
    struct bladerf_stream *stream = NULL;
    struct test_state state = {0};
    bladerf_timestamp now;
    bladerf_loopback loopback;
    int status;

    status = bladerf_open(&dev, NULL);
    if (status != 0) {
        fprintf(stderr, "open: %s\n", bladerf_strerror(status));
        return EXIT_FAILURE;
    }

    status = bladerf_set_loopback(dev, BLADERF_LB_RFIC_BIST);
    if (status == 0) {
        status = bladerf_get_loopback(dev, &loopback);
    }
    if (status == 0 && loopback != BLADERF_LB_RFIC_BIST) {
        status = BLADERF_ERR_UNEXPECTED;
    }
    if (status == 0) {
        status = bladerf_set_rx_mux(dev, BLADERF_RX_MUX_BASEBAND);
    }
    if (status == 0) {
        status = bladerf_set_frequency(dev, BLADERF_CHANNEL_RX(0),
                                       TX_FREQUENCY_HZ);
    }
    if (status == 0) {
        status = bladerf_set_frequency(dev, BLADERF_CHANNEL_TX(0),
                                       TX_FREQUENCY_HZ);
    }
    if (status == 0) {
        status = bladerf_set_sample_rate(dev, BLADERF_CHANNEL_RX(0),
                                         TX_SAMPLE_RATE_HZ, NULL);
    }
    if (status == 0) {
        status = bladerf_set_sample_rate(dev, BLADERF_CHANNEL_TX(0),
                                         TX_SAMPLE_RATE_HZ, NULL);
    }
    if (status == 0) {
        status = bladerf_set_bandwidth(dev, BLADERF_CHANNEL_RX(0),
                                       TX_BANDWIDTH_HZ, NULL);
    }
    if (status == 0) {
        status = bladerf_set_bandwidth(dev, BLADERF_CHANNEL_TX(0),
                                       TX_BANDWIDTH_HZ, NULL);
    }
    if (status == 0) {
        status = bladerf_enable_module(dev, BLADERF_CHANNEL_RX(0), true);
    }
    if (status == 0) {
        status = bladerf_enable_module(dev, BLADERF_CHANNEL_TX(0), true);
    }
    /* Module activation changes TX RF state; establish a fresh LO
     * certificate afterward, matching the synchronous qualification path. */
    if (status == 0) {
        status = bladerf_set_frequency(dev, BLADERF_CHANNEL_TX(0),
                                       TX_FREQUENCY_HZ);
    }
    /* RX configuration setters reserve the shared RF transition path. Certify
     * RX only after every setter and module activation, matching the passing
     * sync TX qualification order. No RX stream data is required for the
     * control-plane transition used by this TX-only timestamp check. */
    if (status == 0) {
        const struct bladerf_rx_transition_request request = {
            .target_frequency_hz = TX_FREQUENCY_HZ,
            .required_events_mask = BLADERF_RF_REQUIRE_PLL_LOCKED |
                BLADERF_RF_REQUIRE_ENSM_RX |
                BLADERF_RF_REQUIRE_BBPLL_LOCKED,
            .timeout_ms = 1000,
            .require_rx_data_valid = false,
        };
        struct bladerf_rf_event final_event = {0};
        uint32_t transaction_id = 0;
        status = bladerf_rx_transition_begin(
            dev, BLADERF_CHANNEL_RX(0), &request, &transaction_id);
        if (status == 0) {
            status = bladerf_rx_transition_wait(
                dev, transaction_id, &final_event, 2000);
        }
    }
    if (status == 0) {
        status = bladerf_get_timestamp(dev, BLADERF_TX, &now);
    }
    if (status != 0) {
        fprintf(stderr, "BIST/TX setup: %s\n", bladerf_strerror(status));
        goto done;
    }
    if (now == 0) {
        fprintf(stderr, "TX timestamp is zero; cannot schedule a past packet\n");
        status = BLADERF_ERR_UNEXPECTED;
        goto done;
    }

    state.scheduled_timestamp = now - 1;
    status = bladerf_init_stream(&stream, dev, tx_callback, &state.buffers,
                                 TX_NUM_BUFFERS, BLADERF_FORMAT_PACKET_META,
                                 TX_BUFFER_SAMPLES, TX_NUM_TRANSFERS, &state);
    if (status != 0) {
        fprintf(stderr, "init stream: %s\n", bladerf_strerror(status));
        goto done;
    }

    status = bladerf_stream(stream, BLADERF_TX_X1);
    printf("loopback=BIST stream_status=%d expected=%d callbacks=%u "
           "scheduled_timestamp=%llu initial_timestamp=%llu\n",
           status, BLADERF_ERR_TIME_PAST, state.callback_count,
           (unsigned long long)state.scheduled_timestamp,
           (unsigned long long)now);
    if (status != BLADERF_ERR_TIME_PAST) {
        static const uint16_t tx_status_regs[] = { 0x287, 0x017, 0x05e, 0x016 };
        static const char *const tx_status_names[] = {
            "tx_pll", "ensm", "bbpll", "calibration"
        };
        for (size_t i = 0;
             i < sizeof(tx_status_regs) / sizeof(tx_status_regs[0]); ++i) {
            uint8_t value = 0;
            const int read_status = bladerf_get_rfic_register(
                dev, tx_status_regs[i], &value);
            fprintf(stderr, "rfic_%s read_status=%d value=0x%02x\n",
                    tx_status_names[i], read_status, value);
        }
        struct bladerf_rf_event events[BLADERF_RF_EVENT_HISTORY_SIZE];
        uint32_t event_count = 0;
        uint64_t next_cursor = 0;
        bool complete = false;
        const int event_status = bladerf_rf_events_get_since(
            dev, 0, events, BLADERF_RF_EVENT_HISTORY_SIZE, &event_count,
            &next_cursor, &complete);
        fprintf(stderr, "event_history status=%d count=%u complete=%u\n",
                event_status, event_count, complete ? 1u : 0u);
        for (uint32_t i = 0; i < event_count; ++i) {
            fprintf(stderr, "event[%u] type=%u epoch=%u state=%u "
                    "error=%d flags=0x%x rfic=0x%x\n", i,
                    (unsigned int)events[i].event_type, events[i].epoch_id,
                    (unsigned int)events[i].fpga_state, events[i].error_code,
                    events[i].flags, events[i].rfic_status);
        }
        fprintf(stderr, "async TX did not reject an expired scheduled packet\n");
        status = BLADERF_ERR_UNEXPECTED;
        goto done;
    }
    status = 0;

done:
    if (stream != NULL) {
        bladerf_deinit_stream(stream);
    }
    if (dev != NULL) {
        bladerf_enable_module(dev, BLADERF_CHANNEL_TX(0), false);
        bladerf_enable_module(dev, BLADERF_CHANNEL_RX(0), false);
        bladerf_close(dev);
    }
    return status == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
