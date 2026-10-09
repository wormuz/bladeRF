#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

#include "backend/usb/nios_legacy_access.h"
#include "backend/usb/nios_transaction.h"
#include "board/board.h"
#include "nios_pkt_formats.h"

static uint8_t response_magic = NIOS_PKT_LEGACY_MAGIC;
static unsigned int transaction_count;

int nios_usb_transaction(struct bladerf_usb *usb, void *buffer,
                         uint32_t buffer_len, uint32_t response_timeout_ms,
                         nios_transaction_out_complete_fn out_complete,
                         void *context,
                         enum nios_transaction_stage *stage)
{
    (void)usb;
    (void)response_timeout_ms;
    (void)out_complete;
    (void)context;
    if (buffer_len != NIOS_PKT_LEN) {
        return BLADERF_ERR_UNEXPECTED;
    }
    ((uint8_t *)buffer)[0] = response_magic;
    transaction_count++;
    *stage = NIOS_TRANSACTION_COMPLETE;
    return 0;
}

void log_write(bladerf_log_level level, const char *format, ...)
{
    (void)level;
    (void)format;
}

const char *bladerf_strerror(int status)
{
    (void)status;
    return "test";
}

int main(void)
{
    uint8_t backend = 0;
    struct bladerf dev = { .backend_data = &backend };
    int status;

    response_magic = NIOS_PKT_LEGACY_ERROR_MAGIC;
    transaction_count = 0;
    status = nios_legacy_config_write(&dev, 0);
    if (status != BLADERF_ERR_FPGA_OP || transaction_count != 1) {
        fprintf(stderr, "legacy error marker mapping failed: status=%d calls=%u\n",
                status, transaction_count);
        return 1;
    }

    response_magic = NIOS_PKT_LEGACY_MAGIC;
    transaction_count = 0;
    status = nios_legacy_config_write(&dev, 0);
    if (status != 0 || transaction_count != sizeof(uint32_t)) {
        fprintf(stderr, "legacy success mapping failed: status=%d calls=%u\n",
                status, transaction_count);
        return 1;
    }

    puts("NIOS legacy peripheral error marker mapping: PASS");
    return 0;
}
