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

#ifndef DEVICES_INLINE_H_
#define DEVICES_INLINE_H_

#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>

#include "system.h"
#include "altera_avalon_spi.h"
#include "altera_avalon_jtag_uart_regs.h"
#include "altera_avalon_pio_regs.h"


static inline uint32_t control_reg_read(void)
{
    return IORD_ALTERA_AVALON_PIO_DATA(CONTROL_BASE);
}

static inline void control_reg_write(uint32_t value)
{
    const size_t CFG_GPIO_CLOCK_SELECT = 18; // Refer to bladerf2_common.h
    const uint32_t CLK_SEL_MASK = (1 << CFG_GPIO_CLOCK_SELECT);
    uint32_t current_clock_select, requested_clock_select;
    bool delay_nios_response = false;

    current_clock_select = control_reg_read() & CLK_SEL_MASK;
    requested_clock_select = value & CLK_SEL_MASK;
    delay_nios_response = current_clock_select != requested_clock_select;

    IOWR_ALTERA_AVALON_PIO_DATA(CONTROL_BASE, value);

    // Adding a delay to allow the Nios and FX3 Plls
    // to stabilize before we send back the Nios response.
    if (delay_nios_response) {
        DBG("__resp_delay__");
        usleep(5);
    }
}

static inline uint32_t rffe_csr_read(void)
{
    #ifdef GPIO_RFFE_0_BASE
    return IORD_ALTERA_AVALON_PIO_DATA(GPIO_RFFE_0_BASE);
    #else
    return 0;
    #endif
}

static inline void rffe_csr_write(uint32_t value)
{
    #ifdef GPIO_RFFE_0_BASE
    IOWR_ALTERA_AVALON_PIO_DATA(GPIO_RFFE_0_BASE, value);
    #endif
}

/* RF link status: which USB speed the GPIF buffer geometry was latched at,
 * and whether the host has since changed it behind our back.
 *
 * FX3 samples the USB speed once in NuandRFLinkStart and never rebuilds
 * pcktSize/burstLen/dmaCfg.size, so the FPGA latches it on a link epoch
 * rather than following the live signal. A mismatch means the two sides
 * disagree on DMA packet geometry, which corrupts the stream silently --
 * the control plane keeps answering. See OC bladerf/hdl-defect-hunt.
 *
 * Bit layout is composed in bladerf-hosted.vhd; bit 3 is the sticky
 * mismatch the host must check before trusting a stream.
 *
 * The PIO exists as of the rf_link_status instance in nios_system.tcl, so
 * RF_LINK_STATUS_BASE comes from the generated system header and this reads
 * the real word. The #ifdef stays for platform revisions that do not
 * instantiate it; there 0 reads as "no link, no mismatch" rather than a
 * false alarm. */
static inline uint32_t rf_link_status_read(void)
{
    #ifdef RF_LINK_STATUS_BASE
    return IORD_ALTERA_AVALON_PIO_DATA(RF_LINK_STATUS_BASE);
    #else
    return 0;
    #endif
}

/* Dwell status, sweep revision only.
 *
 * The hosted image does not instantiate these PIOs, so every accessor here
 * degrades to "nothing to report" rather than failing to build. That is the
 * same #ifdef discipline as rf_link_status above, and for the same reason:
 * one software tree serves both revisions.
 *
 * Bit layout is composed in bladerf_core.vhd:
 *
 *   0        frozen         the pre-trigger ring holds a trigger's history
 *   1        wrapped        it filled once, so all 4096 entries are history
 *   2        triggered      the dwell that ended crossed the threshold
 *   3        measure_valid  the receiver had settled
 *   4        gain_too_high  the previous dwell clipped past 100 ppm
 *   16..27   oldest_index   where to start reading, valid only when wrapped
 *   28..31   format version, currently 1
 */
#define DWELL_STATUS_FROZEN         (1u << 0)
#define DWELL_STATUS_WRAPPED        (1u << 1)
#define DWELL_STATUS_TRIGGERED      (1u << 2)
#define DWELL_STATUS_MEASURE_VALID  (1u << 3)
#define DWELL_STATUS_GAIN_TOO_HIGH  (1u << 4)
#define DWELL_STATUS_OLDEST_SHIFT   16
#define DWELL_STATUS_OLDEST_MASK    0xfffu

#define PRETRIG_DEPTH               4096u

static inline uint32_t dwell_status_read(void)
{
    #ifdef DWELL_STATUS_BASE
    return IORD_ALTERA_AVALON_PIO_DATA(DWELL_STATUS_BASE);
    #else
    return 0;
    #endif
}

/* One entry of the pre-trigger ring: write the address, read the datum.
 *
 * The buffer registers its read port, so the datum is valid the cycle after
 * the address. An Avalon write followed by an Avalon read takes far longer
 * than that, so no explicit wait is needed -- but the two accesses must not
 * be reordered, hence the separate statements rather than one expression.
 *
 * Reading while the ring is not frozen returns whatever the write side is
 * currently overwriting. The caller checks DWELL_STATUS_FROZEN first; this
 * function deliberately does not, so a caller that wants a live peek can
 * have one. */
static inline uint32_t pretrig_read(uint16_t index)
{
    #if defined(PRETRIG_ADDR_BASE) && defined(PRETRIG_DATA_BASE)
    IOWR_ALTERA_AVALON_PIO_DATA(PRETRIG_ADDR_BASE,
                                index & (PRETRIG_DEPTH - 1u));
    return IORD_ALTERA_AVALON_PIO_DATA(PRETRIG_DATA_BASE);
    #else
    (void) index;
    return 0;
    #endif
}

/* Ring index n counting from the oldest sample, wrapping.
 *
 * Before the ring has wrapped there is no history older than entry zero, so
 * the oldest index is zero and this is the identity. After wrapping the
 * write pointer sits on the oldest entry -- the slot about to be
 * overwritten -- and the sequence runs from there.
 *
 * Getting this wrong yields a capture that is correct but rotated, which
 * looks like a signal that starts in the middle and is easy to mistake for
 * a real one. */
static inline uint16_t pretrig_index_from_oldest(uint32_t status, uint16_t n)
{
    uint16_t oldest = 0;

    if (status & DWELL_STATUS_WRAPPED) {
        oldest = (uint16_t) ((status >> DWELL_STATUS_OLDEST_SHIFT)
                             & DWELL_STATUS_OLDEST_MASK);
    }

    return (uint16_t) ((oldest + n) & (PRETRIG_DEPTH - 1u));
}

/* RF link config: the write half. The host declares a link generation here
 * and the fabric obeys it; rf_link_status_read() above reports what the
 * fabric actually did with it.
 *
 * Bits 1..3 are toggles, so the value written depends on what was written
 * last. A read-modify-write cannot recover that -- an output PIO reads back
 * its own register, but nothing guarantees this is the only writer and the
 * toggle state is not derivable from any observable. So the shadow below is
 * the authority, and every helper mutates it and rewrites the whole word.
 *
 * Do not "write 1 to start". A toggle is a transition: writing 1 twice is
 * one command, not two. */
static uint32_t rf_link_cfg_shadow = 0;

#define RF_LINK_CFG_USB_SPEED     (1u << 0)
#define RF_LINK_CFG_START_TOGGLE  (1u << 1)
#define RF_LINK_CFG_STOP_TOGGLE   (1u << 2)
#define RF_LINK_CFG_CLEAR_FAULT   (1u << 3)
#define RF_LINK_CFG_EPOCH_TAG_LSB 8

static inline void rf_link_cfg_commit(void)
{
    #ifdef RF_LINK_CFG_BASE
    IOWR_ALTERA_AVALON_PIO_DATA(RF_LINK_CFG_BASE, rf_link_cfg_shadow);
    #endif
}

/* Speed is a level, not a toggle: it is latched by the fabric at the next
 * start, so setting it twice is harmless and setting it after the start is
 * too late. */
static inline void rf_link_cfg_set_speed(bool high_speed)
{
    if (high_speed) {
        rf_link_cfg_shadow |= RF_LINK_CFG_USB_SPEED;
    } else {
        rf_link_cfg_shadow &= ~RF_LINK_CFG_USB_SPEED;
    }
    rf_link_cfg_commit();
}

static inline void rf_link_cfg_set_epoch_tag(uint8_t tag)
{
    rf_link_cfg_shadow &= ~(0xFFu << RF_LINK_CFG_EPOCH_TAG_LSB);
    rf_link_cfg_shadow |= ((uint32_t)tag) << RF_LINK_CFG_EPOCH_TAG_LSB;
    rf_link_cfg_commit();
}

/* Call only after the FX3 RF link start has actually succeeded. This asserts
 * that it did; it is not evidence about FX3 on its own. */
static inline void rf_link_cfg_start(void)
{
    rf_link_cfg_shadow ^= RF_LINK_CFG_START_TOGGLE;
    rf_link_cfg_commit();
}

static inline void rf_link_cfg_stop(void)
{
    rf_link_cfg_shadow ^= RF_LINK_CFG_STOP_TOGGLE;
    rf_link_cfg_commit();
}

/* Clears the sticky fault bits. Does not restart anything -- only a new
 * epoch does that. */
static inline void rf_link_cfg_clear_faults(void)
{
    rf_link_cfg_shadow ^= RF_LINK_CFG_CLEAR_FAULT;
    rf_link_cfg_commit();
}

static inline uint32_t expansion_port_read(void)
{
    return IORD_ALTERA_AVALON_PIO_DATA(XB_GPIO_BASE);
}

INLINE void expansion_port_write(uint32_t value)
{
    IOWR_ALTERA_AVALON_PIO_DATA(XB_GPIO_BASE, value);
}

INLINE uint32_t expansion_port_get_direction()
{
    return IORD_ALTERA_AVALON_PIO_DATA(XB_GPIO_DIR_BASE);
}

INLINE void expansion_port_set_direction(uint32_t dir)
{
    IOWR_ALTERA_AVALON_PIO_DATA(XB_GPIO_DIR_BASE, dir);
}

INLINE void time_tamer_reset(bladerf_module m)
{
    /* A single write is sufficient to clear the timestamp counter */
    if (m == BLADERF_MODULE_RX) {
        IOWR_8DIRECT(RX_TAMER_BASE, 0, 0);
    } else {
        IOWR_8DIRECT(TX_TAMER_BASE, 0, 0);
    }
}

INLINE void timer_tamer_clear_interrupt(bladerf_module m)
{
    if (m == BLADERF_MODULE_RX) {
        IOWR_8DIRECT(RX_TAMER_BASE, 8, 1) ;
    } else {
        IOWR_8DIRECT(TX_TAMER_BASE, 8, 1) ;
    }
}

INLINE void command_uart_read_request(uint8_t *req) {
    int i, x ;
    uint32_t val ;
    for( x = 0 ; x < 16 ; x+=4 ) {
        val = IORD_32DIRECT(COMMAND_UART_BASE, x) ;
        for( i = 0 ; i < 4 ; i++ ) {
            req[x+i] = val&0xff ;
            val >>= 8 ;
        }
    }
    return ;
}

INLINE void command_uart_write_response(uint8_t *resp) {
    int i ;
    uint32_t val ;
    for( i = 0 ; i < 16 ; i+=4 ) {
        val = ((uint32_t)resp[i+0]) | (((uint32_t)resp[i+1])<<8) | (((uint32_t)resp[i+2])<<16) | (((uint32_t)resp[i+3])<<24) ;
        IOWR_32DIRECT(COMMAND_UART_BASE, i, val) ;
    }
    return ;
}

#endif
