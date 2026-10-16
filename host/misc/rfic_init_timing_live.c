#include <libbladeRF.h>
#include "backend/usb/usb.h"
#include "board/board.h"
#include "nios_pkt_8x64.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#define DEFAULT_SERIAL "f695006ba84a40daa7b777c6a6eba78"

static int read_metric(struct bladerf *dev, uint8_t addr, uint64_t *value)
{
    struct bladerf_usb *usb = dev->backend_data;
    uint8_t packet[NIOS_PKT_8x64_LEN];
    uint32_t transferred = 0;
    bool transferred_valid = false;
    bool success = false;
    uint8_t target = 0;
    uint8_t response_addr = 0;
    int status;

    nios_pkt_8x64_pack(packet, NIOS_PKT_8x64_TARGET_RFIC_TIMING,
                       false, addr, 0);
    status = usb->fn->bulk_transfer(usb->driver, PERIPHERAL_EP_OUT,
                                    packet, sizeof(packet), 0,
                                    &transferred, &transferred_valid);
    if (status != 0 || !transferred_valid || transferred != sizeof(packet)) {
        return status != 0 ? status : BLADERF_ERR_UNEXPECTED;
    }

    transferred = 0;
    transferred_valid = false;
    status = usb->fn->bulk_transfer(usb->driver, PERIPHERAL_EP_IN,
                                    packet, sizeof(packet), 0,
                                    &transferred, &transferred_valid);
    if (status != 0 || !transferred_valid || transferred != sizeof(packet)) {
        return status != 0 ? status : BLADERF_ERR_UNEXPECTED;
    }

    nios_pkt_8x64_resp_unpack(packet, &target, NULL, &response_addr,
                              value, &success);
    if (!success || target != NIOS_PKT_8x64_TARGET_RFIC_TIMING ||
        response_addr != addr) {
        return BLADERF_ERR_FPGA_OP;
    }

    return 0;
}

int main(int argc, char **argv)
{
    const char *serial = argc > 1 ? argv[1] : DEFAULT_SERIAL;
    const unsigned runs = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 1;

    if (argc > 3 || runs == 0 || runs > 1000) {
        fprintf(stderr, "usage: %s [serial] [runs:1..1000]\n", argv[0]);
        return 2;
    }

    for (unsigned run = 0; run < runs; ++run) {
        struct bladerf *dev = NULL;
        uint64_t status = 0;
        uint64_t total_ticks = 0;
        uint64_t max_worker_ticks = 0;
        uint64_t max_service_gap_ticks = 0;
        uint64_t counter_hz = 0;
        uint64_t init_stage = 0;
        uint64_t result_detail = 0;
        uint64_t adi_status = 0;
        uint64_t adi_cleanup_status = 0;
        uint64_t adi_stage = 0;
        uint64_t setup_stage = 0;
        uint64_t error_status = 0;
        uint64_t error_context = 0;
        char device_spec[128];
        snprintf(device_spec, sizeof(device_spec), "*:serial=%s", serial);
        int result = bladerf_open(&dev, device_spec);
        if (result != 0) {
            fprintf(stderr, "open run=%u: %s\n", run,
                    bladerf_strerror(result));
            return 1;
        }

        const int init_status =
            bladerf_set_tuning_mode(dev, BLADERF_TUNING_MODE_FPGA);
        if (init_status != 0)
            fprintf(stderr, "FPGA RFIC INIT run=%u: %s\n", run,
                    bladerf_strerror(init_status));

        result = read_metric(dev, NIOS_PKT_8x64_RFIC_TIMING_STATUS, &status);
        if (result == 0)
            result = read_metric(dev, NIOS_PKT_8x64_RFIC_TIMING_TOTAL_TICKS,
                                 &total_ticks);
        if (result == 0)
            result = read_metric(dev,
                    NIOS_PKT_8x64_RFIC_TIMING_MAX_WORKER_TICKS,
                    &max_worker_ticks);
        if (result == 0)
            result = read_metric(dev,
                    NIOS_PKT_8x64_RFIC_TIMING_MAX_SERVICE_GAP_TICKS,
                    &max_service_gap_ticks);
        if (result == 0)
            result = read_metric(dev, NIOS_PKT_8x64_RFIC_TIMING_COUNTER_HZ,
                                 &counter_hz);
        if (result == 0)
            result = read_metric(dev, NIOS_PKT_8x64_RFIC_TIMING_INIT_STAGE,
                                 &init_stage);
        if (result == 0)
            result = read_metric(dev,
                    NIOS_PKT_8x64_RFIC_TIMING_RESULT_DETAIL,
                    &result_detail);
        if (result == 0)
            result = read_metric(dev, NIOS_PKT_8x64_RFIC_TIMING_ADI_STATUS,
                                 &adi_status);
        if (result == 0)
            result = read_metric(dev,
                    NIOS_PKT_8x64_RFIC_TIMING_ADI_CLEANUP_STATUS,
                    &adi_cleanup_status);
        if (result == 0)
            result = read_metric(dev, NIOS_PKT_8x64_RFIC_TIMING_ADI_STAGE,
                                 &adi_stage);
        if (result == 0)
            result = read_metric(dev,
                    NIOS_PKT_8x64_RFIC_TIMING_SETUP_STAGE, &setup_stage);
        if (result == 0)
            result = read_metric(dev,
                    NIOS_PKT_8x64_RFIC_TIMING_ERROR_STATUS, &error_status);
        if (result == 0)
            result = read_metric(dev,
                    NIOS_PKT_8x64_RFIC_TIMING_ERROR_CONTEXT, &error_context);

        const bool valid = (status & 1u) != 0;
        const bool active = (status & 2u) != 0;
        const uint16_t transaction_id = (uint16_t)(status >> 16);
        const uint32_t sequence = (uint32_t)(status >> 32);
        if (result == 0 && (!valid || active || total_ticks == 0 ||
                            counter_hz == 0 ||
                            max_worker_ticks > total_ticks ||
                            max_service_gap_ticks > total_ticks)) {
            fprintf(stderr,
                    "invalid INIT timing run=%u valid=%u active=%u "
                    "txn=%u seq=%" PRIu32 " total=%" PRIu64
                    " worker=%" PRIu64 " gap=%" PRIu64 " hz=%" PRIu64
                    "\n", run, valid, active, transaction_id, sequence,
                    total_ticks, max_worker_ticks, max_service_gap_ticks,
                    counter_hz);
            result = BLADERF_ERR_UNEXPECTED;
        }

        const int restore_status =
            bladerf_set_tuning_mode(dev, BLADERF_TUNING_MODE_HOST);
        bladerf_close(dev);
        if (result != 0) {
            fprintf(stderr, "timing read run=%u: %s\n", run,
                    bladerf_strerror(result));
            return 1;
        }
        if (restore_status != 0) {
            fprintf(stderr, "HOST RFIC ownership restore run=%u: %s\n",
                    run, bladerf_strerror(restore_status));
            return 1;
        }

        printf("INIT_TIMING run=%u init_status=%d txn=%u seq=%" PRIu32
               " init_stage=%" PRIu64 " result_detail=0x%" PRIx64
               " adi_status=%" PRIi32 " adi_cleanup=%" PRIi32
               " adi_stage=%" PRIu64 " setup_stage=%" PRIu64
               " error_status=%" PRIi32 " error_context=0x%" PRIx64
               " hz=%" PRIu64 " total_ticks=%" PRIu64
               " total_ms=%.3f max_worker_ticks=%" PRIu64
               " max_worker_ms=%.3f max_service_gap_ticks=%" PRIu64
               " max_service_gap_ms=%.3f\n",
               run, init_status, transaction_id, sequence, init_stage,
               result_detail, (int32_t)adi_status,
               (int32_t)adi_cleanup_status, adi_stage, setup_stage,
               (int32_t)error_status, error_context,
               counter_hz, total_ticks,
               1000.0 * (double)total_ticks / (double)counter_hz,
               max_worker_ticks,
               1000.0 * (double)max_worker_ticks / (double)counter_hz,
               max_service_gap_ticks,
               1000.0 * (double)max_service_gap_ticks /
                   (double)counter_hz);
        if (init_status != 0) return 1;
    }

    return 0;
}
