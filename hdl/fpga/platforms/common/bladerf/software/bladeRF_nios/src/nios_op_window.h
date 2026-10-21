/* Host-armed measurement window over the NIOS main loop: max single worker
 * callback, max gap between deferred-work passes, max service-hook gap and
 * duration, in device-clock ticks (32-bit, wrap-safe differences; every
 * measured interval is far below the 80 MHz counter's 53 s wrap). Unlike
 * init_timing it is not tied to cold INIT, so it covers RETUNE2, TXQ,
 * sample-rate and fastlock loader operations issued while the window is
 * open. Diagnostics only: never gates or times out an operation. */
#ifndef NIOS_OP_WINDOW_H_
#define NIOS_OP_WINDOW_H_

#include <stdbool.h>
#include <stdint.h>

#include "nios_pkt_8x64.h"

enum { NOW_WORKER, NOW_GAP, NOW_HOOK_GAP, NOW_HOOK, NOW_N };

struct nios_op_window {
    bool open;
    bool have_pass;
    bool have_hook;
    uint32_t last_pass_end;
    uint32_t last_hook;
    uint32_t passes;
    uint32_t max[NOW_N];
};

extern struct nios_op_window nios_op_window;

/* Device-clock ticks (alt_timestamp), same counter as init_timing. */
uint32_t nios_op_window_now(void);

static inline void nios_op_window_max(uint32_t *max, uint32_t value)
{
    if (value > *max)
        *max = value;
}

static inline void nios_op_window_pass_begin(struct nios_op_window *w,
                                             uint32_t now)
{
    if (w->have_pass)
        nios_op_window_max(&w->max[NOW_GAP], now - w->last_pass_end);
}

static inline void nios_op_window_pass_end(struct nios_op_window *w,
                                           uint32_t now)
{
    if (w->open) {
        w->last_pass_end = now;
        w->have_pass = true;
        w->passes++;
    }
}

static inline void nios_op_window_worker(struct nios_op_window *w,
                                         uint32_t start, uint32_t end)
{
    if (w->open)
        nios_op_window_max(&w->max[NOW_WORKER], end - start);
}

static inline bool nios_op_window_hook_begin(struct nios_op_window *w,
                                             uint32_t now)
{
    if (!w->open)
        return false;
    if (w->have_hook)
        nios_op_window_max(&w->max[NOW_HOOK_GAP], now - w->last_hook);
    w->last_hook = now;
    w->have_hook = true;
    return true;
}

static inline void nios_op_window_hook_end(struct nios_op_window *w,
                                           uint32_t start, uint32_t now)
{
    nios_op_window_max(&w->max[NOW_HOOK], now - start);
}

/* Registers: CONTROL (0x10: 1 = open and clear, 0 = close), then the four
 * maxima (0x11..0x14) and the pass count (0x15). */
static inline bool nios_op_window_read(const struct nios_op_window *w,
                                       uint8_t addr, uint64_t *value)
{
    if (addr == NIOS_PKT_8x64_OPWIN_CONTROL)
        *value = w->open;
    else if (addr == NIOS_PKT_8x64_OPWIN_PASSES)
        *value = w->passes;
    else if (addr > NIOS_PKT_8x64_OPWIN_CONTROL &&
             addr < NIOS_PKT_8x64_OPWIN_PASSES)
        *value = w->max[addr - NIOS_PKT_8x64_OPWIN_MAX_WORKER_TICKS];
    else
        return false;
    return true;
}

static inline bool nios_op_window_write(struct nios_op_window *w,
                                        uint8_t addr, uint64_t value)
{
    if (addr != NIOS_PKT_8x64_OPWIN_CONTROL || value > 1)
        return false;
    if (value) {
        *w = (struct nios_op_window){ .open = true };
    } else {
        w->open = false;
    }
    return true;
}

#endif
