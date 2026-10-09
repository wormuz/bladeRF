#include "nios_transaction.h"

int nios_usb_transaction(struct bladerf_usb *usb, void *buffer,
                         uint32_t buffer_len, uint32_t response_timeout_ms,
                         nios_transaction_out_complete_fn out_complete,
                         void *context,
                         enum nios_transaction_stage *stage)
{
    int status;

    MUTEX_LOCK(&usb->peripheral_lock);
    if (usb->peripheral_desynchronized) {
        if (stage != NULL) {
            *stage = NIOS_TRANSACTION_DESYNCHRONIZED;
        }
        MUTEX_UNLOCK(&usb->peripheral_lock);
        return BLADERF_ERR_UNEXPECTED;
    }

    status = usb->fn->bulk_transfer(usb->driver, PERIPHERAL_EP_OUT, buffer,
                                    buffer_len, PERIPHERAL_TIMEOUT_MS);
    if (status != 0) {
        usb->peripheral_desynchronized = true;
        if (stage != NULL) {
            *stage = NIOS_TRANSACTION_OUT_FAILED;
        }
        MUTEX_UNLOCK(&usb->peripheral_lock);
        return status;
    }

    if (out_complete != NULL) {
        out_complete(context);
    }

    status = usb->fn->bulk_transfer(usb->driver, PERIPHERAL_EP_IN, buffer,
                                    buffer_len, response_timeout_ms);
    if (status != 0) {
        usb->peripheral_desynchronized = true;
        if (stage != NULL) {
            *stage = NIOS_TRANSACTION_IN_FAILED;
        }
    } else if (stage != NULL) {
        *stage = NIOS_TRANSACTION_COMPLETE;
    }

    MUTEX_UNLOCK(&usb->peripheral_lock);
    return status;
}
