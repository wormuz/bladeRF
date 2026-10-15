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
#include "rfic_frequency_policy.h"

#ifdef BLADERF_NIOS_LIBAD936X
void rfic_invalidate_frequency(bladerf_module module);
bool rfic_read_frequency_live(bladerf_module module, uint8_t profile,
                              uint64_t *frequency_hz,
                              uint8_t *failure_stage,
                              bool *txq_pending);
bool rfic_start_tx_quad_calibration_for_retune(bool *pending,
                                                uint8_t *failure_stage);
int32_t rfic_poll_tx_quad_calibration_for_retune(uint8_t *failure_stage);
bool rfic_tx_quad_retune_owner(void);
bool rfic_frequency_state_available(void);
bool rfic_frequency_peer_unknown(bladerf_module target);
bool rfic_command_state_faulted(void);
bool rfic_phy_present(void);
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
    ENTRY_STATE_SCHEDULE_PENDING, /* Arm issued; await FPGA schedule ACK */
    ENTRY_STATE_SCHEDULED,    /* We've scheduled the timer interrupt for
                               * this entry and are awaiting the ISR */
    ENTRY_STATE_CANCEL_PENDING, /* Timer failure reported; await cancel ACK */
    ENTRY_STATE_PROFILE_LOAD_PENDING, /* Cooperative fastlock SPI upload */
    ENTRY_STATE_PROFILE_LOAD_FAILED, /* Deferred upload failure is terminal */
    ENTRY_STATE_READY,        /* The timer interrupt has fired - we should
                               * handle this retune */
    ENTRY_STATE_PLL_PENDING,  /* Fastlock recalled; poll RFPLL lock */
    ENTRY_STATE_TXQ_PENDING,  /* TX profile active; await TXQ before success */
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
    uint64_t readback_frequency_hz;
    uint32_t session_generation;
    uint16_t request_token;
    struct pkt_buf *deferred_response_pkt;
    bool immediate;
    bool cancel_recovery_seen;
    bool cancel_request_pending;
    bool failure_reported;
    uint8_t cancel_ack_before;
};

static struct queue {
    /* The timestamp ISR reads count/rem_idx while the main loop enqueues,
     * dequeues, and clears this queue. Keep both accesses observable across
     * the asynchronous interrupt boundary. */
    volatile uint8_t count; /* Total number of items in the queue */
    uint8_t ins_idx;    /* Insertion index */
    volatile uint8_t rem_idx; /* Removal index, also read by the ISR */
    volatile bool clear_requested;
    bool clear_complete;
    volatile bool deferred_clear_requested;
    bool clear_cancel_pending;
    bool clear_cancel_recovery_seen;
    uint8_t clear_cancel_ack_before;

    struct queue_entry entries[RETUNE2_QUEUE_MAX];
} rx_queue, tx_queue;
static bool profile_preload_tx_first;

bool pkt_retune2_defer_clear(bladerf_module module)
{
    struct queue *q;

    if (module == BLADERF_MODULE_RX) {
        q = &rx_queue;
    } else if (module == BLADERF_MODULE_TX) {
        q = &tx_queue;
    } else {
        return false;
    }

    /* The restricted service hook can run inside an ADI SPI call. Publish
     * intent only; the ordinary packet/queue worker performs cancellation
     * once that complete call stack has unwound. */
    q->deferred_clear_requested = true;
    return true;
}

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
    uint32_t session_generation;
    uint16_t request_token;
};

struct retune2_failure_completion {
    volatile uint32_t sequence;
    uint64_t scheduled_timestamp;
    uint16_t nios_profile;
    uint8_t failure_stage;
    uint32_t session_generation;
    uint16_t request_token;
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
    uint16_t nios_profile, uint32_t session_generation,
    uint16_t request_token)
{
    completion->frequency_hz = frequency_hz;
    completion->scheduled_timestamp = scheduled_timestamp;
    completion->nios_profile = nios_profile;
    completion->session_generation = session_generation;
    completion->request_token = request_token;
    __asm__ __volatile__("" ::: "memory");
    completion->sequence = sequence;
}

static struct retune2_completion
    rx_retune2_completions[RETUNE2_QUEUE_MAX];
static struct retune2_completion
    tx_retune2_completions[RETUNE2_QUEUE_MAX];
static struct retune2_failure_completion
    rx_retune2_failure_completions[RETUNE2_QUEUE_MAX];
static struct retune2_failure_completion
    tx_retune2_failure_completions[RETUNE2_QUEUE_MAX];
static volatile uint32_t selected_completion_sequence;
static volatile bladerf_module selected_completion_module;
static volatile bool selected_completion_is_failure;

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
    selected_completion_is_failure = false;
    return true;
}

bool pkt_retune2_failure_completion_select(bladerf_module module,
                                           uint32_t sequence)
{
    if (module != BLADERF_MODULE_RX && module != BLADERF_MODULE_TX) {
        return false;
    }

    selected_completion_module = module;
    selected_completion_sequence = sequence;
    selected_completion_is_failure = true;
    return true;
}

uint32_t pkt_retune2_completion_word(uint8_t addr)
{
    struct retune2_completion *record;
    uint32_t sequence = selected_completion_sequence;

    if (addr > (selected_completion_is_failure ? 6 : 7)) {
        return 0;
    }
    if (selected_completion_is_failure) {
        struct retune2_failure_completion *failure =
            selected_completion_module == BLADERF_MODULE_RX
                ? &rx_retune2_failure_completions[
                      (sequence - 1u) & (RETUNE2_QUEUE_MAX - 1u)]
                : &tx_retune2_failure_completions[
                      (sequence - 1u) & (RETUNE2_QUEUE_MAX - 1u)];
        switch (addr) {
            case 0:
                return failure->sequence;
            case 1:
                return (uint32_t)failure->scheduled_timestamp;
            case 2:
                return (uint32_t)(failure->scheduled_timestamp >> 32);
            case 3:
                return failure->nios_profile;
            case 4:
                return failure->failure_stage;
            case 5:
                return failure->session_generation;
            case 6:
                return failure->request_token;
            default:
                return 0;
        }
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
        case 6:
            return record->session_generation;
        case 7:
            return record->request_token;
        default:
            return 0;
    }
}

static void record_retune2_failure(bladerf_module module,
                                   struct queue_entry *entry,
                                   uint8_t failure_stage)
{
    struct retune2_failure_completion *completion;
    uint32_t sequence;

    if (module == BLADERF_MODULE_RX) {
        sequence = rx_retune2_failure_count + 1u;
        completion = &rx_retune2_failure_completions[
            (sequence - 1u) & (RETUNE2_QUEUE_MAX - 1u)];
    } else if (module == BLADERF_MODULE_TX) {
        sequence = tx_retune2_failure_count + 1u;
        completion = &tx_retune2_failure_completions[
            (sequence - 1u) & (RETUNE2_QUEUE_MAX - 1u)];
    } else {
        return;
    }

    completion->sequence = ~sequence;
    __asm__ __volatile__("" ::: "memory");
    completion->scheduled_timestamp = entry != NULL ? entry->timestamp : 0;
    completion->nios_profile = entry != NULL ? entry->nios_profile : UINT16_MAX;
    completion->failure_stage = failure_stage;
    completion->session_generation = entry != NULL
        ? entry->session_generation : 0;
    completion->request_token = entry != NULL ? entry->request_token : 0;
    __asm__ __volatile__("" ::: "memory");
    completion->sequence = sequence;
    if (module == BLADERF_MODULE_RX) {
        rx_retune2_failure_count = sequence;
    } else {
        tx_retune2_failure_count = sequence;
    }
    INCREMENT_ERROR_COUNT();
}

static void record_retune2_entry_failure(bladerf_module module,
                                         struct queue_entry *entry,
                                         uint8_t failure_stage)
{
    if (entry != NULL && !entry->failure_reported) {
        record_retune2_failure(module, entry, failure_stage);
        entry->failure_reported = true;
        if (entry->deferred_response_pkt != NULL) {
            struct pkt_buf *pkt = entry->deferred_response_pkt;
            nios_pkt_retune2_resp_pack(
                pkt->resp, 0, 0, failure_stage, entry->request_token,
                entry->session_generation);
            command_uart_response_mark_prepared(&pkt->response_prepared);
            pkt->ready = true;
            pkt->deferred_work_active_at_accept = true;
            entry->deferred_response_pkt = NULL;
        }
    }
}

static bool retune2_profile_indices_valid(uint16_t nios_profile,
                                          uint8_t rffe_profile)
{
    return nios_profile < NUM_BBP_FASTLOCK_PROFILES &&
           rffe_profile < NUM_RFFE_FASTLOCK_PROFILES;
}

/* Returns queue size after enqueue operation, or QUEUE_FULL if we could
 * not enqueue the requested item */
static inline uint8_t enqueue_retune_with_identity(
    struct queue *q, fastlock_profile *p, uint16_t nios_profile,
    uint64_t timestamp, uint32_t session_generation, uint16_t request_token)
{
    uint8_t ret;

    if (q->clear_requested) {
        return QUEUE_FULL;
    }
    q->clear_complete = false;
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
    q->entries[q->ins_idx].session_generation = session_generation;
    q->entries[q->ins_idx].request_token = request_token;
    q->entries[q->ins_idx].deferred_response_pkt = NULL;
    q->entries[q->ins_idx].immediate = false;
    q->entries[q->ins_idx].cancel_recovery_seen = false;
    q->entries[q->ins_idx].cancel_request_pending = false;
    q->entries[q->ins_idx].failure_reported = false;

    q->ins_idx = (q->ins_idx + 1) & (RETUNE2_QUEUE_MAX - 1);

    q->count++;
    ret = q->count;

    return ret;
}

/* Test and legacy internal callers without a host identity use zero fields. */
static inline uint8_t enqueue_retune(struct queue *q, fastlock_profile *p,
                                     uint16_t nios_profile,
                                     uint64_t timestamp)
{
    return enqueue_retune_with_identity(q, p, nios_profile, timestamp, 0, 0);
}

static inline uint8_t enqueue_retune_now_with_identity(
    struct queue *q, fastlock_profile *p, uint16_t nios_profile,
    uint32_t session_generation, uint16_t request_token)
{
    struct queue_entry *entry;

    if (q == NULL || p == NULL || q->count != 0 || q->clear_requested) {
        return QUEUE_FULL;
    }

    entry = &q->entries[q->ins_idx];
    entry->profile = *p;
    entry->nios_profile = nios_profile;
    entry->timestamp = NIOS_PKT_RETUNE2_NOW;
    entry->readback_frequency_hz = 0;
    entry->session_generation = session_generation;
    entry->request_token = request_token;
    entry->deferred_response_pkt = NULL;
    entry->immediate = true;
    entry->cancel_recovery_seen = false;
    entry->cancel_request_pending = false;
    entry->failure_reported = false;
    entry->cancel_ack_before = 0;
    entry->state = ENTRY_STATE_READY;
    q->clear_complete = false;
    q->ins_idx = (q->ins_idx + 1) & (RETUNE2_QUEUE_MAX - 1);
    ++q->count;
    return q->count;
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

static struct queue_entry *find_profile_load_pending(struct queue *q)
{
    uint32_t i;

    if (q == NULL) {
        return NULL;
    }
    for (i = 0; i < q->count; i++) {
        struct queue_entry *entry = peek_next_retune_offset(q, i);
        if (entry != NULL &&
            entry->state == ENTRY_STATE_PROFILE_LOAD_PENDING) {
            return entry;
        }
    }
    return NULL;
}

static bool profile_slot_compare_owned(const struct queue_entry *entry)
{
    return entry != NULL &&
           (entry->state == ENTRY_STATE_SCHEDULE_PENDING ||
            entry->state == ENTRY_STATE_SCHEDULED ||
            entry->state == ENTRY_STATE_CANCEL_PENDING ||
            entry->state == ENTRY_STATE_PROFILE_LOAD_PENDING ||
            entry->state == ENTRY_STATE_READY ||
            entry->state == ENTRY_STATE_PLL_PENDING ||
            entry->state == ENTRY_STATE_TXQ_PENDING);
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
    q->clear_requested = false;
    q->clear_complete = false;
    q->deferred_clear_requested = false;
    q->clear_cancel_pending = false;
    q->clear_cancel_recovery_seen = false;
}

static void cancel_nonhead_profile_upload(struct queue *q,
                                          bladerf_module module,
                                          struct queue_entry *head)
{
    struct queue_entry *staged = find_profile_load_pending(q);

    if (staged != NULL && staged != head) {
        (void)adi_fastlock_load_cancel(module);
        record_retune2_entry_failure(
            module, staged, NIOS_PKT_RETUNE2_FAILURE_CANCELLED);
        staged->state = ENTRY_STATE_PROFILE_LOAD_FAILED;
    }
}

/* Queue clearing may need a timestamp-domain cancellation. Start it
 * asynchronously and report pending/failure to the caller; the worker owns
 * the queue until the matching cancel toggle arrives. */
static bool clear_queue_async(struct queue *q, bladerf_module module,
                              bool *cancel_failed)
{
    struct queue_entry *e = peek_next_retune(q);

    *cancel_failed = false;

    if (q->clear_complete) {
        q->clear_complete = false;
        return true;
    }
    if (q->clear_requested) {
        if (e != NULL && e->state == ENTRY_STATE_PROFILE_LOAD_FAILED) {
            /* A failed upload has no armed time-tamer compare. Its terminal
             * RFIC failure was already published, so clear the remaining
             * unarmed queue directly instead of issuing a spurious timer
             * cancel that could itself remain pending forever. */
            reset_queue(q);
            q->clear_complete = true;
            return true;
        }
        cancel_nonhead_profile_upload(q, module, e);
        if (e != NULL && (e->state == ENTRY_STATE_TXQ_PENDING ||
                          e->state == ENTRY_STATE_PLL_PENDING)) {
            /* The compare already fired and AD9361 calibration owns RFIC.
             * Let its exact request reach a terminal result before clearing
             * the queue or dropping the retained request identity. */
            return false;
        }
        if (e != NULL && e->immediate && e->state == ENTRY_STATE_READY) {
            record_retune2_entry_failure(
                module, e, NIOS_PKT_RETUNE2_FAILURE_CANCELLED);
            reset_queue(q);
            q->clear_complete = true;
            return true;
        }
        if (e != NULL && e->state == ENTRY_STATE_PROFILE_LOAD_PENDING) {
            (void)adi_fastlock_load_cancel(module);
            record_retune2_entry_failure(
                module, e, NIOS_PKT_RETUNE2_FAILURE_CANCELLED);
            reset_queue(q);
            q->clear_complete = true;
            return true;
        }
        if (tamer_clock_stalled(module)) {
            *cancel_failed = true;
        } else if (tamer_cancel_failed(module)) {
            *cancel_failed = true;
            if (e == NULL &&
                tamer_cancel_request(module, &q->clear_cancel_ack_before)) {
                q->clear_cancel_pending = true;
                q->clear_cancel_recovery_seen = true;
            } else if (e != NULL &&
                       tamer_cancel_request(module, &e->cancel_ack_before)) {
                e->cancel_request_pending = true;
                e->cancel_recovery_seen = true;
            }
        } else if (e == NULL && !q->clear_cancel_pending &&
                   q->clear_cancel_recovery_seen &&
                   tamer_cancel_request(
                       module, &q->clear_cancel_ack_before)) {
            q->clear_cancel_pending = true;
        } else if (e != NULL && e->state == ENTRY_STATE_CANCEL_PENDING &&
                   !e->cancel_request_pending && e->cancel_recovery_seen &&
                   tamer_cancel_request(module, &e->cancel_ack_before)) {
            e->cancel_request_pending = true;
        }
        return false;
    }

    q->clear_requested = true;
    if (e != NULL && e->state == ENTRY_STATE_PROFILE_LOAD_FAILED) {
        reset_queue(q);
        q->clear_complete = true;
        return true;
    }
    cancel_nonhead_profile_upload(q, module, e);
    if (e != NULL && (e->state == ENTRY_STATE_TXQ_PENDING ||
                      e->state == ENTRY_STATE_PLL_PENDING)) {
        return false;
    }
    if (e != NULL && e->immediate && e->state == ENTRY_STATE_READY) {
        record_retune2_entry_failure(
            module, e, NIOS_PKT_RETUNE2_FAILURE_CANCELLED);
        reset_queue(q);
        q->clear_complete = true;
        return true;
    }
    if (e != NULL && e->state == ENTRY_STATE_PROFILE_LOAD_PENDING) {
        (void)adi_fastlock_load_cancel(module);
        record_retune2_entry_failure(
            module, e, NIOS_PKT_RETUNE2_FAILURE_CANCELLED);
        reset_queue(q);
        q->clear_complete = true;
        return true;
    }
    if (e == NULL) {
        q->clear_cancel_pending = false;
        q->clear_cancel_recovery_seen = false;
        if (tamer_clock_stalled(module)) {
            *cancel_failed = true;
        } else if (tamer_cancel_failed(module)) {
            *cancel_failed = true;
            if (tamer_cancel_request(module, &q->clear_cancel_ack_before)) {
                q->clear_cancel_pending = true;
                q->clear_cancel_recovery_seen = true;
            }
        } else if (tamer_cancel_request(
                       module, &q->clear_cancel_ack_before)) {
            q->clear_cancel_pending = true;
            q->clear_cancel_recovery_seen = true;
        }
        return false;
    }

    if (e->state != ENTRY_STATE_CANCEL_PENDING) {
        e->state = ENTRY_STATE_CANCEL_PENDING;
        e->cancel_recovery_seen = false;
        e->cancel_request_pending = false;
    }

    if (tamer_clock_stalled(module)) {
        *cancel_failed = true;
        return false;
    }
    if (tamer_cancel_failed(module)) {
        *cancel_failed = true;
        if (tamer_cancel_request(module, &e->cancel_ack_before)) {
            e->cancel_request_pending = true;
            e->cancel_recovery_seen = true;
        }
        return false;
    }

    if (e->cancel_request_pending) {
        return false;
    }

    e->cancel_recovery_seen = true;
    if (tamer_cancel_request(module, &e->cancel_ack_before)) {
        e->cancel_request_pending = true;
    } else {
        e->cancel_recovery_seen = false;
    }

    return false;
}

#ifdef BLADERF_NIOS_LIBAD936X
/* A queued compare is only a timestamp event; NIOS performs the actual RFIC
 * recall after observing it. On a latched fault, fail every accepted item and
 * retain ownership of the active compare until the existing cancel ACK path
 * retires it. NEW/READY entries have no armed compare and can be discarded. */
static void fail_queue_for_rfic_fault(struct queue *q,
                                      bladerf_module module)
{
    struct queue_entry *head;
    bool cancel_failed;
    uint32_t i;

    if (q == NULL || q->clear_requested) {
        return;
    }

    for (i = 0; i < q->count; i++) {
        record_retune2_entry_failure(
            module,
            &q->entries[(q->rem_idx + i) & (RETUNE2_QUEUE_MAX - 1)],
            NIOS_PKT_RETUNE2_FAILURE_RFIC_FAULTED);
    }

    head = peek_next_retune(q);
    if (head == NULL) {
        return;
    }

    cancel_nonhead_profile_upload(q, module, head);

    if (head->state == ENTRY_STATE_PROFILE_LOAD_PENDING) {
        (void)adi_fastlock_load_cancel(module);
        reset_queue(q);
        return;
    }

    if (head->state == ENTRY_STATE_TXQ_PENDING ||
        head->state == ENTRY_STATE_PLL_PENDING) {
        /* The active TXQ/PLL poll owns the queue entry and must reach a
         * terminal hardware observation before fault retirement. */
        return;
    }

    if (head->state == ENTRY_STATE_SCHEDULE_PENDING ||
        head->state == ENTRY_STATE_SCHEDULED ||
        head->state == ENTRY_STATE_CANCEL_PENDING) {
        (void)clear_queue_async(q, module, &cancel_failed);
    } else {
        /* NEW has not armed a comparator; READY means its compare has fired
         * and no RFIC activation is implicit in the FPGA event itself. */
        reset_queue(q);
    }
}
#endif

static inline bool profile_load(bladerf_module module, fastlock_profile *p)
{
    if (p == NULL) {
        return false;
    }
#ifdef BLADERF_NIOS_LIBAD936X
    /* Do not continue profile SPI programming after a board-level RFIC fault.
     * A partial upload can mutate the profile slot even when it never becomes
     * the active LO. */
    if (rfic_command_state_faulted()) {
        return false;
    }
#endif

    return adi_fastlock_load(module, p);
}

static inline bool profile_activate(bladerf_module module, fastlock_profile *p,
                                    bool immediate,
                                    uint64_t *readback_frequency_hz,
                                    uint8_t *failure_stage,
                                    bool allow_txq_yield,
                                    bool recall_started,
                                    bool *pll_pending,
                                    bool *txq_pending)
{
    bool pll_locked = false;
#ifdef BLADERF_NIOS_LIBAD936X
    bool frequency_state_available = rfic_frequency_state_available();
    bool phy_present = rfic_phy_present();
    bool rfic_faulted = rfic_command_state_faulted();
    bool host_owned_fastlock = rfic_fastlock_host_owned_without_phy(
        immediate, phy_present, rfic_faulted);
#endif

    if (failure_stage != NULL) {
        *failure_stage = NIOS_PKT_RETUNE2_FAILURE_NONE;
    }
    if (txq_pending != NULL) {
        *txq_pending = false;
    }
    if (pll_pending != NULL) {
        *pll_pending = false;
    }
    /* Once recall has started, always drain that hardware owner to a
     * terminal lock observation before evaluating any later precondition.
     * Otherwise a newly latched board fault could strand the global owner. */
    if (recall_started) {
        int32_t lock_status = adi_fastlock_recall_poll(module, failure_stage);
        if (lock_status == 1) {
            if (pll_pending != NULL)
                *pll_pending = true;
            return true;
        }
        if (lock_status < 0)
            return false;
        pll_locked = true;
    }
    if (p == NULL) {
        if (failure_stage != NULL) {
            *failure_stage = NIOS_PKT_RETUNE2_FAILURE_INVALID_REQUEST;
        }
        return false;
    }

#ifdef BLADERF_NIOS_LIBAD936X
    /* A faulted-but-present PHY is not HOST tuning mode. In particular, do
     * not let the host-owned fastlock exception bypass this failure state. */
    if (rfic_faulted) {
        if (failure_stage != NULL) {
            *failure_stage = NIOS_PKT_RETUNE2_FAILURE_RFIC_FAULTED;
        }
        return false;
    }

    /* Dependent-FDD PLL programming chooses a VCO table using the peer LO.
     * Do not activate a saved profile while that frequency is invalidated by
     * an earlier partial/failed tune. In HOST mode NIOS has no PHY; only an
     * immediate recall is allowed there because the host verifies the LO
     * synchronously before certifying its transition. */
    if (!frequency_state_available && !host_owned_fastlock) {
        if (failure_stage != NULL) {
            *failure_stage = NIOS_PKT_RETUNE2_FAILURE_PEER_FREQUENCY_UNKNOWN;
        }
        return false;
    }
    if (frequency_state_available && rfic_frequency_peer_unknown(module)) {
        if (failure_stage != NULL) {
            *failure_stage = NIOS_PKT_RETUNE2_FAILURE_PEER_FREQUENCY_UNKNOWN;
        }
        return false;
    }
    if (frequency_state_available) {
        /* Invalidate current frequency knowledge */
        rfic_invalidate_frequency(module);
    }
#endif  // BLADERF_NIOS_LIBAD936X

    /* Queue-worker transitions own the RFIC across this autonomous wait.
     * Tokened NOW enters this worker after its acceptance ACK; tokenless NOW
     * retains its legacy synchronous response, but is also completed by this
     * worker before that response is published. */
#ifdef BLADERF_NIOS_LIBAD936X
    /* The retained AD936x worker is mandatory in this build. Never let a
     * call-site flag route an xA4 NIOS recall through the blocking helper. */
    (void)allow_txq_yield;
    if (!pll_locked) {
        if (!adi_fastlock_recall_start(module, p, failure_stage))
            return false;
        if (pll_pending != NULL)
            *pll_pending = true;
        return true;
    }
#else
    if (allow_txq_yield) {
        if (!pll_locked) {
            if (!adi_fastlock_recall_start(module, p, failure_stage))
                return false;
            if (pll_pending != NULL)
                *pll_pending = true;
            return true;
        }
    } else if (!adi_fastlock_recall(module, p, failure_stage)) {
        return false;
    }
#endif

    /* Adjust the RFFE port */
    if (!adi_rfport_select(p)) {
        if (failure_stage != NULL) {
            *failure_stage = NIOS_PKT_RETUNE2_FAILURE_RF_PORT_SELECT;
        }
        return false;
    }

    /* Adjust the RF switches */
    if (!adi_rfspdt_select(module, p)) {
        if (failure_stage != NULL) {
            *failure_stage = NIOS_PKT_RETUNE2_FAILURE_SPDT_SELECT;
        }
        return false;
    }
#ifdef BLADERF_NIOS_LIBAD936X
    if (readback_frequency_hz != NULL &&
        !host_owned_fastlock &&
        !rfic_read_frequency_live(module, p->profile_num,
                                  readback_frequency_hz, failure_stage,
                                  txq_pending)) {
        if (failure_stage != NULL) {
            if (*failure_stage == NIOS_PKT_RETUNE2_FAILURE_NONE) {
                *failure_stage = NIOS_PKT_RETUNE2_FAILURE_LO_READBACK;
            }
        }
        return false;
    }
    if (readback_frequency_hz != NULL && host_owned_fastlock) {
        *readback_frequency_hz = 0;
    }
#else
    if (readback_frequency_hz != NULL) {
        *readback_frequency_hz = 0;
    }
#endif
    return true;
}

static void perform_profile_load_step(struct queue *q,
                                      bladerf_module module,
                                      struct queue_entry *entry)
{
    int32_t load_status;

    if (q == NULL || entry == NULL) {
        return;
    }

    load_status = adi_fastlock_load_step(module);
    if (load_status < 0) {
        /* Preserve the actual SPI failure even if CLEAR arrived reentrantly
         * from the safe-point hook during this transfer. The slot remains
         * nonresident, and the result must not be downgraded to cancellation. */
        record_retune2_entry_failure(
            module, entry, NIOS_PKT_RETUNE2_FAILURE_PROFILE_LOAD);
        entry->state = ENTRY_STATE_PROFILE_LOAD_FAILED;
        if (q->deferred_clear_requested || q->clear_requested) {
            bool cancel_failed;
            q->deferred_clear_requested = false;
            (void)clear_queue_async(q, module, &cancel_failed);
        }
        return;
    }
    if (q->deferred_clear_requested) {
        bool cancel_failed;
        q->deferred_clear_requested = false;
        (void)clear_queue_async(q, module, &cancel_failed);
        return;
    }
    if (q->clear_requested) {
        bool cancel_failed;
        (void)clear_queue_async(q, module, &cancel_failed);
        return;
    }
    if (load_status == 0) {
        return;
    }
    adi_fastlock_profile_cache_commit(module, entry->nios_profile,
                                      &entry->profile);
    entry->state = entry->immediate ? ENTRY_STATE_READY : ENTRY_STATE_NEW;
}

static void publish_retune2_success(bladerf_module module,
                                    struct queue_entry *entry,
                                    uint64_t readback_frequency_hz)
{
    uint32_t sequence;
    struct retune2_completion *completion;

    if (module != BLADERF_MODULE_RX && module != BLADERF_MODULE_TX) {
        return;
    }
    sequence = pkt_retune2_success_count(module) + 1u;
    completion = module == BLADERF_MODULE_RX
        ? &rx_retune2_completions[(sequence - 1u) & (RETUNE2_QUEUE_MAX - 1u)]
        : &tx_retune2_completions[(sequence - 1u) & (RETUNE2_QUEUE_MAX - 1u)];
    /* Invalidate the old ring slot before replacing its payload. */
    retune2_completion_begin_write(completion, sequence);
    retune2_completion_publish(completion, sequence, readback_frequency_hz,
                               entry->timestamp, entry->nios_profile,
                               entry->session_generation,
                               entry->request_token);
    if (module == BLADERF_MODULE_RX) {
        rx_retune2_success_count = sequence;
    } else {
        tx_retune2_success_count = sequence;
    }
    if (entry->deferred_response_pkt != NULL) {
        struct pkt_buf *pkt = entry->deferred_response_pkt;
        nios_pkt_retune2_resp_pack(
            pkt->resp, 0,
            NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS |
                NIOS_PKT_RETUNE2_RESP_FLAG_TSVTUNE_VALID,
            NIOS_PKT_RETUNE2_FAILURE_NONE, entry->request_token,
            entry->session_generation);
        command_uart_response_mark_prepared(&pkt->response_prepared);
        pkt->ready = true;
        pkt->deferred_work_active_at_accept = true;
        entry->deferred_response_pkt = NULL;
    }
}

static inline void retune_isr(struct queue *q, bladerf_module module)
{
    struct queue_entry *e;

    /* Queue clear owns the active comparator until its timestamp-domain
     * cancel ACK arrives; do not turn a racing IRQ into a READY head. */
    if (q->clear_requested) {
        return;
    }

    e = peek_next_retune(q);
    if (e != NULL) {
        if (e->state == ENTRY_STATE_SCHEDULE_PENDING) {
            if (tamer_compare_fired(module)) {
                e->state = ENTRY_STATE_READY;
            }
        } else if (e->state == ENTRY_STATE_SCHEDULED) {
            /* An IRQ can be left pending across a failed cancel and a later
             * queue replacement. Only the sticky FPGA compare event tied to
             * the currently armed schedule may release this entry. */
            if (tamer_compare_fired(module)) {
                e->state = ENTRY_STATE_READY;
            } else {
                INCREMENT_ERROR_COUNT();
            }
        } else if (e->state != ENTRY_STATE_READY &&
                   e->state != ENTRY_STATE_PLL_PENDING &&
                   e->state != ENTRY_STATE_TXQ_PENDING) {
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
    profile_preload_tx_first = false;
    rx_retune2_failure_count = 0;
    tx_retune2_failure_count = 0;
    rx_retune2_success_count = 0;
    tx_retune2_success_count = 0;
    memset(rx_retune2_completions, 0, sizeof(rx_retune2_completions));
    memset(tx_retune2_completions, 0, sizeof(tx_retune2_completions));
    memset(rx_retune2_failure_completions, 0,
           sizeof(rx_retune2_failure_completions));
    memset(tx_retune2_failure_completions, 0,
           sizeof(tx_retune2_failure_completions));
    selected_completion_sequence = 0;
    selected_completion_module = BLADERF_MODULE_RX;
    selected_completion_is_failure = false;

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
        if (q->clear_requested) {
            if (q->clear_cancel_pending) {
                if (tamer_clock_stalled(module)) {
                    /* The tamer can reset its cancel FSM on a clock-domain
                     * reset before the toggle is observed. Reissue after
                     * recovery instead of waiting forever on a lost ACK. */
                    q->clear_cancel_pending = false;
                    q->clear_cancel_recovery_seen = false;
                } else if (tamer_cancel_acknowledged(
                               module, q->clear_cancel_ack_before)) {
                    q->clear_cancel_pending = false;
                    q->clear_requested = false;
                    q->clear_complete = true;
                } else if (tamer_cancel_failed(module)) {
                    q->clear_cancel_pending = false;
                    q->clear_cancel_recovery_seen = true;
                }
            } else if (tamer_clock_stalled(module)) {
                q->clear_cancel_recovery_seen = false;
            } else if (!q->clear_cancel_recovery_seen) {
                q->clear_cancel_recovery_seen = true;
                if (tamer_cancel_request(
                        module, &q->clear_cancel_ack_before)) {
                    q->clear_cancel_pending = true;
                } else {
                    q->clear_cancel_recovery_seen = false;
                }
            }
        }
        return;
    }

    switch (e->state) {
        case ENTRY_STATE_PROFILE_LOAD_PENDING: {
            perform_profile_load_step(q, module, e);
            break;
        }

        case ENTRY_STATE_PROFILE_LOAD_FAILED:
            dequeue_retune(q, NULL);
            break;

        case ENTRY_STATE_NEW: {
            uint64_t current_time;

            /* Every scheduled request, including legacy tokenless requests,
             * must stage through the retained cooperative uploader. The
             * packet callback only accepts/copies requests; it never owns a
             * multi-write AD9361 profile transaction. Immediate tokenless
             * NOW remains on its separate compatibility path. */
            {
                int32_t load_start = adi_fastlock_load_start(
                    module, &e->profile);
                if (load_start < 0) {
                    record_retune2_entry_failure(
                        module, e, NIOS_PKT_RETUNE2_FAILURE_PROFILE_LOAD);
                    dequeue_retune(q, NULL);
                    break;
                }
                if (load_start == 0) {
                    e->state = ENTRY_STATE_PROFILE_LOAD_PENDING;
                    break;
                }
            }

            /* Profile loading can take long enough for a future timestamp
             * to pass. Never arm a timer in the past; report the deferred
             * request as failed instead of letting the tamer wrap around. */
            if (!time_tamer_read(module, &current_time)) {
                record_retune2_entry_failure(
                    module, e, NIOS_PKT_RETUNE2_FAILURE_TIMER_READ);
                dequeue_retune(q, NULL);
                break;
            }
            if (!timestamp_is_strictly_after(e->timestamp, current_time)) {
                record_retune2_entry_failure(
                    module, e, NIOS_PKT_RETUNE2_FAILURE_INVALID_TIMESTAMP);
                dequeue_retune(q, NULL);
                break;
            }

            /* Request the compare; the worker observes its FPGA ACK without
             * blocking the NIOS command loop. Publish the ISR-recognizable
             * state before the MMIO arm can trigger a near-immediate compare. */
            e->state = ENTRY_STATE_SCHEDULE_PENDING;
            if (tamer_schedule_request(module, e->timestamp)) {
                if (tamer_schedule_failed(module)) {
                    record_retune2_entry_failure(
                        module, e, NIOS_PKT_RETUNE2_FAILURE_SCHEDULE);
                    e->state = ENTRY_STATE_CANCEL_PENDING;
                    e->cancel_recovery_seen = false;
                    e->cancel_request_pending = false;
                } else if (e->state == ENTRY_STATE_SCHEDULE_PENDING &&
                    tamer_schedule_acknowledged(module)) {
                    e->state = ENTRY_STATE_SCHEDULED;
                    /* The ISR can preempt between the state check and the
                     * assignment above. The FPGA event is sticky, so
                     * recheck it after publishing SCHEDULED to avoid
                     * overwriting an IRQ-delivered READY state. */
                    if (tamer_compare_fired(module)) {
                        e->state = ENTRY_STATE_READY;
                    }
                }
            } else {
                record_retune2_entry_failure(
                    module, e, NIOS_PKT_RETUNE2_FAILURE_SCHEDULE);
                e->state = ENTRY_STATE_CANCEL_PENDING;
                e->cancel_recovery_seen = false;
                e->cancel_request_pending = false;
            }

            break;
        }

        case ENTRY_STATE_SCHEDULE_PENDING:
            if (tamer_schedule_failed(module)) {
                record_retune2_entry_failure(
                    module, e, NIOS_PKT_RETUNE2_FAILURE_SCHEDULE);
                e->state = ENTRY_STATE_CANCEL_PENDING;
                e->cancel_recovery_seen = false;
                e->cancel_request_pending = false;
            } else if (tamer_schedule_acknowledged(module)) {
                if (e->state == ENTRY_STATE_SCHEDULE_PENDING) {
                    e->state = ENTRY_STATE_SCHEDULED;
                    if (tamer_compare_fired(module)) {
                        e->state = ENTRY_STATE_READY;
                    }
                }
            } else if (tamer_clock_stalled(module)) {
                record_retune2_entry_failure(
                    module, e, NIOS_PKT_RETUNE2_FAILURE_TIMER_STOPPED);
                e->state = ENTRY_STATE_CANCEL_PENDING;
                e->cancel_recovery_seen = false;
                e->cancel_request_pending = false;
            }
            break;

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
                record_retune2_entry_failure(
                    module, e, NIOS_PKT_RETUNE2_FAILURE_TIMER_STOPPED);
                e->state = ENTRY_STATE_CANCEL_PENDING;
                e->cancel_recovery_seen = false;
                e->cancel_request_pending = false;
                /* The watchdog has already proved the timestamp clock is
                 * stopped. Cancellation cannot cross into that domain yet;
                 * retain ownership and issue an asynchronous request after
                 * clock recovery. */
            }
            break;

        case ENTRY_STATE_CANCEL_PENDING:
            /* The failure was already published on entry. Never activate or
             * discard this entry until the time-tamer confirms disarm. Start
             * cancellation once, then poll its toggle acknowledgement on
             * later worker passes without blocking NIOS command servicing. */
            if (tamer_clock_stalled(module)) {
                e->cancel_recovery_seen = false;
                e->cancel_request_pending = false;
            } else if (e->cancel_request_pending) {
                if (tamer_cancel_acknowledged(module,
                                              e->cancel_ack_before)) {
                    if (q->clear_requested) {
                        reset_queue(q);
                        q->clear_complete = true;
                    } else {
                        dequeue_retune(q, NULL);
                    }
                } else if (tamer_cancel_failed(module)) {
                    e->cancel_request_pending = false;
                    e->cancel_recovery_seen = true;
                }
            } else if (!e->cancel_recovery_seen) {
                e->cancel_recovery_seen = true;
                if (tamer_cancel_request(module, &e->cancel_ack_before)) {
                    e->cancel_request_pending = true;
                } else {
                    e->cancel_recovery_seen = false;
                }
            }
            break;

        case ENTRY_STATE_TXQ_PENDING: {
            uint8_t failure_stage = NIOS_PKT_RETUNE2_FAILURE_NONE;
            int32_t poll_status = rfic_poll_tx_quad_calibration_for_retune(
                &failure_stage);

            if (poll_status == 1) {
                break;
            }
            if (poll_status < 0) {
                if (failure_stage == NIOS_PKT_RETUNE2_FAILURE_NONE) {
                    failure_stage =
                        NIOS_PKT_RETUNE2_FAILURE_TX_QUAD_CALIBRATION;
                }
                record_retune2_entry_failure(module, e, failure_stage);
            } else {
                publish_retune2_success(module, e,
                                        e->readback_frequency_hz);
            }
            if (q->clear_requested) {
                /* This compare already fired, so its TXQ continuation is the
                 * last live hardware owner. The remaining queue entries have
                 * not been armed; retire them with the completed clear. */
                reset_queue(q);
                q->clear_complete = true;
            } else {
                dequeue_retune(q, NULL);
            }
            break;
        }

        case ENTRY_STATE_READY: {
            uint64_t readback_frequency_hz;
            uint8_t failure_stage = NIOS_PKT_RETUNE2_FAILURE_NONE;
            bool pll_pending = false;
            bool txq_pending = false;

            /* Activate the fast lock profile for this retune */
            if (!profile_activate(module, &e->profile, e->immediate,
                                 &readback_frequency_hz, &failure_stage,
                                 true, false, &pll_pending, &txq_pending)) {
                if (failure_stage == NIOS_PKT_RETUNE2_FAILURE_NONE) {
                    failure_stage = NIOS_PKT_RETUNE2_FAILURE_INTERNAL;
                }
                record_retune2_entry_failure(module, e, failure_stage);
            } else if (pll_pending) {
                e->state = ENTRY_STATE_PLL_PENDING;
                break;
            } else if (txq_pending) {
                e->readback_frequency_hz = readback_frequency_hz;
                e->state = ENTRY_STATE_TXQ_PENDING;
                break;
            } else {
                publish_retune2_success(module, e,
                                        readback_frequency_hz);
            }

            /* Drop the item from the queue */
            dequeue_retune(q, NULL);

            break;
        }

        case ENTRY_STATE_PLL_PENDING: {
            uint64_t readback_frequency_hz = 0;
            uint8_t failure_stage = NIOS_PKT_RETUNE2_FAILURE_NONE;
            bool pll_pending = false;
            bool txq_pending = false;

            if (!profile_activate(module, &e->profile, e->immediate,
                                 &readback_frequency_hz, &failure_stage,
                                 true, true, &pll_pending, &txq_pending)) {
                if (failure_stage == NIOS_PKT_RETUNE2_FAILURE_NONE)
                    failure_stage = NIOS_PKT_RETUNE2_FAILURE_INTERNAL;
                record_retune2_entry_failure(module, e, failure_stage);
            } else if (pll_pending) {
                break;
            } else if (txq_pending) {
                e->readback_frequency_hz = readback_frequency_hz;
                e->state = ENTRY_STATE_TXQ_PENDING;
                break;
            } else {
                publish_retune2_success(module, e,
                                        readback_frequency_hz);
            }

            if (q->clear_requested) {
                reset_queue(q);
                q->clear_complete = true;
            } else {
                dequeue_retune(q, NULL);
            }
            break;
        }

        default:
            /* Preserve ownership of a possibly armed compare until the
             * hardware confirms cancellation. Merely logging an impossible
             * state leaves the accepted request at the queue head forever
             * and never tells the host that it failed. */
            record_retune2_failure(module, e,
                                   NIOS_PKT_RETUNE2_FAILURE_INTERNAL);
            e->state = ENTRY_STATE_CANCEL_PENDING;
            e->cancel_recovery_seen = false;
            e->cancel_request_pending = false;
            break;
    }
}

/* Stage queued profiles in the background while the queue head has an armed
 * compare. Live compare slots and already-staged NEW entries reserve their
 * physical slots; every upload starts at most one cooperative job. */
static bool profile_preload_next(struct queue *q, bladerf_module module)
{
    struct queue_entry *head;
    struct queue_entry *entry;
    uint8_t used = 0;
    uint32_t i;

    if (q == NULL || q->count < 2) {
        return false;
    }
    head = peek_next_retune(q);
    if (head == NULL ||
        (head->state != ENTRY_STATE_SCHEDULE_PENDING &&
         head->state != ENTRY_STATE_SCHEDULED)) {
        return false;
    }

    for (i = 0; i < q->count; i++) {
        entry = peek_next_retune_offset(q, i);
        if (profile_slot_compare_owned(entry) ||
            (entry != NULL && entry->state == ENTRY_STATE_NEW &&
             (entry->profile.state == FASTLOCK_STATE_RFFE ||
              entry->profile.state == FASTLOCK_STATE_BBP_RFFE))) {
            used |= (uint8_t)(1u << entry->profile.profile_num);
        }
    }

    for (i = 1; i < q->count; i++) {
        int32_t load_status;

        entry = peek_next_retune_offset(q, i);
        if (entry == NULL || entry->state != ENTRY_STATE_NEW ||
            (used & (1u << entry->profile.profile_num)) != 0) {
            continue;
        }

        load_status = adi_fastlock_load_start(module, &entry->profile);
        if (load_status < 0) {
            record_retune2_entry_failure(
                module, entry, NIOS_PKT_RETUNE2_FAILURE_PROFILE_LOAD);
            entry->state = ENTRY_STATE_PROFILE_LOAD_FAILED;
            return true;
        }
        if (load_status == 0) {
            entry->state = ENTRY_STATE_PROFILE_LOAD_PENDING;
            return true;
        }

        used |= (uint8_t)(1u << entry->profile.profile_num);
    }
    return false;
}

static void perform_profile_load_owner(struct queue *q,
                                       bladerf_module module)
{
    struct queue_entry *entry = find_profile_load_pending(q);
    struct queue_entry *head = peek_next_retune(q);

    if (entry == NULL) {
        (void)adi_fastlock_load_cancel(module);
        return;
    }

    if (q->deferred_clear_requested || q->clear_requested) {
        bool cancel_failed;
        q->deferred_clear_requested = false;
        (void)clear_queue_async(q, module, &cancel_failed);
        return;
    }

    /* If an earlier armed compare fired while a later profile was being
     * staged, abandon the partial upload at this completed-SPI boundary and
     * service the head RF transition first. The unloaded slot remains
     * invalid and that queued entry can restart staging afterward. */
    if (entry != head && head != NULL &&
        head->state != ENTRY_STATE_SCHEDULE_PENDING &&
        head->state != ENTRY_STATE_SCHEDULED) {
        (void)adi_fastlock_load_cancel(module);
        entry->state = ENTRY_STATE_NEW;
        perform_work(q, module);
        return;
    }

    perform_profile_load_step(q, module, entry);
}

void pkt_retune2_work(void)
{
#ifdef BLADERF_NIOS_LIBAD936X
    if (rx_queue.deferred_clear_requested) {
        bool cancel_failed;
        rx_queue.deferred_clear_requested = false;
        (void)clear_queue_async(&rx_queue, BLADERF_MODULE_RX,
                                &cancel_failed);
    }
    if (tx_queue.deferred_clear_requested) {
        bool cancel_failed;
        tx_queue.deferred_clear_requested = false;
        (void)clear_queue_async(&tx_queue, BLADERF_MODULE_TX,
                                &cancel_failed);
    }
    if (rfic_command_state_faulted()) {
        fail_queue_for_rfic_fault(&rx_queue, BLADERF_MODULE_RX);
        fail_queue_for_rfic_fault(&tx_queue, BLADERF_MODULE_TX);
    }
    if (adi_fastlock_load_active()) {
        bladerf_module const owner = adi_fastlock_load_owner();
        if (owner == BLADERF_MODULE_RX)
            perform_profile_load_owner(&rx_queue, owner);
        else if (owner == BLADERF_MODULE_TX)
            perform_profile_load_owner(&tx_queue, owner);
        return;
    }
    if (adi_fastlock_recall_active()) {
        bladerf_module const owner = adi_fastlock_recall_owner();
        if (owner == BLADERF_MODULE_RX)
            perform_work(&rx_queue, owner);
        else if (owner == BLADERF_MODULE_TX)
            perform_work(&tx_queue, owner);
        return;
    }
    if (rfic_tx_quad_retune_owner()) {
        perform_work(&tx_queue, BLADERF_MODULE_TX);
        return;
    }
#endif
    perform_work(&rx_queue, BLADERF_MODULE_RX);
#ifdef BLADERF_NIOS_LIBAD936X
    if (adi_fastlock_load_active() || adi_fastlock_recall_active())
        return;
#endif
    perform_work(&tx_queue, BLADERF_MODULE_TX);
#ifdef BLADERF_NIOS_LIBAD936X
    if (adi_fastlock_load_active() || adi_fastlock_recall_active())
        return;
    if (rfic_tx_quad_retune_owner())
        return;
#endif

    if (profile_preload_tx_first) {
        if (!profile_preload_next(&tx_queue, BLADERF_MODULE_TX)) {
            (void)profile_preload_next(&rx_queue, BLADERF_MODULE_RX);
        }
    } else {
        if (!profile_preload_next(&rx_queue, BLADERF_MODULE_RX)) {
            (void)profile_preload_next(&tx_queue, BLADERF_MODULE_TX);
        }
    }
    profile_preload_tx_first = !profile_preload_tx_first;
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
    bool cancel_failed = false;
    uint8_t failure_stage = NIOS_PKT_RETUNE2_FAILURE_NONE;
    uint16_t nios_profile;
    uint8_t rffe_profile;
    uint8_t port;
    uint8_t spdt;
    uint16_t request_token;
    fastlock_profile *profile;
    fastlock_profile requested_profile;

    flags = NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS;

    nios_pkt_retune2_unpack_token(
        b->req, &module, &timestamp, &nios_profile, &rffe_profile, &port,
        &spdt, &request_token);

    if (timestamp != NIOS_PKT_RETUNE_NOW &&
        timestamp != NIOS_PKT_RETUNE2_CLEAR_QUEUE) {
        if (!time_tamer_read(module, &start_time)) {
            INCREMENT_ERROR_COUNT();
            failure_stage = timestamp == NIOS_PKT_RETUNE2_NOW
                ? NIOS_PKT_RETUNE2_FAILURE_TIMER_READ
                : NIOS_PKT_RETUNE2_FAILURE_NONE;
            flags &= ~NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS;
            nios_pkt_retune2_resp_pack(b->resp, 0, flags, failure_stage,
                                       request_token, b->session_generation);
            return;
        }
        start_time_valid = true;
    }

    if (timestamp == NIOS_PKT_RETUNE2_CLEAR_QUEUE) {
        switch (module) {
            case BLADERF_MODULE_RX:
                if (clear_queue_async(&rx_queue, module, &cancel_failed)) {
                    status = 0;
                } else if (cancel_failed) {
                    flags |= NIOS_PKT_RETUNE2_RESP_FLAG_CANCEL_FAILED;
                    status = -1;
                } else {
                    flags |= NIOS_PKT_RETUNE2_RESP_FLAG_CANCEL_PENDING;
                    status = -1;
                }
                break;

            case BLADERF_MODULE_TX:
                if (clear_queue_async(&tx_queue, module, &cancel_failed)) {
                    status = 0;
                } else if (cancel_failed) {
                    flags |= NIOS_PKT_RETUNE2_RESP_FLAG_CANCEL_FAILED;
                    status = -1;
                } else {
                    flags |= NIOS_PKT_RETUNE2_RESP_FLAG_CANCEL_PENDING;
                    status = -1;
                }
                break;

            default:
                INCREMENT_ERROR_COUNT();
                status = -1;
        }
    }
#ifdef BLADERF_NIOS_LIBAD936X
    else if (timestamp != NIOS_PKT_RETUNE2_CLEAR_QUEUE &&
             rfic_command_state_faulted()) {
        /* Do not accept new immediate or scheduled work while recovery is
         * required. Queue-clear remains available so callers can explicitly
         * retire their pending compares. */
        INCREMENT_ERROR_COUNT();
        failure_stage = NIOS_PKT_RETUNE2_FAILURE_RFIC_FAULTED;
        status = -1;
    }
#endif
    else if (!nios_pkt_retune2_route_valid(module, port, spdt)) {
        /* Do not let malformed route selectors be truncated by the RFIC
         * register helpers into a different, apparently successful path. */
        INCREMENT_ERROR_COUNT();
        failure_stage = NIOS_PKT_RETUNE2_FAILURE_INVALID_REQUEST;
        status = -1;
    } else if (!retune2_profile_indices_valid(nios_profile, rffe_profile)) {
        /* Do not index the per-direction profile arrays or shift a
         * caller-controlled RFFE slot into the preload bitmask unchecked. */
        INCREMENT_ERROR_COUNT();
        failure_stage = NIOS_PKT_RETUNE2_FAILURE_INVALID_REQUEST;
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
            failure_stage = NIOS_PKT_RETUNE2_FAILURE_INVALID_REQUEST;
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
                /* NOW transitions use the retained worker so profile upload,
                 * RFPLL acquisition, and TXQ cannot monopolize packet
                 * dispatch. Tokened callers receive the existing
                 * acceptance/completion protocol; tokenless callers retain
                 * their synchronous response by keeping this packet buffer
                 * until the worker is terminal. */
#ifndef BLADERF_NIOS_LIBAD936X
                uint64_t readback_frequency_hz = 0;
#endif
                struct queue *q = module == BLADERF_MODULE_TX
                    ? &tx_queue : &rx_queue;
                struct queue *other_q = module == BLADERF_MODULE_TX
                    ? &rx_queue : &tx_queue;
                bool async_now = request_token != 0;
                bool deferred_legacy_now = false;
                bool queued_now = async_now || deferred_legacy_now;
                bool owner_busy = false;

#ifdef BLADERF_NIOS_LIBAD936X
                deferred_legacy_now = request_token == 0;
                queued_now = async_now || deferred_legacy_now;
                owner_busy = rfic_command_worker_active() ||
                             rfic_command_owns_device();
#endif

                /* An armed compare can recall its physical RFFE slot
                 * asynchronously. A NOW request must not replace that
                 * profile while the queued transaction still owns it. */
#ifdef BLADERF_NIOS_LIBAD936X
                if (adi_fastlock_recall_active()) {
                    /* A NOW request cannot mutate a second AD9361 profile
                     * while the scheduled recall owns the RFIC PLL path. */
                    INCREMENT_ERROR_COUNT();
                    failure_stage =
                        NIOS_PKT_RETUNE2_FAILURE_PROFILE_SLOT_BUSY;
                    status = -1;
                } else
#endif
                if (queued_now && (owner_busy || q->count != 0 ||
                                   other_q->count != 0)) {
                    INCREMENT_ERROR_COUNT();
                    failure_stage =
                        NIOS_PKT_RETUNE2_FAILURE_PROFILE_SLOT_BUSY;
                    status = -1;
                } else if (profile_slot_armed(q,
                                              requested_profile.profile_num)) {
                    INCREMENT_ERROR_COUNT();
                    failure_stage = NIOS_PKT_RETUNE2_FAILURE_PROFILE_SLOT_BUSY;
                    status = -1;
                } else {
                    switch (module) {
                        case BLADERF_MODULE_RX:
                        case BLADERF_MODULE_TX:
                            if (queued_now) {
                                uint8_t queue_size =
                                    enqueue_retune_now_with_identity(
                                        q, &requested_profile, nios_profile,
                                        b->session_generation, request_token);
                                if (queue_size == QUEUE_FULL) {
                                    INCREMENT_ERROR_COUNT();
                                    failure_stage =
                                        NIOS_PKT_RETUNE2_FAILURE_QUEUE_FULL;
                                    status = -1;
                                } else {
                                    struct queue_entry *entry =
                                        peek_next_retune(q);
                                    int32_t load_status;

                                    /* Queue storage now owns the request
                                     * bytes and terminal identity before the
                                     * profile port can be modified. */
                                    entry->state =
                                        ENTRY_STATE_PROFILE_LOAD_PENDING;
                                    if (deferred_legacy_now) {
                                        entry->deferred_response_pkt = b;
                                        b->response_deferred = true;
                                    }
                                    load_status = adi_fastlock_load_start(
                                        module, &entry->profile);
                                    if (load_status < 0) {
                                        if (deferred_legacy_now) {
                                            entry->deferred_response_pkt = NULL;
                                            b->response_deferred = false;
                                        }
                                        dequeue_retune(q, NULL);
                                        INCREMENT_ERROR_COUNT();
                                        failure_stage =
                                            NIOS_PKT_RETUNE2_FAILURE_PROFILE_LOAD;
                                        status = -1;
                                    } else {
                                        if (load_status > 0) {
                                            adi_fastlock_profile_cache_commit(
                                                module, entry->nios_profile,
                                                &entry->profile);
                                            entry->state = ENTRY_STATE_READY;
                                        }
                                        if (async_now) {
                                            flags |= NIOS_PKT_RETUNE2_RESP_FLAG_ASYNC_PENDING;
                                        }
                                        status = 0;
                                    }
                                }
                            } else {
#ifdef BLADERF_NIOS_LIBAD936X
                                /* Every request in this build must have
                                 * queued_now set above (tokened or retained
                                 * legacy response). Fail closed if that
                                 * admission invariant ever changes; do not
                                 * reintroduce a blocking recall/TXQ path. */
                                INCREMENT_ERROR_COUNT();
                                failure_stage =
                                    NIOS_PKT_RETUNE2_FAILURE_INTERNAL;
                                status = -1;
#else
                                /* Builds without the retained AD936x worker
                                 * keep the original synchronous path. */
                                if (!profile_load(module,
                                                 &requested_profile)) {
                                    INCREMENT_ERROR_COUNT();
                                    failure_stage =
                                        NIOS_PKT_RETUNE2_FAILURE_PROFILE_LOAD;
                                    status = -1;
                                    break;
                                }
                                *profile = requested_profile;
                                /* Legacy untracked NOW retains its
                                 * synchronous completion contract. */
                                if (profile_activate(
                                        module, profile, true,
                                        &readback_frequency_hz,
                                        &failure_stage, false, false,
                                        NULL, NULL)) {
                                    flags |=
                                        NIOS_PKT_RETUNE2_RESP_FLAG_TSVTUNE_VALID;
                                    status = 0;
                                } else {
                                    INCREMENT_ERROR_COUNT();
                                    status = -1;
                                }
#endif
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
                        queue_size = enqueue_retune_with_identity(
                            &rx_queue, &requested_profile, nios_profile,
                            timestamp, b->session_generation,
                            request_token);
                        break;

                    case BLADERF_MODULE_TX:
                        queue_size = enqueue_retune_with_identity(
                            &tx_queue, &requested_profile, nios_profile,
                            timestamp, b->session_generation,
                            request_token);
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

    if (timestamp != NIOS_PKT_RETUNE2_NOW) {
        failure_stage = NIOS_PKT_RETUNE2_FAILURE_NONE;
    }

    nios_pkt_retune2_resp_pack(b->resp, duration, flags, failure_stage,
                               request_token, b->session_generation);
    if ((flags & NIOS_PKT_RETUNE2_RESP_FLAG_ASYNC_PENDING) != 0) {
        /* Async ACK duration is unused; carry the atomic completion-ring
         * boundary in those eight bytes without changing the packet size. */
        nios_pkt_retune2_resp_set_async_baseline(
            b->resp, pkt_retune2_success_count(module),
            pkt_retune2_failure_count(module));
    }
}
