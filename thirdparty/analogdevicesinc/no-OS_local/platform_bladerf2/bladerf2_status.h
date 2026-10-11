#ifndef BLADERF2_STATUS_H_
#define BLADERF2_STATUS_H_

#include <errno.h>

#include "libbladeRF.h"

/* Translate libbladeRF backend statuses into the negative errno convention
 * expected by no-OS. ADI's error bridge maps these errno values back into
 * public bladeRF statuses. Passing the library's negative status through
 * directly is unsafe because both domains use overlapping integer values. */
static inline int bladerf2_status_to_errno(int status)
{
    switch (status) {
    case 0:
        return 0;
    case BLADERF_ERR_TIMEOUT:
        return -ETIMEDOUT;
    case BLADERF_ERR_WOULD_BLOCK:
        return -EAGAIN;
    case BLADERF_ERR_MEM:
        return -ENOMEM;
    case BLADERF_ERR_NODEV:
        return -ENODEV;
    case BLADERF_ERR_INVAL:
        return -EINVAL;
    case BLADERF_ERR_UNSUPPORTED:
#ifdef ENOTSUP
        return -ENOTSUP;
#else
        return -EIO;
#endif
    case BLADERF_ERR_IO:
    case BLADERF_ERR_FPGA_OP:
    case BLADERF_ERR_UNEXPECTED:
    default:
        return -EIO;
    }
}

#endif
