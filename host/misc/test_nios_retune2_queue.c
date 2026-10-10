#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../hdl/fpga/platforms/common/bladerf/software/bladeRF_nios/src/pkt_retune2.c"

static unsigned load_count;
static unsigned schedule_count;
static unsigned recall_count;
static unsigned port_count;
static unsigned spdt_count;
static bladerf_module scheduled_module;
static uint64_t scheduled_timestamp;
static bladerf_module recalled_module;
static fastlock_profile *recalled_profile;
static uint8_t recalled_profile_num;
static uint8_t recalled_port;
static uint8_t recalled_spdt;
static bool fail_profile_load;
static bool fail_profile_recall;

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

void adi_rfspdt_select(bladerf_module module, fastlock_profile *profile)
{
    (void)module;
    (void)profile;
    spdt_count++;
}

void tamer_schedule(bladerf_module module, uint64_t timestamp)
{
    schedule_count++;
    scheduled_module = module;
    scheduled_timestamp = timestamp;
}

static int check_schedule_ready_lifecycle(void)
{
    fastlock_profile rx_profile = { 0 };
    fastlock_profile tx_profile = { 0 };

    memset(&rx_queue, 0, sizeof(rx_queue));
    memset(&tx_queue, 0, sizeof(tx_queue));
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
    retune_isr(&rx_queue);
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
        pkt_retune2_completion_word(3) != 5) {
        fputs("RX completion record did not preserve sequence/profile\n",
              stderr);
        return 1;
    }

    /* TX becomes activatable only after its own ISR marks it READY. */
    retune_isr(&tx_queue);
    pkt_retune2_work();
    if (tx_queue.count != 0 ||
        pkt_retune2_success_count(BLADERF_MODULE_RX) != 1 ||
        pkt_retune2_success_count(BLADERF_MODULE_TX) != 1 ||
        recall_count != 2 ||
        recalled_module != BLADERF_MODULE_TX ||
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
        pkt_retune2_completion_word(3) != 7) {
        fputs("TX completion record did not preserve sequence/profile\n",
              stderr);
        return 1;
    }

    return 0;
}

static int check_deferred_failure_reporting(void)
{
    fastlock_profile tx_profile = { 0 };
    fastlock_profile rx_profile = { 0 };

    reset_queue(&rx_queue);
    reset_queue(&tx_queue);
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
        pkt_retune2_success_count(BLADERF_MODULE_TX) != 1 ||
        !pkt_retune2_completion_select(BLADERF_MODULE_TX, 1) ||
        pkt_retune2_completion_word(0) != 1 ||
        pkt_retune2_completion_word(3) != 7) {
        fputs("deferred TX profile-load failure was not retained\n", stderr);
        return 1;
    }

    if (enqueue_retune(&rx_queue, &rx_profile, 4,
                       UINT64_C(0x456789abc)) != 1) {
        fputs("failed to enqueue RX activation failure case\n", stderr);
        return 1;
    }
    pkt_retune2_work();
    retune_isr(&rx_queue);
    fail_profile_recall = true;
    pkt_retune2_work();
    fail_profile_recall = false;
    if (rx_queue.count != 0 ||
        pkt_retune2_failure_count(BLADERF_MODULE_RX) != 1 ||
        pkt_retune2_failure_count(BLADERF_MODULE_TX) != 1 ||
        pkt_retune2_success_count(BLADERF_MODULE_RX) != 1 ||
        !pkt_retune2_completion_select(BLADERF_MODULE_RX, 1) ||
        pkt_retune2_completion_word(0) != 1 ||
        pkt_retune2_completion_word(3) != 5) {
        fputs("deferred RX activation failure was not retained\n", stderr);
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

int main(void)
{
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
    if (check_deferred_failure_reporting() != 0) {
        return 1;
    }
    if (check_profile_index_validation() != 0) {
        return 1;
    }

    puts("NIOS retune2 queue dequeue/wrap/empty and schedule/ready lifecycle: PASS");
    return 0;
}
