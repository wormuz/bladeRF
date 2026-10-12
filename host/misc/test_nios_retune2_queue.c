#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../hdl/fpga/platforms/common/bladerf/software/bladeRF_nios/src/pkt_retune2.c"

fastlock_profile fastlocks_rx[NUM_BBP_FASTLOCK_PROFILES];
fastlock_profile fastlocks_tx[NUM_BBP_FASTLOCK_PROFILES];

static unsigned load_count;
static unsigned schedule_count;
static unsigned recall_count;
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
        for (size_t reserved = NIOS_PKT_RETUNE2_RESP_IDX_RESV + 1;
             reserved < NIOS_PKT_LEN; ++reserved) {
            if (request.resp[reserved] != 0) {
                fprintf(stderr, "%s failure dirtied reserved response byte %zu\n",
                        failures[i].name, reserved);
                return 1;
            }
        }
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
    if (enqueue_retune(queue, &armed, 1, 100) != 1) {
        fputs("failed to enqueue armed-slot collision case\n", stderr);
        track_rffe_fastlock_slots = false;
        return 1;
    }
    profile_load_scheduled(queue, module);
    pkt_retune2_work();
    if (queue->entries[queue->rem_idx].state != ENTRY_STATE_SCHEDULED) {
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
        load_count != 1 || recall_count != 0 ||
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
    pkt_retune2_work();
    pkt_retune2_work();
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
    rx_profile.profile_num = 1;
    rx_profile.port = 0x11;
    rx_profile.spdt = 0x02;
    rx_profile.state = FASTLOCK_STATE_BBP_RFFE;
    tx_profile.profile_num = 2;
    tx_profile.port = 0x22;
    tx_profile.spdt = 0x03;
    tx_profile.state = FASTLOCK_STATE_BBP_RFFE;

    if (enqueue_retune(&rx_queue, &rx_profile, 5,
                       UINT64_C(0x123456789)) != 1 ||
        enqueue_retune(&tx_queue, &tx_profile, 7,
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
        load_count != 2 || schedule_count != 2 ||
        rx_queue.entries[rx_queue.rem_idx].profile.profile_num != 1 ||
        rx_queue.entries[rx_queue.rem_idx].profile.port != 0x11 ||
        rx_queue.entries[rx_queue.rem_idx].profile.spdt != 0x02 ||
        rx_queue.entries[rx_queue.rem_idx].profile.state != FASTLOCK_STATE_BBP ||
        tx_queue.entries[tx_queue.rem_idx].profile.profile_num != 2 ||
        tx_queue.entries[tx_queue.rem_idx].profile.port != 0x22 ||
        tx_queue.entries[tx_queue.rem_idx].profile.spdt != 0x03 ||
        tx_queue.entries[tx_queue.rem_idx].profile.state != FASTLOCK_STATE_BBP ||
        scheduled_module != BLADERF_MODULE_TX ||
        scheduled_timestamp != UINT64_C(0x23456789a)) {
        fputs("NEW retune did not load and schedule both module queues\n", stderr);
        return 1;
    }

    pkt_retune2_work();
    if (load_count != 2 || schedule_count != 2 || recall_count != 0) {
        fputs("SCHEDULED retune was rescheduled or activated before ISR\n", stderr);
        return 1;
    }

    /* Fire only RX's timer interrupt: TX remains scheduled and untouched. */
    simulate_tamer_irq(&rx_queue, BLADERF_MODULE_RX);
    pkt_retune2_work();
    if (rx_queue.count != 0 ||
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
    pkt_retune2_work();
    if (tx_queue.count != 0 ||
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
    if (enqueue_retune(&tx_queue, &tx_profile, 6,
                       UINT64_C(0x56789abcd)) != 1) {
        fputs("failed to enqueue TX RF switch readback failure case\n",
              stderr);
        return 1;
    }
    pkt_retune2_work();
    simulate_tamer_irq(&tx_queue, BLADERF_MODULE_TX);
    fail_spdt_select = true;
    pkt_retune2_work();
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

    if (enqueue_retune(&rx_queue, &first, 1, 100) != 1) {
        fputs("failed to enqueue first shared-slot retune\n", stderr);
        track_rffe_fastlock_slots = false;
        return 1;
    }
    profile_load_scheduled(&rx_queue, BLADERF_MODULE_RX);
    pkt_retune2_work();
    loads_after_first_arm = load_count;
    if (rx_queue.entries[rx_queue.rem_idx].state != ENTRY_STATE_SCHEDULED ||
        !simulated_rffe_rx_valid[first.profile_num] ||
        simulated_rffe_rx[first.profile_num].profile_data[0] != 0xa5) {
        fputs("first retune did not retain its loaded RFFE profile\n", stderr);
        track_rffe_fastlock_slots = false;
        return 1;
    }

    if (enqueue_retune(&rx_queue, &later, 2, 200) != 2) {
        fputs("failed to enqueue later shared-slot retune\n", stderr);
        track_rffe_fastlock_slots = false;
        return 1;
    }
    profile_load_scheduled(&rx_queue, BLADERF_MODULE_RX);
    if (load_count != loads_after_first_arm ||
        simulated_rffe_rx[first.profile_num].profile_data[0] != 0xa5) {
        fputs("preload overwrote an armed retune's RFFE profile slot\n",
              stderr);
        track_rffe_fastlock_slots = false;
        return 1;
    }

    compare_fired_rx = true;
    pkt_retune2_work();
    pkt_retune2_work();
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

    pkt_retune2_work();
    if (rx_queue.entries[rx_queue.rem_idx].state != ENTRY_STATE_SCHEDULED ||
        load_count != loads_after_first_arm + 1 ||
        simulated_rffe_rx[first.profile_num].profile_data[0] != 0x5a) {
        fputs("deferred colliding profile did not load at queue head\n",
              stderr);
        track_rffe_fastlock_slots = false;
        return 1;
    }

    track_rffe_fastlock_slots = false;
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
    if (enqueue_retune(queue, &first, 1, 100) != 1) {
        fputs("failed to enqueue cancel-pending slot owner\n", stderr);
        track_rffe_fastlock_slots = false;
        return 1;
    }
    profile_load_scheduled(queue, module);
    pkt_retune2_work();
    if (queue->entries[queue->rem_idx].state !=
            ENTRY_STATE_CANCEL_PENDING ||
        !simulated_valid[3] || simulated_slots[3].profile_data[0] != 0xa5) {
        fputs("failed schedule did not retain the physical slot owner\n",
              stderr);
        track_rffe_fastlock_slots = false;
        return 1;
    }

    loads_after_failure = load_count;
    if (enqueue_retune(queue, &later, 2, 200) != 2) {
        fputs("failed to enqueue later cancel-pending slot collision\n",
              stderr);
        track_rffe_fastlock_slots = false;
        return 1;
    }
    profile_load_scheduled(queue, module);
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
    if (enqueue_retune(&rx_queue, &profile, 3, 100) != 1) {
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
    pkt_retune2_work();
    pkt_retune2_work();
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
    if (enqueue_retune(&rx_queue, &profile, 6, 100) != 1) {
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
    pkt_retune2_work();
    pkt_retune2_work();
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
    if (enqueue_retune(&rx_queue, &profile, 4, 100) != 1 ||
        enqueue_retune(&tx_queue, &profile, 4, 100) != 1) {
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
    if (enqueue_retune(&rx_queue, &profile, 2, 100) != 1) {
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
    if (enqueue_retune(&rx_queue, &profile, 2, 200) !=
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
    if (enqueue_retune(&rx_queue, &profile, 3, 100) != 1) {
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
    if (enqueue_retune(&rx_queue, &profile, 5, 100) != 1) {
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
    if (enqueue_retune(&rx_queue, &profile, 6, 100) != 1) {
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
    if (enqueue_retune(&rx_queue, &profile, 8, 100) != 1) {
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
    if (enqueue_retune(&rx_queue, &profile, 4, 100) != 1) {
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
    if (enqueue_retune(&rx_queue, &profile, 7, 1) != 1) {
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

    pkt_retune2_work();
    if (rx_queue.count != 0 || recall_count != 1 ||
        pkt_retune2_success_count(BLADERF_MODULE_RX) != 1) {
        fputs("compare received during ARM was not activated\n", stderr);
        return 1;
    }
    return 0;
}

static int check_deferred_failure_reporting(void)
{
    fastlock_profile tx_profile = { 0 };
    fastlock_profile rx_profile = { 0 };

    pkt_retune2_init();
    tx_profile.profile_num = 3;
    rx_profile.profile_num = 4;

    if (enqueue_retune(&tx_queue, &tx_profile, 3,
                       UINT64_C(0x3456789ab)) != 1) {
        fputs("failed to enqueue TX profile-load failure case\n", stderr);
        return 1;
    }
    fail_profile_load = true;
    pkt_retune2_work();
    fail_profile_load = false;
    if (tx_queue.count != 0 ||
        pkt_retune2_failure_count(BLADERF_MODULE_TX) != 1 ||
        pkt_retune2_failure_count(BLADERF_MODULE_RX) != 0 ||
        pkt_retune2_success_count(BLADERF_MODULE_TX) != 0) {
        fputs("deferred TX profile-load failure was not retained\n", stderr);
        return 1;
    }

    if (enqueue_retune(&rx_queue, &rx_profile, 4,
                       UINT64_C(0x456789abc)) != 1) {
        fputs("failed to enqueue RX activation failure case\n", stderr);
        return 1;
    }
    pkt_retune2_work();
    simulate_tamer_irq(&rx_queue, BLADERF_MODULE_RX);
    fail_profile_recall = true;
    pkt_retune2_work();
    fail_profile_recall = false;
    if (rx_queue.count != 0 ||
        pkt_retune2_failure_count(BLADERF_MODULE_RX) != 1 ||
        pkt_retune2_failure_count(BLADERF_MODULE_TX) != 1 ||
        pkt_retune2_success_count(BLADERF_MODULE_RX) != 0) {
        fputs("deferred RX activation failure was not retained\n", stderr);
        return 1;
    }

    return 0;
}

static int check_preload_retry_is_not_a_terminal_failure(void)
{
    fastlock_profile tx_profile = { 0 };

    pkt_retune2_init();
    tx_profile.profile_num = 5;
    tx_profile.state = FASTLOCK_STATE_BBP;
    if (enqueue_retune(&tx_queue, &tx_profile, 5, 10) != 1) {
        fputs("failed to enqueue speculative preload case\n", stderr);
        return 1;
    }

    fail_profile_load = true;
    profile_load_scheduled(&tx_queue, BLADERF_MODULE_TX);
    fail_profile_load = false;
    if (pkt_retune2_failure_count(BLADERF_MODULE_TX) != 0 ||
        tx_queue.count != 1 ||
        tx_queue.entries[tx_queue.rem_idx].state != ENTRY_STATE_NEW) {
        fputs("speculative preload failure was reported as terminal\n",
              stderr);
        return 1;
    }

    /* The execution path retries the preload; a successful retry and recall
     * must leave the host-visible failure counter unchanged. */
    pkt_retune2_work();
    simulate_tamer_irq(&tx_queue, BLADERF_MODULE_TX);
    pkt_retune2_work();
    if (pkt_retune2_failure_count(BLADERF_MODULE_TX) != 0 ||
        pkt_retune2_success_count(BLADERF_MODULE_TX) != 1 ||
        tx_queue.count != 0) {
        fputs("successful preload retry still reported a retune failure\n",
              stderr);
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
    if (enqueue_retune(&rx_queue, &profile, 2, 10) != 1) {
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
    if (enqueue_retune(&rx_queue, &profile, 2, 10) != 1) {
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
    if (enqueue_retune(&rx_queue, &profile, 3, 10) != 1 ||
        enqueue_retune(&rx_queue, &profile, 3, 20) != 2) {
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
    pkt_retune2_work();
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
    pkt_retune2_work();
    if (rx_queue.count != 0 || recall_count != 1 ||
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
    if (enqueue_retune(&ordered, &profile, 3, UINT64_MAX - 3) != 1 ||
        enqueue_retune(&ordered, &profile, 3, UINT64_C(2)) != 2 ||
        enqueue_retune(&ordered, &profile, 3, UINT64_MAX - 1) !=
            QUEUE_INVALID_TIMESTAMP) {
        fputs("retune queue rejected wrapped order or accepted stale order\n",
              stderr);
        return 1;
    }

    pkt_retune2_init();
    tamer_timestamp = UINT64_MAX - 4;
    if (enqueue_retune(&rx_queue, &profile, 3, UINT64_C(2)) != 1) {
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
                               1800000000u, 0x100000002ull, 5);
    if (!pkt_retune2_completion_select(BLADERF_MODULE_TX,
                                       requested_sequence) ||
        pkt_retune2_completion_word(
            NIOS_PKT_8x32_RETUNE2_COMPLETION_SEQUENCE) !=
                overwritten_sequence) {
        fputs("completion ring overwrite was not externally visible\n",
              stderr);
        return 1;
    }
    return 0;
}

int main(void)
{
    uint8_t retune_request[16] = { 0 };
    if (!nios_pkt_retune2_reserved_valid(retune_request)) {
        fputs("zeroed retune2 reserved fields were rejected\n", stderr);
        return 1;
    }
    retune_request[NIOS_PKT_RETUNE2_IDX_RESV + 1] = 1;
    if (nios_pkt_retune2_reserved_valid(retune_request)) {
        fputs("nonzero retune2 reserved field was accepted\n", stderr);
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
    if (enqueue_retune(&ordered, &ordered_profile, 6, 200) != 1 ||
        enqueue_retune(&ordered, &ordered_profile, 6, 200) !=
            QUEUE_INVALID_TIMESTAMP ||
        enqueue_retune(&ordered, &ordered_profile, 6, 199) !=
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
    if (check_deferred_failure_reporting() != 0) {
        return 1;
    }
    if (check_preload_retry_is_not_a_terminal_failure() != 0) {
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

    puts("NIOS retune2 queue, time-tamer failure, and completion ring: PASS");
    return 0;
}
