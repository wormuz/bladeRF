#include <libusb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BLADERF_VID 0x2cf0
#define BLADERF2_PID 0x5250
#define NIOS_LEGACY_MAGIC 'N'
#define NIOS_LEGACY_READ 0x80
#define NIOS_FPGA_VERSION_ADDR 12
#define NIOS_FPGA_VERSION_LENGTH 4
#define USB_IF_NULL 0
#define USB_IF_RF_LINK 1
#define PERIPHERAL_EP_OUT 0x02
#define PERIPHERAL_EP_IN 0x82
#define TRANSFER_TIMEOUT_MS 250

static uint64_t monotonic_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        perror("clock_gettime");
        exit(2);
    }
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) +
           (uint64_t)ts.tv_nsec;
}

static libusb_device_handle *open_serial(libusb_context *ctx,
                                         const char *wanted_serial)
{
    libusb_device **devices = NULL;
    libusb_device_handle *match = NULL;
    ssize_t count = libusb_get_device_list(ctx, &devices);

    if (count < 0) {
        fprintf(stderr, "libusb_get_device_list: %s\n",
                libusb_error_name((int)count));
        return NULL;
    }

    for (ssize_t i = 0; i < count && match == NULL; i++) {
        struct libusb_device_descriptor desc;
        libusb_device_handle *candidate = NULL;
        unsigned char serial[256];
        int status;

        status = libusb_get_device_descriptor(devices[i], &desc);
        if (status != 0 || desc.idVendor != BLADERF_VID ||
            desc.idProduct != BLADERF2_PID || desc.iSerialNumber == 0) {
            continue;
        }
        status = libusb_open(devices[i], &candidate);
        if (status != 0) {
            fprintf(stderr, "libusb_open: %s\n", libusb_error_name(status));
            continue;
        }
        status = libusb_get_string_descriptor_ascii(
            candidate, desc.iSerialNumber, serial, sizeof(serial));
        if (status > 0 && strcmp((const char *)serial, wanted_serial) == 0) {
            match = candidate;
        } else {
            libusb_close(candidate);
        }
    }

    libusb_free_device_list(devices, 1);
    return match;
}

int main(int argc, char **argv)
{
    const char *serial = argc > 1 ? argv[1] :
        "f695006ba84a40daa7b777c6a6eba78";
    const unsigned long iterations = argc > 2 ? strtoul(argv[2], NULL, 10) : 100;
    libusb_context *ctx = NULL;
    libusb_device_handle *dev = NULL;
    uint8_t request[16] = {0};
    uint8_t response[16] = {0};
    uint8_t version[NIOS_FPGA_VERSION_LENGTH] = {0};
    int actual = 0;
    int status;
    int claimed = 0;
    int result = 1;
    uint64_t start_ns;
    uint64_t out_done_ns;
    uint64_t in_done_ns;
    uint64_t series_start_ns;
    uint64_t series_end_ns;

    if (argc > 3 || iterations == 0 || iterations > 100000) {
        fprintf(stderr, "usage: %s [serial] [iterations:1..100000]\n",
                argv[0]);
        return 2;
    }

    status = libusb_init(&ctx);
    if (status != 0) {
        fprintf(stderr, "libusb_init: %s\n", libusb_error_name(status));
        return 2;
    }
    dev = open_serial(ctx, serial);
    if (dev == NULL) {
        fprintf(stderr, "bladeRF 2 not found: serial=%s\n", serial);
        goto cleanup;
    }

    (void)libusb_set_auto_detach_kernel_driver(dev, 1);
    status = libusb_claim_interface(dev, 0);
    if (status != 0) {
        fprintf(stderr, "claim interface 0: %s\n", libusb_error_name(status));
        goto cleanup;
    }
    claimed = 1;
    status = libusb_set_interface_alt_setting(dev, 0, USB_IF_RF_LINK);
    if (status != 0) {
        fprintf(stderr, "select RF-link USB altsetting: %s\n",
                libusb_error_name(status));
        goto cleanup;
    }

    /* Match libbladeRF's four-byte FPGA-version read. Legacy config reads
     * advance a cursor across four one-byte requests, so preserve that exact
     * sequence and finish it before releasing the USB interface. */
    request[0] = NIOS_LEGACY_MAGIC;
    series_start_ns = monotonic_ns();
    for (unsigned long iteration = 0; iteration < iterations; ++iteration) {
        start_ns = monotonic_ns();
        for (uint8_t i = 0; i < NIOS_FPGA_VERSION_LENGTH; i++) {
            request[1] = NIOS_LEGACY_READ | 1;
            request[2] = NIOS_FPGA_VERSION_ADDR + i;
            request[3] = 0xff;
            actual = 0;
            status = libusb_bulk_transfer(dev, PERIPHERAL_EP_OUT, request,
                                          sizeof(request), &actual,
                                          TRANSFER_TIMEOUT_MS);
            out_done_ns = monotonic_ns();
            if (status != 0 || actual != (int)sizeof(request)) {
                fprintf(stderr, "NIOS request OUT iteration=%lu byte=%u failed: "
                        "%s transferred=%d\n", iteration, i,
                        libusb_error_name(status), actual);
                goto cleanup;
            }
            if (iteration == 0 && i == 0) {
                printf("first_nios_out_status=%d transferred=%d elapsed_ms=%.3f\n",
                       status, actual, (double)(out_done_ns - start_ns) / 1.0e6);
                fflush(stdout);
            }

            actual = 0;
            status = libusb_bulk_transfer(dev, PERIPHERAL_EP_IN, response,
                                          sizeof(response), &actual,
                                          TRANSFER_TIMEOUT_MS);
            in_done_ns = monotonic_ns();
            if (status != 0 || actual != (int)sizeof(response)) {
                fprintf(stderr, "NIOS response IN iteration=%lu byte=%u failed: "
                        "%s transferred=%d\n", iteration, i,
                        libusb_error_name(status), actual);
                goto cleanup;
            }
            /* A full USB packet alone only proves that bytes reached the IN
             * endpoint. Validate each NIOS legacy reply and its requested offset. */
            if (response[0] != NIOS_LEGACY_MAGIC ||
                response[1] != (NIOS_LEGACY_READ | 1) || response[2] != i) {
                fprintf(stderr,
                        "NIOS response[%u] mismatch: magic=0x%02x cfg=0x%02x "
                        "offset=0x%02x\n", i, response[0], response[1],
                        response[2]);
                goto cleanup;
            }
            version[i] = response[3];
            if (iteration == 0 && i == 0) {
                printf("first_nios_in_status=%d transferred=%d elapsed_ms=%.3f "
                       "total_ms=%.3f response_magic=0x%02x response_cfg=0x%02x\n",
                       status, actual, (double)(in_done_ns - out_done_ns) / 1.0e6,
                       (double)(in_done_ns - start_ns) / 1.0e6,
                       response[0], response[1]);
                fflush(stdout);
            }
        }
    }
    series_end_ns = monotonic_ns();
    printf("nios_fpga_version=%u.%u.%u.%u\n",
           version[0], version[1], version[2], version[3]);
    printf("nios_read_responses=%lu/%lu total_ms=%.3f avg_group_ms=%.3f\n",
           iterations * NIOS_FPGA_VERSION_LENGTH,
           iterations * NIOS_FPGA_VERSION_LENGTH,
           (double)(series_end_ns - series_start_ns) / 1.0e6,
           (double)(series_end_ns - series_start_ns) /
               ((double)iterations * 1.0e6));
    result = 0;

cleanup:
    if (dev != NULL) {
        /* Restore the idle USB altsetting; this does not reset the FPGA. */
        (void)libusb_set_interface_alt_setting(dev, 0, USB_IF_NULL);
        if (claimed) {
            (void)libusb_release_interface(dev, 0);
        }
        libusb_close(dev);
    }
    libusb_exit(ctx);
    return result;
}
