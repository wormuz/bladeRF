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
static bool fail_profile_recall;
static bool fail_spdt_select;
static bool fail_tamer_schedule;
static bool fail_tamer_read;
static bool fail_tamer_cancel;
static unsigned cancel_count;
static bool compare_fired_rx;
static bool compare_fired_tx;
static bool simulated_tamer_clock_stalled;

bool adi_fastlock_load(bladerf_module module, fastlock_profile *profile)
{
    (void)module;
    (void)profile;
    load_count++;
    return !fail_profile_load;
}

bool adi_fastlock_recall(bladerf_module module, fastlock_profile *profile)
{
    recall_count++;
    recalled_module = module;
    recalled_profile = profile;
    recalled_profile_num = profile->profile_num;
    recalled_port = profile->port;
    recalled_spdt = profile->spdt;
    return !fail_profile_recall;
}

bool adi_rfport_select(fastlock_profile *profile)
{
    (void)profile;
    port_count++;
    return true;
}

bool adi_rfspdt_select(bladerf_module module, fastlock_profile *profile)
{
    (void)module;
    (void)profile;
    spdt_count++;
    return !fail_spdt_select;
}

bool tamer_schedule(bladerf_module module, uint64_t timestamp)
{
    schedule_count++;
    scheduled_module = module;
    scheduled_timestamp = timestamp;
    if (module == BLADERF_MODULE_RX) {
        compare_fired_rx = false;
    } else if (module == BLADERF_MODULE_TX) {
        compare_fired_tx = false;
    }
    return !fail_tamer_schedule;
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

bool tamer_cancel(bladerf_module module)
{
    cancel_count++;
    if (!fail_tamer_cancel) {
        if (module == BLADERF_MODULE_RX) {
            compare_fired_rx = false;
        } else if (module == BLADERF_MODULE_TX) {
            compare_fired_tx = false;
        }
    }
    return !fail_tamer_cancel;
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
            [NIOS_PKT_RETUNE2_IDX_SPDT] = 0x20,
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

    /* The independent FPGA watchdog reports loss for each tamer. If the
     * cancel ACK is missing, report failure once but keep ownership of the
     * armed compare and do not activate another queued request. */
    fail_tamer_cancel = true;
    cancel_count = 0;
    simulated_tamer_clock_stalled = true;
    pkt_retune2_work();
    if (rx_queue.count != 1 || tx_queue.count != 1 ||
        rx_queue.entries[rx_queue.rem_idx].state !=
            ENTRY_STATE_CANCEL_PENDING ||
        tx_queue.entries[tx_queue.rem_idx].state !=
            ENTRY_STATE_CANCEL_PENDING ||
        cancel_count != 2 ||
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
        cancel_count != 2 ||
        pkt_retune2_failure_count(BLADERF_MODULE_RX) != 1 ||
        pkt_retune2_failure_count(BLADERF_MODULE_TX) != 1) {
        fputs("stalled cancel-pending path retried or duplicated failure\n",
              stderr);
        return 1;
    }

    /* A recovered clock permits one bounded retry. If that retry still
     * fails, later worker passes wait until another stall/recovery cycle. */
    simulated_tamer_clock_stalled = false;
    pkt_retune2_work();
    pkt_retune2_work();
    if (rx_queue.count != 1 || tx_queue.count != 1 ||
        cancel_count != 4 ||
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
    if (rx_queue.count != 0 || tx_queue.count != 0 || cancel_count != 6 ||
        pkt_retune2_success_count(BLADERF_MODULE_RX) != 0 ||
        pkt_retune2_success_count(BLADERF_MODULE_TX) != 0) {
        fputs("acknowledged cancel did not retire failed RX/TX entries\n",
              stderr);
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
    fail_tamer_schedule = true;
    pkt_retune2_work();
    fail_tamer_schedule = false;
    if (rx_queue.count != 0 ||
        pkt_retune2_failure_count(BLADERF_MODULE_RX) != 1 ||
        schedule_count == 0) {
        fputs("tamer arm timeout did not fail closed\n", stderr);
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
        rx_queue.count != 0 || cancel_count != 1) {
        fputs("invalid retune state was not reported/cancelled\n", stderr);
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
    if (check_lost_irq_recovers_from_fpga_event() != 0) {
        return 1;
    }

    if (check_stale_irq_cannot_release_rearmed_retune() != 0) {
        return 1;
    }
    if (check_stopped_tamer_clock_fails_queued_retunes() != 0) {
        return 1;
    }
    if (check_immediate_upload_failure_preserves_profile() != 0) {
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
