/*
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

#ifndef BLADERF_NIOS_PKT_8x32_H_
#define BLADERF_NIOS_PKT_8x32_H_

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/*
 * This file defines the Host <-> FPGA (NIOS II) packet formats for accesses
 * to devices/blocks with 8-bit addresses and 32-bit data
 *
 *
 *                              Request
 *                      ----------------------
 *
 * +================+=========================================================+
 * |  Byte offset   |                       Description                       |
 * +================+=========================================================+
 * |        0       | Magic Value                                             |
 * +----------------+---------------------------------------------------------+
 * |        1       | Target ID (Note 1)                                      |
 * +----------------+---------------------------------------------------------+
 * |        2       | Flags (Note 2)                                          |
 * +----------------+---------------------------------------------------------+
 * |        3       | Reserved. Set to 0x00.                                  |
 * +----------------+---------------------------------------------------------+
 * |        4       | 8-bit address                                           |
 * +----------------+---------------------------------------------------------+
 * |       8:5      | 32-bit data, little-endian                              |
 * +----------------+---------------------------------------------------------+
 * |      15:9      | Reserved. Set to 0.                                     |
 * +----------------+---------------------------------------------------------+
 *
 *
 *                              Response
 *                      ----------------------
 *
 * The response packet contains the same information as the request.
 * A status flag will be set if the operation completed successfully.
 *
 * In the case of a read request, the data field will contain the read data, if
 * the read succeeded.
 *
 * (Note 1)
 *  The "Target ID" refers to the peripheral, device, or block to access.
 *  See the NIOS_PKT_8x32_TARGET_* values.
 *
 * (Note 2)
 *  The flags are defined as follows:
 *
 *    +================+========================+
 *    |      Bit(s)    |         Value          |
 *    +================+========================+
 *    |       7:2      | Reserved. Set to 0.    |
 *    +----------------+------------------------+
 *    |                | Status. Only used in   |
 *    |                | response packet.       |
 *    |                | Ignored in request.    |
 *    |        1       |                        |
 *    |                |   1 = Success          |
 *    |                |   0 = Failure          |
 *    +----------------+------------------------+
 *    |        0       |   0 = Read operation   |
 *    |                |   1 = Write operation  |
 *    +----------------+------------------------+
 *
 */

#define NIOS_PKT_8x32_MAGIC           ((uint8_t) 'C')

/* Request packet indices */
#define NIOS_PKT_8x32_IDX_MAGIC       0
#define NIOS_PKT_8x32_IDX_TARGET_ID   1
#define NIOS_PKT_8x32_IDX_FLAGS       2
#define NIOS_PKT_8x32_IDX_RESV1       3
#define NIOS_PKT_8x32_IDX_ADDR        4
#define NIOS_PKT_8x32_IDX_DATA        5
#define NIOS_PKT_8x32_IDX_RESV2       9

/* Target IDs */
#define NIOS_PKT_8x32_TARGET_VERSION  0x00   /* FPGA version (read only) */
#define NIOS_PKT_8x32_TARGET_CONTROL  0x01   /* FPGA control/config register */
#define NIOS_PKT_8x32_TARGET_ADF4351  0x02   /* XB-200 ADF4351 register access
                                              * (write-only) */
#define NIOS_PKT_8x32_TARGET_RFFE_CSR 0x03   /* RFFE control & status GPIO */
#define NIOS_PKT_8x32_TARGET_ADF400X  0x04   /* ADF400x config */
#define NIOS_PKT_8x32_TARGET_FASTLOCK 0x05   /* Save AD9361 fast lock profile
                                              * to Nios */

/* IDs 0x80 through 0xff will not be assigned by Nuand. These are reserved
 * for user customizations */
#define NIOS_PKT_8x32_TARGET_USR1     0x80
#define NIOS_PKT_8x32_TARGET_USR128   0xff

/* RF link status word (read-only): assembled from tx.vhd/rx.vhd fifo_reader/
 * fifo_writer status ports in bladerf-hosted.vhd (signal rf_link_status),
 * carried to the Nios by the rf_link_status input PIO in nios_system.tcl.
 *
 * All tx_clock and rx_clock domain bits below are synchronized into sys_clock
 * by the U_sync_tx_ and U_sync_rx_ instances in bladerf-hosted.vhd before
 * reaching this PIO. No raw tx or rx signal crosses a domain uncrossed.
 *
 *   bit  0      TX link active
 *   bit  1      TX usb speed latched
 *   bit  2      usb speed as the host last set it
 *   bit  3      reserved, read as zero (was speed mismatch OR of TX/RX,
 *               a combinational cross-domain OR -- host can OR bits 4/5)
 *   bit  4      speed mismatch, RX
 *   bit  5      speed mismatch, TX
 *   bit  6      reserved, read as zero (was protocol start violation OR of
 *               TX/RX, same combinational cross-domain defect -- host can
 *               OR bits 7/18)
 *   bit  7      protocol start violation, RX
 *   bit  8      RX epoch current: RX mirrored back the epoch toggle the host
 *               issued, so RX consumed THIS epoch
 *   bit  9      TX epoch current, same meaning for the other direction
 *   bit  10     epoch applied: both directions current AND both valid. This
 *               is the bit the host waits on before trusting a stream.
 *   bit  11     RX epoch valid: RX has consumed at least one epoch since
 *               reset
 *   bit  12     TX epoch valid, same for TX
 *
 *               Bits 8 and 9 already include the validity term: after reset
 *               the issued toggle and a zeroed acknowledgement compare equal,
 *               so the comparison alone would report "current" for a
 *               direction that had never consumed an epoch. Bits 11 and 12
 *               are exposed separately so the host can tell "never started"
 *               apart from "started, but not this epoch".
 *
 *               "Current" is deliberately not "link active": link_active
 *               drops on stop or abort while the epoch toggle stands still,
 *               so it means "still running", not "took this epoch".
 *   bit  13     start refused: requested speed disagreed with the live
 *               GPIO speed bit at the moment START was issued
 *   bits 15:14  reserved, read as zero
 *   bit  16     RX link active
 *   bit  17     RX usb speed latched
 *   bit  18     protocol start violation, TX
 *   bit  19     reserved, read as zero
 *   bits 27:20  link epoch count, 8 bits, advances once per accepted START,
 *               shared by both directions. Diagnostic only -- what the host
 *               waits on is bit 10, not this number. Eight bits rather than
 *               four because it is read while retrying a start, and four
 *               would wrap inside a single bad recovery session.
 *   bits 31:28  RF_LINK_STATUS protocol version, currently 0x1
 *
 * A mismatch means FX3 latched one USB speed for the epoch and the link is
 * now running at another, so the DMA buffer geometry no longer matches the
 * transfers. A start violation means the datapath was enabled without the
 * host first signalling a new epoch. Both are sticky within an epoch. */
#define NIOS_PKT_8x32_TARGET_RF_LINK_STATUS  0x81

/* RF link control (write-only). The write half of the mechanism above: the
 * host declares a link generation, the fabric latches USB speed at that
 * moment and reports back through RF_LINK_STATUS.
 *
 * It exists because FX3 samples the USB speed exactly once per RF link start
 * and never rebuilds pcktSize/burstLen/dmaCfg.size afterwards, while the FPGA
 * re-reads it continuously. On a renegotiation the two sides of one GPIF end
 * up with different DMA geometry and nothing reports it -- the control path
 * still answers. We cannot rebuild the FX3 firmware, so the fabric has to be
 * told explicitly when a new generation begins rather than inferring it from
 * the long-lived enable level, which can stay high across an FX3 restart.
 *
 * The 8-bit address field carries the command rather than a register offset.
 * The underlying hardware bits are toggles, so "start" is a transition, not a
 * value; keeping that in the firmware means the host cannot desynchronise a
 * shadow copy of them.
 *
 * Host ordering, as measured on stock FX3 firmware (not as first designed):
 *   enable the datapath (BLADE_USB_CMD_RF_RX/RF_TX to FX3) FIRST
 *   -> determine actual USB speed -> SET_SPEED -> START
 *   -> poll RF_LINK_STATUS until bit 10 (both directions applied the epoch)
 *   ... stream ...
 *   -> disable the datapath -> STOP.
 *
 * Enable comes first because FX3 pulses GPIO_SYS_RST inside the RF_RX/RF_TX
 * vendor command whenever both RX_EN and TX_EN are low, and raises the
 * enable line in the same handler (fx3_firmware/src/bladeRF.c). Anything
 * declared before that command is erased by it, and there is no window
 * between the reset and the enable edge. The writer therefore treats enable
 * without an epoch as ARMED -- FIFO held in clear, samples discarded, no
 * fault -- and streams from the START.
 *
 * The START toggle is shared by both directions. A direction whose enable
 * is low ignores it (no link, no ack, no fault), so an RX-only session does
 * not fault TX; when that direction is enabled later the host announces
 * again and that START is a fresh edge for it. A START on an already
 * streaming direction is harmless: it re-latches speed and restarts the
 * progress watchdogs, and does not touch the FIFO. Bits 7 and 18 (protocol
 * start violation) therefore always read zero. The host waits on the bit of
 * the direction it enabled (8 for RX, 9 for TX), not on bit 10.
 *
 * Cycling the USB alternate setting does NOT restart the FX3 RF link (epoch
 * counter continuous across it) and is not part of the sequence.
 *
 * START asserts that the FX3 link start succeeded. It is not evidence about
 * FX3 by itself; only observed data progress is that. If the requested speed
 * disagrees with the live GPIO speed bit at that instant, the fabric fails
 * closed and refuses the epoch rather than guessing which one is right. */
#define NIOS_PKT_8x32_TARGET_RF_LINK_CFG     0x82

/* Dwell status (read-only), sweep revision.
 *
 * One word describing the dwell that just ended and the state of the
 * pre-trigger ring. Read as a unit: these bits are only meaningful together,
 * and separate reads would show them from different instants.
 *
 *   bit  0      frozen         the ring holds a trigger's history and has
 *                              stopped recording
 *   bit  1      wrapped        the ring filled at least once, so all
 *                              PRETRIG_DEPTH entries are history. When
 *                              clear, only entries 0..oldest-1 are valid
 *   bit  2      triggered      the dwell crossed the energy threshold for
 *                              long enough. Advisory: the host decides what
 *                              to do, the fabric never discards a dwell
 *   bit  3      measure_valid  the receiver had settled. Low means the
 *                              samples describe a gain transient rather
 *                              than the band
 *   bit  4      gain_too_high  the dwell clipped past 100 ppm, so its
 *                              amplitude figures are bounded by the ADC
 *                              rail, not by the signal
 *   bits 15:5   reserved, read as zero
 *   bits 27:16  oldest_index, where the ring's oldest sample lives. Only
 *               meaningful with bit 1 set
 *   bits 31:28  protocol version, currently 0x1
 *
 * Reads 0 on the hosted revision, which instantiates none of this: 0 means
 * "nothing frozen, nothing triggered", which is the truth there. */
#define NIOS_PKT_8x32_TARGET_DWELL_STATUS    0x83

/* Pre-trigger ring read (read-only), sweep revision.
 *
 * The returned word is one IQ sample, Q in bits 31:16 and I in bits 15:0,
 * both signed 16-bit in the same SC16 Q11 format the sample path uses --
 * full scale 2048, not 32768.
 *
 * Paged, because the packet's addr field is 8 bits and the ring is 4096
 * deep. WRITE this target to set the page base (low 12 bits of data), then
 * READ with addr as the offset within the page; the entry returned is
 * base + addr. The host writes a base every 256 entries.
 *
 * The field was not widened on purpose: this is the vendor packet format
 * and it stays compatible.
 *
 * ⛔ addr is a RAW ring index, not an offset from the oldest sample. The
 * ring wrapped, so reading 0..N-1 in order yields a rotated capture: correct
 * samples in the wrong order, which looks like a signal that begins
 * mid-burst. Start from the oldest_index reported by DWELL_STATUS and wrap.
 *
 * Only meaningful while DWELL_STATUS reports frozen; otherwise the write
 * side is still overwriting entries as they are read. */
#define NIOS_PKT_8x32_TARGET_PRETRIG_READ    0x84

/* Dwell summary readout (read-only), sweep revision.
 *
 * addr selects a word of the latched summary:
 *
 *    0,1   energy_sum, low word first
 *    2     peak
 *    3     clip_count
 *    4     sample_count
 *    5,6   first_timestamp, low word first
 *    7     mean_power        energy per window, dwell_energy >> WINDOW_LOG2
 *    8,9   noise_floor       quietest window of the dwell
 *   10,11  peak_window       loudest window
 *   12     first_window      window index where the trigger first crossed
 *   15     generation        advances once per completed dwell
 *
 * All values are in raw ADC units squared, summed -- no scaling, no dB. The
 * fabric does not divide except by powers of two, so nothing here has been
 * rounded on the way out.
 *
 * ⛔ Read word by word, this cannot be trusted on its own. The fabric
 * latches the whole summary at each dwell boundary, so a read that
 * straddles one returns an energy from one band with a peak from another --
 * a record that describes no dwell and looks entirely plausible. Read the
 * generation before and after and keep the record only if they match; that
 * is what nios_dwell_summary_read() does.
 *
 * Generation 0 is legitimate: it is what a device reads after reset, before
 * the first dwell completes. */
#define NIOS_PKT_8x32_TARGET_DWELL_READOUT   0x85

/* Sample-loss counters (read-only).
 *
 * addr selects one 32-bit half of one 64-bit counter:
 *
 *   0   RX overflow, low word    samples the writer had to discard
 *   1   RX overflow, high word
 *   2   TX underflow, low word   reads that found the FIFO empty
 *   3   TX underflow, high word
 *
 * Why this target exists: the fabric counted both of these all along and
 * the numbers never left the chip (rx.vhd/tx.vhd wired the counters to
 * `open`, leaving an LED as the only evidence). BLADERF_META_STATUS_OVERRUN
 * does NOT cover it -- that flag is computed on the host from USB transfer
 * queue state and never reads the fabric, so a drop the fabric absorbed on
 * its own is invisible to it.
 *
 * The two halves cannot tear against each other: both PIOs are driven from
 * ONE 64-bit capture register in the system domain, refreshed as a whole by
 * a handshake crossing from the sample domain (bladerf_core.vhd,
 * U_handshake_rx_overflow / U_handshake_tx_underflow). Either half may be
 * one snapshot stale relative to the counter, never half of one value and
 * half of another. Read order therefore does not matter.
 *
 * Counters are free-running and monotonic; they clear only on fabric reset.
 * The host takes differences between reads rather than expecting zero. */
#define NIOS_PKT_8x32_TARGET_LOSS_COUNTERS   0x86

/* RX data-plane epoch gate control (write-only). ADR-0207 §6: a retune
 * leaves stale pre-retune samples in flight through the FIFO writer; this
 * gate suppresses sample admission from epoch_arm until the host confirms
 * RFIC completion and the first subsequent ADC sample defines the boundary.
 *
 * The addr field carries the command, same discipline as RF_LINK_CFG above
 * and for the same reason: bits 0..2 of the underlying PIO word are
 * toggles, not levels, so the firmware (not a host-side shadow copy) is the
 * only thing allowed to decide what changed since the last write.
 *
 * ARM carries the new epoch_id in `data` bits 7..0; the fabric mirrors it
 * back in RX_EPOCH_STATUS once the gate opens, so the host can tell this
 * epoch's open event apart from an earlier one it never saw close.
 *
 * Host ordering: ARM before the retune's bladerf_set_frequency() call ->
 * COMPLETE once the required RFIC events confirm the retune landed -> poll
 * RX_EPOCH_STATUS until ACTIVE_NEW/ACTIVE or ERROR. ABORT always fails
 * closed in ERROR: after a partial/failed LO operation the old epoch cannot
 * safely be assumed valid. A later explicit ARM starts recovery directly
 * from ERROR; it never reopens the failed epoch. */
#define NIOS_PKT_8x32_TARGET_RX_EPOCH_CTRL   0x87

/* Deprecated legacy target retained for old host packets. NIOS accepts but
 * ignores writes; epoch validity never uses fixed sample discard. */
#define NIOS_PKT_8x32_TARGET_RX_EPOCH_SETTLE 0x88

/* RX data-plane epoch gate status (read-only). One word:
 *
 *   bits 31:24  epoch_id        mirrors the ARM that opened this epoch
 *   bits 23:20  state           0=ACTIVE 1=PENDING
 *                                2=WAIT_FIRST_SAMPLE 3=ACTIVE_NEW 4=ERROR
 *   bit  19     discard_active  samples are being suppressed right now
 *   bits 18:0   reserved, read as zero
 *
 * Crosses rx_clock -> sys_clock through its own work.handshake instance
 * (bladerf_core.vhd, U_rx_epoch_status_handshake), same single-register
 * snapshot guarantee as RF_LINK_STATUS above. */
#define NIOS_PKT_8x32_TARGET_RX_EPOCH_STATUS 0x89

/* RX data-plane epoch gate first-valid timestamp (read-only), 64 bits
 * across two targets -- same "a half may be one snapshot stale, never half
 * of one value and half of another" guarantee as LOSS_COUNTERS above, each
 * half behind its own handshake rather than one 64-bit crossing. */
#define NIOS_PKT_8x32_TARGET_RX_EPOCH_TS_LO   0x8a
#define NIOS_PKT_8x32_TARGET_RX_EPOCH_TS_HI   0x8b

/* RX FIFO-writer sticky causes, captured from rx_clock into sys_clock as one
 * coherent word. Bits 0..4 match fifo_writer fault_sticky bit indices:
 * speed mismatch, no progress, GPIF timeout, protocol error, FIFO abort. */
#define NIOS_PKT_8x32_TARGET_RX_FAULT_CAUSES  0x8c
#define NIOS_PKT_8x32_RX_FAULT_SPEED_MISMATCH    (1u << 0)
#define NIOS_PKT_8x32_RX_FAULT_START_NO_PROGRESS (1u << 1)
#define NIOS_PKT_8x32_RX_FAULT_GPIF_TIMEOUT      (1u << 2)
#define NIOS_PKT_8x32_RX_FAULT_PROTOCOL_ERROR    (1u << 3)
#define NIOS_PKT_8x32_RX_FAULT_FIFO_ABORT        (1u << 4)

#define NIOS_PKT_8x32_RX_EPOCH_CMD_ARM       0x00 /* data: 8-bit epoch_id */
#define NIOS_PKT_8x32_RX_EPOCH_CMD_COMPLETE  0x01
#define NIOS_PKT_8x32_RX_EPOCH_CMD_ABORT     0x02

/* RX_EPOCH_STATUS word layout, shared between the Nios firmware that
 * composes it (rx_epoch_gate.vhd via bladerf_core.vhd) and the host that
 * decodes it (rf_transition.c) -- one definition, not two copies that can
 * drift apart. See NIOS_PKT_8x32_TARGET_RX_EPOCH_STATUS above for the bit
 * table. */
#define NIOS_PKT_8x32_RX_EPOCH_STATUS_EPOCH_ID_SHIFT 24
#define NIOS_PKT_8x32_RX_EPOCH_STATUS_EPOCH_ID_MASK  0xFFu
#define NIOS_PKT_8x32_RX_EPOCH_STATUS_STATE_SHIFT    20
#define NIOS_PKT_8x32_RX_EPOCH_STATUS_STATE_MASK     0xFu
#define NIOS_PKT_8x32_RX_EPOCH_STATUS_DISCARD_ACTIVE (1u << 19)

#define NIOS_PKT_8x32_RX_EPOCH_STATE_ACTIVE      0x0u
#define NIOS_PKT_8x32_RX_EPOCH_STATE_PENDING     0x1u
#define NIOS_PKT_8x32_RX_EPOCH_STATE_WAIT_FIRST_SAMPLE 0x2u
/* Source compatibility only: state 2 waits for an ADC valid event; it does
 * not count or discard a fixed number of samples. */
#define NIOS_PKT_8x32_RX_EPOCH_STATE_SETTLING \
    NIOS_PKT_8x32_RX_EPOCH_STATE_WAIT_FIRST_SAMPLE
#define NIOS_PKT_8x32_RX_EPOCH_STATE_ACTIVE_NEW  0x3u
#define NIOS_PKT_8x32_RX_EPOCH_STATE_ERROR       0x4u

/* A transition is complete only when the gate is open for the exact epoch
 * armed by the host. ACTIVE alone is insufficient: it can describe a prior
 * epoch if status is stale or a control command was lost. */
static inline bool nios_pkt_8x32_rx_epoch_status_is_active(
    uint32_t status, uint8_t expected_epoch_id)
{
    const uint8_t state = (uint8_t)((status >>
        NIOS_PKT_8x32_RX_EPOCH_STATUS_STATE_SHIFT) &
        NIOS_PKT_8x32_RX_EPOCH_STATUS_STATE_MASK);
    const uint8_t epoch_id = (uint8_t)((status >>
        NIOS_PKT_8x32_RX_EPOCH_STATUS_EPOCH_ID_SHIFT) &
        NIOS_PKT_8x32_RX_EPOCH_STATUS_EPOCH_ID_MASK);

    return epoch_id == expected_epoch_id &&
        (state == NIOS_PKT_8x32_RX_EPOCH_STATE_ACTIVE_NEW ||
         state == NIOS_PKT_8x32_RX_EPOCH_STATE_ACTIVE);
}

#define NIOS_PKT_8x32_RF_LINK_CMD_SET_SPEED     0x00 /* data: 0 = SS, 1 = HS */
#define NIOS_PKT_8x32_RF_LINK_CMD_SET_TAG       0x01 /* data: 8-bit host tag */
#define NIOS_PKT_8x32_RF_LINK_CMD_START         0x02
#define NIOS_PKT_8x32_RF_LINK_CMD_STOP          0x03
#define NIOS_PKT_8x32_RF_LINK_CMD_CLEAR_FAULTS  0x04

/* Flag bits */
#define NIOS_PKT_8x32_FLAG_WRITE      (1 << 0)
#define NIOS_PKT_8x32_FLAG_SUCCESS    (1 << 1)

/* Function to convert target ID to string */
static inline const char* target2str(uint8_t target_id) {
    switch (target_id) {
        case NIOS_PKT_8x32_TARGET_VERSION:
            return "FPGA Version";
        case NIOS_PKT_8x32_TARGET_CONTROL:
            return "FPGA Control/Config Register";
        case NIOS_PKT_8x32_TARGET_ADF4351:
            return "XB-200 ADF4351 Register (Write-Only)";
        case NIOS_PKT_8x32_TARGET_RFFE_CSR:
            return "RFFE Control & Status GPIO";
        case NIOS_PKT_8x32_TARGET_ADF400X:
            return "ADF400x Config";
        case NIOS_PKT_8x32_TARGET_FASTLOCK:
            return "AD9361 Fast Lock Profile";

        case NIOS_PKT_8x32_TARGET_RX_EPOCH_CTRL:
            return "RX Epoch Gate Control (Write-Only)";
        case NIOS_PKT_8x32_TARGET_RX_EPOCH_SETTLE:
            return "Deprecated RX Epoch Sample-Discard Target (Ignored)";
        case NIOS_PKT_8x32_TARGET_RX_EPOCH_STATUS:
            return "RX Epoch Gate Status";
        case NIOS_PKT_8x32_TARGET_RX_EPOCH_TS_LO:
            return "RX Epoch Gate First-Valid Timestamp, Low Word";
        case NIOS_PKT_8x32_TARGET_RX_EPOCH_TS_HI:
            return "RX Epoch Gate First-Valid Timestamp, High Word";

        /* Reserved for user customizations */
        case NIOS_PKT_8x32_TARGET_USR1:
            return "User Defined 1";
        case NIOS_PKT_8x32_TARGET_USR128:
            return "User Defined 128";

        default:
            return "Unknown Target ID";
    }
}

/* Pack the request buffer */
static inline void nios_pkt_8x32_pack(uint8_t *buf, uint8_t target, bool write,
                                      uint8_t addr, uint32_t data)
{
    buf[NIOS_PKT_8x32_IDX_MAGIC]     = NIOS_PKT_8x32_MAGIC;
    buf[NIOS_PKT_8x32_IDX_TARGET_ID] = target;

    if (write) {
        buf[NIOS_PKT_8x32_IDX_FLAGS] = NIOS_PKT_8x32_FLAG_WRITE;
    } else {
        buf[NIOS_PKT_8x32_IDX_FLAGS] = 0x00;
    }

    buf[NIOS_PKT_8x32_IDX_RESV1] = 0x00;

    buf[NIOS_PKT_8x32_IDX_ADDR] = addr;

    buf[NIOS_PKT_8x32_IDX_DATA + 0] = data & 0xff;
    buf[NIOS_PKT_8x32_IDX_DATA + 1] = (data >> 8);
    buf[NIOS_PKT_8x32_IDX_DATA + 2] = (data >> 16);
    buf[NIOS_PKT_8x32_IDX_DATA + 3] = (data >> 24);

    buf[NIOS_PKT_8x32_IDX_RESV2 + 0] = 0x00;
    buf[NIOS_PKT_8x32_IDX_RESV2 + 1] = 0x00;
    buf[NIOS_PKT_8x32_IDX_RESV2 + 2] = 0x00;
    buf[NIOS_PKT_8x32_IDX_RESV2 + 3] = 0x00;
    buf[NIOS_PKT_8x32_IDX_RESV2 + 4] = 0x00;
    buf[NIOS_PKT_8x32_IDX_RESV2 + 5] = 0x00;
    buf[NIOS_PKT_8x32_IDX_RESV2 + 6] = 0x00;
}

/* Unpack the request buffer */
static inline void nios_pkt_8x32_unpack(const uint8_t *buf, uint8_t *target,
                                        bool *write, uint8_t *addr,
                                        uint32_t *data)
{
    if (target != NULL) {
        *target = buf[NIOS_PKT_8x32_IDX_TARGET_ID];
    }

    if (write != NULL) {
        *write  = (buf[NIOS_PKT_8x32_IDX_FLAGS] & NIOS_PKT_8x32_FLAG_WRITE) != 0;
    }

    if (addr != NULL) {
        *addr   = buf[NIOS_PKT_8x32_IDX_ADDR];
    }

    if (data != NULL) {
        *data   = (buf[NIOS_PKT_8x32_IDX_DATA + 0] << 0)  |
                  (buf[NIOS_PKT_8x32_IDX_DATA + 1] << 8)  |
                  (buf[NIOS_PKT_8x32_IDX_DATA + 2] << 16) |
                  (buf[NIOS_PKT_8x32_IDX_DATA + 3] << 24);
    }
}

/* Pack the response buffer */
static inline void nios_pkt_8x32_resp_pack(uint8_t *buf, uint8_t target,
                                           bool write, uint8_t addr,
                                           uint32_t data, bool success)
{
    nios_pkt_8x32_pack(buf, target, write, addr, data);

    if (success) {
        buf[NIOS_PKT_8x32_IDX_FLAGS] |= NIOS_PKT_8x32_FLAG_SUCCESS;
    }
}

/* Unpack the response buffer */
static inline void nios_pkt_8x32_resp_unpack(const uint8_t *buf,
                                             uint8_t *target, bool *write,
                                             uint8_t *addr, uint32_t *data,
                                             bool *success)
{
    nios_pkt_8x32_unpack(buf, target, write, addr, data);

    if ((buf[NIOS_PKT_8x32_IDX_FLAGS] & NIOS_PKT_8x32_FLAG_SUCCESS) != 0) {
        *success = true;
    } else {
        *success = false;
    }
}

#endif
