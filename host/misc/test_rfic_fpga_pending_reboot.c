/* Drive the production tagged RFIC write path (_rfic_cmd_write) against a
 * scripted NIOS RFIC queue. Covers a NIOS reboot (FPGA reconfiguration) under
 * a pending command, including a new boot whose first command aliases the
 * pending ID and sequence, the QUEUE_FULL refusal, and a status word that
 * reports more entries than the queue depth.
 */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static unsigned int test_sleeps;
static void test_usleep(unsigned int us)
{
    (void)us;
    ++test_sleeps;
}
#define usleep(us) test_usleep(us)

#include "board/bladerf2/rfic_fpga.c"

struct fake_nios {
    uint64_t caps;
    uint32_t nonce;
    uint32_t seq;
    bool active;
    uint16_t active_id;
    uint16_t active_addr;
    uint64_t active_value;
    bool last_valid;
    uint16_t last_id;
    uint16_t last_addr;
    uint8_t last_result;
    uint16_t acked_id;
    uint32_t acked_seq;
    /* script */
    unsigned int retire_after_status_reads; /* 0: never */
    unsigned int status_reads;
    int fail_status_read_at;       /* fail the Nth STATUS read after enqueue */
    int reboot_at_status_read;     /* reboot on the Nth STATUS read */
    int displace_at_status_read;   /* same boot, retirement record jumps */
    bool foreign_before_next_write;
    uint8_t force_wqlen;
    /* observations */
    unsigned int executed;
    unsigned int nonce_reads;
    unsigned int nonce_writes;
};

static struct fake_nios nios;
static struct bladerf dev;
static struct bladerf2_board_data board_data;
static struct backend_fns backend;
static struct board_fns board;

static void nios_reboot(void)
{
    uint64_t caps = nios.caps;
    unsigned int executed = nios.executed;
    unsigned int nonce_reads = nios.nonce_reads;
    unsigned int nonce_writes = nios.nonce_writes;

    memset(&nios, 0, sizeof(nios));
    nios.caps = caps;
    nios.executed = executed;
    nios.nonce_reads = nonce_reads;
    nios.nonce_writes = nonce_writes;
}

static void nios_enqueue(uint16_t id, uint16_t addr, uint64_t value)
{
    nios.active = true;
    nios.active_id = id;
    nios.active_addr = addr;
    nios.active_value = value;
    nios.status_reads = 0;
}

static void nios_retire(void)
{
    nios.active = false;
    nios.seq++;
    nios.last_valid = true;
    nios.last_id = nios.active_id;
    nios.last_addr = nios.active_addr;
    nios.last_result = BLADERF_RFIC_RESULT_SUCCESS;
}

static int fake_status(uint64_t *data)
{
    uint64_t v = 1u; /* initialized */

    if (nios.active) {
        nios.status_reads++;
        if (nios.fail_status_read_at != 0 &&
            (int)nios.status_reads == nios.fail_status_read_at) {
            nios.fail_status_read_at = 0;
            return BLADERF_ERR_TIMEOUT;
        }
        if (nios.reboot_at_status_read != 0 &&
            (int)nios.status_reads == nios.reboot_at_status_read) {
            nios_reboot();
        } else if (nios.displace_at_status_read != 0 &&
                   (int)nios.status_reads == nios.displace_at_status_read) {
            nios.active = false;
            nios.seq += 5;
        } else if (nios.retire_after_status_reads != 0 &&
                   nios.status_reads >= nios.retire_after_status_reads) {
            nios_retire();
        }
    }

    v |= (uint64_t)(nios.last_valid &&
                    nios.last_result == BLADERF_RFIC_RESULT_SUCCESS)
         << BLADERF_RFIC_STATUS_WQSUCCESS_SHIFT;
    v |= (uint64_t)(nios.force_wqlen ? nios.force_wqlen : nios.active)
         << BLADERF_RFIC_STATUS_WQLEN_SHIFT;
    v |= (uint64_t)nios.last_valid << BLADERF_RFIC_STATUS_WQRESULT_VALID_SHIFT;
    if (nios.active) {
        v |= (uint64_t)1u << BLADERF_RFIC_STATUS_ACTIVE_VALID_SHIFT;
        v |= (uint64_t)(nios.active_addr & 0xffu)
             << BLADERF_RFIC_STATUS_ACTIVE_CMD_SHIFT;
        v |= (uint64_t)((nios.active_addr >> 8) & 0xfu)
             << BLADERF_RFIC_STATUS_ACTIVE_CH_SHIFT;
    }
    v |= (uint64_t)nios.seq << BLADERF_RFIC_STATUS_WQSEQUENCE_SHIFT;
    *data = v;
    return 0;
}

static int fake_read(struct bladerf *d, uint16_t addr, uint64_t *data)
{
    struct bladerf_rfic_progress progress;

    (void)d;
    switch (addr & 0xffu) {
        case BLADERF_RFIC_COMMAND_STATUS:
            return fake_status(data);
        case BLADERF_RFIC_COMMAND_TRANSACTION_STATUS:
            *data = (uint64_t)(nios.active ? nios.active_id : 0) |
                    ((uint64_t)nios.last_id << 16) |
                    ((uint64_t)nios.seq << 32);
            return 0;
        case BLADERF_RFIC_COMMAND_PROGRESS:
            memset(&progress, 0, sizeof(progress));
            progress.sequence = nios.seq;
            progress.transaction_id = nios.active ? nios.active_id
                                                  : nios.last_id;
            progress.command = (uint8_t)(nios.active ? nios.active_addr
                                                     : nios.last_addr);
            progress.phase_or_result =
                nios.active ? 0
                            : (nios.last_valid ? nios.last_result
                                               : BLADERF_RFIC_RESULT_NO_RESULT);
            *data = bladerf_rfic_progress_pack(&progress);
            return 0;
        default:
            return BLADERF_ERR_UNSUPPORTED;
    }
}

static int fake_write(struct bladerf *d, uint16_t addr, uint64_t data,
                      uint16_t id)
{
    (void)d;
    if ((addr & 0xffu) == BLADERF_RFIC_COMMAND_RESULT_ACK) {
        if (id == nios.acked_id && data == nios.acked_seq && id != 0) {
            return 0;
        }
        if (!nios.active && nios.last_valid && nios.last_id == id &&
            nios.seq == data) {
            nios.last_valid = false;
            nios.last_result = BLADERF_RFIC_RESULT_NO_RESULT;
            nios.acked_id = id;
            nios.acked_seq = (uint32_t)data;
            return 0;
        }
        return BLADERF_ERR_FPGA_OP;
    }

    assert(id != 0);
    if (nios.foreign_before_next_write) {
        nios.foreign_before_next_write = false;
        nios_enqueue(0x7777, RFIC_ADDRESS(BLADERF_RFIC_COMMAND_GAIN, 0), 1);
    }
    if (nios.active && nios.active_id == id) {
        return nios.active_addr == addr && nios.active_value == data
                   ? 0 : BLADERF_ERR_FPGA_OP;
    }
    if (!bladerf_rfic_tagged_write_allowed(nios.active ? 1 : 0,
                                           nios.last_valid, nios.last_id)) {
        return BLADERF_ERR_QUEUE_FULL;
    }
    nios_enqueue(id, addr, data);
    nios.executed++;
    return 0;
}

static uint64_t fake_caps(struct bladerf *d)
{
    (void)d;
    return nios.caps;
}

int nios_boot_nonce_read(struct bladerf *d, uint32_t *nonce)
{
    (void)d;
    nios.nonce_reads++;
    *nonce = nios.nonce;
    return 0;
}

int nios_boot_nonce_write(struct bladerf *d, uint32_t nonce)
{
    (void)d;
    nios.nonce_writes++;
    return nios_pkt_8x32_boot_nonce_store(&nios.nonce, nonce)
               ? 0 : BLADERF_ERR_FPGA_OP;
}

int bladerf_monotonic_now_ns(uint64_t *now_ns)
{
    static uint64_t t;
    *now_ns = ++t;
    return 0;
}

uint64_t wallclock_get_current_nsec(void)
{
    return 12345;
}

const char *bladerf_strerror(int error)
{
    (void)error;
    return "error";
}

void log_write(bladerf_log_level level, const char *format, ...)
{
    (void)level;
    (void)format;
}

static const uint64_t all_caps =
    BLADERF_CAP_RFIC_WQ_RESULT_VALID | BLADERF_CAP_RFIC_WQ_ACTIVE |
    BLADERF_CAP_RFIC_PROGRESS | BLADERF_CAP_RFIC_RESULT_ACK |
    BLADERF_CAP_RFIC_BUSY_RESPONSE | BLADERF_CAP_NIOS_BOOT_NONCE;

static void setup(uint64_t caps)
{
    memset(&nios, 0, sizeof(nios));
    memset(&board_data, 0, sizeof(board_data));
    nios.caps = caps;
    backend.rfic_command_read = fake_read;
    backend.rfic_command_write = fake_write;
    board.get_capabilities = fake_caps;
    dev.backend = &backend;
    dev.board = &board;
    dev.board_data = &board_data;
}

static int write_freq(uint64_t hz)
{
    return _rfic_cmd_write(&dev, BLADERF_CHANNEL_RX(0),
                           BLADERF_RFIC_COMMAND_FREQUENCY, hz);
}

static void test_normal_stamps_once(void)
{
    setup(all_caps);
    nios.retire_after_status_reads = 1;
    assert(write_freq(1000000000u) == 0);
    assert(nios.executed == 1 && !board_data.rfic_wq_pending);
    assert(nios.nonce != 0 && nios.nonce_writes == 1);
    assert(board_data.rfic_boot_nonce_valid &&
           board_data.rfic_boot_nonce == nios.nonce);
    assert(write_freq(1100000000u) == 0);
    assert(nios.executed == 2 && nios.nonce_writes == 1);
}

static void test_without_capability_no_nonce_traffic(void)
{
    setup(all_caps & ~BLADERF_CAP_NIOS_BOOT_NONCE);
    nios.retire_after_status_reads = 1;
    assert(write_freq(1000000000u) == 0);
    assert(nios.executed == 1);
    assert(nios.nonce_reads == 0 && nios.nonce_writes == 0);
}

/* Pending ID 1 / sequence 1 on boot A. Boot B's first command from another
 * host is also ID 1 / sequence 1. Without the nonce the retry would adopt
 * that result as its own. */
static void test_reboot_with_aliasing_first_command(void)
{
    unsigned int executed;

    setup(all_caps);
    nios.fail_status_read_at = 1;
    assert(write_freq(1000000000u) == BLADERF_ERR_TIMEOUT);
    assert(board_data.rfic_wq_pending);
    assert(board_data.rfic_wq_pending_id == 1);
    assert(board_data.rfic_wq_pending_sequence == 1);

    nios_reboot();
    nios.nonce = 0x13572468u;
    nios_enqueue(1, RFIC_ADDRESS(BLADERF_RFIC_COMMAND_FREQUENCY, 0),
                 1000000000u);
    nios_retire();
    executed = nios.executed;

    assert(write_freq(1000000000u) == BLADERF_ERR_NOT_INIT);
    assert(!board_data.rfic_wq_pending);
    assert(!board_data.rfic_boot_nonce_valid);
    assert(nios.executed == executed);
}

/* NIOS reboots while the host polls for retirement: the new boot shows an
 * empty queue at sequence 0. */
static void test_reboot_during_retirement_wait(void)
{
    unsigned int executed;

    setup(all_caps);
    nios.retire_after_status_reads = 1;
    assert(write_freq(1000000000u) == 0);
    nios.retire_after_status_reads = 0;
    nios.reboot_at_status_read = 3;
    executed = nios.executed;

    assert(write_freq(1200000000u) == BLADERF_ERR_NOT_INIT);
    assert(nios.executed == executed + 1);
    assert(!board_data.rfic_wq_pending);

    /* The next command stamps the new boot and executes once. */
    nios.retire_after_status_reads = 1;
    assert(write_freq(1200000000u) == 0);
    assert(nios.executed == executed + 2);
    assert(nios.nonce != 0 && board_data.rfic_boot_nonce == nios.nonce);
}

/* Same boot, contradictory retirement record: still a plain mismatch. */
static void test_same_boot_mismatch_unexpected(void)
{
    setup(all_caps);
    nios.displace_at_status_read = 2;
    assert(write_freq(1000000000u) == BLADERF_ERR_UNEXPECTED);
    assert(board_data.rfic_boot_nonce_valid);
}

static void test_queue_full_is_terminal(void)
{
    setup(all_caps);
    nios.foreign_before_next_write = true;
    assert(write_freq(1000000000u) == BLADERF_ERR_QUEUE_FULL);
    assert(!board_data.rfic_wq_pending);
    assert(nios.executed == 0);
    assert(nios.active && nios.active_id == 0x7777);
}

static void test_status_beyond_queue_depth(void)
{
    struct bladerf_rfic_status_register status;

    setup(all_caps);
    nios.force_wqlen = BLADERF_RFIC_QUEUE_DEPTH;
    assert(_rfic_fpga_get_status(&dev, &status) == 0);
    nios.force_wqlen = BLADERF_RFIC_QUEUE_DEPTH + 1;
    assert(_rfic_fpga_get_status(&dev, &status) == BLADERF_ERR_UNEXPECTED);
}

int main(void)
{
    test_normal_stamps_once();
    test_without_capability_no_nonce_traffic();
    test_reboot_with_aliasing_first_command();
    test_reboot_during_retirement_wait();
    test_same_boot_mismatch_unexpected();
    test_queue_full_is_terminal();
    test_status_beyond_queue_depth();
    printf("rfic_fpga pending/reboot/queue-depth: PASS\n");
    return 0;
}
