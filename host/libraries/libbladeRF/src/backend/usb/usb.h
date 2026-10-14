/*
 * This file is part of the bladeRF project:
 *   http://www.github.com/nuand/bladeRF
 *
 * Copyright (C) 2014-2015 Nuand LLC
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef BACKEND_USB_H_
#define BACKEND_USB_H_

#include "host_config.h"

#include "board/board.h"
#include "thread.h"

#if ENABLE_USB_DEV_RESET_ON_OPEN
extern bool bladerf_usb_reset_device_on_open;
#endif

#ifndef SAMPLE_EP_IN
#define SAMPLE_EP_IN 0x81
#endif

#ifndef SAMPLE_EP_OUT
#define SAMPLE_EP_OUT 0x01
#endif

#ifndef PERIPHERAL_EP_IN
#define PERIPHERAL_EP_IN 0x82
#endif

#ifndef PERIPHERAL_EP_OUT
#define PERIPHERAL_EP_OUT 0x02
#endif

#ifndef PERIPHERAL_TIMEOUT_MS
#define PERIPHERAL_TIMEOUT_MS 250
#endif

/* Be careful when lowering this value. The control request for flash erase
 * operations take some time */
#ifndef CTRL_TIMEOUT_MS
#define CTRL_TIMEOUT_MS 1000
#endif

/* 64KB sector erase worst-case: 2250ms (Renesas AT25FF321A) */
#ifndef FLASH_ERASE_TIMEOUT_MS
#define FLASH_ERASE_TIMEOUT_MS 3000
#endif

#ifndef BULK_TIMEOUT_MS
#define BULK_TIMEOUT_MS 1000
#endif

/* Size of a host<->FPGA message in BYTES */
#define USB_MSG_SIZE_SS 8192
#define USB_MSG_SIZE_HS 4096
#define USB_MSG_SIZE_SS_LEGACY 2048
#define USB_MSG_SIZE_HS_LEGACY 1024


static const struct bladerf_version FW_LARGER_BUFFER_VERSION = {
    .major = 2,
    .minor = 5,
    .patch = 0,
    .describe = NULL
};

static const struct bladerf_version FPGA_LARGER_BUFFER_VERSION = {
    .major = 0,
    .minor = 16,
    .patch = 0,
    .describe = NULL
};

typedef enum {
    USB_TARGET_DEVICE,
    USB_TARGET_INTERFACE,
    USB_TARGET_ENDPOINT,
    USB_TARGET_OTHER
} usb_target;

typedef enum {
    USB_REQUEST_STANDARD,
    USB_REQUEST_CLASS,
    USB_REQUEST_VENDOR
} usb_request;

typedef enum {
    USB_DIR_HOST_TO_DEVICE = 0x00,
    USB_DIR_DEVICE_TO_HOST = 0x80
} usb_direction;

/**
 * USB backend driver function table
 *
 * All return values are expected to be 0 on success, or a BLADERF_ERR_*
 * value on failure
 */
struct usb_fns {
    int (*probe)(backend_probe_target probe_target,
                 struct bladerf_devinfo_list *info_list);

    /* Populates the `driver` pointer with a handle for the specific USB driver.
     * `info_in` describes the device to open, and may contain wildcards.
     * On success, the driver should fill in `info_out` with the complete
     * details of the device. */
    int (*open)(void **driver,
                struct bladerf_devinfo *info_in,
                struct bladerf_devinfo *info_out);

    void (*close)(void *driver);

    int (*get_vid_pid)(void *driver, uint16_t *vid, uint16_t *pid);

    int (*get_flash_id)(void *driver, uint8_t *mid, uint8_t *did);

    int (*get_handle)(void *driver, void **handle);

    int (*get_speed)(void *driver, bladerf_dev_speed *speed);

    int (*change_setting)(void *driver, uint8_t setting);

    int (*control_transfer)(void *driver,
                            usb_target target_type,
                            usb_request req_type,
                            usb_direction direction,
                            uint8_t request,
                            uint16_t wvalue,
                            uint16_t windex,
                            void *buffer,
                            uint32_t buffer_len,
                            uint32_t timeout_ms);

    /* `transferred_valid` is false when a backend cannot prove the byte count
     * (notably some Windows error paths). When true, `transferred` is valid
     * even if the transfer status is an error such as timeout. */
    int (*bulk_transfer)(void *driver,
                         uint8_t endpoint,
                         void *buffer,
                         uint32_t buffer_len,
                         uint32_t timeout_ms,
                         uint32_t *transferred,
                         bool *transferred_valid);

    int (*get_string_descriptor)(void *driver,
                                 uint8_t index,
                                 void *buffer,
                                 uint32_t buffer_len);

    int (*init_stream)(void *driver,
                       struct bladerf_stream *stream,
                       size_t num_transfers);

    int (*stream)(void *driver,
                  struct bladerf_stream *stream,
                  bladerf_channel_layout layout);

    /* Called with stream->lock held to interrupt a blocking stream loop. */
    int (*shutdown_stream)(void *driver, struct bladerf_stream *stream);

    int (*submit_stream_buffer)(void *driver,
                                struct bladerf_stream *stream,
                                void *buffer,
                                size_t *length,
                                unsigned int timeout_ms,
                                bool nonblock,
                                uint64_t tx_generation);

    int (*deinit_stream)(void *driver, struct bladerf_stream *stream);

    int (*open_bootloader)(void **driver, uint8_t bus, uint8_t addr);
    void (*close_bootloader)(void *driver);
};

struct usb_driver {
    const struct usb_fns *fn;
    bladerf_backend id;
};

/* NIOS protocol state belongs to the USB session. The POSIX registry below
 * enforces one active handle per session and preserves ambiguous state across
 * close/reopen; Windows already enforces exclusive device opens. */
struct usb_peripheral_state {
    MUTEX lock;
    bool desynchronized;
    /* A fixed-width read request was fully sent, but its IN response timed
     * out before any bytes arrived. Keep its identity until a distinct
     * ordered barrier response proves the endpoint has drained it. */
    bool pending_read_response;
    uint8_t pending_read_request[16];
    bool pending_rfic_write;
    uint8_t pending_rfic_request[16];
    /* A scheduled RETUNE2 can be accepted even when its USB IN ACK is lost.
     * Keep that request identity so later read-only telemetry can drain the
     * late ACK without permitting a second state-changing request. */
    bool pending_retune2_response;
    uint8_t pending_retune2_request[16];
    /* Retune replies are matched against the generation observed before OUT.
     * Keep pending and currently observed generations separate so a late
     * reply can be drained while a newer session read is in flight. */
    uint32_t pending_retune2_session_generation;
    bool command_session_generation_valid;
    uint32_t command_session_generation;
#if defined(USE_PTHREADS)
    char serial[BLADERF_SERIAL_LENGTH];
    uint8_t usb_bus;
    uint8_t usb_addr;
    unsigned int references;
    struct usb_peripheral_state *next;
#endif
};

struct bladerf_usb {
    const struct usb_fns *fn;
    void *driver;
    struct usb_peripheral_state *peripheral;
    /* Windows open already has a named exclusive device mutex. Use embedded
     * storage there; pthread platforms attach a registry-owned session state. */
    struct usb_peripheral_state peripheral_local;
};

/* Issue USB CLEAR_FEATURE(ENDPOINT_HALT). On bladeRF2 FX3 firmware this also
 * resets and restarts the DMA channel bound to the endpoint. Caller must first
 * ensure no stream transfers remain in flight. */
int usb_clear_endpoint_halt(struct bladerf *dev, uint8_t endpoint);
int usb_pause_tx_dma(struct bladerf *dev);
int usb_query_tx_dma_pause(struct bladerf *dev, uint32_t *state);

#endif
