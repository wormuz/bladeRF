/* Internal NIOS USB request/response transport. */
#ifndef BACKEND_USB_NIOS_TRANSACTION_H_
#define BACKEND_USB_NIOS_TRANSACTION_H_

#include <stdint.h>

/* Long RFIC commands run synchronously in the NIOS packet handler. AD9361
 * calibration polls are bounded at 24 s nominal per calibration. A preserved
 * mode-switch request remained blocked for about 55 s before the host probe
 * was interrupted; that is a lower bound on the wait, not a measured
 * successful init duration. Keep the transport watchdog beyond that trace
 * while the true command bound is investigated. This timeout is failure
 * detection only, never RF/IQ validity evidence. */
#define NIOS_RFIC_RESPONSE_TIMEOUT_MS 120000u

#include "usb.h"

enum nios_transaction_stage {
    NIOS_TRANSACTION_DESYNCHRONIZED,
    NIOS_TRANSACTION_OUT_FAILED,
    NIOS_TRANSACTION_IN_FAILED,
    NIOS_TRANSACTION_COMPLETE,
};

typedef void (*nios_transaction_out_complete_fn)(void *context);

int nios_usb_transaction(struct bladerf_usb *usb, void *buffer,
                         uint32_t buffer_len, uint32_t response_timeout_ms,
                         nios_transaction_out_complete_fn out_complete,
                         void *context,
                         enum nios_transaction_stage *stage);

#endif
