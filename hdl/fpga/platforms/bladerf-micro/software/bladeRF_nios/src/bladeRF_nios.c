/* This file is part of the bladeRF project:
 *   http://www.github.com/nuand/bladeRF
 *
 * Copyright (c) 2015 Nuand LLC
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/* Define this to run this code on a PC instead of the NIOS II */
//#define BLADERF_NULL_HARDWARE

/* Will send debug alt_printf info to JTAG console while running on NIOS.
 * This can slow performance and cause timing issues... be careful. */
//#define BLADERF_NIOS_DEBUG

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include "devices.h"
#include "pkt_handler.h"
#include "pkt_8x8.h"
#include "pkt_8x16.h"
#include "pkt_8x32.h"
#include "pkt_8x64.h"
#include "pkt_16x64.h"
#include "pkt_32x32.h"
#include "pkt_retune2.h"
#include "pkt_legacy.h"
#include "rfic_scheduler_policy.h"
#include "debug.h"
#ifdef BLADERF_NIOS_LIBAD936X
void bladerf_platform_set_service_hook(void (*hook)(void));
#endif

#define BLADERF_DEVICE_NAME "Nuand bladeRF 2.0 Micro"

#ifdef BLADERF_NIOS_PC_SIMULATION
    extern bool run_nios;
#   define HAVE_REQUEST() ({ \
        have_request = !have_request; \
        if (have_request) { \
            command_uart_read_request( (uint8_t*) pkt.req); \
            pkt.session_generation = command_uart_request_session_generation(); \
        } \
        have_request; \
    })

    /* We need to reset the response buffer to known values so we can
     * compare against expected test case responses */
#   define RESET_RESPONSE_BUF

#else
#   define run_nios true
#   define HAVE_REQUEST() (pkt.ready == true)
#endif

#ifdef RESET_RESPONSE_BUF
#   undef RESET_RESPONSE_BUF
#   define RESET_RESPONSE_BUF() do { \
        memset(pkt.resp, 0xff, NIOS_PKT_LEN); \
    } while (0)

#else
#   define RESET_RESPONSE_BUF() do {} while (0)
#endif

/* When adding packet handlers here, you must also ensure that you update
 * the bladeRF/hdl/fpga/ip/nuand/command_uart/vhdl/command_uart.vhd
 * to include the magic header byte value in the `magics` array.
 */
static const struct pkt_handler pkt_handlers[] = {
    PKT_RETUNE2,
    PKT_8x8,
    PKT_8x16,
    PKT_8x32,
    PKT_8x64,
    PKT_16x64,
    PKT_32x32,
    PKT_LEGACY,
};

#ifdef BLADERF_NIOS_LIBAD936X
static struct pkt_buf *cooperative_pkt;
#endif

static void run_deferred_packet_work(void)
{
    size_t i;

#ifdef BLADERF_NIOS_LIBAD936X
    /* Close the gap between HAVE_REQUEST and worker entry. If the ISR
     * already accepted a packet in that gap, fence it here. */
    command_uart_set_worker_busy(true);
    if (cooperative_pkt != NULL && cooperative_pkt->ready) {
        cooperative_pkt->deferred_work_active_at_accept = true;
    }
#endif

#ifdef BLADERF_NIOS_LIBAD936X
    /* A scheduled TXQ continuation belongs to RETUNE2 rather than the RFIC
     * command queue. Route exactly that owner's poll and keep every other
     * RFIC-capable worker out until the transaction terminates. */
    if (adi_fastlock_load_active() || adi_fastlock_recall_active() ||
        rfic_tx_quad_retune_owner()) {
        for (i = 0; i < ARRAY_SIZE(pkt_handlers); i++) {
            if (pkt_handlers[i].magic == NIOS_PKT_RETUNE2_MAGIC) {
                if (pkt_handlers[i].do_work != NULL) {
                    pkt_handlers[i].do_work();
                }
                break;
            }
        }
        return;
    }

    /* The RFIC queue has exclusive ownership of AD9361 across yielded
     * calibration polls. Service it before any other deferred handler, then
     * keep unrelated work out until the queue has retired. In particular,
     * RETUNE2 can read/program RFIC state and must not run between polls. */
    for (i = 0; i < ARRAY_SIZE(pkt_handlers); i++) {
        if (pkt_handlers[i].magic == NIOS_PKT_16x64_MAGIC) {
            if (pkt_handlers[i].do_work != NULL) {
                pkt_handlers[i].do_work();
            }
            break;
        }
    }

    if (rfic_command_owns_device()) {
        if (rfic_tx_quad_retune_owner()) {
            for (i = 0; i < ARRAY_SIZE(pkt_handlers); i++) {
                if (pkt_handlers[i].magic == NIOS_PKT_RETUNE2_MAGIC) {
                    if (pkt_handlers[i].do_work != NULL) {
                        pkt_handlers[i].do_work();
                    }
                    break;
                }
            }
        }
        return;
    }

    for (i = 0; i < ARRAY_SIZE(pkt_handlers); i++) {
        if (pkt_handlers[i].magic != NIOS_PKT_16x64_MAGIC &&
            pkt_handlers[i].do_work != NULL) {
            pkt_handlers[i].do_work();
        }
    }
#else
    for (i = 0; i < ARRAY_SIZE(pkt_handlers); i++) {
        if (pkt_handlers[i].do_work != NULL) {
            pkt_handlers[i].do_work();
        }
    }
#endif

#ifdef BLADERF_NIOS_LIBAD936X
    /* RFIC work can now yield between resumable calibration polls. Keep the
     * UART's logical ownership fence asserted across those main-loop turns,
     * not merely while this C function happens to be on the stack. */
    command_uart_set_worker_busy(rfic_command_worker_active() ||
                                 rfic_command_owns_device());
#endif
}

#ifdef BLADERF_NIOS_LIBAD936X
/* A synchronous RFIC callback owns the NIOS execution context. Reject every
 * queued non-whitelisted packet using its normal response encoding, without
 * invoking the packet handler (which could touch RFIC, RFFE, clocks, or queue
 * state). This makes rejection terminal instead of allowing the request to
 * execute after the caller has timed out. */
static bool reject_packet_during_rfic_operation(struct pkt_buf *b)
{
    uint8_t target;
    uint8_t addr8;
    uint8_t data8;
    uint16_t data16;
    uint32_t addr32;
    uint32_t data32;
    uint64_t data64;
    bool write;
    bool response_packed = true;

    switch (b->req[PKT_MAGIC_IDX]) {
        case NIOS_PKT_8x8_MAGIC:
            nios_pkt_8x8_unpack(b->req, &target, &write, &addr8, &data8);
            nios_pkt_8x8_resp_pack(b->resp, target, write, addr8, data8,
                                   false);
            break;
        case NIOS_PKT_8x16_MAGIC:
            nios_pkt_8x16_unpack(b->req, &target, &write, &addr8, &data16);
            nios_pkt_8x16_resp_pack(b->resp, target, write, addr8, data16,
                                    false);
            break;
        case NIOS_PKT_8x32_MAGIC:
            nios_pkt_8x32_unpack(b->req, &target, &write, &addr8, &data32);
            nios_pkt_8x32_resp_pack(b->resp, target, write, addr8, data32,
                                    false);
            break;
        case NIOS_PKT_8x64_MAGIC:
            nios_pkt_8x64_unpack(b->req, &target, &write, &addr8, &data64);
            nios_pkt_8x64_resp_pack(b->resp, target, write, addr8, data64,
                                    false);
            break;
        case NIOS_PKT_32x32_MAGIC:
            nios_pkt_32x32_unpack(b->req, &target, &write, &addr32, &data32);
            nios_pkt_32x32_resp_pack(b->resp, target, write, addr32, data32,
                                     false);
            break;
        case NIOS_PKT_RETUNE_MAGIC:
            nios_pkt_retune_resp_pack(
                b->resp, 0, 0, NIOS_PKT_RETUNERESP_FLAG_BUSY);
            break;
        case NIOS_PKT_RETUNE2_MAGIC:
            nios_pkt_retune2_resp_pack(
                b->resp, 0, NIOS_PKT_RETUNE2_RESP_FLAG_BUSY,
                NIOS_PKT_RETUNE2_FAILURE_NONE,
                nios_pkt_retune2_unpack_request_token(b->req),
                b->session_generation);
            break;
        case NIOS_PKT_LEGACY_MAGIC:
            pkt_legacy_abort();
            memset(b->resp, 0, sizeof(b->resp));
            b->resp[PKT_MAGIC_IDX] = NIOS_PKT_LEGACY_ERROR_MAGIC;
            b->resp[PKT_CFG_IDX] = b->req[PKT_CFG_IDX];
            break;
        default:
            /* The command UART only presents registered packet magics. Keep
             * an unknown frame pending for the normal parser/error path. */
            response_packed = false;
            break;
    }
    return response_packed;
}

/* AD9361 wait loops call this after platform delays and completed NIOS SPI
 * transfers. Besides read-only RFIC STATUS and exact duplicate tagged writes,
 * service scheduled RETUNE2 admission and deferred queue-clear intent. All
 * scheduled requests only snapshot/append queue state; their profile SPI
 * writes belong to the cooperative RETUNE2 worker. Reject every other
 * recognized packet normally so it cannot execute after timeout. */
static void service_rfic_status_request(void)
{
    bladerf_module retune_module;
    uint64_t retune_timestamp;
    uint16_t retune_nios_profile;
    uint16_t retune_token;
    uint8_t retune_rffe_profile;
    uint8_t retune_port;
    uint8_t retune_spdt;
    uint8_t target;
    uint16_t addr;
    uint16_t transaction_id;
    uint64_t data;
    bool is_write;

    rfic_command_progress_tick();

    if (cooperative_pkt == NULL || !cooperative_pkt->ready) {
        return;
    }

    if (cooperative_pkt->session_generation !=
        command_uart_current_session_generation()) {
        /* USB reconnect invalidated this request's response session. Retire
         * it without allowing a stale command to run in the new session. */
        cooperative_pkt->ready = false;
        cooperative_pkt->deferred_work_active_at_accept = false;
        command_uart_response_retire(&cooperative_pkt->response_prepared);
        command_uart_enable_isr(true);
        return;
    }

    /* A late-dispatch request also comes through this path. Its captured
     * arrival-time fence, not the worker's now-idle state, makes rejection
     * mandatory. Clear it once the response is consumed. */
    cooperative_pkt->deferred_work_active_at_accept = false;

    /* Retry only the immutable response when backpressured. The request's
     * queue or RFIC side effects must never run twice. */
    if (!command_uart_response_should_prepare(
            cooperative_pkt->response_prepared)) {
        goto publish_cooperative_response;
    }

    if (cooperative_pkt->req[PKT_MAGIC_IDX] == NIOS_PKT_RETUNE2_MAGIC) {
        nios_pkt_retune2_unpack_token(
            cooperative_pkt->req, &retune_module, &retune_timestamp,
            &retune_nios_profile, &retune_rffe_profile, &retune_port,
            &retune_spdt, &retune_token);
        if (retune_timestamp == NIOS_PKT_RETUNE2_CLEAR_QUEUE &&
            pkt_retune2_defer_clear(retune_module)) {
            nios_pkt_retune2_resp_pack(
                cooperative_pkt->resp, 0,
                NIOS_PKT_RETUNE2_RESP_FLAG_CANCEL_PENDING,
                NIOS_PKT_RETUNE2_FAILURE_NONE, retune_token,
                cooperative_pkt->session_generation);
        } else if (retune_timestamp != NIOS_PKT_RETUNE2_NOW &&
                   retune_timestamp != NIOS_PKT_RETUNE2_CLEAR_QUEUE) {
            /* Scheduled RETUNE2 is queue-only for both protocol generations;
             * the worker performs profile upload and timer arming later. */
            pkt_retune2(cooperative_pkt);
        } else if (!reject_packet_during_rfic_operation(cooperative_pkt)) {
            return;
        }
        goto publish_cooperative_response;
    }

    if (cooperative_pkt->req[PKT_MAGIC_IDX] != NIOS_PKT_16x64_MAGIC) {
        if (!reject_packet_during_rfic_operation(cooperative_pkt)) {
            return;
        }
        goto publish_cooperative_response;
    }

    nios_pkt_16x64_unpack(cooperative_pkt->req, &target, &is_write, &addr,
                          &data);
    transaction_id =
        nios_pkt_16x64_transaction_id(cooperative_pkt->req);
    if (target != NIOS_PKT_16x64_TARGET_RFIC) {
        /* Direct AD9361 SPI, gain-table and batch operations bypass the RFIC
         * command queue. Never let one run reentrantly while an ADI callback
         * owns the device. */
        nios_pkt_16x64_resp_pack(
            cooperative_pkt->resp, target, is_write, addr, data, false);
    } else if (is_write && transaction_id != 0 &&
        rfic_command_transaction_is_duplicate(addr, data, transaction_id)) {
        nios_pkt_16x64_resp_pack_with_transaction_id(
            cooperative_pkt->resp, target, true, addr, data, true,
            transaction_id);
    } else if (is_write) {
        /* Do not leave a new RFIC mutation parked in the one-entry command
         * UART slot until this synchronous callback returns: the host could
         * time out, then the supposedly unacknowledged request would execute
         * later. Reject it now without touching SPI or the RFIC queue. */
        if (transaction_id != 0) {
            nios_pkt_16x64_resp_pack_busy(
                cooperative_pkt->resp, target, addr, data, transaction_id);
        } else {
            nios_pkt_16x64_resp_pack_with_transaction_id(
                cooperative_pkt->resp, target, true, addr, data, false, 0);
        }
    } else if (!is_write && transaction_id == 0 &&
               (addr == 0x0f00u || addr == 0x0f0cu ||
                addr == 0x0f0du)) {
        pkt_16x64(cooperative_pkt);
    } else {
        nios_pkt_16x64_resp_pack_with_transaction_id(
            cooperative_pkt->resp, target, false, addr, data, false, 0);
    }

publish_cooperative_response:
    command_uart_response_mark_prepared(&cooperative_pkt->response_prepared);
    switch (command_uart_try_publish_response(
        cooperative_pkt->resp, cooperative_pkt->session_generation)) {
    case COMMAND_UART_TRY_PUBLISH_OK:
    case COMMAND_UART_TRY_PUBLISH_STALE:
        cooperative_pkt->ready = false;
        command_uart_response_retire(&cooperative_pkt->response_prepared);
        command_uart_enable_isr(true);
        break;
    case COMMAND_UART_TRY_PUBLISH_BACKPRESSURE:
        /* Keep the request slot fenced. The main loop and later safe points
         * retry publication without re-running the packet handler. */
        break;
    case COMMAND_UART_TRY_PUBLISH_ERROR:
    default:
        cooperative_pkt->ready = false;
        command_uart_response_retire(&cooperative_pkt->response_prepared);
        DBG("Command UART response failed; request admission fenced until session reset\n");
        break;
    }
}
#endif

/* A structure that represents a point on a line. Used for calibrating
 * the VCTCXO */
typedef struct point {
    int32_t  x; // Error counts
    uint16_t y; // DAC count
} point_t;

typedef struct line {
    point_t  point[2];
    int32_t  slope;
    uint16_t y_intercept; // in DAC counts
} line_t;

/* State machine for VCTCXO tuning */
typedef enum state {
    COARSE_TUNE_MIN,
    COARSE_TUNE_MAX,
    COARSE_TUNE_DONE,
    FINE_TUNE,
    DO_NOTHING
} state_t;

int main(void)
{
    DBG(BLADERF_DEVICE_NAME " FPGA v%x.%x.%x\n",
        FPGA_VERSION_MAJOR, FPGA_VERSION_MINOR, FPGA_VERSION_PATCH);
    DBG("Built " __DATE__ " " __TIME__ " with love <3\n");
#ifdef BLADERF_NIOS_LIBAD936X
    DBG("libad936x found: This FPGA image has magic transgirl powers\n");
#endif  // BLADERF_NIOS_LIBAD936X

    uint8_t i;

    /* Pointer to currently active packet handler */
    const struct pkt_handler *handler;

    struct pkt_buf pkt;
    struct vctcxo_tamer_pkt_buf vctcxo_tamer_pkt;

    /* Marked volatile to ensure we actually read the byte populated by
     * the UART ISR */
    const volatile uint8_t *magic = &pkt.req[PKT_MAGIC_IDX];

    volatile bool have_request = false;

#ifdef BLADERF_NIOS_DEBUG
    // Twiddler: gratuitous screen placebo
    size_t const TWIDDLE_DELAY_CONSTANT = 100000;
    size_t twiddle_count = 0;
    enum {
        TWIDDLE_STATE_PIPE = '|',
        TWIDDLE_STATE_FWD_SLASH = '/',
        TWIDDLE_STATE_HYPHEN = '-',
        TWIDDLE_STATE_BACKSLASH = '\\',
    } twiddle_state = TWIDDLE_STATE_PIPE;
#endif

    // Trim DAC constants
    const uint16_t trimdac_min       = 0x28F5;
    const uint16_t trimdac_max       = 0xF5C3;

    // Trim DAC calibration line
    line_t trimdac_cal_line;

    // VCTCXO Tune State machine
    state_t tune_state = COARSE_TUNE_MIN;

    // Set the known/default values of the trim DAC cal line
    trimdac_cal_line.point[0].x  = 0;
    trimdac_cal_line.point[0].y  = trimdac_min;
    trimdac_cal_line.point[1].x  = 0;
    trimdac_cal_line.point[1].y  = trimdac_max;
    trimdac_cal_line.slope       = 0;
    trimdac_cal_line.y_intercept = 0;

    /* Sanity check */
    ASSERT(PKT_MAGIC_IDX == 0);

    memset(&pkt, 0, sizeof(pkt));
    pkt.ready = false;
    bladerf_nios_init(&pkt, &vctcxo_tamer_pkt);
#ifdef BLADERF_NIOS_LIBAD936X
    cooperative_pkt = &pkt;
    bladerf_platform_set_service_hook(service_rfic_status_request);
#endif

    /* Initialize packet handlers */
    for (i = 0; i < ARRAY_SIZE(pkt_handlers); i++) {
        if (pkt_handlers[i].init != NULL) {
            pkt_handlers[i].init();
        }
    }

    /* ====================
     * AD9361 SPI TESTS
     * ==================== */
    #ifdef BLADERF_NIOS_AD9361_SPI_TESTS
        uint16_t adi_spi_addr;
        uint64_t adi_spi_data;

        while ( 1 ) {
            //             W/Rb        | NB2:0       | A[9:0]
            adi_spi_addr = (0x0 << 15) | (0x0 << 12) | (0x000 << 0);
            adi_spi_data = UINT64_C(0x0);

            // Read AD9361 registers 0x045-0x04c
            for( i=0x45; i < 0x4d; i++ ) {
                adi_spi_addr = (adi_spi_addr & 0xfc00) | i;
                adi_spi_data = adi_spi_read(adi_spi_addr);
            }

            // Read 0x028
            adi_spi_addr = (adi_spi_addr & 0xfc00) | 0x28;
            adi_spi_data = adi_spi_read(adi_spi_addr);

            // Write 0x5a to 0x028
            adi_spi_addr = (adi_spi_addr & 0xfc00) | 0x28;
            adi_spi_data = (UINT64_C(0x5a) << (64-8));
            adi_spi_write((adi_spi_addr | 0x8000), adi_spi_data);

            // Read 0x028
            adi_spi_addr = (adi_spi_addr & 0xfc00) | 0x28;
            adi_spi_data = adi_spi_read(adi_spi_addr);

            // Write 0x5a to 0x028
            adi_spi_addr = (adi_spi_addr & 0xfc00) | 0x28;
            adi_spi_data = (UINT64_C(0x0) << (64-8));
            adi_spi_write((adi_spi_addr | 0x8000), adi_spi_data);

            usleep(100);
        }
    #endif


    /* ====================
     * AD5621 SPI TESTS
     * ==================== */
    #ifdef BLADERF_NIOS_AD5621_SPI_TESTS
        uint16_t dac_val;

        // Disable the ADF400x
        control_reg_write( 0x0 );
        usleep(2000000);
        while( 1 ) {
            for( i = 0; i < 6; i++ ) {
                // Calculate DAC value
                dac_val = ((((uint16_t)i) * 0x333) << 2) & 0x3fff;
                // Write DAC value
                ad56x1_vctcxo_trim_dac_write(dac_val);
                usleep(2000000);
                // Tristate the DAC, keeping same DAC value in register
                ad56x1_vctcxo_trim_dac_write(dac_val | 0xc000 );
                usleep(2000000);
            }
        }
    #endif


    /* ====================
     * ADF4001 SPI TESTS
     * ==================== */
    #ifdef BLADERF_NIOS_ADF4001_SPI_TESTS
        // Tristate the DAC
        ad56x1_vctcxo_trim_dac_write( 0xc000 );
        while( 1 ) {

            // Enable the ADF400x
            control_reg_write( 0x1 << 11 );
            usleep(2000000);

            // 0x000003 -- MUXOUT = 'Z'; CP = Normal
            adf400x_spi_write(0x3);
            usleep(2000000);

            // 0x000137 -- MUXOUT = AVDD (3.3 V); CP = 'Z'
            adf400x_spi_write(0x137);
            usleep(2000000);

            // 0x000177 -- MUXOUT = DGND; CP = 'Z'
            adf400x_spi_write(0x177);
            usleep(2000000);

            // 0x000167 -- MUXOUT = SDO; CP = 'Z'
            //adf400x_spi_write(0x167);
            //usleep(2000000);

            for( i = 0; i < 4; i++ ) {
                // Read all the registers
                adf400x_spi_read((uint32_t)i);
                usleep(100);
            }

            // Disable the ADF400x
            control_reg_write( 0x0 );
            usleep(2000000);
        }
    #endif

    DBG("=== System Ready ===\n");

    while (run_nios) {
        /* A response enqueue failure poisons the current command transport.
         * Only the FPGA's session reset clears its sticky overflow state. */
        command_uart_check_session_recovery();
        have_request = HAVE_REQUEST();

#ifdef BLADERF_NIOS_DEBUG
        if (have_request) {
            twiddle_count = 0;
        } else if (TWIDDLE_DELAY_CONSTANT == ++twiddle_count) {
            DBG("%c\b", twiddle_state);

            switch (twiddle_state) {
                case TWIDDLE_STATE_PIPE:
                    twiddle_state = TWIDDLE_STATE_FWD_SLASH;
                    break;
                case TWIDDLE_STATE_FWD_SLASH:
                    twiddle_state = TWIDDLE_STATE_HYPHEN;
                    break;
                case TWIDDLE_STATE_HYPHEN:
                    twiddle_state = TWIDDLE_STATE_BACKSLASH;
                    break;
                case TWIDDLE_STATE_BACKSLASH:
                    twiddle_state = TWIDDLE_STATE_PIPE;
                    break;
            }

            twiddle_count = 0;
        }
#endif

        /* We have a command in the UART */
        if (have_request) {
            if (pkt.session_generation !=
                command_uart_current_session_generation()) {
                /* Do not execute a frame retained from a previous FX3
                 * session after reconnect. Its reply would be discarded, but
                 * its side effects would otherwise still happen. */
                pkt.ready = false;
                pkt.deferred_work_active_at_accept = false;
                command_uart_response_retire(&pkt.response_prepared);
                command_uart_enable_isr(true);
                have_request = false;
            }
        }

        if (have_request) {
#ifdef BLADERF_NIOS_LIBAD936X
            if (pkt.deferred_work_active_at_accept) {
                service_rfic_status_request();
                have_request = false;
            }
#endif
        }

        if (have_request) {
            pkt.ready = false;
            handler = NULL;

            /* Determine which packet handler should receive this message */
            for (i = 0; i < ARRAY_SIZE(pkt_handlers); i++) {
                if (pkt_handlers[i].magic == *magic) {
                    handler = &pkt_handlers[i];
                }
            }

            if (handler == NULL) {
                /* We somehow got out of sync. Throw away request data until
                 * we hit a magic value */
                DBG("Got invalid magic value: 0x%x\n", pkt.req[PKT_MAGIC_IDX]);
                run_deferred_packet_work();
                command_uart_enable_isr(true);
                continue;
            }

            print_bytes("Request data:", pkt.req, NIOS_PKT_LEN);

            /* If building with RESET_RESPONSE_BUF defined, reset response buffer
             * contents to ensure unused values are known values. */
            RESET_RESPONSE_BUF();

            /* Process data and execute requested actions */
#ifdef BLADERF_NIOS_LIBAD936X
            /* Keep newly admitted commands fenced across this response and
             * the worker pass immediately following it. */
            command_uart_set_worker_busy(true);
#endif
            handler->exec(&pkt);

            /* Response publication may be backpressured by the FPGA's
             * active+pending serializer slots. Preserve the already-computed
             * response and route retries through the same nonblocking pump
             * used by RFIC safe points. */
#ifdef BLADERF_NIOS_LIBAD936X
            command_uart_response_mark_prepared(&pkt.response_prepared);
            pkt.ready = true;
            pkt.deferred_work_active_at_accept = true;
            service_rfic_status_request();
#else
            if (!command_uart_publish_response(pkt.resp,
                                               pkt.session_generation)) {
                DBG("Command UART response failed; request admission fenced until session reset\n");
            }
            command_uart_enable_isr(true);
#endif
        } else {

            /* Temporarily putting the VCTCXO Calibration stuff here. */
            /* The tamer changes the reference oscillator trim. Defer its
             * buffered sample while an AD9361 callback/calibration owns the
             * RFIC, so CLKRF cannot move during a hardware calibration. The
             * ISR is already disabled and the sample remains in this packet. */
            if (rfic_scheduler_reference_tune_allowed(
                    vctcxo_tamer_pkt.ready,
#ifdef BLADERF_NIOS_LIBAD936X
                    rfic_command_owns_device()
#else
                    false
#endif
                    )) {

                vctcxo_tamer_pkt.ready = false;

                switch(tune_state) {

                case COARSE_TUNE_MIN:

                    /* Tune to the minimum DAC value */
                    vctcxo_trim_dac_write( 0x08, trimdac_min );

                    /* State to enter upon the next interrupt */
                    tune_state = COARSE_TUNE_MAX;

                    break;

                case COARSE_TUNE_MAX:

                    /* We have the error from the minimum DAC setting, store it
                     * as the 'x' coordinate for the first point */
                    trimdac_cal_line.point[0].x = vctcxo_tamer_pkt.pps_1s_error;

                    /* Tune to the maximum DAC value */
                    vctcxo_trim_dac_write( 0x08, trimdac_max );

                    /* State to enter upon the next interrupt */
                    tune_state = COARSE_TUNE_DONE;

                    break;

                case COARSE_TUNE_DONE:

                    /* We have the error from the maximum DAC setting, store it
                     * as the 'x' coordinate for the second point */
                    trimdac_cal_line.point[1].x = vctcxo_tamer_pkt.pps_1s_error;

                    /* We now have two points, so we can calculate the equation
                     * for a line plotted with DAC counts on the Y axis and
                     * error on the X axis. We want a PPM of zero, which ideally
                     * corresponds to the y-intercept of the line. */
                    trimdac_cal_line.slope = ( (trimdac_cal_line.point[1].y - trimdac_cal_line.point[0].y) /
                                               (trimdac_cal_line.point[1].x - trimdac_cal_line.point[0].x) );
                    trimdac_cal_line.y_intercept = ( trimdac_cal_line.point[0].y -
                                                     (trimdac_cal_line.slope * trimdac_cal_line.point[0].x) );

                    /* Set the trim DAC count to the y-intercept */
                    vctcxo_trim_dac_write( 0x08, trimdac_cal_line.y_intercept );

                    /* State to enter upon the next interrupt */
                    tune_state = FINE_TUNE;

                    break;

                case FINE_TUNE:

                    /* We should be extremely close to a perfectly tuned
                     * VCTCXO, but some minor adjustments need to be made */

                    /* Check the magnitude of the errors starting with the
                     * one second count. If an error is greater than the maxium
                     * tolerated error, adjust the trim DAC by the error (Hz)
                     * multiplied by the slope (in counts/Hz) and scale the
                     * result by the precision interval (e.g. 1s, 10s, 100s). */
                    if( vctcxo_tamer_pkt.pps_1s_error_flag ) {
                        vctcxo_trim_dac_write( 0x08, (vctcxo_trim_dac_value -
                            ((vctcxo_tamer_pkt.pps_1s_error * trimdac_cal_line.slope)/1)) );
                    } else if( vctcxo_tamer_pkt.pps_10s_error_flag ) {
                        vctcxo_trim_dac_write( 0x08, (vctcxo_trim_dac_value -
                            ((vctcxo_tamer_pkt.pps_10s_error * trimdac_cal_line.slope)/10)) );
                    } else if( vctcxo_tamer_pkt.pps_100s_error_flag ) {
                        vctcxo_trim_dac_write( 0x08, (vctcxo_trim_dac_value -
                            ((vctcxo_tamer_pkt.pps_100s_error * trimdac_cal_line.slope)/100)) );
                    }

                    break;

                default:
                    break;

                } /* switch */

                /* Take PPS counters out of reset */
                vctcxo_tamer_reset_counters( false );

                /* Enable interrupts */
                vctcxo_tamer_enable_isr( true );

            } /* VCTCXO Tamer interrupt */

        }

        /* Deferred RFIC/retune work runs after every request response and on
         * idle iterations. A RUNNING callback may still perform synchronous
         * ADI work; yielded calibration polls retain RFIC ownership while
         * unrelated RFIC-capable deferred work is suppressed. */
        run_deferred_packet_work();
    }

    return 0;
}
