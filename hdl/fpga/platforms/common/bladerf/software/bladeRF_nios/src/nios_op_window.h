/* Host-armed measurement window over the NIOS main loop: max single worker
 * callback, max gap between deferred-work passes, max service-hook gap and
 * duration. Unlike init_timing it is not tied to cold INIT, so it covers
 * RETUNE2, TXQ, sample-rate and fastlock loader operations issued while the
 * window is open. Diagnostics only: never gates or times out an operation. */
#ifndef NIOS_OP_WINDOW_H_
#define NIOS_OP_WINDOW_H_

#include <stdbool.h>
#include <stdint.h>

#include "nios_pkt_8x64.h"

struct nios_op_window {
    bool open;
    bool have_pass;
    bool have_hook;
    uint64_t open_ticks;
    uint64_t closed_elapsed_ticks;
    uint64_t last_pass_end_ticks;
    uint64_t last_hook_ticks;
    uint64_t max_worker_ticks;
    uint64_t max_service_gap_ticks;
    uint64_t max_hook_gap_ticks;
    uint64_t max_hook_ticks;
    uint64_t passes;
};

extern struct nios_op_window nios_op_window;

/* Device-clock ticks (alt_timestamp), same counter as init_timing. */
uint64_t nios_op_window_now(void);

static inline void nios_op_window_max(uint64_t *max, uint64_t value)
{
    if (value > *max)
        *max = value;
}

static inline void nios_op_window_open(struct nios_op_window *w, uint64_t now)
{
    *w = (struct nios_op_window){ .open = true, .open_ticks = now };
}

static inline void nios_op_window_close(struct nios_op_window *w, uint64_t now)
{
    if (w->open) {
        w->closed_elapsed_ticks = now - w->open_ticks;
        w->open = false;
    }
}

static inline void nios_op_window_pass_begin(struct nios_op_window *w,
                                             uint64_t now)
{
    if (w->open && w->have_pass)
        nios_op_window_max(&w->max_service_gap_ticks,
                           now - w->last_pass_end_ticks);
}

static inline void nios_op_window_pass_end(struct nios_op_window *w,
                                           uint64_t now)
{
    if (w->open) {
        w->last_pass_end_ticks = now;
        w->have_pass = true;
        w->passes++;
    }
}

static inline void nios_op_window_worker(struct nios_op_window *w,
                                         uint64_t start, uint64_t end)
{
    if (w->open)
        nios_op_window_max(&w->max_worker_ticks, end - start);
}

static inline bool nios_op_window_hook_begin(struct nios_op_window *w,
                                             uint64_t now)
{
    if (!w->open)
        return false;
    if (w->have_hook)
        nios_op_window_max(&w->max_hook_gap_ticks, now - w->last_hook_ticks);
    w->last_hook_ticks = now;
    w->have_hook = true;
    return true;
}

static inline void nios_op_window_hook_end(struct nios_op_window *w,
                                           uint64_t start, uint64_t now)
{
    nios_op_window_max(&w->max_hook_ticks, now - start);
}

static inline bool nios_op_window_read(const struct nios_op_window *w,
                                       uint8_t addr, uint64_t now,
                                       uint64_t *value)
{
    switch (addr) {
        case NIOS_PKT_8x64_OPWIN_CONTROL:
            *value = w->open ? 1u : 0u;
            return true;
        case NIOS_PKT_8x64_OPWIN_MAX_WORKER_TICKS:
            *value = w->max_worker_ticks;
            return true;
        case NIOS_PKT_8x64_OPWIN_MAX_SERVICE_GAP_TICKS:
            *value = w->max_service_gap_ticks;
            return true;
        case NIOS_PKT_8x64_OPWIN_MAX_HOOK_GAP_TICKS:
            *value = w->max_hook_gap_ticks;
            return true;
        case NIOS_PKT_8x64_OPWIN_MAX_HOOK_TICKS:
            *value = w->max_hook_ticks;
            return true;
        case NIOS_PKT_8x64_OPWIN_ELAPSED_TICKS:
            *value = w->open ? now - w->open_ticks : w->closed_elapsed_ticks;
            return true;
        case NIOS_PKT_8x64_OPWIN_PASSES:
            *value = w->passes;
            return true;
        default:
            *value = 0;
            return false;
    }
}

/* CONTROL write: 1 opens (and clears) the window, 0 closes it. */
static inline bool nios_op_window_write(struct nios_op_window *w,
                                        uint8_t addr, uint64_t value,
                                        uint64_t now)
{
    if (addr != NIOS_PKT_8x64_OPWIN_CONTROL || value > 1)
        return false;
    if (value)
        nios_op_window_open(w, now);
    else
        nios_op_window_close(w, now);
    return true;
}

#endif
