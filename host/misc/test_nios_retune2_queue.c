#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "../../hdl/fpga/platforms/common/bladerf/software/bladeRF_nios/src/pkt_retune2.c"

/* Most existing cases isolate timer/retirement behavior and model an
 * already-resident RFIC profile. Dedicated upload cases use
 * enqueue_retune_with_identity() directly and exercise the real worker path. */
static uint8_t enqueue_retune_resident(struct queue *queue,
                                       fastlock_profile *profile,
                                       uint16_t nios_profile,
                                       uint64_t timestamp)
{
    uint8_t result = enqueue_retune(queue, profile, nios_profile, timestamp);

    if (result != QUEUE_FULL && result != QUEUE_INVALID_TIMESTAMP) {
        queue->entries[(queue->ins_idx - 1) &
                       (RETUNE2_QUEUE_MAX - 1)].profile.state =
            FASTLOCK_STATE_RFFE;
    }
    return result;
}

fastlock_profile fastlocks_rx[NUM_BBP_FASTLOCK_PROFILES];
fastlock_profile fastlocks_tx[NUM_BBP_FASTLOCK_PROFILES];
static bool simulated_rfic_phy_present = true;
static bool simulated_rfic_faulted;
static bool simulate_txq_yield;
static bool simulated_txq_owner;
static unsigned simulated_txq_pending_polls;
static int32_t simulated_txq_terminal_status;
static bool simulated_fastlock_active;
static bool staged_profile_load_active;
static bool defer_clear_during_staged_step;
static bool simulated_rfic_worker_active;
static bool simulated_rfic_device_owned;

bool rfic_command_worker_active(void)
{
    return simulated_rfic_worker_active;
}

bool rfic_command_owns_device(void)
{
    return simulated_rfic_device_owned || simulated_fastlock_active ||
           staged_profile_load_active;
}

bool rfic_frequency_state_available(void)
{
    return simulated_rfic_phy_present && !simulated_rfic_faulted;
}

bool rfic_phy_present(void)
{
    return simulated_rfic_phy_present;
}

bool rfic_command_state_faulted(void)
{
    return simulated_rfic_faulted;
}

bool rfic_frequency_peer_unknown(bladerf_module module)
{
    (void)module;
    return false;
}

void rfic_invalidate_frequency(bladerf_module module)
{
    (void)module;
}

bool rfic_read_frequency_live(bladerf_module module, uint8_t profile,
                              uint64_t *frequency_hz,
                              uint8_t *failure_stage,
                              bool allow_txq_yield, bool *txq_pending)
{
    (void)profile;
    if (failure_stage != NULL) {
        *failure_stage = NIOS_PKT_RETUNE2_FAILURE_NONE;
    }
    if (frequency_hz != NULL) {
        *frequency_hz = module == BLADERF_MODULE_TX
            ? UINT64_C(1835000000) : UINT64_C(1840000000);
    }
    if (txq_pending != NULL) {
        *txq_pending = false;
    }
    if (module == BLADERF_MODULE_TX && allow_txq_yield &&
        simulate_txq_yield) {
        return rfic_start_tx_quad_calibration_for_retune(txq_pending,
                                                         failure_stage);
    }
    return simulated_rfic_phy_present && !simulated_rfic_faulted;
}

bool rfic_start_tx_quad_calibration_for_retune(bool *pending,
                                                uint8_t *failure_stage)
{
    if (failure_stage != NULL) {
        *failure_stage = NIOS_PKT_RETUNE2_FAILURE_NONE;
    }
    if (pending == NULL || simulated_rfic_faulted) {
        return false;
    }
    *pending = simulate_txq_yield;
    simulated_txq_owner = *pending;
    simulated_txq_pending_polls = 1;
    simulated_txq_terminal_status = 0;
    return true;
}

int32_t rfic_poll_tx_quad_calibration_for_retune(uint8_t *failure_stage)
{
    if (!simulated_txq_owner) {
        return -1;
    }
    if (simulated_txq_pending_polls > 0) {
        simulated_txq_pending_polls--;
        return 1;
    }
    simulated_txq_owner = false;
    if (simulated_txq_terminal_status < 0 && failure_stage != NULL) {
        *failure_stage = NIOS_PKT_RETUNE2_FAILURE_TX_QUAD_CALIBRATION;
    }
    return simulated_txq_terminal_status;
}

bool rfic_tx_quad_retune_owner(void)
{
    return simulated_txq_owner;
}

static unsigned load_count;
static unsigned staged_profile_write_count;
static bladerf_module staged_profile_load_module;
static fastlock_profile staged_profile_load_copy;
static int staged_profile_fail_after_write = -1;
static unsigned schedule_count;
static unsigned recall_count;
static bladerf_module simulated_fastlock_owner;
static unsigned simulated_fastlock_pending_polls;
static int32_t simulated_fastlock_terminal_status;
static unsigned port_count;
static unsigned spdt_count;
static bladerf_module scheduled_module;
static uint64_t scheduled_timestamp;
static uint64_t tamer_timestamp;
static bladerf_module recalled_module;
static fastlock_profile *recalled_profile;
static uint8_t recalled_profile_num;
static uint8_t recalled_port;
static uint8_t recalled_spdt;
static bool fail_profile_load;
static bool fail_profile_load_after_slot_write;
static bool fail_profile_recall;
static bool fail_port_select;
static bool fail_spdt_select;
static bool fail_tamer_schedule;
static bool fail_tamer_schedule_ack;
static bool fail_tamer_schedule_status;
static bool fire_during_schedule;
static bool fail_tamer_read;
static bool fail_tamer_cancel;
static bool fail_tamer_cancel_status;
static unsigned cancel_count;
static bool compare_fired_rx;
static bool compare_fired_tx;
static bool simulated_tamer_clock_stalled;
static bool track_rffe_fastlock_slots;
static fastlock_profile simulated_rffe_rx[NUM_RFFE_FASTLOCK_PROFILES];
static fastlock_profile simulated_rffe_tx[NUM_RFFE_FASTLOCK_PROFILES];
static bool simulated_rffe_rx_valid[NUM_RFFE_FASTLOCK_PROFILES];
static bool simulated_rffe_tx_valid[NUM_RFFE_FASTLOCK_PROFILES];

bool adi_fastlock_load(bladerf_module module, fastlock_profile *profile)
{
    fastlock_profile *cache;
    if (profile == NULL) {
        return false;
    }
    if (track_rffe_fastlock_slots) {
        if (profile->profile_num >= NUM_RFFE_FASTLOCK_PROFILES) {
            return false;
        }
        if (profile->state == FASTLOCK_STATE_RFFE ||
            profile->state == FASTLOCK_STATE_BBP_RFFE) {
            return true;
        }
        load_count++;
        fastlock_profile *slots = module == BLADERF_MODULE_TX
            ? simulated_rffe_tx : simulated_rffe_rx;
        bool *valid = module == BLADERF_MODULE_TX
            ? simulated_rffe_tx_valid : simulated_rffe_rx_valid;
        cache = module == BLADERF_MODULE_TX ? fastlocks_tx : fastlocks_rx;
        fastlock_cache_invalidate_slot(cache, NUM_BBP_FASTLOCK_PROFILES,
                                       profile->profile_num);
        valid[profile->profile_num] = false;
        if (fail_profile_load || fail_profile_load_after_slot_write) {
            return false;
        }
        slots[profile->profile_num] = *profile;
        valid[profile->profile_num] = true;
        profile->state = FASTLOCK_STATE_BBP_RFFE;
    } else if (fail_profile_load) {
        load_count++;
        return false;
    } else {
        load_count++;
    }
    return true;
}

int32_t adi_fastlock_load_start(bladerf_module module,
                                fastlock_profile *profile)
{
    fastlock_profile *cache;
    bool *valid;

    if (profile == NULL || staged_profile_load_active) {
        return -1;
    }
    if (profile->state == FASTLOCK_STATE_RFFE ||
        profile->state == FASTLOCK_STATE_BBP_RFFE) {
        return 1;
    }

    cache = module == BLADERF_MODULE_TX ? fastlocks_tx : fastlocks_rx;
    fastlock_cache_invalidate_slot(cache, NUM_BBP_FASTLOCK_PROFILES,
                                   profile->profile_num);
    staged_profile_load_copy = *profile;
    staged_profile_load_copy.state = FASTLOCK_STATE_BBP;
    staged_profile_load_module = module;
    staged_profile_write_count = 0;
    staged_profile_load_active = true;

    if (track_rffe_fastlock_slots &&
        profile->profile_num < NUM_RFFE_FASTLOCK_PROFILES) {
        valid = module == BLADERF_MODULE_TX
            ? simulated_rffe_tx_valid : simulated_rffe_rx_valid;
        valid[profile->profile_num] = false;
    }
    return 0;
}

int32_t adi_fastlock_load_step(bladerf_module module)
{
    fastlock_profile *slots;
    bool *valid;

    if (!staged_profile_load_active ||
        staged_profile_load_module != module) {
        return -1;
    }
    if (defer_clear_during_staged_step) {
        defer_clear_during_staged_step = false;
        (void)pkt_retune2_defer_clear(module);
    }
    if ((int)staged_profile_write_count == staged_profile_fail_after_write) {
        staged_profile_load_active = false;
        return -1;
    }

    ++staged_profile_write_count;
    ++load_count;
    if (staged_profile_write_count < 18) {
        return 0;
    }

    if (track_rffe_fastlock_slots &&
        staged_profile_load_copy.profile_num < NUM_RFFE_FASTLOCK_PROFILES) {
        slots = module == BLADERF_MODULE_TX
            ? simulated_rffe_tx : simulated_rffe_rx;
        valid = module == BLADERF_MODULE_TX
            ? simulated_rffe_tx_valid : simulated_rffe_rx_valid;
        slots[staged_profile_load_copy.profile_num] =
            staged_profile_load_copy;
        valid[staged_profile_load_copy.profile_num] = true;
    }
    staged_profile_load_active = false;
    return 1;
}

bool adi_fastlock_load_cancel(bladerf_module module)
{
    if (!staged_profile_load_active ||
        staged_profile_load_module != module) {
        return false;
    }
    staged_profile_load_active = false;
    return true;
}

bool adi_fastlock_load_active(void)
{
    return staged_profile_load_active;
}

bladerf_module adi_fastlock_load_owner(void)
{
    return staged_profile_load_module;
}

void adi_fastlock_profile_cache_commit(bladerf_module module,
                                       uint16_t nios_profile,
                                       fastlock_profile *profile)
{
    fastlock_profile *cache = module == BLADERF_MODULE_TX
        ? fastlocks_tx : fastlocks_rx;

    if (profile == NULL || nios_profile >= NUM_BBP_FASTLOCK_PROFILES) {
        return;
    }
    fastlock_cache_invalidate_slot(cache, NUM_BBP_FASTLOCK_PROFILES,
                                   profile->profile_num);
    profile->state = FASTLOCK_STATE_BBP_RFFE;
    cache[nios_profile] = *profile;
}

static bool drain_staged_profile_upload(bladerf_module module)
{
    unsigned turns = 0;

    while (staged_profile_load_active && turns < 18) {
        unsigned before = staged_profile_write_count;
        pkt_retune2_work();
        ++turns;
        if (staged_profile_load_active &&
            staged_profile_write_count != before + 1) {
            return false;
        }
    }
    return !staged_profile_load_active &&
           staged_profile_load_module == module && turns <= 18;
}

static bool run_until_queue_head_state(struct queue *queue,
                                      enum entry_state expected,
                                      unsigned max_turns)
{
    unsigned turn;

    for (turn = 0; turn < max_turns; turn++) {
        struct queue_entry *head = peek_next_retune(queue);
        if (head != NULL && head->state == expected) {
            return true;
        }
        pkt_retune2_work();
    }
    return false;
}

static bool run_until_queue_empty(struct queue *queue, unsigned max_turns)
{
    unsigned turn;

    for (turn = 0; turn < max_turns && queue->count != 0; turn++) {
        pkt_retune2_work();
    }
    return queue->count == 0;
}

bool adi_fastlock_recall(bladerf_module module, fastlock_profile *profile,
                         uint8_t *failure_stage)
{
    recall_count++;
    recalled_module = module;
    recalled_profile = profile;
    recalled_profile_num = profile->profile_num;
    recalled_port = profile->port;
    recalled_spdt = profile->spdt;
    if (failure_stage != NULL) {
        *failure_stage = fail_profile_recall
            ? NIOS_PKT_RETUNE2_FAILURE_PLL_LOCK_TIMEOUT
            : NIOS_PKT_RETUNE2_FAILURE_NONE;
    }
    if (track_rffe_fastlock_slots) {
        fastlock_profile *slots = module == BLADERF_MODULE_TX
            ? simulated_rffe_tx : simulated_rffe_rx;
        bool *valid = module == BLADERF_MODULE_TX
            ? simulated_rffe_tx_valid : simulated_rffe_rx_valid;
        if (profile->profile_num >= NUM_RFFE_FASTLOCK_PROFILES ||
            !valid[profile->profile_num] ||
            memcmp(slots[profile->profile_num].profile_data,
                   profile->profile_data,
                   sizeof(profile->profile_data)) != 0) {
            return false;
        }
    }
    return !fail_profile_recall;
}

bool adi_fastlock_recall_start(bladerf_module module,
                               fastlock_profile *profile,
                               uint8_t *failure_stage)
{
    if (failure_stage != NULL)
        *failure_stage = NIOS_PKT_RETUNE2_FAILURE_NONE;
    if (simulated_fastlock_active || profile == NULL ||
        (module != BLADERF_MODULE_RX && module != BLADERF_MODULE_TX)) {
        if (failure_stage != NULL)
            *failure_stage = NIOS_PKT_RETUNE2_FAILURE_INTERNAL;
        return false;
    }
    recall_count++;
    recalled_module = module;
    recalled_profile = profile;
    recalled_profile_num = profile->profile_num;
    recalled_port = profile->port;
    recalled_spdt = profile->spdt;
    simulated_fastlock_terminal_status = fail_profile_recall ? -EIO : 0;
    simulated_fastlock_owner = module;
    simulated_fastlock_pending_polls = 2;
    simulated_fastlock_active = true;
    return true;
}

int32_t adi_fastlock_recall_poll(bladerf_module module,
                                 uint8_t *failure_stage)
{
    if (failure_stage != NULL)
        *failure_stage = NIOS_PKT_RETUNE2_FAILURE_NONE;
    if (!simulated_fastlock_active || simulated_fastlock_owner != module) {
        if (failure_stage != NULL)
            *failure_stage = NIOS_PKT_RETUNE2_FAILURE_INTERNAL;
        return -EINVAL;
    }
    if (simulated_fastlock_pending_polls > 0) {
        simulated_fastlock_pending_polls--;
        return 1;
    }
    simulated_fastlock_active = false;
    if (simulated_fastlock_terminal_status < 0) {
        if (failure_stage != NULL)
            *failure_stage = NIOS_PKT_RETUNE2_FAILURE_PLL_LOCK_TIMEOUT;
        return simulated_fastlock_terminal_status;
    }
    return 0;
}

bool adi_fastlock_recall_active(void)
{
    return simulated_fastlock_active;
}

bladerf_module adi_fastlock_recall_owner(void)
{
    return simulated_fastlock_owner;
}

bool adi_rfport_select(fastlock_profile *profile)
{
    (void)profile;
    port_count++;
    return !fail_port_select;
}

bool adi_rfspdt_select(bladerf_module module, fastlock_profile *profile)
{
    (void)module;
    (void)profile;
    spdt_count++;
    return !fail_spdt_select;
}

bool tamer_schedule_request(bladerf_module module, uint64_t timestamp)
{
    schedule_count++;
    scheduled_module = module;
    scheduled_timestamp = timestamp;
    if (module == BLADERF_MODULE_RX) {
        compare_fired_rx = false;
    } else if (module == BLADERF_MODULE_TX) {
        compare_fired_tx = false;
    }
    if (fire_during_schedule) {
        if (module == BLADERF_MODULE_RX) {
            compare_fired_rx = true;
            retune_isr(&rx_queue, module);
        } else if (module == BLADERF_MODULE_TX) {
            compare_fired_tx = true;
            retune_isr(&tx_queue, module);
        }
    }
    return !fail_tamer_schedule;
}

bool tamer_schedule_acknowledged(bladerf_module module)
{
    return (module == BLADERF_MODULE_RX || module == BLADERF_MODULE_TX) &&
           !fail_tamer_schedule_ack && !fail_tamer_schedule_status;
}

bool tamer_schedule_failed(bladerf_module module)
{
    return (module == BLADERF_MODULE_RX || module == BLADERF_MODULE_TX) &&
           fail_tamer_schedule_status;
}

bool time_tamer_read(bladerf_module module, uint64_t *timestamp)
{
    (void)module;
    if (timestamp == NULL || fail_tamer_read) {
        return false;
    }
    *timestamp = tamer_timestamp;
    return true;
}

bool tamer_compare_fired(bladerf_module module)
{
    return module == BLADERF_MODULE_RX ? compare_fired_rx :
           module == BLADERF_MODULE_TX ? compare_fired_tx : false;
}

bool tamer_clock_stalled(bladerf_module module)
{
    (void)module;
    return simulated_tamer_clock_stalled;
}

bool tamer_cancel_failed(bladerf_module module)
{
    return (module == BLADERF_MODULE_RX || module == BLADERF_MODULE_TX) &&
           fail_tamer_cancel_status;
}

bool tamer_cancel_request(bladerf_module module, uint8_t *ack_before)
{
    if ((module != BLADERF_MODULE_RX && module != BLADERF_MODULE_TX) ||
        ack_before == NULL || simulated_tamer_clock_stalled) {
        return false;
    }
    cancel_count++;
    *ack_before = 0;
    return true;
}

bool tamer_cancel_acknowledged(bladerf_module module, uint8_t ack_before)
{
    (void)ack_before;
    return (module == BLADERF_MODULE_RX || module == BLADERF_MODULE_TX) &&
           !fail_tamer_cancel && !simulated_tamer_clock_stalled;
}

static void simulate_tamer_irq(struct queue *queue, bladerf_module module)
{
    if (module == BLADERF_MODULE_RX) {
        compare_fired_rx = true;
    } else {
        compare_fired_tx = true;
    }
    retune_isr(queue, module);
}

static int check_immediate_upload_failure_preserves_profile(void)
{
    struct pkt_buf request = {
        .req = {
            [NIOS_PKT_RETUNE2_IDX_MAGIC] = NIOS_PKT_RETUNE2_MAGIC,
            [NIOS_PKT_RETUNE2_IDX_NIOS_PROFILE] = 0,
            [NIOS_PKT_RETUNE2_IDX_RFFE_PROFILE] = 3,
            [NIOS_PKT_RETUNE2_IDX_RFFE_PORT] = 0x40,
            [NIOS_PKT_RETUNE2_IDX_SPDT] = 0xa0,
        },
    };

    memset(fastlocks_tx, 0, sizeof(fastlocks_tx));
    fastlocks_tx[0].profile_num = 1;
    fastlocks_tx[0].port = 0x10;
    fastlocks_tx[0].spdt = 0x04;
    fastlocks_tx[0].state = FASTLOCK_STATE_BBP;

    fail_profile_load = true;
    pkt_retune2(&request);
    fail_profile_load = false;

    if ((request.resp[9] & NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS) != 0 ||
        fastlocks_tx[0].profile_num != 1 ||
        fastlocks_tx[0].port != 0x10 ||
        fastlocks_tx[0].spdt != 0x04 ||
        fastlocks_tx[0].state != FASTLOCK_STATE_BBP) {
        fputs("failed immediate TX upload corrupted cached profile\n",
              stderr);
        return 1;
    }
    return 0;
}

static int check_immediate_failure_stage_response(void)
{
    const struct {
        bool *failure_switch;
        uint8_t expected_stage;
        const char *name;
    } failures[] = {
        { &fail_profile_recall,
          NIOS_PKT_RETUNE2_FAILURE_PLL_LOCK_TIMEOUT, "PLL recall" },
        { &fail_port_select,
          NIOS_PKT_RETUNE2_FAILURE_RF_PORT_SELECT, "RF port" },
        { &fail_spdt_select,
          NIOS_PKT_RETUNE2_FAILURE_SPDT_SELECT, "SPDT" },
    };

    for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i) {
        struct pkt_buf request = { 0 };
        pkt_retune2_init();
        memset(fastlocks_rx, 0, sizeof(fastlocks_rx));
        fastlocks_rx[0].profile_num = 1;
        fastlocks_rx[0].state = FASTLOCK_STATE_BBP;
        fail_profile_load = false;
        fail_profile_recall = false;
        fail_port_select = false;
        fail_spdt_select = false;
        *failures[i].failure_switch = true;

        nios_pkt_retune2_pack(request.req, BLADERF_MODULE_RX,
                              NIOS_PKT_RETUNE2_NOW, 0, 1, 0x83, 0x00);
        pkt_retune2(&request);
        *failures[i].failure_switch = false;

        if ((request.resp[NIOS_PKT_RETUNE2_RESP_IDX_FLAGS] &
             NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS) != 0 ||
            nios_pkt_retune2_resp_failure(request.resp) !=
                failures[i].expected_stage) {
            fprintf(stderr, "%s activation failure lost its stage: "
                    "flags=0x%02x stage=%u expected=%u\n",
                    failures[i].name,
                    request.resp[NIOS_PKT_RETUNE2_RESP_IDX_FLAGS],
                    nios_pkt_retune2_resp_failure(request.resp),
                    failures[i].expected_stage);
            return 1;
        }
        for (size_t reserved = NIOS_PKT_RETUNE2_RESP_IDX_RESV;
             reserved < NIOS_PKT_LEN; ++reserved) {
            if (request.resp[reserved] != 0) {
                fprintf(stderr, "%s failure dirtied reserved response byte %zu\n",
                        failures[i].name, reserved);
                return 1;
            }
        }
    }

    {
        struct pkt_buf request = {0};
        unsigned loads_before = load_count;
        unsigned recalls_before = recall_count;

        pkt_retune2_init();
        memset(fastlocks_rx, 0, sizeof(fastlocks_rx));
        fastlocks_rx[0].profile_num = 1;
        fastlocks_rx[0].state = FASTLOCK_STATE_BBP;
        simulated_fastlock_active = true;
        simulated_fastlock_owner = BLADERF_MODULE_TX;
        nios_pkt_retune2_pack(request.req, BLADERF_MODULE_RX,
                              NIOS_PKT_RETUNE2_NOW, 0, 1, 0x83, 0x00);
        pkt_retune2(&request);
        simulated_fastlock_active = false;
        if (nios_pkt_retune2_resp_failure(request.resp) !=
                NIOS_PKT_RETUNE2_FAILURE_PROFILE_SLOT_BUSY ||
            load_count != loads_before || recall_count != recalls_before) {
            fputs("NOW fastlock request mutated AD9361 during active owner\n",
                  stderr);
            return 1;
        }
    }

    return 0;
}

static int check_tokened_now_uses_terminal_completion(void)
{
    struct pkt_buf request = { 0 };
    uint8_t const token = 0x41;
    uint32_t const session = 0x12345678;
    uint8_t flags;

    pkt_retune2_init();
    simulated_fastlock_active = false;
    simulated_rfic_worker_active = false;
    simulated_rfic_device_owned = false;
    simulated_rfic_phy_present = true;
    simulated_rfic_faulted = false;
    simulated_fastlock_terminal_status = 0;
    simulated_txq_owner = false;
    recall_count = 0;
    port_count = 0;
    spdt_count = 0;
    memset(fastlocks_rx, 0, sizeof(fastlocks_rx));
    memset(fastlocks_tx, 0, sizeof(fastlocks_tx));
    memset(simulated_rffe_rx_valid, 0, sizeof(simulated_rffe_rx_valid));
    load_count = 0;
    staged_profile_fail_after_write = -1;
    fastlocks_rx[0].profile_num = 1;
    fastlocks_rx[0].state = FASTLOCK_STATE_BBP;
    request.session_generation = session;

    nios_pkt_retune2_pack_token(
        request.req, BLADERF_MODULE_RX, NIOS_PKT_RETUNE2_NOW, 0, 1,
        0x83, 0x00, token);
    pkt_retune2(&request);
    flags = request.resp[NIOS_PKT_RETUNE2_RESP_IDX_FLAGS];
    if ((flags & (NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS |
                  NIOS_PKT_RETUNE2_RESP_FLAG_ASYNC_PENDING)) !=
            (NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS |
             NIOS_PKT_RETUNE2_RESP_FLAG_ASYNC_PENDING) ||
        (flags & NIOS_PKT_RETUNE2_RESP_FLAG_TSVTUNE_VALID) != 0 ||
        rx_queue.count != 1 ||
        rx_queue.entries[rx_queue.rem_idx].state !=
            ENTRY_STATE_PROFILE_LOAD_PENDING ||
        !rx_queue.entries[rx_queue.rem_idx].immediate ||
        simulated_fastlock_active || recall_count != 0 ||
        staged_profile_write_count != 0 || load_count != 0) {
        fputs("tokened NOW did not return asynchronous acceptance", stderr);
        return 1;
    }

    if (!drain_staged_profile_upload(BLADERF_MODULE_RX) ||
        staged_profile_write_count != 18 ||
        rx_queue.entries[rx_queue.rem_idx].state != ENTRY_STATE_READY ||
        fastlocks_rx[0].state != FASTLOCK_STATE_BBP_RFFE) {
        fputs("profile upload did not yield one write per worker turn\n",
              stderr);
        return 1;
    }

    pkt_retune2_work();
    if (!simulated_fastlock_active ||
        rx_queue.entries[rx_queue.rem_idx].state != ENTRY_STATE_PLL_PENDING ||
        pkt_retune2_success_count(BLADERF_MODULE_RX) != 0) {
        fputs("NOW worker did not retain PLL ownership", stderr);
        return 1;
    }
    if (!run_until_queue_empty(&rx_queue, 8)) {
        fputs("tokened NOW did not finish RFPLL polling\n", stderr);
        return 1;
    }
    if (simulated_fastlock_active || rx_queue.count != 0 ||
        pkt_retune2_success_count(BLADERF_MODULE_RX) != 1 ||
        pkt_retune2_failure_count(BLADERF_MODULE_RX) != 0 ||
        rx_retune2_completions[0].request_token != token ||
        rx_retune2_completions[0].session_generation != session ||
        rx_retune2_completions[0].scheduled_timestamp !=
            NIOS_PKT_RETUNE2_NOW) {
        fputs("NOW terminal result was not correlated to its acceptance", stderr);
        return 1;
    }

    /* The acceptance ACK packs an atomic ring boundary into its unused
     * duration bytes while preserving the 16-byte wire packet. */
    pkt_retune2_init();
    rx_retune2_success_count = 0x10203040;
    rx_retune2_failure_count = 0x50607080;
    request.session_generation = session;
    nios_pkt_retune2_pack_token(
        request.req, BLADERF_MODULE_RX, NIOS_PKT_RETUNE2_NOW, 0, 1,
        0x83, 0x00, token);
    pkt_retune2(&request);
    uint32_t success_baseline;
    uint32_t failure_baseline;
    nios_pkt_retune2_resp_get_async_baseline(
        request.resp, &success_baseline, &failure_baseline);
    if (success_baseline != 0x10203040 ||
        failure_baseline != 0x50607080) {
        fputs("NOW acceptance did not carry atomic completion baselines\n",
              stderr);
        return 1;
    }
    if (!drain_staged_profile_upload(BLADERF_MODULE_RX)) {
        fputs("baseline request left profile upload active\n", stderr);
        return 1;
    }

    /* A profile-port failure happens after acceptance and must produce one
     * correlated terminal failure while keeping residency invalid. */
    pkt_retune2_init();
    memset(fastlocks_rx, 0, sizeof(fastlocks_rx));
    memset(simulated_rffe_rx_valid, 0, sizeof(simulated_rffe_rx_valid));
    fastlocks_rx[0].profile_num = 1;
    fastlocks_rx[0].state = FASTLOCK_STATE_BBP;
    staged_profile_fail_after_write = 3;
    request.session_generation = session;
    nios_pkt_retune2_pack_token(
        request.req, BLADERF_MODULE_RX, NIOS_PKT_RETUNE2_NOW, 0, 1,
        0x83, 0x00, token + 1);
    pkt_retune2(&request);
    if ((request.resp[NIOS_PKT_RETUNE2_RESP_IDX_FLAGS] &
         NIOS_PKT_RETUNE2_RESP_FLAG_ASYNC_PENDING) == 0 ||
        staged_profile_write_count != 0 || rx_queue.count != 1) {
        fputs("profile upload failure was not deferred past acceptance\n",
              stderr);
        return 1;
    }
    for (unsigned i = 0; i < 4; ++i) {
        pkt_retune2_work();
    }
    staged_profile_fail_after_write = -1;
    if (rx_queue.count != 0 ||
        pkt_retune2_failure_count(BLADERF_MODULE_RX) != 1 ||
        !pkt_retune2_failure_completion_select(BLADERF_MODULE_RX, 1) ||
        pkt_retune2_completion_word(4) !=
            NIOS_PKT_RETUNE2_FAILURE_PROFILE_LOAD ||
        pkt_retune2_completion_word(6) != session ||
        pkt_retune2_completion_word(7) != (uint8_t)(token + 1) ||
        fastlocks_rx[0].state != FASTLOCK_STATE_BBP ||
        simulated_rffe_rx_valid[1]) {
        fputs("partial fastlock upload lost its terminal failure or cache fence\n",
              stderr);
        return 1;
    }
    return 0;
}

static int check_tokenless_now_defers_terminal_response(void)
{
    struct pkt_buf request = { 0 };

    pkt_retune2_init();
    simulated_fastlock_active = false;
    simulated_rfic_worker_active = false;
    simulated_rfic_device_owned = false;
    simulated_rfic_phy_present = true;
    simulated_rfic_faulted = false;
    simulated_fastlock_terminal_status = 0;
    simulated_txq_owner = false;
    simulated_txq_pending_polls = 0;
    simulated_txq_terminal_status = 0;
    simulate_txq_yield = true;
    memset(fastlocks_tx, 0, sizeof(fastlocks_tx));
    fastlocks_tx[0].profile_num = 1;
    fastlocks_tx[0].port = 0x83;
    fastlocks_tx[0].spdt = 0x00;
    fastlocks_tx[0].state = FASTLOCK_STATE_BBP_RFFE;

    request.session_generation = 0x12345678;
    nios_pkt_retune2_pack(
        request.req, BLADERF_MODULE_TX, NIOS_PKT_RETUNE2_NOW,
        0, 1, 0x83, 0x00);
    pkt_retune2(&request);
    if (!request.response_deferred || request.response_prepared ||
        request.ready || tx_queue.count != 1 ||
        tx_queue.entries[tx_queue.rem_idx].deferred_response_pkt != &request ||
        (request.resp[NIOS_PKT_RETUNE2_RESP_IDX_FLAGS] &
         NIOS_PKT_RETUNE2_RESP_FLAG_ASYNC_PENDING) != 0) {
        fputs("tokenless TX NOW returned before worker terminal result\n",
              stderr);
        return 1;
    }

    if (!run_until_queue_empty(&tx_queue, 8) || !request.response_deferred ||
        !request.response_prepared || !request.ready ||
        pkt_retune2_success_count(BLADERF_MODULE_TX) != 1 ||
        (request.resp[NIOS_PKT_RETUNE2_RESP_IDX_FLAGS] &
         (NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS |
          NIOS_PKT_RETUNE2_RESP_FLAG_TSVTUNE_VALID)) !=
            (NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS |
             NIOS_PKT_RETUNE2_RESP_FLAG_TSVTUNE_VALID)) {
        fputs("tokenless TX NOW did not prepare one terminal success\n",
              stderr);
        return 1;
    }

    pkt_retune2_init();
    memset(&request, 0, sizeof(request));
    request.session_generation = 0x87654321;
    nios_pkt_retune2_pack(
        request.req, BLADERF_MODULE_TX, NIOS_PKT_RETUNE2_NOW,
        0, 1, 0x83, 0x00);
    fastlocks_tx[0].profile_num = 1;
    fastlocks_tx[0].state = FASTLOCK_STATE_BBP_RFFE;
    simulated_txq_owner = false;
    simulated_txq_pending_polls = 0;
    simulated_txq_terminal_status = -5;
    pkt_retune2(&request);
    if (!request.response_deferred || !run_until_queue_empty(&tx_queue, 8) ||
        !request.response_deferred || !request.response_prepared ||
        (request.resp[NIOS_PKT_RETUNE2_RESP_IDX_FLAGS] &
         NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS) != 0 ||
        nios_pkt_retune2_resp_failure(request.resp) !=
            NIOS_PKT_RETUNE2_FAILURE_TX_QUAD_CALIBRATION) {
        fputs("tokenless TX NOW did not prepare its terminal failure\n",
              stderr);
        return 1;
    }

    /* RX NOW used to run the complete profile recall and PLL wait in the
     * packet handler. It must retain the same legacy response while the
     * worker observes lock and readback. */
    pkt_retune2_init();
    memset(&request, 0, sizeof(request));
    request.session_generation = 0x13572468;
    memset(fastlocks_rx, 0, sizeof(fastlocks_rx));
    fastlocks_rx[0].profile_num = 1;
    fastlocks_rx[0].port = 0x83;
    fastlocks_rx[0].spdt = 0x00;
    fastlocks_rx[0].state = FASTLOCK_STATE_BBP_RFFE;
    fail_profile_recall = false;
    simulated_fastlock_terminal_status = 0;
    simulate_txq_yield = false;
    nios_pkt_retune2_pack(
        request.req, BLADERF_MODULE_RX, NIOS_PKT_RETUNE2_NOW,
        0, 1, 0x83, 0x00);
    pkt_retune2(&request);
    if (!request.response_deferred || request.response_prepared ||
        request.ready || rx_queue.count != 1 ||
        rx_queue.entries[rx_queue.rem_idx].deferred_response_pkt != &request ||
        !run_until_queue_empty(&rx_queue, 8) || !request.response_prepared ||
        !request.ready ||
        pkt_retune2_success_count(BLADERF_MODULE_RX) != 1 ||
        (request.resp[NIOS_PKT_RETUNE2_RESP_IDX_FLAGS] &
         (NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS |
          NIOS_PKT_RETUNE2_RESP_FLAG_TSVTUNE_VALID)) !=
            (NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS |
             NIOS_PKT_RETUNE2_RESP_FLAG_TSVTUNE_VALID)) {
        fputs("tokenless RX NOW did not defer and publish terminal success\n",
              stderr);
        return 1;
    }

    /* A nonresident RX profile must also upload one SPI word per worker
     * pass; the dispatcher response stays deferred until upload and recall
     * have both completed. */
    pkt_retune2_init();
    memset(&request, 0, sizeof(request));
    request.session_generation = 0x11223344;
    fastlocks_rx[0].profile_num = 1;
    fastlocks_rx[0].port = 0x83;
    fastlocks_rx[0].spdt = 0x00;
    fastlocks_rx[0].state = FASTLOCK_STATE_BBP;
    staged_profile_load_active = false;
    staged_profile_fail_after_write = -1;
    load_count = 0;
    nios_pkt_retune2_pack(
        request.req, BLADERF_MODULE_RX, NIOS_PKT_RETUNE2_NOW,
        0, 1, 0x83, 0x00);
    pkt_retune2(&request);
    if (!request.response_deferred || request.response_prepared ||
        !staged_profile_load_active || rx_queue.count != 1 ||
        !run_until_queue_empty(&rx_queue, 32) || !request.response_prepared ||
        !request.ready || load_count != 18 ||
        (request.resp[NIOS_PKT_RETUNE2_RESP_IDX_FLAGS] &
         NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS) == 0) {
        fputs("tokenless RX NOW did not retain its response through profile upload\n",
              stderr);
        return 1;
    }

    pkt_retune2_init();
    memset(&request, 0, sizeof(request));
    request.session_generation = 0x24681357;
    fastlocks_rx[0].profile_num = 1;
    fastlocks_rx[0].state = FASTLOCK_STATE_BBP_RFFE;
    fail_profile_recall = true;
    nios_pkt_retune2_pack(
        request.req, BLADERF_MODULE_RX, NIOS_PKT_RETUNE2_NOW,
        0, 1, 0x83, 0x00);
    pkt_retune2(&request);
    if (!request.response_deferred || !run_until_queue_empty(&rx_queue, 8) ||
        !request.response_prepared || !request.ready ||
        (request.resp[NIOS_PKT_RETUNE2_RESP_IDX_FLAGS] &
         NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS) != 0 ||
        nios_pkt_retune2_resp_failure(request.resp) !=
            NIOS_PKT_RETUNE2_FAILURE_PLL_LOCK_TIMEOUT) {
        fputs("tokenless RX NOW did not publish terminal PLL failure\n",
              stderr);
        fail_profile_recall = false;
        return 1;
    }
    fail_profile_recall = false;

    simulate_txq_yield = false;
    return 0;
}

static int check_tokened_now_clear_preserves_terminal_identity(void)
{
    struct pkt_buf request = { 0 };
    uint8_t const token = 0x52;
    uint32_t const session = 0x87654321;

    pkt_retune2_init();
    simulated_fastlock_active = false;
    simulated_rfic_worker_active = false;
    simulated_rfic_device_owned = false;
    simulated_rfic_phy_present = true;
    simulated_rfic_faulted = false;
    simulated_fastlock_terminal_status = 0;
    simulated_txq_owner = false;
    recall_count = 0;
    memset(fastlocks_rx, 0, sizeof(fastlocks_rx));
    memset(fastlocks_tx, 0, sizeof(fastlocks_tx));
    memset(simulated_rffe_rx_valid, 0, sizeof(simulated_rffe_rx_valid));
    fastlocks_rx[0].profile_num = 1;
    fastlocks_rx[0].state = FASTLOCK_STATE_BBP;
    request.session_generation = session;

    nios_pkt_retune2_pack_token(
        request.req, BLADERF_MODULE_RX, NIOS_PKT_RETUNE2_NOW, 0, 1,
        0x83, 0x00, token);
    pkt_retune2(&request);
    if ((request.resp[NIOS_PKT_RETUNE2_RESP_IDX_FLAGS] &
         NIOS_PKT_RETUNE2_RESP_FLAG_ASYNC_PENDING) == 0 ||
        rx_queue.count != 1 ||
        rx_queue.entries[rx_queue.rem_idx].state !=
            ENTRY_STATE_PROFILE_LOAD_PENDING) {
        fputs("tokened NOW clear test did not get an accepted upload entry\n",
              stderr);
        return 1;
    }

    /* Before the worker starts, clear can retire the accepted request as a
     * correlated cancellation. It must not silently discard its identity. */
    nios_pkt_retune2_pack_token(
        request.req, BLADERF_MODULE_RX, NIOS_PKT_RETUNE2_CLEAR_QUEUE,
        0, 0, 0, 0, 0);
    pkt_retune2(&request);
    if ((request.resp[NIOS_PKT_RETUNE2_RESP_IDX_FLAGS] &
         NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS) == 0 ||
        rx_queue.count != 0 || pkt_retune2_failure_count(BLADERF_MODULE_RX) != 1 ||
        !pkt_retune2_failure_completion_select(BLADERF_MODULE_RX, 1) ||
        pkt_retune2_completion_word(1) != (uint32_t)NIOS_PKT_RETUNE2_NOW ||
        pkt_retune2_completion_word(2) !=
            (uint32_t)(NIOS_PKT_RETUNE2_NOW >> 32) ||
        pkt_retune2_completion_word(4) !=
            NIOS_PKT_RETUNE2_FAILURE_CANCELLED ||
        pkt_retune2_completion_word(5) != session ||
        pkt_retune2_completion_word(6) != token) {
        fputs("cleared tokened NOW lost its correlated cancellation result\n",
              stderr);
        return 1;
    }

    /* Cancellation after several complete writes is still safe: the upload
     * stops between SPI transactions and the partially replaced RFFE slot
     * stays invalid in the host-side residency model. */
    pkt_retune2_init();
    memset(fastlocks_rx, 0, sizeof(fastlocks_rx));
    memset(simulated_rffe_rx_valid, 0, sizeof(simulated_rffe_rx_valid));
    fastlocks_rx[0].profile_num = 1;
    fastlocks_rx[0].state = FASTLOCK_STATE_BBP;
    request.session_generation = session;
    nios_pkt_retune2_pack_token(
        request.req, BLADERF_MODULE_RX, NIOS_PKT_RETUNE2_NOW, 0, 1,
        0x83, 0x00, token + 1);
    pkt_retune2(&request);
    for (unsigned i = 0; i < 5; ++i) {
        pkt_retune2_work();
    }
    nios_pkt_retune2_pack_token(
        request.req, BLADERF_MODULE_RX, NIOS_PKT_RETUNE2_CLEAR_QUEUE,
        0, 0, 0, 0, 0);
    pkt_retune2(&request);
    if (staged_profile_write_count != 5 || staged_profile_load_active ||
        rx_queue.count != 0 || !rx_queue.clear_complete ||
        pkt_retune2_failure_count(BLADERF_MODULE_RX) != 1 ||
        !pkt_retune2_failure_completion_select(BLADERF_MODULE_RX, 1) ||
        pkt_retune2_completion_word(4) !=
            NIOS_PKT_RETUNE2_FAILURE_CANCELLED ||
        pkt_retune2_completion_word(7) != (uint8_t)(token + 1) ||
        simulated_rffe_rx_valid[1]) {
        fputs("profile upload clear did not stop at a safe transaction boundary\n",
              stderr);
        return 1;
    }

    /* The synchronous cooperative hook must only request the clear. It may
     * not tear down queue/job state while the current SPI call is on-stack;
     * the worker consumes that intent at the next completed-transfer point. */
    pkt_retune2_init();
    memset(fastlocks_rx, 0, sizeof(fastlocks_rx));
    memset(simulated_rffe_rx_valid, 0, sizeof(simulated_rffe_rx_valid));
    fastlocks_rx[0].profile_num = 1;
    fastlocks_rx[0].state = FASTLOCK_STATE_BBP;
    request.session_generation = session;
    nios_pkt_retune2_pack_token(
        request.req, BLADERF_MODULE_RX, NIOS_PKT_RETUNE2_NOW, 0, 1,
        0x83, 0x00, token + 2);
    pkt_retune2(&request);
    for (unsigned i = 0; i < 4; ++i) {
        pkt_retune2_work();
    }
    if (!pkt_retune2_defer_clear(BLADERF_MODULE_RX) ||
        !staged_profile_load_active || rx_queue.count != 1 ||
        staged_profile_write_count != 4) {
        fputs("deferred clear mutated the active SPI owner reentrantly\n",
              stderr);
        return 1;
    }
    pkt_retune2_work();
    if (staged_profile_load_active || rx_queue.count != 0 ||
        staged_profile_write_count != 4 || !rx_queue.clear_complete ||
        pkt_retune2_failure_count(BLADERF_MODULE_RX) != 1 ||
        !pkt_retune2_failure_completion_select(BLADERF_MODULE_RX, 1) ||
        pkt_retune2_completion_word(4) !=
            NIOS_PKT_RETUNE2_FAILURE_CANCELLED ||
        pkt_retune2_completion_word(7) != (uint8_t)(token + 2)) {
        fputs("worker did not consume deferred clear at SPI boundary\n",
              stderr);
        return 1;
    }

    /* Once PLL polling owns the RFIC, clear must wait for the real terminal
     * result rather than claiming cancellation or dropping the owner. */
    pkt_retune2_init();
    request.session_generation = session;
    nios_pkt_retune2_pack_token(
        request.req, BLADERF_MODULE_RX, NIOS_PKT_RETUNE2_NOW, 0, 1,
        0x83, 0x00, token);
    pkt_retune2(&request);
    pkt_retune2_work();
    if (!drain_staged_profile_upload(BLADERF_MODULE_RX)) {
        fputs("tokened NOW upload did not complete before PLL\n", stderr);
        return 1;
    }
    pkt_retune2_work();
    if (rx_queue.count != 1 ||
        rx_queue.entries[rx_queue.rem_idx].state != ENTRY_STATE_PLL_PENDING) {
        fputs("tokened NOW did not enter PLL ownership before clear\n", stderr);
        return 1;
    }
    /* FX3 SYS_RST advances the command-link session while the NIOS core and
     * this retained RFIC job survive. A clear from the new session must not
     * rewrite the old request's result identity. */
    request.session_generation = session + 1u;
    nios_pkt_retune2_pack_token(
        request.req, BLADERF_MODULE_RX, NIOS_PKT_RETUNE2_CLEAR_QUEUE,
        0, 0, 0, 0, 0);
    pkt_retune2(&request);
    if (rx_queue.count != 1 || !rx_queue.clear_requested ||
        pkt_retune2_success_count(BLADERF_MODULE_RX) != 0) {
        fputs("queue clear discarded an active tokened NOW recall\n", stderr);
        return 1;
    }
    pkt_retune2_work();
    pkt_retune2_work();
    pkt_retune2_work();
    if (rx_queue.count != 0 || !rx_queue.clear_complete ||
        pkt_retune2_success_count(BLADERF_MODULE_RX) != 1 ||
        !pkt_retune2_completion_select(BLADERF_MODULE_RX, 1) ||
        pkt_retune2_completion_word(4) !=
            (uint32_t)NIOS_PKT_RETUNE2_NOW ||
        pkt_retune2_completion_word(6) != session ||
        pkt_retune2_completion_word(7) != token) {
        fputs("queue clear lost the active NOW terminal result\n", stderr);
        return 1;
    }
    return 0;
}

static void setup_rfic_fault_retune_test(void)
{
    pkt_retune2_init();
    simulated_rfic_phy_present = true;
    simulated_rfic_faulted = false;
    fail_profile_load = false;
    fail_profile_recall = false;
    fail_port_select = false;
    fail_spdt_select = false;
    fail_tamer_schedule = false;
    fail_tamer_schedule_ack = false;
    fail_tamer_schedule_status = false;
    fail_tamer_cancel = false;
    fail_tamer_cancel_status = false;
    simulated_tamer_clock_stalled = false;
    tamer_timestamp = 0;
    load_count = 0;
    schedule_count = 0;
    recall_count = 0;
    port_count = 0;
    spdt_count = 0;
    cancel_count = 0;
    compare_fired_rx = false;
    compare_fired_tx = false;
    memset(fastlocks_rx, 0, sizeof(fastlocks_rx));
    memset(fastlocks_tx, 0, sizeof(fastlocks_tx));
    fastlocks_rx[0].profile_num = 1;
    fastlocks_rx[0].state = FASTLOCK_STATE_BBP;
    fastlocks_rx[1].profile_num = 2;
    fastlocks_rx[1].state = FASTLOCK_STATE_BBP;
}

static int check_host_fastlock_owner_is_not_fault_inference(void)
{
    struct pkt_buf request = { 0 };

    setup_rfic_fault_retune_test();
    simulated_rfic_phy_present = false;
    nios_pkt_retune2_pack(request.req, BLADERF_MODULE_RX,
                          NIOS_PKT_RETUNE2_NOW, 0, 1, 0x83, 0x00);
    pkt_retune2(&request);
    if ((request.resp[NIOS_PKT_RETUNE2_RESP_IDX_FLAGS] &
         NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS) == 0 || recall_count != 1) {
        fputs("healthy HOST-mode immediate fastlock was not admitted\n",
              stderr);
        return 1;
    }

    setup_rfic_fault_retune_test();
    simulated_rfic_faulted = true;
    nios_pkt_retune2_pack(request.req, BLADERF_MODULE_RX,
                          NIOS_PKT_RETUNE2_NOW, 0, 1, 0x83, 0x00);
    pkt_retune2(&request);
    if ((request.resp[NIOS_PKT_RETUNE2_RESP_IDX_FLAGS] &
         NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS) != 0 ||
        nios_pkt_retune2_resp_failure(request.resp) !=
            NIOS_PKT_RETUNE2_FAILURE_RFIC_FAULTED ||
        load_count != 0 || recall_count != 0 || port_count != 0 ||
        spdt_count != 0) {
        fputs("faulted PHY was mistaken for HOST-owned fastlock\n", stderr);
        return 1;
    }
    return 0;
}

static int check_fault_retires_unarmed_and_ready_retunes(void)
{
    struct pkt_buf request = { 0 };
    unsigned loads_before_fault;

    /* NEW was accepted but not yet loaded/armed. Fault rejects its eventual
     * activation and reports exactly one deferred failure. */
    setup_rfic_fault_retune_test();
    request.session_generation = 0x12345678;
    nios_pkt_retune2_pack_token(request.req, BLADERF_MODULE_RX, 100, 0, 1,
                                0x83, 0x00, 0x1111);
    pkt_retune2(&request);
    loads_before_fault = load_count;
    simulated_rfic_faulted = true;
    pkt_retune2_work();
    if (rx_queue.count != 0 || load_count != loads_before_fault ||
        recall_count != 0 ||
        pkt_retune2_failure_count(BLADERF_MODULE_RX) != 1) {
        fputs("fault did not retire an unarmed retune exactly once\n", stderr);
        return 1;
    }

    /* A schedule request without its ACK may still have reached the FPGA;
     * fault cleanup must retain ownership until cancellation completes. */
    setup_rfic_fault_retune_test();
    fail_tamer_schedule_ack = true;
    nios_pkt_retune2_pack(request.req, BLADERF_MODULE_RX, 100, 0, 1,
                          0x83, 0x00);
    pkt_retune2(&request);
    pkt_retune2_work();
    if (rx_queue.count != 1 || rx_queue.entries[rx_queue.rem_idx].state !=
            ENTRY_STATE_SCHEDULE_PENDING) {
        fputs("test setup failed to create unacknowledged schedule\n", stderr);
        return 1;
    }
    simulated_rfic_faulted = true;
    fail_tamer_schedule_ack = false;
    pkt_retune2_work();
    if (rx_queue.count != 0 || recall_count != 0 ||
        pkt_retune2_failure_count(BLADERF_MODULE_RX) != 1) {
        fputs("fault did not cancel unacknowledged schedule safely\n", stderr);
        return 1;
    }

    /* A READY compare has fired, but the actual RFIC recall is still NIOS
     * work. The fault must reject that activation rather than report success. */
    setup_rfic_fault_retune_test();
    nios_pkt_retune2_pack(request.req, BLADERF_MODULE_RX, 100, 0, 1,
                          0x83, 0x00);
    pkt_retune2(&request);
    pkt_retune2_work();
    simulate_tamer_irq(&rx_queue, BLADERF_MODULE_RX);
    simulated_rfic_faulted = true;
    pkt_retune2_work();
    if (rx_queue.count != 0 || recall_count != 0 ||
        pkt_retune2_success_count(BLADERF_MODULE_RX) != 0 ||
        pkt_retune2_failure_count(BLADERF_MODULE_RX) != 1) {
        fputs("faulted READY retune was activated or misreported\n", stderr);
        return 1;
    }
    return 0;
}

static int check_fault_cancels_armed_queue_once(void)
{
    struct pkt_buf request = { 0 };

    setup_rfic_fault_retune_test();
    nios_pkt_retune2_pack(request.req, BLADERF_MODULE_RX, 100, 0, 1,
                          0x83, 0x00);
    pkt_retune2(&request);
    pkt_retune2_work();
    nios_pkt_retune2_pack_token(request.req, BLADERF_MODULE_RX, 200, 1, 2,
                                0x83, 0x00, 0x2222);
    pkt_retune2(&request);
    if (rx_queue.count != 2 || rx_queue.entries[rx_queue.rem_idx].state !=
            ENTRY_STATE_SCHEDULED) {
        fputs("test setup failed to create armed and queued retunes\n", stderr);
        return 1;
    }

    simulated_rfic_faulted = true;
    fail_tamer_cancel = true;
    pkt_retune2_work();
    if (!rx_queue.clear_requested || rx_queue.count != 2 ||
        pkt_retune2_failure_count(BLADERF_MODULE_RX) != 2 ||
        recall_count != 0) {
        fputs("fault did not fence armed and queued requests\n", stderr);
        return 1;
    }
    if (!pkt_retune2_failure_completion_select(BLADERF_MODULE_RX, 1) ||
        pkt_retune2_completion_word(0) != 1 ||
        pkt_retune2_completion_word(1) != 100 ||
        pkt_retune2_completion_word(2) != 0 ||
        pkt_retune2_completion_word(3) != 0 ||
        pkt_retune2_completion_word(4) !=
            NIOS_PKT_RETUNE2_FAILURE_RFIC_FAULTED ||
        pkt_retune2_completion_word(5) != 0x12345678 ||
        pkt_retune2_completion_word(6) != 0x1111 ||
        !pkt_retune2_failure_completion_select(BLADERF_MODULE_RX, 2) ||
        pkt_retune2_completion_word(0) != 2 ||
        pkt_retune2_completion_word(1) != 200 ||
        pkt_retune2_completion_word(2) != 0 ||
        pkt_retune2_completion_word(3) != 1 ||
        pkt_retune2_completion_word(4) !=
            NIOS_PKT_RETUNE2_FAILURE_RFIC_FAULTED ||
        pkt_retune2_completion_word(5) != 0x12345678 ||
        pkt_retune2_completion_word(6) != 0x2222) {
        fputs("fault failures lost per-request retune identity\n", stderr);
        return 1;
    }

    /* A missing ACK retains queue ownership; after the cancel status recovers,
     * the same requests are not counted twice and the queue can retire. */
    pkt_retune2_work();
    if (pkt_retune2_failure_count(BLADERF_MODULE_RX) != 2 ||
        rx_queue.count != 2) {
        fputs("lost cancel ACK changed fault outcome accounting\n", stderr);
        return 1;
    }
    fail_tamer_cancel = false;
    pkt_retune2_work();
    if (rx_queue.count != 0 ||
        pkt_retune2_failure_count(BLADERF_MODULE_RX) != 2 ||
        recall_count != 0) {
        fputs("retune queue failed to retire after cancel ACK recovery\n",
              stderr);
        return 1;
    }
    return 0;
}

static int check_immediate_retune_reloads_reassigned_rffe_slot(void)
{
    struct pkt_buf request = { 0 };
    fastlock_profile *profile;

    pkt_retune2_init();
    memset(fastlocks_tx, 0, sizeof(fastlocks_tx));
    memset(simulated_rffe_tx_valid, 0, sizeof(simulated_rffe_tx_valid));
    profile = &fastlocks_tx[0];
    profile->profile_num = 1;
    profile->profile_data[0] = 0xa5;
    profile->state = FASTLOCK_STATE_BBP_RFFE;

    track_rffe_fastlock_slots = true;
    load_count = 0;
    recall_count = 0;
    fail_profile_load = false;
    fail_profile_recall = false;
    nios_pkt_retune2_pack(request.req, BLADERF_MODULE_TX,
                          NIOS_PKT_RETUNE2_NOW, 0, 3, 0x40, 0xa0);
    pkt_retune2(&request);
    track_rffe_fastlock_slots = false;

    if ((request.resp[9] & NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS) == 0 ||
        load_count != 1 || recall_count != 1 ||
        !simulated_rffe_tx_valid[3] ||
        simulated_rffe_tx[3].profile_data[0] != 0xa5 ||
        profile->profile_num != 3 ||
        profile->state != FASTLOCK_STATE_BBP_RFFE) {
        fputs("immediate retune reused stale RFFE residency after slot reassignment\n",
              stderr);
        return 1;
    }
    return 0;
}

static int check_immediate_retune_reuses_matching_rffe_slot(void)
{
    struct pkt_buf request = { 0 };
    fastlock_profile *profile;

    pkt_retune2_init();
    memset(simulated_rffe_tx_valid, 0, sizeof(simulated_rffe_tx_valid));
    profile = &fastlocks_tx[0];
    profile->profile_num = 3;
    profile->profile_data[0] = 0xa5;
    profile->state = FASTLOCK_STATE_BBP_RFFE;
    simulated_rffe_tx[3] = *profile;
    simulated_rffe_tx_valid[3] = true;

    track_rffe_fastlock_slots = true;
    load_count = 0;
    recall_count = 0;
    fail_profile_load = false;
    fail_profile_recall = false;
    nios_pkt_retune2_pack(request.req, BLADERF_MODULE_TX,
                          NIOS_PKT_RETUNE2_NOW, 0, 3, 0x40, 0xa0);
    pkt_retune2(&request);
    track_rffe_fastlock_slots = false;

    if ((request.resp[9] & NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS) == 0 ||
        load_count != 0 || recall_count != 1 ||
        profile->state != FASTLOCK_STATE_BBP_RFFE) {
        fputs("immediate retune reloaded a profile already resident in its RFFE slot\n",
              stderr);
        return 1;
    }
    return 0;
}

static int check_partial_upload_invalidates_slot_residency(void)
{
    struct pkt_buf request = { 0 };
    fastlock_profile *old_profile;

    pkt_retune2_init();
    memset(fastlocks_tx, 0, sizeof(fastlocks_tx));
    memset(simulated_rffe_tx_valid, 0, sizeof(simulated_rffe_tx_valid));
    old_profile = &fastlocks_tx[1];
    fastlocks_tx[0].profile_num = 1;
    fastlocks_tx[0].profile_data[0] = 0xa5;
    fastlocks_tx[0].state = FASTLOCK_STATE_BBP;
    old_profile->profile_num = 3;
    old_profile->profile_data[0] = 0xa5;
    old_profile->state = FASTLOCK_STATE_BBP_RFFE;
    simulated_rffe_tx[3] = *old_profile;
    simulated_rffe_tx_valid[3] = true;

    track_rffe_fastlock_slots = true;
    load_count = 0;
    recall_count = 0;
    fail_profile_load = false;
    fail_profile_load_after_slot_write = true;
    nios_pkt_retune2_pack(request.req, BLADERF_MODULE_TX,
                          NIOS_PKT_RETUNE2_NOW, 0, 3, 0x40, 0xa0);
    pkt_retune2(&request);
    if ((request.resp[9] & NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS) != 0 ||
        old_profile->state != FASTLOCK_STATE_BBP ||
        simulated_rffe_tx_valid[3]) {
        fputs("partial fastlock upload left a stale RFFE residency cache\n",
              stderr);
        track_rffe_fastlock_slots = false;
        fail_profile_load_after_slot_write = false;
        return 1;
    }

    fail_profile_load_after_slot_write = false;
    memset(&request, 0, sizeof(request));
    nios_pkt_retune2_pack(request.req, BLADERF_MODULE_TX,
                          NIOS_PKT_RETUNE2_NOW, 0, 3, 0x40, 0xa0);
    pkt_retune2(&request);
    track_rffe_fastlock_slots = false;
    if ((request.resp[9] & NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS) == 0 ||
        load_count != 2 || recall_count != 1 ||
        !simulated_rffe_tx_valid[3] ||
        simulated_rffe_tx[3].profile_data[0] != 0xa5) {
        fprintf(stderr, "profile reload after partial corruption failed: "
                "flags=0x%02x loads=%u recalls=%u valid=%u data=0x%02x\n",
                request.resp[9], load_count, recall_count,
                simulated_rffe_tx_valid[3],
                simulated_rffe_tx[3].profile_data[0]);
        return 1;
    }
    return 0;
}

static int check_immediate_retune_cannot_clobber_armed_slot(
    bladerf_module module)
{
    struct pkt_buf request = { 0 };
    fastlock_profile armed = { 0 };
    struct queue *queue = module == BLADERF_MODULE_TX ? &tx_queue : &rx_queue;
    fastlock_profile *profiles = module == BLADERF_MODULE_TX
        ? fastlocks_tx : fastlocks_rx;
    fastlock_profile *simulated_slots = module == BLADERF_MODULE_TX
        ? simulated_rffe_tx : simulated_rffe_rx;
    bool *simulated_valid = module == BLADERF_MODULE_TX
        ? simulated_rffe_tx_valid : simulated_rffe_rx_valid;
    uint8_t port = module == BLADERF_MODULE_TX ? 0x40 : 0x83;
    uint8_t spdt = module == BLADERF_MODULE_TX ? 0xa0 : 0x05;

    pkt_retune2_init();
    memset(profiles, 0,
           NUM_BBP_FASTLOCK_PROFILES * sizeof(*profiles));
    memset(simulated_valid, 0,
           NUM_RFFE_FASTLOCK_PROFILES * sizeof(*simulated_valid));
    track_rffe_fastlock_slots = true;
    load_count = 0;
    recall_count = 0;
    fail_profile_load = false;
    fail_profile_recall = false;
    fail_tamer_read = false;
    fail_tamer_schedule = false;
    fail_tamer_cancel = false;
    simulated_tamer_clock_stalled = false;
    compare_fired_rx = false;
    tamer_timestamp = 0;

    armed.profile_num = 3;
    armed.state = FASTLOCK_STATE_BBP;
    armed.profile_data[0] = 0xa5;
    if (enqueue_retune_with_identity(queue, &armed, 1, 100, 0, 0) != 1) {
        fputs("failed to enqueue armed-slot collision case\n", stderr);
        track_rffe_fastlock_slots = false;
        return 1;
    }
    if (!run_until_queue_head_state(queue, ENTRY_STATE_SCHEDULED, 24)) {
        fputs("failed to arm scheduled profile for collision case\n", stderr);
        track_rffe_fastlock_slots = false;
        return 1;
    }

    profiles[0].profile_num = 0;
    profiles[0].profile_data[0] = 0x5a;
    profiles[0].state = FASTLOCK_STATE_BBP;
    nios_pkt_retune2_pack(request.req, module, NIOS_PKT_RETUNE2_NOW,
                          0, 3, port, spdt);
    pkt_retune2(&request);
    if ((request.resp[9] & NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS) != 0 ||
        load_count != 18 || recall_count != 0 ||
        simulated_slots[3].profile_data[0] != 0xa5) {
        fputs("immediate retune overwrote a slot owned by an armed compare\n",
              stderr);
        track_rffe_fastlock_slots = false;
        return 1;
    }

    if (module == BLADERF_MODULE_TX) {
        compare_fired_tx = true;
    } else {
        compare_fired_rx = true;
    }
    if (!run_until_queue_empty(queue, 8)) {
        track_rffe_fastlock_slots = false;
        fputs("armed retune did not finish RFPLL polling\n", stderr);
        return 1;
    }
    compare_fired_rx = false;
    compare_fired_tx = false;
    track_rffe_fastlock_slots = false;
    if (queue->count != 0 || pkt_retune2_success_count(module) != 1 ||
        pkt_retune2_failure_count(module) != 0 ||
        recall_count != 1 || recalled_profile->profile_data[0] != 0xa5) {
        fputs("armed retune failed after conflicting NOW request\n", stderr);
        return 1;
    }
    return 0;
}

static int check_immediate_retune_cannot_clobber_armed_slot_rx_tx(void)
{
    return check_immediate_retune_cannot_clobber_armed_slot(
               BLADERF_MODULE_RX) == 0 &&
           check_immediate_retune_cannot_clobber_armed_slot(
               BLADERF_MODULE_TX) == 0 ? 0 : 1;
}

static int check_schedule_ready_lifecycle(void)
{
    fastlock_profile rx_profile = { 0 };
    fastlock_profile tx_profile = { 0 };

    pkt_retune2_init();
    load_count = 0;
    rx_profile.profile_num = 1;
    rx_profile.port = 0x11;
    rx_profile.spdt = 0x02;
    rx_profile.state = FASTLOCK_STATE_BBP_RFFE;
    tx_profile.profile_num = 2;
    tx_profile.port = 0x22;
    tx_profile.spdt = 0x03;
    tx_profile.state = FASTLOCK_STATE_BBP_RFFE;

    if (enqueue_retune_resident(&rx_queue, &rx_profile, 5,
                       UINT64_C(0x123456789)) != 1 ||
        enqueue_retune_resident(&tx_queue, &tx_profile, 7,
                       UINT64_C(0x23456789a)) != 1) {
        fputs("failed to enqueue RX/TX retunes\n", stderr);
        return 1;
    }

    /* The shared profile upload slots may be reused immediately after the
     * queue accepts a request. A pending entry must retain its own data. */
    rx_profile.profile_num = 6;
    rx_profile.port = 0x66;
    rx_profile.spdt = 0x06;
    tx_profile.profile_num = 7;
    tx_profile.port = 0x77;
    tx_profile.spdt = 0x07;

    /* NEW schedules once; repeated worker passes must wait for the ISR. */
    pkt_retune2_work();
    if (rx_queue.entries[rx_queue.rem_idx].state != ENTRY_STATE_SCHEDULED ||
        tx_queue.entries[tx_queue.rem_idx].state != ENTRY_STATE_SCHEDULED ||
        load_count != 0 || schedule_count != 2 ||
        rx_queue.entries[rx_queue.rem_idx].profile.profile_num != 1 ||
        rx_queue.entries[rx_queue.rem_idx].profile.port != 0x11 ||
        rx_queue.entries[rx_queue.rem_idx].profile.spdt != 0x02 ||
        rx_queue.entries[rx_queue.rem_idx].profile.state != FASTLOCK_STATE_RFFE ||
        tx_queue.entries[tx_queue.rem_idx].profile.profile_num != 2 ||
        tx_queue.entries[tx_queue.rem_idx].profile.port != 0x22 ||
        tx_queue.entries[tx_queue.rem_idx].profile.spdt != 0x03 ||
        tx_queue.entries[tx_queue.rem_idx].profile.state != FASTLOCK_STATE_RFFE ||
        scheduled_module != BLADERF_MODULE_TX ||
        scheduled_timestamp != UINT64_C(0x23456789a)) {
        fputs("NEW retune did not load and schedule both module queues\n", stderr);
        return 1;
    }

    pkt_retune2_work();
    if (load_count != 0 || schedule_count != 2 || recall_count != 0) {
        fputs("SCHEDULED retune was rescheduled or activated before ISR\n", stderr);
        return 1;
    }

    /* Fire only RX's timer interrupt: TX remains scheduled and untouched. */
    simulate_tamer_irq(&rx_queue, BLADERF_MODULE_RX);
    if (!run_until_queue_empty(&rx_queue, 8) ||
        tx_queue.entries[tx_queue.rem_idx].state != ENTRY_STATE_SCHEDULED ||
        pkt_retune2_success_count(BLADERF_MODULE_RX) != 1 ||
        pkt_retune2_success_count(BLADERF_MODULE_TX) != 0 ||
        recall_count != 1 || recalled_module != BLADERF_MODULE_RX ||
        recalled_profile == &rx_profile || recalled_profile_num != 1 ||
        recalled_port != 0x11 || recalled_spdt != 0x02 ||
        port_count != 1 || spdt_count != 1) {
        fputs("RX READY transition activated the wrong queue/profile\n", stderr);
        return 1;
    }
    if (!pkt_retune2_completion_select(BLADERF_MODULE_RX, 1) ||
        pkt_retune2_completion_word(0) != 1 ||
        pkt_retune2_completion_word(1) != 0 ||
        pkt_retune2_completion_word(2) != 0 ||
        pkt_retune2_completion_word(3) != 5 ||
        pkt_retune2_completion_word(4) != 0x23456789u ||
        pkt_retune2_completion_word(5) != 1u) {
        fputs("RX completion record did not preserve sequence/profile\n",
              stderr);
        return 1;
    }

    /* TX remains scheduled until its own interrupt. */
    simulate_tamer_irq(&tx_queue, BLADERF_MODULE_TX);
    if (!run_until_queue_empty(&tx_queue, 8) ||
        pkt_retune2_success_count(BLADERF_MODULE_RX) != 1 ||
        pkt_retune2_success_count(BLADERF_MODULE_TX) != 1 ||
        recall_count != 2 || recalled_module != BLADERF_MODULE_TX ||
        recalled_profile == &tx_profile || recalled_profile_num != 2 ||
        recalled_port != 0x22 || recalled_spdt != 0x03 ||
        port_count != 2 || spdt_count != 2) {
        fputs("TX READY transition did not activate its queued profile\n", stderr);
        return 1;
    }

    if (!pkt_retune2_completion_select(BLADERF_MODULE_TX, 1) ||
        pkt_retune2_completion_word(0) != 1 ||
        pkt_retune2_completion_word(1) != 0 ||
        pkt_retune2_completion_word(2) != 0 ||
        pkt_retune2_completion_word(3) != 7 ||
        pkt_retune2_completion_word(4) != 0x3456789au ||
        pkt_retune2_completion_word(5) != 2u) {
        fputs("TX completion record did not preserve sequence/profile\n",
              stderr);
        return 1;
    }

    tx_profile.profile_num = 6;
    if (enqueue_retune_resident(&tx_queue, &tx_profile, 6,
                       UINT64_C(0x56789abcd)) != 1) {
        fputs("failed to enqueue TX RF switch readback failure case\n",
              stderr);
        return 1;
    }
    simulate_tamer_irq(&tx_queue, BLADERF_MODULE_TX);
    fail_spdt_select = true;
    if (!run_until_queue_empty(&tx_queue, 8)) {
        fputs("TX RF switch failure did not retire scheduled request\n",
              stderr);
        fail_spdt_select = false;
        return 1;
    }
    fail_spdt_select = false;
    if (tx_queue.count != 0 ||
        pkt_retune2_failure_count(BLADERF_MODULE_TX) != 1 ||
        pkt_retune2_success_count(BLADERF_MODULE_TX) != 1) {
        fputs("TX RF switch readback failure was reported as success\n",
              stderr);
        return 1;
    }

    return 0;
}

static int check_preload_preserves_scheduled_profile_slot(void)
{
    fastlock_profile first = { 0 };
    fastlock_profile later = { 0 };
    unsigned int loads_after_first_arm;

    pkt_retune2_init();
    memset(simulated_rffe_rx_valid, 0, sizeof(simulated_rffe_rx_valid));
    memset(simulated_rffe_tx_valid, 0, sizeof(simulated_rffe_tx_valid));
    track_rffe_fastlock_slots = true;
    load_count = 0;
    recall_count = 0;
    fail_profile_load = false;
    fail_profile_recall = false;
    compare_fired_rx = false;

    first.profile_num = 3;
    first.state = FASTLOCK_STATE_BBP;
    first.profile_data[0] = 0xa5;
    later.profile_num = 3;
    later.state = FASTLOCK_STATE_BBP;
    later.profile_data[0] = 0x5a;

    if (enqueue_retune_with_identity(&rx_queue, &first, 1, 100, 0, 0) != 1) {
        fputs("failed to enqueue first shared-slot retune\n", stderr);
        track_rffe_fastlock_slots = false;
        return 1;
    }
    if (!run_until_queue_head_state(&rx_queue, ENTRY_STATE_SCHEDULED, 24)) {
        fputs("first scheduled profile did not finish cooperative staging\n",
              stderr);
        track_rffe_fastlock_slots = false;
        return 1;
    }
    loads_after_first_arm = load_count;
    if (!simulated_rffe_rx_valid[first.profile_num] ||
        simulated_rffe_rx[first.profile_num].profile_data[0] != 0xa5) {
        fputs("first retune did not retain its loaded RFFE profile\n", stderr);
        track_rffe_fastlock_slots = false;
        return 1;
    }

    if (enqueue_retune_with_identity(&rx_queue, &later, 2, 200, 0, 0) != 2) {
        fputs("failed to enqueue later shared-slot retune\n", stderr);
        track_rffe_fastlock_slots = false;
        return 1;
    }
    pkt_retune2_work();
    if (load_count != loads_after_first_arm ||
        staged_profile_load_active ||
        simulated_rffe_rx[first.profile_num].profile_data[0] != 0xa5) {
        fputs("preload overwrote an armed retune's RFFE profile slot\n",
              stderr);
        track_rffe_fastlock_slots = false;
        return 1;
    }

    compare_fired_rx = true;
    for (unsigned turn = 0; turn < 8 && rx_queue.count > 1; turn++) {
        pkt_retune2_work();
    }
    compare_fired_rx = false;
    if (rx_queue.count != 1 ||
        pkt_retune2_success_count(BLADERF_MODULE_RX) != 1 ||
        pkt_retune2_failure_count(BLADERF_MODULE_RX) != 0 ||
        recall_count != 1 || recalled_profile->profile_data[0] != 0xa5) {
        fputs("armed retune recalled a different profile than requested\n",
              stderr);
        track_rffe_fastlock_slots = false;
        return 1;
    }

    if (!run_until_queue_head_state(&rx_queue, ENTRY_STATE_SCHEDULED, 24) ||
        load_count != loads_after_first_arm + 18 ||
        simulated_rffe_rx[first.profile_num].profile_data[0] != 0x5a) {
        fputs("deferred colliding profile did not load at queue head\n",
              stderr);
        track_rffe_fastlock_slots = false;
        return 1;
    }

    track_rffe_fastlock_slots = false;
    return 0;
}

static int check_legacy_schedule_accepts_without_profile_spi(void)
{
    struct pkt_buf request = { 0 };
    struct pkt_buf concurrent_request = { 0 };

    pkt_retune2_init();
    staged_profile_load_active = false;
    track_rffe_fastlock_slots = false;
    load_count = 0;
    fail_profile_load = false;
    simulated_rfic_device_owned = false;
    simulated_rfic_worker_active = false;
    simulated_fastlock_active = false;
    simulated_txq_owner = false;
    tamer_timestamp = 10;
    fastlocks_rx[0].profile_num = 2;
    fastlocks_rx[0].state = FASTLOCK_STATE_BBP;

    nios_pkt_retune2_pack_token(request.req, BLADERF_MODULE_RX, 100,
                                0, 2, 0x83, 0, 0);
    pkt_retune2(&request);

    if (rx_queue.count != 1 || load_count != 0 ||
        (request.resp[9] & NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS) == 0) {
        fputs("legacy scheduled request performed profile SPI before ACK\n",
              stderr);
        return 1;
    }
    pkt_retune2_work();
    if (!staged_profile_load_active || staged_profile_write_count != 0) {
        fputs("accepted legacy scheduled profile did not start in worker\n",
              stderr);
        return 1;
    }
    pkt_retune2_work();
    if (!staged_profile_load_active || staged_profile_write_count != 1) {
        fputs("accepted legacy scheduled profile did not yield to worker\n",
              stderr);
        return 1;
    }

    fastlocks_rx[1].profile_num = 3;
    fastlocks_rx[1].state = FASTLOCK_STATE_BBP;
    nios_pkt_retune2_pack_token(concurrent_request.req, BLADERF_MODULE_RX,
                                200, 1, 3, 0x83, 0, 0x1234);
    pkt_retune2(&concurrent_request);
    if (rx_queue.count != 2 || load_count != 1 ||
        staged_profile_write_count != 1 ||
        (concurrent_request.resp[9] &
         NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS) == 0) {
        fputs("scheduled admission interleaved profile SPI with active upload\n",
              stderr);
        return 1;
    }

    return 0;
}

static int check_cancel_pending_preserves_profile_slot(bladerf_module module)
{
    struct pkt_buf request = { 0 };
    fastlock_profile first = { 0 };
    fastlock_profile later = { 0 };
    struct queue *queue = module == BLADERF_MODULE_TX ? &tx_queue : &rx_queue;
    fastlock_profile *profiles = module == BLADERF_MODULE_TX
        ? fastlocks_tx : fastlocks_rx;
    fastlock_profile *simulated_slots = module == BLADERF_MODULE_TX
        ? simulated_rffe_tx : simulated_rffe_rx;
    bool *simulated_valid = module == BLADERF_MODULE_TX
        ? simulated_rffe_tx_valid : simulated_rffe_rx_valid;
    uint8_t port = module == BLADERF_MODULE_TX ? 0x40 : 0x83;
    uint8_t spdt = module == BLADERF_MODULE_TX ? 0xa0 : 0x05;
    unsigned int loads_after_failure;

    pkt_retune2_init();
    memset(profiles, 0,
           NUM_BBP_FASTLOCK_PROFILES * sizeof(*profiles));
    memset(simulated_valid, 0,
           NUM_RFFE_FASTLOCK_PROFILES * sizeof(*simulated_valid));
    track_rffe_fastlock_slots = true;
    load_count = 0;
    recall_count = 0;
    cancel_count = 0;
    fail_profile_load = false;
    fail_profile_recall = false;
    fail_tamer_read = false;
    fail_tamer_schedule = true;
    fail_tamer_cancel = true;
    simulated_tamer_clock_stalled = true;
    compare_fired_rx = false;
    compare_fired_tx = false;

    first.profile_num = 3;
    first.state = FASTLOCK_STATE_BBP;
    first.profile_data[0] = 0xa5;
    later = first;
    later.profile_data[0] = 0x5a;
    if (enqueue_retune_with_identity(queue, &first, 1, 100, 0, 0) != 1) {
        fputs("failed to enqueue cancel-pending slot owner\n", stderr);
        track_rffe_fastlock_slots = false;
        return 1;
    }
    if (!run_until_queue_head_state(queue, ENTRY_STATE_CANCEL_PENDING, 24) ||
        !simulated_valid[3] || simulated_slots[3].profile_data[0] != 0xa5) {
        fputs("failed schedule did not retain the physical slot owner\n",
              stderr);
        track_rffe_fastlock_slots = false;
        return 1;
    }

    loads_after_failure = load_count;
    if (enqueue_retune_with_identity(queue, &later, 2, 200, 0, 0) != 2) {
        fputs("failed to enqueue later cancel-pending slot collision\n",
              stderr);
        track_rffe_fastlock_slots = false;
        return 1;
    }
    pkt_retune2_work();
    if (load_count != loads_after_failure ||
        simulated_slots[3].profile_data[0] != 0xa5) {
        fputs("preload overwrote a CANCEL_PENDING compare's RFFE slot\n",
              stderr);
        track_rffe_fastlock_slots = false;
        return 1;
    }

    profiles[0].profile_num = 0;
    profiles[0].profile_data[0] = 0x33;
    profiles[0].state = FASTLOCK_STATE_BBP;
    nios_pkt_retune2_pack(request.req, module, NIOS_PKT_RETUNE2_NOW,
                          0, 3, port, spdt);
    pkt_retune2(&request);
    fail_tamer_schedule = false;
    fail_tamer_cancel = false;
    simulated_tamer_clock_stalled = false;
    track_rffe_fastlock_slots = false;
    if ((request.resp[9] & NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS) != 0 ||
        load_count != loads_after_failure || recall_count != 0 ||
        simulated_slots[3].profile_data[0] != 0xa5) {
        fputs("NOW retune overwrote a CANCEL_PENDING compare's RFFE slot\n",
              stderr);
        return 1;
    }
    return 0;
}

static int check_cancel_pending_preserves_profile_slot_rx_tx(void)
{
    return check_cancel_pending_preserves_profile_slot(BLADERF_MODULE_RX) == 0 &&
           check_cancel_pending_preserves_profile_slot(BLADERF_MODULE_TX) == 0
               ? 0 : 1;
}

static int check_lost_irq_recovers_from_fpga_event(void)
{
    fastlock_profile profile = { 0 };

    pkt_retune2_init();
    profile.profile_num = 3;
    if (enqueue_retune_resident(&rx_queue, &profile, 3, 100) != 1) {
        fputs("failed to enqueue lost-IRQ recovery case\n", stderr);
        return 1;
    }

    compare_fired_rx = false;
    pkt_retune2_work();
    pkt_retune2_work();
    if (rx_queue.count != 1 ||
        pkt_retune2_success_count(BLADERF_MODULE_RX) != 0) {
        fputs("scheduled retune advanced without comparator event\n", stderr);
        return 1;
    }

    /* No ISR is delivered: only the FPGA event may release the queue. */
    compare_fired_rx = true;
    if (!run_until_queue_empty(&rx_queue, 8)) {
        fputs("FPGA comparator event did not finish RFPLL polling\n",
              stderr);
        return 1;
    }
    compare_fired_rx = false;
    if (rx_queue.count != 0 ||
        pkt_retune2_success_count(BLADERF_MODULE_RX) != 1 ||
        pkt_retune2_failure_count(BLADERF_MODULE_RX) != 0) {
        fputs("FPGA comparator event did not recover the lost IRQ\n", stderr);
        return 1;
    }
    return 0;
}

static int check_stale_irq_cannot_release_rearmed_retune(void)
{
    fastlock_profile profile = { 0 };

    pkt_retune2_init();
    recall_count = 0;
    profile.profile_num = 6;
    compare_fired_rx = false;
    if (enqueue_retune_resident(&rx_queue, &profile, 6, 100) != 1) {
        fputs("failed to enqueue stale-IRQ case\n", stderr);
        return 1;
    }

    pkt_retune2_work();
    if (rx_queue.entries[rx_queue.rem_idx].state != ENTRY_STATE_SCHEDULED) {
        fputs("retune did not arm before stale-IRQ case\n", stderr);
        return 1;
    }

    /* This represents an IRQ left pending by an earlier compare/cancel. The
     * new arm cleared its own sticky event, so the old IRQ cannot mark it
     * READY or activate the profile ahead of the new timestamp. */
    retune_isr(&rx_queue, BLADERF_MODULE_RX);
    pkt_retune2_work();
    if (rx_queue.count != 1 ||
        rx_queue.entries[rx_queue.rem_idx].state != ENTRY_STATE_SCHEDULED ||
        pkt_retune2_success_count(BLADERF_MODULE_RX) != 0 ||
        recall_count != 0) {
        fputs("stale IRQ released a retune without its compare event\n",
              stderr);
        return 1;
    }

    compare_fired_rx = true;
    if (!run_until_queue_empty(&rx_queue, 8)) {
        fputs("new compare did not finish RFPLL polling\n", stderr);
        return 1;
    }
    compare_fired_rx = false;
    if (rx_queue.count != 0 ||
        pkt_retune2_success_count(BLADERF_MODULE_RX) != 1 ||
        recall_count != 1) {
        fputs("valid compare event did not release the rearmed retune\n",
              stderr);
        return 1;
    }
    return 0;
}

static int check_stopped_tamer_clock_fails_queued_retunes(void)
{
    fastlock_profile profile = { 0 };

    pkt_retune2_init();
    profile.profile_num = 4;
    if (enqueue_retune_resident(&rx_queue, &profile, 4, 100) != 1 ||
        enqueue_retune_resident(&tx_queue, &profile, 4, 100) != 1) {
        fputs("failed to enqueue stopped-clock cases\n", stderr);
        return 1;
    }

    pkt_retune2_work();
    pkt_retune2_work();
    if (rx_queue.count != 1 || tx_queue.count != 1) {
        fputs("retune did not remain pending before clock-loss event\n",
              stderr);
        return 1;
    }

    /* The independent FPGA watchdog reports loss for each tamer. While the
     * timestamp clock is stopped, report failure once, retain ownership of
     * the armed compare, and avoid a blocking cancel-ACK poll. */
    fail_tamer_cancel = true;
    cancel_count = 0;
    simulated_tamer_clock_stalled = true;
    pkt_retune2_work();
    if (rx_queue.count != 1 || tx_queue.count != 1 ||
        rx_queue.entries[rx_queue.rem_idx].state !=
            ENTRY_STATE_CANCEL_PENDING ||
        tx_queue.entries[tx_queue.rem_idx].state !=
            ENTRY_STATE_CANCEL_PENDING ||
        cancel_count != 0 ||
        pkt_retune2_failure_count(BLADERF_MODULE_RX) != 1 ||
        pkt_retune2_failure_count(BLADERF_MODULE_TX) != 1 ||
        pkt_retune2_success_count(BLADERF_MODULE_RX) != 0 ||
        pkt_retune2_success_count(BLADERF_MODULE_TX) != 0) {
        fputs("unacknowledged cancel lost RX/TX retune ownership\n", stderr);
        return 1;
    }

    /* While the clock remains stalled, servicing must not repeat the
     * million-read cancellation watchdog. */
    pkt_retune2_work();
    if (rx_queue.count != 1 || tx_queue.count != 1 ||
        cancel_count != 0 ||
        pkt_retune2_failure_count(BLADERF_MODULE_RX) != 1 ||
        pkt_retune2_failure_count(BLADERF_MODULE_TX) != 1) {
        fputs("stalled cancel-pending path retried or duplicated failure\n",
              stderr);
        return 1;
    }

    /* Recovery starts cancellation once; subsequent worker passes poll the
     * FPGA acknowledgement without blocking or issuing duplicate requests. */
    simulated_tamer_clock_stalled = false;
    pkt_retune2_work();
    pkt_retune2_work();
    if (rx_queue.count != 1 || tx_queue.count != 1 ||
        cancel_count != 2 ||
        pkt_retune2_failure_count(BLADERF_MODULE_RX) != 1 ||
        pkt_retune2_failure_count(BLADERF_MODULE_TX) != 1) {
        fputs("cancel recovery retry was not bounded\n", stderr);
        return 1;
    }

    simulated_tamer_clock_stalled = true;
    pkt_retune2_work();
    simulated_tamer_clock_stalled = false;
    fail_tamer_cancel = false;
    pkt_retune2_work();
    pkt_retune2_work();
    if (rx_queue.count != 0 || tx_queue.count != 0 || cancel_count != 4 ||
        pkt_retune2_success_count(BLADERF_MODULE_RX) != 0 ||
        pkt_retune2_success_count(BLADERF_MODULE_TX) != 0) {
        fprintf(stderr, "acknowledged cancel left entries: rx=%u tx=%u "
                "cancels=%u rx_fail=%u tx_fail=%u\n",
                rx_queue.count, tx_queue.count, cancel_count,
                pkt_retune2_failure_count(BLADERF_MODULE_RX),
                pkt_retune2_failure_count(BLADERF_MODULE_TX));
        return 1;
    }
    return 0;
}

static int check_clear_queue_does_not_poll_a_stopped_tamer(void)
{
    struct pkt_buf request = { 0 };
    fastlock_profile profile = { 0 };

    pkt_retune2_init();
    profile.profile_num = 2;
    if (enqueue_retune_resident(&rx_queue, &profile, 2, 100) != 1) {
        fputs("failed to set up stopped-tamer queue-clear case\n", stderr);
        return 1;
    }

    cancel_count = 0;
    fail_tamer_cancel = false;
    simulated_tamer_clock_stalled = true;
    nios_pkt_retune2_pack(request.req, BLADERF_MODULE_RX,
                          NIOS_PKT_RETUNE2_CLEAR_QUEUE, 0, 0, 0, 0);
    pkt_retune2(&request);
    if ((request.resp[9] & NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS) != 0 ||
        (request.resp[9] & NIOS_PKT_RETUNE2_RESP_FLAG_CANCEL_FAILED) == 0 ||
        (request.resp[9] & NIOS_PKT_RETUNE2_RESP_FLAG_CANCEL_PENDING) != 0 ||
        rx_queue.count != 1 || cancel_count != 0) {
        fputs("stopped-tamer queue clear did not report failure fail-closed\n",
              stderr);
        return 1;
    }
    if (enqueue_retune_resident(&rx_queue, &profile, 2, 200) !=
            QUEUE_FULL || rx_queue.count != 1) {
        fputs("queue accepted a retune while asynchronous clear was pending\n",
              stderr);
        return 1;
    }

    simulated_tamer_clock_stalled = false;
    pkt_retune2_work();
    pkt_retune2(&request);
    if ((request.resp[9] & NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS) != 0 ||
        (request.resp[9] & NIOS_PKT_RETUNE2_RESP_FLAG_CANCEL_PENDING) == 0 ||
        rx_queue.count != 1 || cancel_count != 1) {
        fputs("recovered queue clear did not report pending cancellation\n",
              stderr);
        return 1;
    }

    /* A timestamp-domain reset can erase the in-flight cancel FSM before its
     * ACK toggle reaches NIOS. The worker must retry after clock recovery. */
    simulated_tamer_clock_stalled = true;
    fail_tamer_cancel = true;
    pkt_retune2_work();
    if (rx_queue.count != 1 ||
        rx_queue.entries[rx_queue.rem_idx].cancel_request_pending) {
        fputs("clock stall did not release the lost queue-clear ACK wait\n",
              stderr);
        return 1;
    }
    simulated_tamer_clock_stalled = false;
    fail_tamer_cancel = false;
    pkt_retune2_work();
    if (!rx_queue.entries[rx_queue.rem_idx].cancel_request_pending ||
        cancel_count != 2) {
        fputs("queue clear did not retry cancellation after clock recovery\n",
              stderr);
        return 1;
    }
    pkt_retune2_work();
    pkt_retune2(&request);
    if ((request.resp[9] & NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS) == 0 ||
        rx_queue.count != 0 || cancel_count != 2) {
        fputs("queue clear did not complete after cancel-ACK retry\n",
              stderr);
        return 1;
    }

    /* Even an empty software queue may follow a NIOS restart with a compare
     * still armed in FPGA. Queue clear must obtain a hardware cancel ACK. */
    pkt_retune2_init();
    cancel_count = 0;
    nios_pkt_retune2_pack(request.req, BLADERF_MODULE_RX,
                          NIOS_PKT_RETUNE2_CLEAR_QUEUE, 0, 0, 0, 0);
    pkt_retune2(&request);
    if ((request.resp[9] & NIOS_PKT_RETUNE2_RESP_FLAG_CANCEL_PENDING) == 0 ||
        rx_queue.count != 0 || cancel_count != 1) {
        fputs("empty queue clear skipped FPGA comparator cancellation\n",
              stderr);
        return 1;
    }
    simulated_tamer_clock_stalled = true;
    fail_tamer_cancel = true;
    pkt_retune2_work();
    if (!rx_queue.clear_requested || rx_queue.clear_cancel_pending) {
        fputs("empty queue clear retained a lost cancel ACK across reset\n",
              stderr);
        return 1;
    }
    pkt_retune2(&request);
    if ((request.resp[9] & NIOS_PKT_RETUNE2_RESP_FLAG_CANCEL_FAILED) == 0 ||
        (request.resp[9] & NIOS_PKT_RETUNE2_RESP_FLAG_CANCEL_PENDING) != 0) {
        fputs("empty queue clear did not report the stopped cancel domain\n",
              stderr);
        return 1;
    }
    simulated_tamer_clock_stalled = false;
    fail_tamer_cancel = false;
    pkt_retune2_work();
    if (!rx_queue.clear_cancel_pending || cancel_count != 2) {
        fputs("empty queue clear did not retry cancel after reset\n",
              stderr);
        return 1;
    }
    pkt_retune2_work();
    pkt_retune2(&request);
    if ((request.resp[9] & NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS) == 0 ||
        cancel_count != 2) {
        fputs("empty queue clear did not complete after FPGA cancel ACK\n",
              stderr);
        return 1;
    }

    return 0;
}

static int check_clear_queue_fences_compare_irq(void)
{
    struct pkt_buf request = { 0 };
    fastlock_profile profile = { 0 };

    pkt_retune2_init();
    profile.profile_num = 3;
    tamer_timestamp = 0;
    simulated_tamer_clock_stalled = false;
    fail_tamer_schedule = false;
    fail_tamer_schedule_ack = false;
    fail_tamer_schedule_status = false;
    fail_tamer_cancel = false;
    compare_fired_rx = false;
    recall_count = 0;
    if (enqueue_retune_resident(&rx_queue, &profile, 3, 100) != 1) {
        fputs("failed to enqueue clear/IRQ race case\n", stderr);
        return 1;
    }
    pkt_retune2_work();
    simulate_tamer_irq(&rx_queue, BLADERF_MODULE_RX);
    if (rx_queue.entries[rx_queue.rem_idx].state != ENTRY_STATE_READY) {
        fputs("compare IRQ did not prepare the clear/IRQ race\n", stderr);
        return 1;
    }

    nios_pkt_retune2_pack(request.req, BLADERF_MODULE_RX,
                          NIOS_PKT_RETUNE2_CLEAR_QUEUE, 0, 0, 0, 0);
    pkt_retune2(&request);
    if (!rx_queue.clear_requested ||
        rx_queue.entries[rx_queue.rem_idx].state != ENTRY_STATE_CANCEL_PENDING) {
        fputs("queue clear did not take ownership of the ready compare\n",
              stderr);
        return 1;
    }

    simulate_tamer_irq(&rx_queue, BLADERF_MODULE_RX);
    if (rx_queue.entries[rx_queue.rem_idx].state != ENTRY_STATE_CANCEL_PENDING ||
        recall_count != 0) {
        fputs("compare IRQ resurrected a head after queue clear\n", stderr);
        return 1;
    }

    pkt_retune2_work();
    pkt_retune2(&request);
    if ((request.resp[9] & NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS) == 0 ||
        rx_queue.count != 0 || recall_count != 0) {
        fputs("queue clear failed to retire the raced compare\n", stderr);
        return 1;
    }
    return 0;
}

static int check_cancel_ack_wait_is_nonblocking(void)
{
    fastlock_profile profile = { 0 };

    pkt_retune2_init();
    profile.profile_num = 5;
    fail_tamer_schedule = true;
    fail_tamer_cancel = true;
    simulated_tamer_clock_stalled = false;
    cancel_count = 0;
    if (enqueue_retune_resident(&rx_queue, &profile, 5, 100) != 1) {
        fputs("failed to set up asynchronous cancel-ACK case\n", stderr);
        return 1;
    }

    pkt_retune2_work();
    if (rx_queue.entries[rx_queue.rem_idx].state !=
            ENTRY_STATE_CANCEL_PENDING || cancel_count != 0) {
        fputs("schedule failure did not enter cancel-pending state\n",
              stderr);
        return 1;
    }

    /* Start cancellation once, then let the worker poll the FPGA toggle on
     * subsequent passes. A missing ACK must not trap the command loop in a
     * million-read synchronous wait or cause duplicate cancel commands. */
    pkt_retune2_work();
    pkt_retune2_work();
    if (rx_queue.count != 1 || cancel_count != 1 ||
        !rx_queue.entries[rx_queue.rem_idx].cancel_request_pending) {
        fputs("missing cancel ACK blocked or duplicated cancellation\n",
              stderr);
        return 1;
    }

    fail_tamer_cancel = false;
    pkt_retune2_work();
    fail_tamer_schedule = false;
    if (rx_queue.count != 0 || cancel_count != 1 ||
        pkt_retune2_failure_count(BLADERF_MODULE_RX) != 1) {
        fputs("asynchronous cancel ACK did not retire failed request\n",
              stderr);
        return 1;
    }
    return 0;
}

static int check_schedule_ack_wait_is_nonblocking(void)
{
    fastlock_profile profile = { 0 };

    pkt_retune2_init();
    profile.profile_num = 6;
    fail_tamer_schedule = false;
    fail_tamer_schedule_ack = true;
    simulated_tamer_clock_stalled = false;
    cancel_count = 0;
    if (enqueue_retune_resident(&rx_queue, &profile, 6, 100) != 1) {
        fputs("failed to set up asynchronous schedule-ACK case\n", stderr);
        return 1;
    }

    pkt_retune2_work();
    pkt_retune2_work();
    if (rx_queue.count != 1 ||
        rx_queue.entries[rx_queue.rem_idx].state !=
            ENTRY_STATE_SCHEDULE_PENDING || cancel_count != 0 ||
        pkt_retune2_failure_count(BLADERF_MODULE_RX) != 0) {
        fputs("missing schedule ACK blocked or failed the request\n",
              stderr);
        return 1;
    }

    fail_tamer_schedule_ack = false;
    pkt_retune2_work();
    if (rx_queue.entries[rx_queue.rem_idx].state != ENTRY_STATE_SCHEDULED) {
        fputs("schedule ACK did not advance the pending request\n", stderr);
        return 1;
    }
    compare_fired_rx = true;
    pkt_retune2_work();
    pkt_retune2_work();
    compare_fired_rx = false;
    if (rx_queue.count != 0 ||
        pkt_retune2_success_count(BLADERF_MODULE_RX) != 1 ||
        pkt_retune2_failure_count(BLADERF_MODULE_RX) != 0) {
        fputs("acknowledged scheduled request did not complete\n", stderr);
        return 1;
    }
    return 0;
}

static int check_schedule_no_response_is_reported(void)
{
    fastlock_profile profile = { 0 };

    pkt_retune2_init();
    profile.profile_num = 8;
    fail_tamer_schedule = false;
    fail_tamer_schedule_ack = false;
    fail_tamer_schedule_status = true;
    simulated_tamer_clock_stalled = false;
    fail_tamer_cancel = false;
    cancel_count = 0;
    if (enqueue_retune_resident(&rx_queue, &profile, 8, 100) != 1) {
        fputs("failed to set up schedule no-response case\n", stderr);
        return 1;
    }

    pkt_retune2_work();
    pkt_retune2_work();
    if (rx_queue.count != 1 ||
        rx_queue.entries[rx_queue.rem_idx].state !=
            ENTRY_STATE_CANCEL_PENDING || cancel_count != 1 ||
        pkt_retune2_failure_count(BLADERF_MODULE_RX) != 1) {
        fprintf(stderr, "schedule no-response state mismatch: count=%u "
                "state=%d cancels=%u failures=%u\n", rx_queue.count,
                rx_queue.count != 0
                    ? (int)rx_queue.entries[rx_queue.rem_idx].state : -1,
                cancel_count,
                pkt_retune2_failure_count(BLADERF_MODULE_RX));
        return 1;
    }

    fail_tamer_schedule_status = false;
    pkt_retune2_work();
    pkt_retune2_work();
    if (rx_queue.count != 0 || cancel_count != 1 ||
        pkt_retune2_success_count(BLADERF_MODULE_RX) != 0 ||
        pkt_retune2_failure_count(BLADERF_MODULE_RX) != 1) {
        fputs("schedule no-response was not cancelled as a failure\n",
              stderr);
        return 1;
    }
    return 0;
}

static int check_cancel_no_response_is_reported_fail_closed(void)
{
    struct pkt_buf request = { 0 };
    fastlock_profile profile = { 0 };
    struct queue_entry *entry;

    pkt_retune2_init();
    profile.profile_num = 4;
    if (enqueue_retune_resident(&rx_queue, &profile, 4, 100) != 1) {
        fputs("failed to set up cancel no-response case\n", stderr);
        return 1;
    }
    entry = &rx_queue.entries[rx_queue.rem_idx];
    entry->state = ENTRY_STATE_CANCEL_PENDING;
    entry->cancel_request_pending = true;
    entry->cancel_recovery_seen = true;
    rx_queue.clear_requested = true;
    cancel_count = 0;
    fail_tamer_cancel = true;
    fail_tamer_cancel_status = true;

    pkt_retune2_work();
    nios_pkt_retune2_pack(request.req, BLADERF_MODULE_RX,
                          NIOS_PKT_RETUNE2_CLEAR_QUEUE, 0, 0, 0, 0);
    pkt_retune2(&request);
    if ((request.resp[9] & NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS) != 0 ||
        (request.resp[9] & NIOS_PKT_RETUNE2_RESP_FLAG_CANCEL_FAILED) == 0 ||
        (request.resp[9] & NIOS_PKT_RETUNE2_RESP_FLAG_CANCEL_PENDING) != 0 ||
        rx_queue.count != 1 || entry->state != ENTRY_STATE_CANCEL_PENDING ||
        !entry->cancel_request_pending || cancel_count != 1 ||
        pkt_retune2_success_count(BLADERF_MODULE_RX) != 0) {
        fputs("cancel no-response was not surfaced fail-closed\n", stderr);
        return 1;
    }

    fail_tamer_cancel = false;
    fail_tamer_cancel_status = false;
    pkt_retune2_work();
    pkt_retune2(&request);
    if ((request.resp[9] & NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS) == 0 ||
        rx_queue.count != 0 || pkt_retune2_success_count(BLADERF_MODULE_RX) != 0) {
        fputs("explicit retry did not complete the pending queue clear\n",
              stderr);
        return 1;
    }
    return 0;
}

static int check_compare_irq_during_schedule_ack(void)
{
    fastlock_profile profile = { 0 };

    pkt_retune2_init();
    profile.profile_num = 7;
    fail_tamer_schedule = false;
    fail_tamer_schedule_ack = false;
    fire_during_schedule = true;
    tamer_timestamp = 0;
    compare_fired_rx = false;
    recall_count = 0;
    if (enqueue_retune_resident(&rx_queue, &profile, 7, 1) != 1) {
        fire_during_schedule = false;
        fputs("failed to enqueue compare-during-arm case\n", stderr);
        return 1;
    }

    pkt_retune2_work();
    fire_during_schedule = false;
    if (rx_queue.count != 1 ||
        rx_queue.entries[rx_queue.rem_idx].state != ENTRY_STATE_READY ||
        recall_count != 0) {
        fputs("schedule ACK overwrote a compare IRQ received during ARM\n",
              stderr);
        return 1;
    }

    if (!run_until_queue_empty(&rx_queue, 8) || recall_count != 1 ||
        pkt_retune2_success_count(BLADERF_MODULE_RX) != 1) {
        fputs("compare received during ARM was not activated\n", stderr);
        return 1;
    }
    return 0;
}

static int check_deferred_failure_reporting(void)
{
    fastlock_profile rx_profile = { 0 };

    pkt_retune2_init();
    rx_profile.profile_num = 4;
    if (enqueue_retune_resident(&rx_queue, &rx_profile, 4,
                       UINT64_C(0x456789abc)) != 1) {
        fputs("failed to enqueue RX activation failure case\n", stderr);
        return 1;
    }
    pkt_retune2_work();
    simulate_tamer_irq(&rx_queue, BLADERF_MODULE_RX);
    fail_profile_recall = true;
    if (!run_until_queue_empty(&rx_queue, 8)) {
        fail_profile_recall = false;
        fputs("failed RFPLL recall did not retire scheduled request\n",
              stderr);
        return 1;
    }
    fail_profile_recall = false;
    if (rx_queue.count != 0 ||
        pkt_retune2_failure_count(BLADERF_MODULE_RX) != 1 ||
        pkt_retune2_failure_count(BLADERF_MODULE_TX) != 0 ||
        pkt_retune2_success_count(BLADERF_MODULE_RX) != 0) {
        fputs("deferred RX activation failure was not retained\n", stderr);
        return 1;
    }

    return 0;
}

static int check_scheduled_profile_upload_failure_is_terminal(void)
{
    fastlock_profile tx_profile = { 0 };

    pkt_retune2_init();
    tx_profile.profile_num = 5;
    tx_profile.state = FASTLOCK_STATE_BBP;
    if (enqueue_retune_with_identity(&tx_queue, &tx_profile, 5, 10,
                                     0, 0) != 1) {
        fputs("failed to enqueue scheduled upload failure case\n", stderr);
        return 1;
    }

    staged_profile_fail_after_write = 0;
    pkt_retune2_work(); /* Start retained upload without SPI. */
    pkt_retune2_work(); /* First attempted profile SPI write fails. */
    staged_profile_fail_after_write = -1;
    if (pkt_retune2_failure_count(BLADERF_MODULE_TX) != 1 ||
        tx_queue.count != 1 ||
        tx_queue.entries[tx_queue.rem_idx].state !=
            ENTRY_STATE_PROFILE_LOAD_FAILED) {
        fputs("scheduled upload failure was not reported immediately\n",
              stderr);
        return 1;
    }
    pkt_retune2_work();
    if (pkt_retune2_failure_count(BLADERF_MODULE_TX) != 1 ||
        tx_queue.count != 0 || staged_profile_load_active) {
        fputs("terminal upload failure did not retire queue ownership\n",
              stderr);
        return 1;
    }

    /* If CLEAR arrives from the cooperative hook during the failing SPI
     * transaction, retain PROFILE_LOAD as the terminal cause and clear only
     * the unarmed queue. */
    pkt_retune2_init();
    tx_profile.profile_num = 6;
    tx_profile.state = FASTLOCK_STATE_BBP;
    if (enqueue_retune_with_identity(&tx_queue, &tx_profile, 6, 20,
                                     0x10203040, 0x3344) != 1) {
        fputs("failed to enqueue upload-failure/CLEAR race case\n", stderr);
        return 1;
    }
    staged_profile_fail_after_write = 0;
    pkt_retune2_work();
    defer_clear_during_staged_step = true;
    pkt_retune2_work();
    staged_profile_fail_after_write = -1;
    if (defer_clear_during_staged_step || tx_queue.count != 0 ||
        !tx_queue.clear_complete || staged_profile_load_active ||
        pkt_retune2_failure_count(BLADERF_MODULE_TX) != 1 ||
        !pkt_retune2_failure_completion_select(BLADERF_MODULE_TX, 1) ||
        pkt_retune2_completion_word(4) !=
            NIOS_PKT_RETUNE2_FAILURE_PROFILE_LOAD ||
        pkt_retune2_completion_word(5) != 0x10203040 ||
        pkt_retune2_completion_word(6) != 0x3344) {
        fputs("CLEAR masked or detached the profile SPI failure\n", stderr);
        return 1;
    }

    return 0;
}

static int check_tamer_read_and_arm_fail_closed(void)
{
    fastlock_profile profile = { 0 };
    unsigned schedules_before;

    pkt_retune2_init();
    profile.profile_num = 2;
    if (enqueue_retune_resident(&rx_queue, &profile, 2, 10) != 1) {
        fputs("failed to enqueue tamer read failure case\n", stderr);
        return 1;
    }
    schedules_before = schedule_count;
    fail_tamer_read = true;
    pkt_retune2_work();
    fail_tamer_read = false;
    if (rx_queue.count != 0 ||
        pkt_retune2_failure_count(BLADERF_MODULE_RX) != 1 ||
        schedule_count != schedules_before) {
        fputs("tamer snapshot failure did not fail closed\n", stderr);
        return 1;
    }

    pkt_retune2_init();
    if (enqueue_retune_resident(&rx_queue, &profile, 2, 10) != 1) {
        fputs("failed to enqueue tamer arm failure case\n", stderr);
        return 1;
    }
    schedule_count = 0;
    cancel_count = 0;
    fail_tamer_cancel = false;
    fail_tamer_cancel_status = false;
    simulated_tamer_clock_stalled = false;
    fail_tamer_schedule = true;
    pkt_retune2_work();
    fail_tamer_schedule = false;
    if (rx_queue.count != 1 ||
        rx_queue.entries[rx_queue.rem_idx].state !=
            ENTRY_STATE_CANCEL_PENDING ||
        pkt_retune2_failure_count(BLADERF_MODULE_RX) != 1 ||
        schedule_count == 0) {
        fprintf(stderr, "tamer arm timeout mismatch: count=%u failures=%u "
                "schedules=%u\n", rx_queue.count,
                pkt_retune2_failure_count(BLADERF_MODULE_RX),
                schedule_count);
        return 1;
    }
    pkt_retune2_work();
    pkt_retune2_work();
    if (rx_queue.count != 0 ||
        pkt_retune2_failure_count(BLADERF_MODULE_RX) != 1) {
        fprintf(stderr, "tamer arm cancel ACK pending: count=%u state=%d "
                "failures=%u cancels=%u\n", rx_queue.count,
                rx_queue.count != 0
                    ? (int)rx_queue.entries[rx_queue.rem_idx].state : -1,
                pkt_retune2_failure_count(BLADERF_MODULE_RX), cancel_count);
        return 1;
    }

    return 0;
}

static int check_failed_schedule_retains_timer_ownership(void)
{
    fastlock_profile profile = { 0 };

    pkt_retune2_init();
    profile.profile_num = 3;
    fail_tamer_schedule = true;
    fail_tamer_cancel = true;
    simulated_tamer_clock_stalled = true;
    cancel_count = 0;
    schedule_count = 0;
    recall_count = 0;
    if (enqueue_retune_resident(&rx_queue, &profile, 3, 10) != 1 ||
        enqueue_retune_resident(&rx_queue, &profile, 3, 20) != 2) {
        fputs("failed to enqueue schedule/cancel ownership cases\n", stderr);
        return 1;
    }

    pkt_retune2_work();
    if (rx_queue.count != 2 ||
        rx_queue.entries[rx_queue.rem_idx].state !=
            ENTRY_STATE_CANCEL_PENDING ||
        pkt_retune2_failure_count(BLADERF_MODULE_RX) != 1 ||
        schedule_count != 1 || cancel_count != 0) {
        fputs("schedule timeout released an unacknowledged compare\n",
              stderr);
        return 1;
    }

    /* A late compare and its IRQ cannot activate either request while the
     * failed schedule still owns the comparator. */
    compare_fired_rx = true;
    simulate_tamer_irq(&rx_queue, BLADERF_MODULE_RX);
    if (!run_until_queue_empty(&rx_queue, 8)) {
        fputs("recovered schedule did not finish RFPLL polling\n", stderr);
        return 1;
    }
    if (rx_queue.count != 2 ||
        rx_queue.entries[rx_queue.rem_idx].state !=
            ENTRY_STATE_CANCEL_PENDING ||
        recall_count != 0 || schedule_count != 1) {
        fputs("late compare activated a request before cancel ACK\n", stderr);
        return 1;
    }

    fail_tamer_schedule = false;
    pkt_retune2_work();
    if (rx_queue.count != 2 || schedule_count != 1 || cancel_count != 0) {
        fputs("stalled compare ownership advanced the following request\n",
              stderr);
        return 1;
    }

    simulated_tamer_clock_stalled = false;
    pkt_retune2_work();
    if (rx_queue.count != 2 || schedule_count != 1 || cancel_count != 1) {
        fputs("failed cancel retry released the compare owner\n", stderr);
        return 1;
    }

    simulated_tamer_clock_stalled = true;
    pkt_retune2_work();
    fail_tamer_cancel = false;
    simulated_tamer_clock_stalled = false;
    pkt_retune2_work();
    pkt_retune2_work();
    if (rx_queue.count != 1 || schedule_count != 1 || cancel_count != 2 ||
        rx_queue.entries[rx_queue.rem_idx].state != ENTRY_STATE_NEW) {
        fputs("acknowledged cancellation did not release queue ownership\n",
              stderr);
        return 1;
    }

    pkt_retune2_work();
    if (rx_queue.count != 1 || schedule_count != 2 ||
        rx_queue.entries[rx_queue.rem_idx].state != ENTRY_STATE_SCHEDULED ||
        scheduled_timestamp != 20 || recall_count != 0) {
        fputs("next compare did not wait for prior cancellation\n", stderr);
        return 1;
    }

    compare_fired_rx = true;
    simulate_tamer_irq(&rx_queue, BLADERF_MODULE_RX);
    if (!run_until_queue_empty(&rx_queue, 8) || recall_count != 1 ||
        pkt_retune2_success_count(BLADERF_MODULE_RX) != 1) {
        fputs("recovered schedule did not complete after new compare\n",
              stderr);
        return 1;
    }

    simulated_tamer_clock_stalled = false;
    fail_tamer_schedule = false;
    fail_tamer_cancel = false;
    return 0;
}

static int check_timestamp_wrap_ordering(void)
{
    fastlock_profile profile = { 0 };
    struct queue ordered = { 0 };

    if (!timestamp_is_strictly_after(UINT64_C(2), UINT64_MAX - 3) ||
        timestamp_is_strictly_after(UINT64_C(0x8000000000000000), 0) ||
        timestamp_is_strictly_after(UINT64_MAX - 3, UINT64_C(2)) ||
        timestamp_is_strictly_after(10, 10)) {
        fputs("modulo timestamp ordering failed boundary checks\n", stderr);
        return 1;
    }

    profile.profile_num = 3;
    if (enqueue_retune_resident(&ordered, &profile, 3, UINT64_MAX - 3) != 1 ||
        enqueue_retune_resident(&ordered, &profile, 3, UINT64_C(2)) != 2 ||
        enqueue_retune_resident(&ordered, &profile, 3, UINT64_MAX - 1) !=
            QUEUE_INVALID_TIMESTAMP) {
        fputs("retune queue rejected wrapped order or accepted stale order\n",
              stderr);
        return 1;
    }

    pkt_retune2_init();
    tamer_timestamp = UINT64_MAX - 4;
    if (enqueue_retune_resident(&rx_queue, &profile, 3, UINT64_C(2)) != 1) {
        fputs("failed to enqueue wrapped future retune\n", stderr);
        return 1;
    }
    pkt_retune2_work();
    if (rx_queue.count != 1 ||
        rx_queue.entries[rx_queue.rem_idx].state != ENTRY_STATE_SCHEDULED ||
        schedule_count == 0 || scheduled_timestamp != UINT64_C(2) ||
        pkt_retune2_failure_count(BLADERF_MODULE_RX) != 0) {
        fputs("wrapped future retune was not scheduled\n", stderr);
        return 1;
    }

    tamer_timestamp = 0;
    return 0;
}

static int check_invalid_queue_state_fails_closed(void)
{
    pkt_retune2_init();
    cancel_count = 0;
    fail_tamer_cancel = false;
    simulated_tamer_clock_stalled = false;
    rx_queue.count = 1;
    rx_queue.rem_idx = 0;
    rx_queue.ins_idx = 1;
    rx_queue.entries[0].state = (enum entry_state)0xff;

    pkt_retune2_work();
    if (pkt_retune2_failure_count(BLADERF_MODULE_RX) != 1 ||
        pkt_retune2_success_count(BLADERF_MODULE_RX) != 0 ||
        rx_queue.count != 1 || cancel_count != 0 ||
        rx_queue.entries[0].state != ENTRY_STATE_CANCEL_PENDING) {
        fprintf(stderr, "invalid state mismatch: failures=%u successes=%u "
                "count=%u cancels=%u state=%d\n",
                pkt_retune2_failure_count(BLADERF_MODULE_RX),
                pkt_retune2_success_count(BLADERF_MODULE_RX),
                rx_queue.count, cancel_count,
                (int)rx_queue.entries[0].state);
        return 1;
    }
    pkt_retune2_work();
    if (rx_queue.count != 1 || cancel_count != 1) {
        fputs("invalid retune did not issue cancel request\n", stderr);
        return 1;
    }
    pkt_retune2_work();
    if (rx_queue.count != 0) {
        fputs("invalid retune state survived acknowledged cancel\n", stderr);
        return 1;
    }

    /* If the hardware does not acknowledge cancellation, the request must
     * remain owned in CANCEL_PENDING and must never activate by a late event. */
    pkt_retune2_init();
    cancel_count = 0;
    fail_tamer_cancel = true;
    rx_queue.count = 1;
    rx_queue.rem_idx = 0;
    rx_queue.ins_idx = 1;
    rx_queue.entries[0].state = (enum entry_state)0xff;
    pkt_retune2_work();
    compare_fired_rx = true;
    pkt_retune2_work();
    if (pkt_retune2_failure_count(BLADERF_MODULE_RX) != 1 ||
        pkt_retune2_success_count(BLADERF_MODULE_RX) != 0 ||
        rx_queue.count != 1 ||
        rx_queue.entries[0].state != ENTRY_STATE_CANCEL_PENDING) {
        fputs("uncancelled invalid retune state was falsely released\n", stderr);
        return 1;
    }

    fail_tamer_cancel = false;
    simulated_tamer_clock_stalled = true;
    pkt_retune2_work();
    simulated_tamer_clock_stalled = false;
    pkt_retune2_work();
    pkt_retune2_work();
    if (rx_queue.count != 0 ||
        pkt_retune2_success_count(BLADERF_MODULE_RX) != 0) {
        fputs("invalid retune state survived acknowledged recovery cancel\n",
              stderr);
        return 1;
    }
    return 0;
}

static int check_profile_index_validation(void)
{
    if (!retune2_profile_indices_valid(0, 0) ||
        !retune2_profile_indices_valid(NUM_BBP_FASTLOCK_PROFILES - 1,
                                       NUM_RFFE_FASTLOCK_PROFILES - 1) ||
        retune2_profile_indices_valid(NUM_BBP_FASTLOCK_PROFILES, 0) ||
        retune2_profile_indices_valid(0, NUM_RFFE_FASTLOCK_PROFILES) ||
        retune2_profile_indices_valid(UINT16_MAX, UINT8_MAX)) {
        fputs("retune2 profile index validation accepted invalid input\n",
              stderr);
        return 1;
    }
    return 0;
}

static int check_completion_ring_overwrite_is_visible(void)
{
    const uint32_t requested_sequence = 1;
    const uint32_t overwritten_sequence =
        requested_sequence + NIOS_PKT_8x32_RETUNE2_COMPLETION_DEPTH;
    struct retune2_completion *record =
        &tx_retune2_completions[(requested_sequence - 1u) &
            (NIOS_PKT_8x32_RETUNE2_COMPLETION_DEPTH - 1u)];

    /* Model a full ring lap while the reader still requests the old slot.
     * It must reject during the payload write and after the new publish. */
    record->sequence = requested_sequence;
    retune2_completion_begin_write(record, overwritten_sequence);
    if (!pkt_retune2_completion_select(BLADERF_MODULE_TX,
                                       requested_sequence) ||
        pkt_retune2_completion_word(
            NIOS_PKT_8x32_RETUNE2_COMPLETION_SEQUENCE) ==
                requested_sequence) {
        fputs("completion reader accepted an in-progress ring slot\n",
              stderr);
        return 1;
    }

    retune2_completion_publish(record, overwritten_sequence,
                               1800000000u, 0x100000002ull, 5, 17, 0x4567);
    if (!pkt_retune2_completion_select(BLADERF_MODULE_TX,
                                       requested_sequence) ||
        pkt_retune2_completion_word(
            NIOS_PKT_8x32_RETUNE2_COMPLETION_SEQUENCE) !=
                overwritten_sequence ||
        pkt_retune2_completion_word(
            NIOS_PKT_8x32_RETUNE2_COMPLETION_SESSION_GENERATION) != 17 ||
        pkt_retune2_completion_word(
            NIOS_PKT_8x32_RETUNE2_COMPLETION_REQUEST_TOKEN) != 0x4567) {
        fputs("completion ring overwrite was not externally visible\n",
              stderr);
        return 1;
    }
    return 0;
}

static int check_txq_yield_preserves_retune_identity(void)
{
    fastlock_profile profile = { 0 };
    bool cancel_failed = false;
    unsigned cancel_before;

    pkt_retune2_init();
    simulate_txq_yield = true;
    simulated_txq_owner = false;
    simulated_txq_terminal_status = 0;
    simulated_rfic_faulted = false;
    compare_fired_rx = false;
    compare_fired_tx = false;
    fail_tamer_schedule = false;
    fail_tamer_schedule_ack = false;
    fail_tamer_schedule_status = false;
    profile.profile_num = 2;
    profile.port = 0x22;
    profile.spdt = 0x03;
    profile.state = FASTLOCK_STATE_BBP;
    if (enqueue_retune_with_identity(&tx_queue, &profile, 0x1234, 100,
                                     0x89abcdef, 0x4567) != 1) {
        fputs("failed to enqueue TXQ continuation case\n", stderr);
        return 1;
    }
    if (!run_until_queue_head_state(&tx_queue, ENTRY_STATE_SCHEDULED, 24)) {
        fputs("TXQ test profile did not reach scheduled state\n", stderr);
        return 1;
    }
    simulate_tamer_irq(&tx_queue, BLADERF_MODULE_TX);
    if (!run_until_queue_head_state(&tx_queue, ENTRY_STATE_TXQ_PENDING, 8)) {
        fputs("TXQ continuation did not acquire queue ownership\n", stderr);
        return 1;
    }
    if (!simulated_txq_owner ||
        tx_queue.entries[tx_queue.rem_idx].state != ENTRY_STATE_TXQ_PENDING ||
        pkt_retune2_success_count(BLADERF_MODULE_TX) != 0 ||
        pkt_retune2_failure_count(BLADERF_MODULE_TX) != 0) {
        fputs("TXQ start was reported as a completed fastlock retune\n",
              stderr);
        return 1;
    }

    /* A clear request cannot discard the in-flight ADI context or its
     * correlation identity; it waits for a terminal calibration outcome. */
    cancel_before = cancel_count;
    if (clear_queue_async(&tx_queue, BLADERF_MODULE_TX, &cancel_failed) ||
        cancel_failed || cancel_count != cancel_before ||
        !tx_queue.clear_requested || !simulated_txq_owner) {
        fputs("queue clear cancelled or detached an active TXQ job\n",
              stderr);
        return 1;
    }
    pkt_retune2_work();
    if (!simulated_txq_owner ||
        pkt_retune2_success_count(BLADERF_MODULE_TX) != 0) {
        fputs("TXQ pending poll released request ownership early\n", stderr);
        return 1;
    }
    pkt_retune2_work();
    if (simulated_txq_owner || tx_queue.count != 0 ||
        pkt_retune2_success_count(BLADERF_MODULE_TX) != 1 ||
        !pkt_retune2_completion_select(BLADERF_MODULE_TX, 1) ||
        pkt_retune2_completion_word(1) != UINT32_C(1835000000) ||
        pkt_retune2_completion_word(3) != 0x1234 ||
        pkt_retune2_completion_word(6) != 0x89abcdef ||
        pkt_retune2_completion_word(7) != 0x4567 ||
        !clear_queue_async(&tx_queue, BLADERF_MODULE_TX, &cancel_failed)) {
        fputs("TXQ success lost readback or request identity\n", stderr);
        return 1;
    }

    pkt_retune2_init();
    simulate_txq_yield = true;
    simulated_txq_owner = false;
    simulated_txq_terminal_status = -5;
    compare_fired_rx = false;
    compare_fired_tx = false;
    if (enqueue_retune_with_identity(&tx_queue, &profile, 0x4321, 200,
                                     0x76543210, 0x7654) != 1) {
        fputs("failed to enqueue TXQ failure case\n", stderr);
        return 1;
    }
    pkt_retune2_work();
    simulate_tamer_irq(&tx_queue, BLADERF_MODULE_TX);
    pkt_retune2_work();
    pkt_retune2_work();
    pkt_retune2_work();
    if (simulated_txq_owner || tx_queue.count != 0 ||
        pkt_retune2_success_count(BLADERF_MODULE_TX) != 0 ||
        pkt_retune2_failure_count(BLADERF_MODULE_TX) != 1 ||
        !pkt_retune2_failure_completion_select(BLADERF_MODULE_TX, 1) ||
        pkt_retune2_completion_word(3) != 0x4321 ||
        pkt_retune2_completion_word(4) !=
            NIOS_PKT_RETUNE2_FAILURE_TX_QUAD_CALIBRATION ||
        pkt_retune2_completion_word(5) != 0x76543210 ||
        pkt_retune2_completion_word(6) != 0x7654) {
        fputs("TXQ failure was not correlated to the pending retune\n",
              stderr);
        return 1;
    }

    simulate_txq_yield = false;
    return 0;
}

static int check_pll_lock_yield_preserves_retune_ownership(void)
{
    fastlock_profile rx_profile = {0};
    fastlock_profile tx_profile = {0};
    bool cancel_failed = false;
    unsigned cancel_before;

    pkt_retune2_init();
    simulated_fastlock_active = false;
    simulated_fastlock_pending_polls = 0;
    simulated_fastlock_terminal_status = 0;
    simulated_txq_owner = false;
    simulated_rfic_faulted = false;
    fail_profile_recall = false;
    fail_port_select = false;
    fail_spdt_select = false;
    port_count = 0;
    spdt_count = 0;
    compare_fired_rx = false;
    compare_fired_tx = false;
    rx_profile.profile_num = 1;
    rx_profile.port = 0x11;
    rx_profile.spdt = 0x05;
    rx_profile.state = FASTLOCK_STATE_BBP;
    tx_profile.profile_num = 2;
    tx_profile.state = FASTLOCK_STATE_BBP;

    if (enqueue_retune_with_identity(&rx_queue, &rx_profile, 0x1111, 100,
                                     0x11112222, 0x3333) != 1 ||
        enqueue_retune_with_identity(&tx_queue, &tx_profile, 0x2222, 200,
                                     0x33334444, 0x5555) != 1) {
        fputs("failed to enqueue PLL-owner regression requests\n", stderr);
        return 1;
    }
    if (!run_until_queue_head_state(&rx_queue, ENTRY_STATE_SCHEDULED, 24)) {
        fputs("PLL-owner RX profile did not reach scheduled state\n", stderr);
        return 1;
    }
    pkt_retune2_work();
    simulate_tamer_irq(&rx_queue, BLADERF_MODULE_RX);
    pkt_retune2_work();
    if (!simulated_fastlock_active ||
        rx_queue.entries[rx_queue.rem_idx].state != ENTRY_STATE_PLL_PENDING ||
        port_count != 0 || spdt_count != 0 ||
        pkt_retune2_success_count(BLADERF_MODULE_RX) != 0 ||
        tx_queue.entries[tx_queue.rem_idx].state != ENTRY_STATE_NEW) {
        fputs("PLL recall was not yielded under exclusive owner\n", stderr);
        return 1;
    }

    cancel_before = cancel_count;
    if (clear_queue_async(&rx_queue, BLADERF_MODULE_RX, &cancel_failed) ||
        cancel_failed || cancel_count != cancel_before ||
        !rx_queue.clear_requested || !simulated_fastlock_active) {
        fputs("queue clear detached an active PLL recall\n", stderr);
        return 1;
    }
    pkt_retune2_work();
    if (!simulated_fastlock_active || port_count != 0 || spdt_count != 0 ||
        tx_queue.entries[tx_queue.rem_idx].state != ENTRY_STATE_NEW ||
        pkt_retune2_success_count(BLADERF_MODULE_RX) != 0) {
        fputs("PLL pending poll released owner or routed RF early\n", stderr);
        return 1;
    }
    pkt_retune2_work();
    if (simulated_fastlock_active || rx_queue.count != 0 ||
        rx_queue.clear_complete == false || port_count != 1 || spdt_count != 1 ||
        pkt_retune2_success_count(BLADERF_MODULE_RX) != 1 ||
        !pkt_retune2_completion_select(BLADERF_MODULE_RX, 1) ||
        pkt_retune2_completion_word(3) != 0x1111 ||
        pkt_retune2_completion_word(6) != 0x11112222 ||
        pkt_retune2_completion_word(7) != 0x3333 ||
        !clear_queue_async(&rx_queue, BLADERF_MODULE_RX, &cancel_failed)) {
        fputs("PLL lock completion did not commit correlated retune\n", stderr);
        return 1;
    }

    /* A terminal lock failure is correlated to the scheduled request and
     * never performs port/SPDT changes or publishes success. */
    pkt_retune2_init();
    simulated_fastlock_active = false;
    simulated_fastlock_terminal_status = -EIO;
    simulated_fastlock_pending_polls = 1;
    port_count = 0;
    spdt_count = 0;
    compare_fired_rx = false;
    if (enqueue_retune_with_identity(&rx_queue, &rx_profile, 0xaaaa, 300,
                                     0x55556666, 0x7777) != 1) {
        fputs("failed to enqueue PLL failure regression request\n", stderr);
        return 1;
    }
    pkt_retune2_work();
    simulate_tamer_irq(&rx_queue, BLADERF_MODULE_RX);
    pkt_retune2_work();
    pkt_retune2_work();
    pkt_retune2_work();
    if (simulated_fastlock_active || rx_queue.count != 0 ||
        port_count != 0 || spdt_count != 0 ||
        pkt_retune2_success_count(BLADERF_MODULE_RX) != 0 ||
        pkt_retune2_failure_count(BLADERF_MODULE_RX) != 1 ||
        !pkt_retune2_failure_completion_select(BLADERF_MODULE_RX, 1) ||
        pkt_retune2_completion_word(3) != 0xaaaa ||
        pkt_retune2_completion_word(4) !=
            NIOS_PKT_RETUNE2_FAILURE_PLL_LOCK_TIMEOUT ||
        pkt_retune2_completion_word(5) != 0x55556666 ||
        pkt_retune2_completion_word(6) != 0x7777) {
        fputs("PLL lock timeout was not reported against retune token\n",
              stderr);
        return 1;
    }
    simulated_fastlock_terminal_status = 0;
    return 0;
}

int main(void)
{
    uint8_t retune_request[16] = { 0 };
    bladerf_module decoded_module;
    uint64_t decoded_timestamp;
    uint16_t decoded_profile;
    uint16_t decoded_token;
    uint8_t decoded_rffe_profile, decoded_port, decoded_spdt;

    nios_pkt_retune2_pack_token(retune_request, BLADERF_MODULE_RX, 123,
                                4, 5, 0x83, 0, 0xbeef);
    nios_pkt_retune2_unpack_token(
        retune_request, &decoded_module, &decoded_timestamp,
        &decoded_profile, &decoded_rffe_profile, &decoded_port,
        &decoded_spdt, &decoded_token);
    if (decoded_module != BLADERF_MODULE_RX || decoded_timestamp != 123 ||
        decoded_profile != 4 || decoded_rffe_profile != 5 ||
        decoded_port != (0x83 | NIOS_PKT_RETUNE2_PORT_IS_RX_MASK) ||
        decoded_spdt != 0 || decoded_token != 0xbeef) {
        fputs("retune2 request token did not round-trip\n", stderr);
        return 1;
    }

    if (!nios_pkt_retune2_route_valid(BLADERF_MODULE_RX, 0x83, 0x00) ||
        !nios_pkt_retune2_route_valid(BLADERF_MODULE_RX, 0x8c, 0x0a) ||
        !nios_pkt_retune2_route_valid(BLADERF_MODULE_RX, 0x83, 0x05) ||
        !nios_pkt_retune2_route_valid(BLADERF_MODULE_TX, 0x00, 0x00) ||
        !nios_pkt_retune2_route_valid(BLADERF_MODULE_TX, 0x40, 0xa0) ||
        !nios_pkt_retune2_route_valid(BLADERF_MODULE_TX, 0x00, 0x50) ||
        nios_pkt_retune2_route_valid(BLADERF_MODULE_RX, 0x80, 0x00) ||
        nios_pkt_retune2_route_valid(BLADERF_MODULE_RX, 0x84, 0x05) ||
        nios_pkt_retune2_route_valid(BLADERF_MODULE_RX, 0x83, 0x0a) ||
        nios_pkt_retune2_route_valid(BLADERF_MODULE_RX, 0x8c, 0x05) ||
        nios_pkt_retune2_route_valid(BLADERF_MODULE_TX, 0x80, 0x50) ||
        nios_pkt_retune2_route_valid(BLADERF_MODULE_TX, 0x00, 0xa0) ||
        nios_pkt_retune2_route_valid(BLADERF_MODULE_TX, 0x40, 0x60)) {
        fputs("retune2 route validation accepted an invalid RF path\n", stderr);
        return 1;
    }

    fastlock_profile profile = { 0 };
    struct queue q = { 0 };
    struct queue ordered = { 0 };
    struct queue_entry actual = { 0 };
    fastlock_profile ordered_profile = { 0 };
    const uint8_t empty = QUEUE_EMPTY;
    uint8_t remaining;

    profile.profile_num = 5;
    ordered_profile.profile_num = 6;
    if (enqueue_retune_resident(&ordered, &ordered_profile, 6, 200) != 1 ||
        enqueue_retune_resident(&ordered, &ordered_profile, 6, 200) !=
            QUEUE_INVALID_TIMESTAMP ||
        enqueue_retune_resident(&ordered, &ordered_profile, 6, 199) !=
            QUEUE_INVALID_TIMESTAMP ||
        ordered.count != 1 ||
        ordered.entries[ordered.rem_idx].timestamp != 200) {
        fputs("duplicate/out-of-order retune changed the queue\n", stderr);
        return 1;
    }

    q.count = 1;
    q.rem_idx = RETUNE2_QUEUE_MAX - 1;
    q.entries[q.rem_idx].state = ENTRY_STATE_READY;
    q.entries[q.rem_idx].profile = profile;
    q.entries[q.rem_idx].timestamp = UINT64_C(0x123456789abcdef0);

    remaining = dequeue_retune(&q, &actual);
    if (remaining != 0 || q.count != 0 || q.rem_idx != 0 ||
        actual.state != ENTRY_STATE_READY ||
        actual.profile.profile_num != profile.profile_num ||
        actual.timestamp != UINT64_C(0x123456789abcdef0) ||
        q.entries[RETUNE2_QUEUE_MAX - 1].state != ENTRY_STATE_DONE) {
        fputs("retune2 dequeue failed to copy/advance queue entry\n", stderr);
        return 1;
    }

    actual.timestamp = 77;
    if (dequeue_retune(&q, &actual) != empty || actual.timestamp != 77 ||
        q.rem_idx != 0 || q.count != 0) {
        fputs("empty retune2 dequeue changed queue/output\n", stderr);
        return 1;
    }

    if (check_host_fastlock_owner_is_not_fault_inference() != 0) {
        return 1;
    }
    if (check_fault_retires_unarmed_and_ready_retunes() != 0) {
        return 1;
    }
    if (check_fault_cancels_armed_queue_once() != 0) {
        return 1;
    }
    if (check_schedule_ready_lifecycle() != 0) {
        return 1;
    }
    if (check_immediate_retune_reloads_reassigned_rffe_slot() != 0) {
        return 1;
    }
    if (check_immediate_retune_reuses_matching_rffe_slot() != 0) {
        return 1;
    }
    if (check_partial_upload_invalidates_slot_residency() != 0) {
        return 1;
    }
    if (check_immediate_retune_cannot_clobber_armed_slot_rx_tx() != 0) {
        return 1;
    }
    if (check_preload_preserves_scheduled_profile_slot() != 0) {
        return 1;
    }
    if (check_legacy_schedule_accepts_without_profile_spi() != 0) {
        return 1;
    }
    if (check_cancel_pending_preserves_profile_slot_rx_tx() != 0) {
        return 1;
    }
    if (check_lost_irq_recovers_from_fpga_event() != 0) {
        return 1;
    }

    if (check_stale_irq_cannot_release_rearmed_retune() != 0) {
        return 1;
    }
    if (check_stopped_tamer_clock_fails_queued_retunes() != 0) {
        return 1;
    }
    if (check_clear_queue_does_not_poll_a_stopped_tamer() != 0) {
        return 1;
    }
    if (check_clear_queue_fences_compare_irq() != 0) {
        return 1;
    }
    if (check_cancel_ack_wait_is_nonblocking() != 0) {
        return 1;
    }
    if (check_schedule_ack_wait_is_nonblocking() != 0) {
        return 1;
    }
    if (check_schedule_no_response_is_reported() != 0) {
        return 1;
    }
    if (check_cancel_no_response_is_reported_fail_closed() != 0) {
        return 1;
    }
    if (check_compare_irq_during_schedule_ack() != 0) {
        return 1;
    }
    if (check_immediate_upload_failure_preserves_profile() != 0) {
        return 1;
    }
    if (check_immediate_failure_stage_response() != 0) {
        return 1;
    }
    if (check_tokened_now_uses_terminal_completion() != 0) {
        return 1;
    }
    if (check_tokenless_now_defers_terminal_response() != 0) {
        return 1;
    }
    if (check_tokened_now_clear_preserves_terminal_identity() != 0) {
        return 1;
    }
    if (check_deferred_failure_reporting() != 0) {
        return 1;
    }
    if (check_scheduled_profile_upload_failure_is_terminal() != 0) {
        return 1;
    }
    if (check_tamer_read_and_arm_fail_closed() != 0) {
        return 1;
    }
    if (check_failed_schedule_retains_timer_ownership() != 0) {
        return 1;
    }
    if (check_timestamp_wrap_ordering() != 0) {
        return 1;
    }
    if (check_invalid_queue_state_fails_closed() != 0) {
        return 1;
    }
    if (check_profile_index_validation() != 0) {
        return 1;
    }
    if (check_completion_ring_overwrite_is_visible() != 0) {
        return 1;
    }
    if (check_txq_yield_preserves_retune_identity() != 0) {
        return 1;
    }
    if (check_pll_lock_yield_preserves_retune_ownership() != 0) {
        return 1;
    }

    puts("NIOS retune2 queue, time-tamer failure, and completion ring: PASS");
    return 0;
}
