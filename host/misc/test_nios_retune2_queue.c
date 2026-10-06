#include <stdint.h>
#include <stdio.h>

#include "../../hdl/fpga/platforms/common/bladerf/software/bladeRF_nios/src/pkt_retune2.c"

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

    puts("NIOS retune2 queue dequeue/wrap/empty: PASS");
    return 0;
}
