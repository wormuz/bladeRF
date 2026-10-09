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

bool adi_fastlock_load(bladerf_module module, fastlock_profile *profile)
{
    (void)module;
    (void)profile;
    load_count++;
    return true;
}

bool adi_fastlock_recall(bladerf_module module, fastlock_profile *profile)
{
    recall_count++;
    recalled_module = module;
    recalled_profile = profile;
    return true;
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
    tx_profile.profile_num = 2;

    if (enqueue_retune(&rx_queue, &rx_profile, UINT64_C(0x123456789)) != 1 ||
        enqueue_retune(&tx_queue, &tx_profile, UINT64_C(0x23456789a)) != 1) {
        fputs("failed to enqueue RX/TX retunes\n", stderr);
        return 1;
    }

    /* NEW schedules once; repeated worker passes must wait for the ISR. */
    pkt_retune2_work();
    if (rx_queue.entries[rx_queue.rem_idx].state != ENTRY_STATE_SCHEDULED ||
        tx_queue.entries[tx_queue.rem_idx].state != ENTRY_STATE_SCHEDULED ||
        load_count != 2 || schedule_count != 2 ||
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
        recall_count != 1 || recalled_module != BLADERF_MODULE_RX ||
        recalled_profile != &rx_profile || port_count != 1 || spdt_count != 1) {
        fputs("RX READY transition activated the wrong queue/profile\n", stderr);
        return 1;
    }

    /* TX becomes activatable only after its own ISR marks it READY. */
    retune_isr(&tx_queue);
    pkt_retune2_work();
    if (tx_queue.count != 0 || recall_count != 2 ||
        recalled_module != BLADERF_MODULE_TX ||
        recalled_profile != &tx_profile || port_count != 2 || spdt_count != 2) {
        fputs("TX READY transition did not activate its queued profile\n", stderr);
        return 1;
    }

    return 0;
}

int main(void)
{
    fastlock_profile profile = { 0 };
    struct queue q = { 0 };
    struct queue_entry actual = { 0 };
    const uint8_t empty = QUEUE_EMPTY;
    uint8_t remaining;

    profile.profile_num = 5;
    q.count = 1;
    q.rem_idx = RETUNE2_QUEUE_MAX - 1;
    q.entries[q.rem_idx].state = ENTRY_STATE_READY;
    q.entries[q.rem_idx].profile = &profile;
    q.entries[q.rem_idx].timestamp = UINT64_C(0x123456789abcdef0);

    remaining = dequeue_retune(&q, &actual);
    if (remaining != 0 || q.count != 0 || q.rem_idx != 0 ||
        actual.state != ENTRY_STATE_READY || actual.profile != &profile ||
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

    puts("NIOS retune2 queue dequeue/wrap/empty and schedule/ready lifecycle: PASS");
    return 0;
}
