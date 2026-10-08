#define _POSIX_C_SOURCE 200809L

#include <libbladeRF.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    struct bladerf *dev = NULL;
    struct bladerf_version version = {0};
    const char *device = argc > 1 ? argv[1] : NULL;
    const char *image = argc > 2 ? argv[2] : NULL;
    int status;

    if (argc != 3) {
        fprintf(stderr, "usage: %s <device-spec> <volatile-rbf>\n", argv[0]);
        return 2;
    }

    /* Open without asking the unresponsive FPGA/NIOS for its version. This
     * only skips FPGA discovery; the FX3 firmware still has to enumerate and
     * answer its normal readiness/version requests. */
    if (setenv("BLADERF_FORCE_NO_FPGA_PRESENT", "1", 1) != 0 ||
        setenv("BLADERF_FORCE_FPGA_A4", "1", 1) != 0) {
        perror("setenv");
        return 2;
    }
    bladerf_log_set_verbosity(BLADERF_LOG_LEVEL_DEBUG);

    status = bladerf_open(&dev, device);
    if (status != 0) {
        fprintf(stderr, "open-without-fpga-probe: %s\n",
                bladerf_strerror(status));
        return 1;
    }

    /* The FPGA loader must see the real configuration-status response after
     * programming, so remove the discovery override before loading. */
    if (unsetenv("BLADERF_FORCE_NO_FPGA_PRESENT") != 0) {
        perror("unsetenv");
        bladerf_close(dev);
        return 2;
    }

    fprintf(stderr, "Loading volatile xA4 FPGA image: %s\n", image);
    status = bladerf_load_fpga(dev, image);
    if (status != 0) {
        fprintf(stderr, "volatile-fpga-load: %s\n", bladerf_strerror(status));
        bladerf_close(dev);
        return 1;
    }

    status = bladerf_is_fpga_configured(dev);
    if (status != 1) {
        fprintf(stderr, "fpga-configured-check: %s (%d)\n",
                status < 0 ? bladerf_strerror(status) : "not configured", status);
        bladerf_close(dev);
        return 1;
    }

    status = bladerf_fpga_version(dev, &version);
    if (status != 0) {
        fprintf(stderr, "fpga-version-readback: %s\n",
                bladerf_strerror(status));
        bladerf_close(dev);
        return 1;
    }

    printf("volatile FPGA load verified: %u.%u.%u\n", version.major,
           version.minor, version.patch);
    bladerf_close(dev);
    return 0;
}
