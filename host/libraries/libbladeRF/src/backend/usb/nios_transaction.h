/* Internal NIOS USB request/response transport. */
#ifndef BACKEND_USB_NIOS_TRANSACTION_H_
#define BACKEND_USB_NIOS_TRANSACTION_H_

#include <stdint.h>

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
