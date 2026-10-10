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
bool rfic_read_frequency_live(bladerf_module module, uint64_t *frequency_hz);
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

/* State of items in the retune queue */
enum entry_state {
    ENTRY_STATE_INVALID = 0,  /* Marks entry invalid and not in use */
    ENTRY_STATE_NEW,          /* We have a new retune request to satisfy */
    ENTRY_STATE_SCHEDULED,    /* We've scheduled the timer interrupt for
                               * this entry and are awaiting the ISR */
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
};

static struct queue {
    uint8_t count;      /* Total number of items in the queue */
    uint8_t ins_idx;    /* Insertion index */
    uint8_t rem_idx;    /* Removal index */

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
        if (timestamp <= q->entries[last_idx].timestamp) {
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

    /* Check the contents of the retune queue and load all the profiles we can
     * without causing them to step on each other. This should reduce retune
     * times in most scenarios because the profile will have already been
     * loaded into the RFFE when it becomes time to retune. */
    for (i = 0; i < q->count; i++) {
        e = peek_next_retune_offset(q, i);
        if( e != NULL ) {
            if (e->state == ENTRY_STATE_NEW) {
                if (!(used & (1 << e->profile.profile_num))) {
                    /* Profile slot is available in RFFE, fill it */
                    if (profile_load(module, &e->profile)) {
                        /* Mark profile slot used */
                        used |= 1 << e->profile.profile_num;
                    } else {
                        INCREMENT_ERROR_COUNT();
                    }
                }
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
    adi_rfspdt_select(module, p);
#ifdef BLADERF_NIOS_LIBAD936X
    if (readback_frequency_hz != NULL &&
        !rfic_read_frequency_live(module, readback_frequency_hz)) {
        return false;
    }
#else
    if (readback_frequency_hz != NULL) {
        *readback_frequency_hz = 0;
    }
#endif
    return true;
}

static inline void retune_isr(struct queue *q)
{
    struct queue_entry *e = peek_next_retune(q);
    if (e != NULL) {
        if (e->state == ENTRY_STATE_SCHEDULED) {
            e->state = ENTRY_STATE_READY;
        } else {
            INCREMENT_ERROR_COUNT();
        }
    }
}


#ifndef BLADERF_NIOS_PC_SIMULATION
static void retune_rx(void *context)
{
    /* Handle the ISR */
    retune_isr(&rx_queue);

    /* Clear the interrupt */
    timer_tamer_clear_interrupt(BLADERF_MODULE_RX);
}

static void retune_tx(void *context)
{
    /* Handle the ISR */
    retune_isr(&tx_queue);

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
        case ENTRY_STATE_NEW:

            /* Load the fast lock profile into the RFFE */
            if (!profile_load(module, &e->profile)) {
                record_retune2_failure(module);
                dequeue_retune(q, NULL);
                break;
            }

            /* Profile loading can take long enough for a future timestamp
             * to pass. Never arm a timer in the past; report the deferred
             * request as failed instead of letting the tamer wrap around. */
            if (e->timestamp <= time_tamer_read(module)) {
                record_retune2_failure(module);
                dequeue_retune(q, NULL);
                break;
            }

            /* Schedule the retune */
            e->state = ENTRY_STATE_SCHEDULED;
            tamer_schedule(module, e->timestamp);

            break;

        case ENTRY_STATE_SCHEDULED:

            /* Nothing to do.
             * Waiting for this entry to become ready */
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
                completion->frequency_hz = readback_frequency_hz;
                completion->scheduled_timestamp = e->timestamp;
                completion->nios_profile = e->nios_profile;
                /* Publish the slot sequence last, then advance the public
                 * count. A reader verifies sequence before trusting fields. */
                completion->sequence = completion_sequence;
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
            INCREMENT_ERROR_COUNT();
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
    uint16_t nios_profile;
    uint8_t rffe_profile;
    uint8_t port;
    uint8_t spdt;
    fastlock_profile *profile;
    fastlock_profile requested_profile;

    flags = NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS;

    nios_pkt_retune2_unpack(b->req, &module, &timestamp,
                            &nios_profile, &rffe_profile, &port, &spdt);

    start_time = time_tamer_read(module);

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
            requested_profile = *profile;
            requested_profile.profile_num = rffe_profile;
            requested_profile.port = port;
            requested_profile.spdt = spdt;
            if (timestamp == NIOS_PKT_RETUNE2_NOW) {
                /* Fire off this retune operation now */
                *profile = requested_profile;
                switch (module) {
                    case BLADERF_MODULE_RX:
                    case BLADERF_MODULE_TX:
                        /* Load the profile data into RFFE memory */
                        if (!profile_load(module, profile)) {
                            INCREMENT_ERROR_COUNT();
                            status = -1;
                            break;
                        }

                        /* Activate the fast lock profile for this retune */
                        if (profile_activate(module, profile, NULL)) {
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
            } else if (timestamp <= start_time) {
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

    end_time = time_tamer_read(module);
    duration = end_time - start_time;

    if (status != 0) {
        flags &= ~(NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS);
    }

    nios_pkt_retune2_resp_pack(b->resp, duration, flags);
}
