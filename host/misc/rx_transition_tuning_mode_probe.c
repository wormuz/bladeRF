#include <libbladeRF.h>

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    struct bladerf *dev = NULL;
    struct bladerf_rx_transition_request request;
    bladerf_tuning_mode mode = BLADERF_TUNING_MODE_INVALID;
    uint32_t transaction_id = UINT32_C(0xa5a5a5a5);
    int status;
    int result = 1;
    bool fpga_mode_set = false;

    status = bladerf_open(&dev, NULL);
    if (status != 0) {
        fprintf(stderr, "OPEN_FAILED status=%s (%d)\n",
                bladerf_strerror(status), status);
        return 1;
    }

    status = bladerf_set_tuning_mode(dev, BLADERF_TUNING_MODE_FPGA);
    if (status != 0) {
        fprintf(stderr, "SET_FPGA_TUNING_MODE_FAILED status=%s (%d)\n",
                bladerf_strerror(status), status);
        goto done;
    }
    fpga_mode_set = true;

    status = bladerf_get_tuning_mode(dev, &mode);
    if (status != 0 || mode != BLADERF_TUNING_MODE_FPGA) {
        fprintf(stderr, "GET_FPGA_TUNING_MODE_FAILED status=%s (%d) mode=%d\n",
                bladerf_strerror(status), status, mode);
        goto done;
    }

    memset(&request, 0, sizeof(request));
    request.target_frequency_hz = UINT64_C(1835000000);
    request.required_events_mask = BLADERF_RF_REQUIRE_EPOCH_VALID |
                                   BLADERF_RF_REQUIRE_RX_RFDC_CAL_DONE;
    request.timeout_ms = 1000;
    request.require_rx_data_valid = true;

    status = bladerf_rx_transition_begin(dev, BLADERF_CHANNEL_RX(0),
                                         &request, &transaction_id);
    if (status != BLADERF_ERR_UNSUPPORTED ||
        transaction_id != UINT32_C(0xa5a5a5a5)) {
        fprintf(stderr,
                "TRANSITION_GUARD_FAILED status=%s (%d) txn=0x%08" PRIx32
                "\n", bladerf_strerror(status), status, transaction_id);
        goto done;
    }

    printf("TRANSITION_GUARD_OK mode=FPGA status=%s txn_unchanged=1\n",
           bladerf_strerror(status));
    result = 0;

done:
    if (fpga_mode_set) {
        int restore_status = bladerf_set_tuning_mode(
            dev, BLADERF_TUNING_MODE_HOST);
        if (restore_status != 0) {
            fprintf(stderr, "RESTORE_HOST_TUNING_MODE_FAILED status=%s (%d)\n",
                    bladerf_strerror(restore_status), restore_status);
            result = 1;
        }
    }
    bladerf_close(dev);
    return result;
}
