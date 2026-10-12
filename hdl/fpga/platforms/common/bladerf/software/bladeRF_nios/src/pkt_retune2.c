/* This file is part of the bladeRF project:
 *   http://www.github.com/nuand/bladeRF
 *
 * Copyright (c) 2018 Nuand LLC
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
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "pkt_handler.h"
#include "pkt_retune2.h"
#include "nios_pkt_retune2.h"    /* Packet format definition */
#include "nios_pkt_8x32.h"       /* Completion record protocol */
#include "devices.h"
#include "debug.h"

#ifdef BLADERF_NIOS_LIBAD936X
void rfic_invalidate_frequency(bladerf_module module);
bool rfic_read_frequency_live(bladerf_module module, uint8_t profile,
                              uint64_t *frequency_hz);
bool rfic_frequency_peer_unknown(bladerf_module target);
#endif  // BLADERF_NIOS_LIBAD936X

#ifdef BLADERF_NIOS_DEBUG
    volatile uint32_t pkt_retune2_error_count = 0;
#   define INCREMENT_ERROR_COUNT() do { pkt_retune2_error_count++; } while (0)
#else
#   define INCREMENT_ERROR_COUNT() do {} while (0)
#endif

/* The enqueue/dequeue routines require that this be a power of two */
#define RETUNE2_QUEUE_MAX   NIOS_PKT_8x32_RETUNE2_COMPLETION_DEPTH
#define QUEUE_FULL          0xff
#define QUEUE_EMPTY         0xfe
#define QUEUE_INVALID_TIMESTAMP 0xfd

#if RETUNE2_QUEUE_MAX < 2 || \
    (RETUNE2_QUEUE_MAX & (RETUNE2_QUEUE_MAX - 1)) != 0
#error "RETUNE2_QUEUE_MAX must be an even power of two"
#endif

/* State of items in the retune queue */
enum entry_state {
    ENTRY_STATE_INVALID = 0,  /* Marks entry invalid and not in use */
    ENTRY_STATE_NEW,          /* We have a new retune request to satisfy */
    ENTRY_STATE_SCHEDULED,    /* We've scheduled the timer interrupt for
                               * this entry and are awaiting the ISR */
    ENTRY_STATE_CANCEL_PENDING, /* Timer failure reported; await cancel ACK */
    ENTRY_STATE_READY,        /* The timer interrupt has fired - we should
                               * handle this retune */
    ENTRY_STATE_DONE,         /* Retune is complete */
};

struct queue_entry {
    volatile enum entry_state state;
    /* Snapshot the selected profile. `fastlocks_*[nios_profile]` is a shared
     * upload slot and may be overwritten by a later scheduled request before
     * this timer fires. */
    fastlock_profile profile;
    uint16_t nios_profile;
    uint64_t timestamp;
    bool cancel_recovery_seen;
};

static struct queue {
    /* The timestamp ISR reads count/rem_idx while the main loop enqueues,
     * dequeues, and clears this queue. Keep both accesses observable across
     * the asynchronous interrupt boundary. */
    volatile uint8_t count; /* Total number of items in the queue */
    uint8_t ins_idx;    /* Insertion index */
    volatile uint8_t rem_idx; /* Removal index, also read by the ISR */

    struct queue_entry entries[RETUNE2_QUEUE_MAX];
} rx_queue, tx_queue;

/* Deferred failures happen after the host has already received the queue
 * acceptance response. Keep per-direction monotonic counters so host
 * monitoring can report an execution failure instead of silently losing the
 * scheduled transition. */
static volatile uint32_t rx_retune2_failure_count;
static volatile uint32_t tx_retune2_failure_count;
static volatile uint32_t rx_retune2_success_count;
static volatile uint32_t tx_retune2_success_count;

struct retune2_completion {
    volatile uint32_t sequence;
    uint64_t frequency_hz;
    uint64_t scheduled_timestamp;
    uint16_t nios_profile;
};

/* Mark a slot unavailable before replacing its multiword payload. The
 * complement cannot alias a valid sequence for this slot: the ring depth is
 * an even power of two, so sequences selecting the same slot differ by an
 * even multiple of that depth, while `~sequence` differs by an odd value
 * modulo 2^32. The compiler barriers keep payload stores between the marker
 * and final sequence publication on the single-core NIOS reader path. */
static inline void retune2_completion_begin_write(
    struct retune2_completion *completion, uint32_t sequence)
{
    completion->sequence = ~sequence;
    __asm__ __volatile__("" ::: "memory");
}

static inline void retune2_completion_publish(
    struct retune2_completion *completion, uint32_t sequence,
    uint64_t frequency_hz, uint64_t scheduled_timestamp,
    uint16_t nios_profile)
{
    completion->frequency_hz = frequency_hz;
    completion->scheduled_timestamp = scheduled_timestamp;
    completion->nios_profile = nios_profile;
    __asm__ __volatile__("" ::: "memory");
    completion->sequence = sequence;
}

static struct retune2_completion
    rx_retune2_completions[RETUNE2_QUEUE_MAX];
static struct retune2_completion
    tx_retune2_completions[RETUNE2_QUEUE_MAX];
static volatile uint32_t selected_completion_sequence;
static volatile bladerf_module selected_completion_module;

uint32_t pkt_retune2_failure_count(bladerf_module module)
{
    switch (module) {
        case BLADERF_MODULE_RX:
            return rx_retune2_failure_count;
        case BLADERF_MODULE_TX:
            return tx_retune2_failure_count;
        default:
            return 0;
    }
}

uint32_t pkt_retune2_success_count(bladerf_module module)
{
    switch (module) {
        case BLADERF_MODULE_RX:
            return rx_retune2_success_count;
        case BLADERF_MODULE_TX:
            return tx_retune2_success_count;
        default:
            return 0;
    }
}

bool pkt_retune2_completion_select(bladerf_module module, uint32_t sequence)
{
    if (module != BLADERF_MODULE_RX && module != BLADERF_MODULE_TX) {
        return false;
    }

    selected_completion_module = module;
    selected_completion_sequence = sequence;
    return true;
}

uint32_t pkt_retune2_completion_word(uint8_t addr)
{
    struct retune2_completion *record;
    uint32_t sequence = selected_completion_sequence;

    if (addr > 5) {
        return 0;
    }
    record = selected_completion_module == BLADERF_MODULE_RX
        ? &rx_retune2_completions[(sequence - 1u) &
                                  (RETUNE2_QUEUE_MAX - 1u)]
        : &tx_retune2_completions[(sequence - 1u) &
                                  (RETUNE2_QUEUE_MAX - 1u)];

    switch (addr) {
        case 0:
            return record->sequence;
        case 1:
            return (uint32_t)record->frequency_hz;
        case 2:
            return (uint32_t)(record->frequency_hz >> 32);
        case 3:
            return record->nios_profile;
        case 4:
            return (uint32_t)record->scheduled_timestamp;
        case 5:
            return (uint32_t)(record->scheduled_timestamp >> 32);
        default:
            return 0;
    }
}

static void record_retune2_failure(bladerf_module module)
{
    if (module == BLADERF_MODULE_RX) {
        rx_retune2_failure_count++;
    } else if (module == BLADERF_MODULE_TX) {
        tx_retune2_failure_count++;
    }
    INCREMENT_ERROR_COUNT();
}

static bool retune2_profile_indices_valid(uint16_t nios_profile,
                                          uint8_t rffe_profile)
{
    return nios_profile < NUM_BBP_FASTLOCK_PROFILES &&
           rffe_profile < NUM_RFFE_FASTLOCK_PROFILES;
}

/* Returns queue size after enqueue operation, or QUEUE_FULL if we could
 * not enqueue the requested item */
static inline uint8_t enqueue_retune(struct queue *q,
                                     fastlock_profile *p,
                                     uint16_t nios_profile,
                                     uint64_t timestamp)
{
    uint8_t ret;

    if (q->count >= RETUNE2_QUEUE_MAX) {
        return QUEUE_FULL;
    }

    if (q->count != 0) {
        const uint8_t last_idx =
            (q->ins_idx - 1) & (RETUNE2_QUEUE_MAX - 1);
        if (!timestamp_is_strictly_after(
                timestamp, q->entries[last_idx].timestamp)) {
            return QUEUE_INVALID_TIMESTAMP;
        }
    }

    q->entries[q->ins_idx].profile = *p;
    q->entries[q->ins_idx].nios_profile = nios_profile;
    /* The source cache's residency belongs to the shared upload slot, not
     * this snapshot. Re-establish RFFE residency for the copied settings. */
    q->entries[q->ins_idx].profile.state = FASTLOCK_STATE_BBP;
    q->entries[q->ins_idx].state = ENTRY_STATE_NEW;
    q->entries[q->ins_idx].timestamp = timestamp;
    q->entries[q->ins_idx].cancel_recovery_seen = false;

    q->ins_idx = (q->ins_idx + 1) & (RETUNE2_QUEUE_MAX - 1);

    q->count++;
    ret = q->count;

    return ret;
}

/* Retune number of items left in the queue after the dequeue operation,
 * or QUEUE_EMPTY if there was nothing to dequeue */
static inline uint8_t dequeue_retune(struct queue *q, struct queue_entry *e)
{
    uint8_t ret;

    if (q->count == 0) {
        return QUEUE_EMPTY;
    }

    if (e != NULL) {
        memcpy(e, &q->entries[q->rem_idx], sizeof(*e));
    }

    q->entries[q->rem_idx].state = ENTRY_STATE_DONE;
    q->rem_idx = (q->rem_idx + 1) & (RETUNE2_QUEUE_MAX - 1);

    q->count--;
    ret = q->count;

    return ret;
}

/* Get the state of the next item in the retune queue */
static inline struct queue_entry* peek_next_retune(struct queue *q)
{
    if (q == NULL) {
        return NULL;
    } else if (q->count == 0) {
        return NULL;
    } else {
        return &q->entries[q->rem_idx];
    }
}

/* Get the queue element at the given offset relative to the removal index */
static inline struct queue_entry* peek_next_retune_offset(struct queue *q,
                                                           uint8_t offset)
{
    if (q == NULL) {
        return NULL;
    } else if (q->count == 0) {
        return NULL;
    } else {
        return &q->entries[(q->rem_idx + offset) & (RETUNE2_QUEUE_MAX - 1)];
    }
}

static bool profile_slot_compare_owned(const struct queue_entry *entry)
{
    return entry != NULL &&
           (entry->state == ENTRY_STATE_SCHEDULED ||
            entry->state == ENTRY_STATE_CANCEL_PENDING ||
            entry->state == ENTRY_STATE_READY);
}

static bool profile_slot_armed(const struct queue *q, uint8_t profile_num)
{
    uint32_t i;

    if (q == NULL) {
        return false;
    }

    for (i = 0; i < q->count; i++) {
        const struct queue_entry *e =
            &q->entries[(q->rem_idx + i) & (RETUNE2_QUEUE_MAX - 1)];
        if (profile_slot_compare_owned(e) &&
            e->profile.profile_num == profile_num) {
            return true;
        }
    }

    return false;
}

/* The retune interrupt may fire while this call is occuring, so we should
 * perform these operations in an order that minimizes the race window, and
 * does not cause the race to be problematic. It's fine if the last retune
 * occurs before we can cancel it. */
static void reset_queue(struct queue *q)
{
    unsigned int i;

    q->count = 0;

    for (i = 0; i < RETUNE2_QUEUE_MAX; i++) {
        q->entries[i].state = ENTRY_STATE_INVALID;
    }

    q->rem_idx = q->ins_idx = 0;
}

static inline bool profile_load(bladerf_module module, fastlock_profile *p)
{
    if (p == NULL) {
        return false;
    }

    return adi_fastlock_load(module, p);
}

static inline void profile_load_scheduled(struct queue *q,
                                          bladerf_module module)
{
    struct queue_entry *e;
    uint32_t i;
    uint8_t  used = 0;

    if ((q == NULL) || (q->count == 0)) {
        return;
    }

    /* Slots named by armed, cancel-pending, or fired entries are live
     * hardware state: their compare may still activate them before the queue
     * head is retired. Keep those slots reserved while preloading later
     * requests, otherwise a new request using the same RFFE slot overwrites
     * the profile of a still-live timer compare. */
    for (i = 0; i < q->count; i++) {
        e = peek_next_retune_offset(q, i);
        if (profile_slot_compare_owned(e)) {
            used |= 1u << e->profile.profile_num;
        }
    }

    /* Load each available profile slot at most once. Requests that collide
     * with a live slot remain NEW and are loaded when they reach the head. */
    for (i = 0; i < q->count; i++) {
        e = peek_next_retune_offset(q, i);
        if (e != NULL && e->state == ENTRY_STATE_NEW) {
            if (!(used & (1u << e->profile.profile_num))) {
                /* Profile slot is available in RFFE, fill it. */
                if (profile_load(module, &e->profile)) {
                    used |= 1u << e->profile.profile_num;
                }
                /* Preload is opportunistic. ENTRY_STATE_NEW retries the
                 * load before arming this entry, and only that terminal
                 * failure should be published to the host. */
            }
        }
    }
}

static inline bool profile_activate(bladerf_module module, fastlock_profile *p,
                                    uint64_t *readback_frequency_hz)
{
    if (p == NULL) {
        return false;
    }

#ifdef BLADERF_NIOS_LIBAD936X
    /* Dependent-FDD PLL programming chooses a VCO table using the peer LO.
     * Do not activate a saved profile while that frequency is invalidated by
     * an earlier partial/failed tune. */
    if (rfic_frequency_peer_unknown(module)) {
        return false;
    }
    /* Invalidate current frequency knowledge */
    rfic_invalidate_frequency(module);
#endif  // BLADERF_NIOS_LIBAD936X

    /* Activate the RFFE fast lock profile */
    if (!adi_fastlock_recall(module, p)) {
        return false;
    }

    /* Adjust the RFFE port */
    if (!adi_rfport_select(p)) {
        return false;
    }

    /* Adjust the RF switches */
    if (!adi_rfspdt_select(module, p)) {
        return false;
    }
#ifdef BLADERF_NIOS_LIBAD936X
    if (readback_frequency_hz != NULL &&
        !rfic_read_frequency_live(module, p->profile_num,
                                  readback_frequency_hz)) {
        return false;
    }
#else
    if (readback_frequency_hz != NULL) {
        *readback_frequency_hz = 0;
    }
#endif
    return true;
}

static inline void retune_isr(struct queue *q, bladerf_module module)
{
    struct queue_entry *e = peek_next_retune(q);
    if (e != NULL) {
        if (e->state == ENTRY_STATE_SCHEDULED) {
            /* An IRQ can be left pending across a failed cancel and a later
             * queue replacement. Only the sticky FPGA compare event tied to
             * the currently armed schedule may release this entry. */
            if (tamer_compare_fired(module)) {
                e->state = ENTRY_STATE_READY;
            } else {
                INCREMENT_ERROR_COUNT();
            }
        } else if (e->state != ENTRY_STATE_READY) {
            INCREMENT_ERROR_COUNT();
        }
    }
}


#ifndef BLADERF_NIOS_PC_SIMULATION
static void retune_rx(void *context)
{
    /* Handle the ISR */
    retune_isr(&rx_queue, BLADERF_MODULE_RX);

    /* Clear the interrupt */
    timer_tamer_clear_interrupt(BLADERF_MODULE_RX);
}

static void retune_tx(void *context)
{
    /* Handle the ISR */
    retune_isr(&tx_queue, BLADERF_MODULE_TX);

    /* Clear the interrupt */
    timer_tamer_clear_interrupt(BLADERF_MODULE_TX);
}
#endif


void pkt_retune2_init()
{
    reset_queue(&rx_queue);
    reset_queue(&tx_queue);
    rx_retune2_failure_count = 0;
    tx_retune2_failure_count = 0;
    rx_retune2_success_count = 0;
    tx_retune2_success_count = 0;
    memset(rx_retune2_completions, 0, sizeof(rx_retune2_completions));
    memset(tx_retune2_completions, 0, sizeof(tx_retune2_completions));
    selected_completion_sequence = 0;
    selected_completion_module = BLADERF_MODULE_RX;

#ifndef BLADERF_NIOS_PC_SIMULATION

    /* Register RX Time Tamer ISR */
    alt_ic_isr_register(
        RX_TAMER_IRQ_INTERRUPT_CONTROLLER_ID,
        RX_TAMER_IRQ,
        retune_rx,
        NULL,
        NULL
    ) ;

    /* Register TX Time Tamer ISR */
    alt_ic_isr_register(
        TX_TAMER_IRQ_INTERRUPT_CONTROLLER_ID,
        TX_TAMER_IRQ,
        retune_tx,
        NULL,
        NULL
    ) ;
#endif
}

static inline void perform_work(struct queue *q, bladerf_module module)
{
    struct queue_entry *e = peek_next_retune(q);

    if (e == NULL) {
        return;
    }

    switch (e->state) {
        case ENTRY_STATE_NEW: {
            uint64_t current_time;

            /* Load the fast lock profile into the RFFE */
            if (!profile_load(module, &e->profile)) {
                record_retune2_failure(module);
                dequeue_retune(q, NULL);
                break;
            }

            /* Profile loading can take long enough for a future timestamp
             * to pass. Never arm a timer in the past; report the deferred
             * request as failed instead of letting the tamer wrap around. */
            if (!time_tamer_read(module, &current_time) ||
                !timestamp_is_strictly_after(e->timestamp, current_time)) {
                record_retune2_failure(module);
                dequeue_retune(q, NULL);
                break;
            }

            /* Schedule the retune */
            e->state = ENTRY_STATE_SCHEDULED;
            if (!tamer_schedule(module, e->timestamp)) {
                record_retune2_failure(module);
                /* A schedule timeout is ambiguous: the compare payload may
                 * already be crossing into ts_clock. Keep the queue head
                 * until cancellation is acknowledged so a later request
                 * cannot inherit a late compare event. */
                e->state = ENTRY_STATE_CANCEL_PENDING;
                e->cancel_recovery_seen = !tamer_clock_stalled(module);
                if (e->cancel_recovery_seen && tamer_cancel(module)) {
                    dequeue_retune(q, NULL);
                }
            }

            break;
        }

        case ENTRY_STATE_SCHEDULED:
            /* The IRQ is the fast path. Recover from a lost IRQ by polling
             * the FPGA's sticky comparator event; time passing alone never
             * authorizes activation. */
            if (tamer_compare_fired(module)) {
                e->state = ENTRY_STATE_READY;
            } else if (tamer_clock_stalled(module)) {
                /* A stopped timestamp clock cannot generate the compare
                 * event. Fail the accepted request visibly instead of
                 * pinning this module's queue in SCHEDULED forever. Keep
                 * owning the compare until cancellation is acknowledged:
                 * after clock recovery an un-cancelled compare could still
                 * fire and interfere with the next queued request. */
                record_retune2_failure(module);
                e->state = ENTRY_STATE_CANCEL_PENDING;
                e->cancel_recovery_seen = false;
                if (tamer_cancel(module)) {
                    dequeue_retune(q, NULL);
                }
            }
            break;

        case ENTRY_STATE_CANCEL_PENDING:
            /* The failure was already published on entry. Retry only the
             * hardware cancellation; never activate or discard this entry
             * until the time-tamer confirms that its compare is disarmed.
             * Do not spin through the million-read cancel watchdog on every
             * worker pass: make one retry after the FPGA reports clock
             * recovery, then wait for another observed stall/recovery cycle
             * if that retry also fails. */
            if (tamer_clock_stalled(module)) {
                e->cancel_recovery_seen = false;
            } else if (!e->cancel_recovery_seen) {
                e->cancel_recovery_seen = true;
                if (tamer_cancel(module)) {
                    dequeue_retune(q, NULL);
                }
            }
            break;

        case ENTRY_STATE_READY: {
            uint64_t readback_frequency_hz;
            uint32_t completion_sequence;
            struct retune2_completion *completion;

            /* Activate the fast lock profile for this retune */
            if (!profile_activate(module, &e->profile,
                                 &readback_frequency_hz)) {
                record_retune2_failure(module);
            } else if (module == BLADERF_MODULE_RX ||
                       module == BLADERF_MODULE_TX) {
                completion_sequence = pkt_retune2_success_count(module) + 1u;
                completion = module == BLADERF_MODULE_RX
                    ? &rx_retune2_completions[(completion_sequence - 1u) &
                                              (RETUNE2_QUEUE_MAX - 1u)]
                    : &tx_retune2_completions[(completion_sequence - 1u) &
                                              (RETUNE2_QUEUE_MAX - 1u)];
                /* A reader may still request this ring slot's previous
                 * sequence while the payload is replaced. Invalidate the
                 * slot first, publish all payload words, then publish the
                 * new sequence before advancing the public success count. */
                retune2_completion_begin_write(completion,
                                               completion_sequence);
                retune2_completion_publish(
                    completion, completion_sequence, readback_frequency_hz,
                    e->timestamp, e->nios_profile);
                if (module == BLADERF_MODULE_RX) {
                    rx_retune2_success_count = completion_sequence;
                } else {
                    tx_retune2_success_count = completion_sequence;
                }
            }

            /* Drop the item from the queue */
            dequeue_retune(q, NULL);

            break;
        }

        default:
            /* Preserve ownership of a possibly armed compare until the
             * hardware confirms cancellation. Merely logging an impossible
             * state leaves the accepted request at the queue head forever
             * and never tells the host that it failed. */
            record_retune2_failure(module);
            e->state = ENTRY_STATE_CANCEL_PENDING;
            e->cancel_recovery_seen = !tamer_clock_stalled(module);
            if (e->cancel_recovery_seen && tamer_cancel(module)) {
                dequeue_retune(q, NULL);
            }
            break;
    }
}

void pkt_retune2_work(void)
{
    perform_work(&rx_queue, BLADERF_MODULE_RX);
    perform_work(&tx_queue, BLADERF_MODULE_TX);
}

void pkt_retune2(struct pkt_buf *b)
{
    int status = -1;
    bladerf_module module;
    uint8_t flags;
    uint64_t timestamp;
    uint64_t start_time;
    uint64_t end_time;
    uint64_t duration = 0;
    bool start_time_valid = false;
    uint16_t nios_profile;
    uint8_t rffe_profile;
    uint8_t port;
    uint8_t spdt;
    fastlock_profile *profile;
    fastlock_profile requested_profile;

    flags = NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS;

    nios_pkt_retune2_unpack(b->req, &module, &timestamp,
                            &nios_profile, &rffe_profile, &port, &spdt);

    /* Reserved request bytes are part of the wire contract. Reject them
     * before even reading the timer, so malformed commands cannot enter any
     * retune or queue-clear path. */
    if (!nios_pkt_retune2_reserved_valid(b->req)) {
        INCREMENT_ERROR_COUNT();
        flags &= ~NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS;
        nios_pkt_retune2_resp_pack(b->resp, 0, flags);
        return;
    }

    if (timestamp != NIOS_PKT_RETUNE_NOW &&
        timestamp != NIOS_PKT_RETUNE2_CLEAR_QUEUE) {
        if (!time_tamer_read(module, &start_time)) {
            INCREMENT_ERROR_COUNT();
            flags &= ~NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS;
            nios_pkt_retune2_resp_pack(b->resp, 0, flags);
            return;
        }
        start_time_valid = true;
    }

    if (timestamp == NIOS_PKT_RETUNE2_CLEAR_QUEUE) {
        switch (module) {
            case BLADERF_MODULE_RX:
                if (tamer_cancel(module)) {
                    reset_queue(&rx_queue);
                    status = 0;
                } else {
                    flags |= NIOS_PKT_RETUNE2_RESP_FLAG_CANCEL_FAILED;
                    status = -1;
                }
                break;

            case BLADERF_MODULE_TX:
                if (tamer_cancel(module)) {
                    reset_queue(&tx_queue);
                    status = 0;
                } else {
                    flags |= NIOS_PKT_RETUNE2_RESP_FLAG_CANCEL_FAILED;
                    status = -1;
                }
                break;

            default:
                INCREMENT_ERROR_COUNT();
                status = -1;
        }
    } else if (!nios_pkt_retune2_route_valid(module, port, spdt)) {
        /* Do not let malformed route selectors be truncated by the RFIC
         * register helpers into a different, apparently successful path. */
        INCREMENT_ERROR_COUNT();
        status = -1;
    } else if (!retune2_profile_indices_valid(nios_profile, rffe_profile)) {
        /* Do not index the per-direction profile arrays or shift a
         * caller-controlled RFFE slot into the preload bitmask unchecked. */
        INCREMENT_ERROR_COUNT();
        status = -1;
    } else {
        switch (module) {
            case BLADERF_MODULE_RX:
                profile = &fastlocks_rx[nios_profile];
                break;
            case BLADERF_MODULE_TX:
                profile = &fastlocks_tx[nios_profile];
                break;
            default:
                profile = NULL;
        }

        if (profile == NULL) {
            INCREMENT_ERROR_COUNT();
            status = -1;
        } else {
            const bool profile_already_in_target_slot =
                profile->profile_num == rffe_profile &&
                profile->state == FASTLOCK_STATE_BBP_RFFE;
            requested_profile = *profile;
            requested_profile.profile_num = rffe_profile;
            requested_profile.port = port;
            requested_profile.spdt = spdt;
            /* The residency state belongs to the source NIOS cache entry.
             * Retune2 can target a different physical RFFE slot, so do not
             * let adi_fastlock_load() mistake that old residency for the
             * requested slot already containing these profile bytes. Keep
             * it only when the cache proves this exact profile is still in
             * the requested slot; devices.c invalidates cache residency on
             * every successful slot replacement. */
            requested_profile.state = profile_already_in_target_slot
                ? FASTLOCK_STATE_BBP_RFFE : FASTLOCK_STATE_BBP;
            if (timestamp == NIOS_PKT_RETUNE2_NOW) {
                /* Fire off this retune operation now */
                uint64_t readback_frequency_hz = 0;
                struct queue *q = module == BLADERF_MODULE_TX
                    ? &tx_queue : &rx_queue;

                /* An armed compare can recall its physical RFFE slot
                 * asynchronously. A NOW request must not replace that
                 * profile while the queued transaction still owns it. */
                if (profile_slot_armed(q, requested_profile.profile_num)) {
                    INCREMENT_ERROR_COUNT();
                    status = -1;
                } else {
                    switch (module) {
                        case BLADERF_MODULE_RX:
                        case BLADERF_MODULE_TX:
                            /* Load the profile data into RFFE memory */
                            /* Do not publish the new BBP profile cache entry
                             * until every SPI write needed to load it has
                             * succeeded. A failed upload must leave the prior
                             * NIOS profile usable for a later retune. */
                            if (!profile_load(module, &requested_profile)) {
                                INCREMENT_ERROR_COUNT();
                                status = -1;
                                break;
                            }

                            *profile = requested_profile;

                            /* Activate the fast lock profile for this retune */
                            if (profile_activate(module, profile,
                                                 &readback_frequency_hz)) {
                                flags |=
                                    NIOS_PKT_RETUNE2_RESP_FLAG_TSVTUNE_VALID;
                                status = 0;
                            } else {
                                INCREMENT_ERROR_COUNT();
                                status = -1;
                            }
                            break;

                        default:
                            INCREMENT_ERROR_COUNT();
                            status = -1;
                    }
                }
            } else if (!timestamp_is_strictly_after(timestamp, start_time)) {
                flags |= NIOS_PKT_RETUNE2_RESP_FLAG_INVALID_TIMESTAMP;
                status = -1;
            } else {
                uint8_t queue_size;

                switch (module) {
                    case BLADERF_MODULE_RX:
                        queue_size = enqueue_retune(
                            &rx_queue, &requested_profile, nios_profile,
                            timestamp);
                        if (queue_size != QUEUE_FULL &&
                            queue_size != QUEUE_INVALID_TIMESTAMP) {
                            *profile = requested_profile;
                            profile_load_scheduled(&rx_queue, module);
                        }
                        break;

                    case BLADERF_MODULE_TX:
                        queue_size = enqueue_retune(
                            &tx_queue, &requested_profile, nios_profile,
                            timestamp);
                        if (queue_size != QUEUE_FULL &&
                            queue_size != QUEUE_INVALID_TIMESTAMP) {
                            *profile = requested_profile;
                            profile_load_scheduled(&tx_queue, module);
                        }
                        break;

                    default:
                        INCREMENT_ERROR_COUNT();
                        queue_size = QUEUE_FULL;
                }

                if (queue_size == QUEUE_FULL) {
                    status = -1;
                } else if (queue_size == QUEUE_INVALID_TIMESTAMP) {
                    flags |= NIOS_PKT_RETUNE2_RESP_FLAG_INVALID_TIMESTAMP;
                    status = -1;
                } else {
                    status = 0;
                }
            }
        }
    }

    if (start_time_valid && time_tamer_read(module, &end_time)) {
        duration = end_time - start_time;
    }

    if (status != 0) {
        flags &= ~(NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS);
    }

    nios_pkt_retune2_resp_pack(b->resp, duration, flags);
}
