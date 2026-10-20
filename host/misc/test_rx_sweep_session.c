#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Host RX sweep session against a scripted NIOS: sequence assignment,
 * record assembly from success and failure records, LOST on an unreadable
 * record, CANCELLED and END on session end. */
#include "board/bladerf2/rx_sweep.c"

#define MAX_REQUESTS 32

struct scripted_request {
    uint64_t timestamp;
    uint16_t nios_profile;
    uint16_t token;
    bool failed;
    uint8_t failure_stage;
    uint64_t executed;
    uint8_t flags;
    uint64_t readback;
};

static struct scripted_request requests[MAX_REQUESTS];
static unsigned int request_count;
/* Request indices in NIOS completion order, per ring. */
static unsigned int success_order[MAX_REQUESTS];
static unsigned int failure_order[MAX_REQUESTS];
static uint32_t success_base = 100;
static uint32_t failure_base = 7;
static uint32_t successes_published;
static uint32_t failures_published;
static bool success_read_fails;
static uint16_t next_token;
static int schedule_status;
static bool cancelled;
static uint64_t now_timestamp;

int nios_retune2_success_count_read(struct bladerf *dev,
                                    bladerf_module module, uint32_t *value)
{
    (void)dev;
    (void)module;
    *value = success_base + successes_published;
    return 0;
}

int nios_retune2_failure_count_read(struct bladerf *dev,
                                    bladerf_module module, uint32_t *value)
{
    (void)dev;
    (void)module;
    *value = failure_base + failures_published;
    return 0;
}

int nios_retune2_completion_execution_read(
    struct bladerf *dev, bladerf_module module, uint32_t sequence,
    uint64_t *frequency_hz, uint64_t *scheduled_timestamp,
    uint16_t *nios_profile, uint32_t *session_generation,
    uint16_t *request_token, uint64_t *executed_timestamp,
    uint8_t *execution_flags)
{
    const struct scripted_request *r;

    (void)dev;
    (void)module;
    if (success_read_fails || sequence <= success_base ||
        sequence > success_base + successes_published) {
        return BLADERF_ERR_WOULD_BLOCK;
    }
    r = &requests[success_order[sequence - success_base - 1u]];
    *frequency_hz = r->readback;
    *scheduled_timestamp = r->timestamp;
    *nios_profile = r->nios_profile;
    *session_generation = 5;
    *request_token = r->token;
    *executed_timestamp = r->executed;
    *execution_flags = r->flags;
    return 0;
}

int nios_retune2_failure_completion_read(
    struct bladerf *dev, bladerf_module module, uint32_t sequence,
    uint64_t *scheduled_timestamp, uint16_t *nios_profile,
    uint8_t *failure_stage, uint32_t *session_generation,
    uint16_t *request_token)
{
    const struct scripted_request *r;

    (void)dev;
    (void)module;
    if (sequence <= failure_base ||
        sequence > failure_base + failures_published) {
        return BLADERF_ERR_WOULD_BLOCK;
    }
    r = &requests[failure_order[sequence - failure_base - 1u]];
    *scheduled_timestamp = r->timestamp;
    *nios_profile = r->nios_profile;
    *failure_stage = r->failure_stage;
    *session_generation = 5;
    *request_token = r->token;
    return 0;
}

/* Stands in for bladerf2.c: the ledger insertion that tags the sequence. */
int bladerf2_schedule_retune_sequenced(
    struct bladerf *dev, bladerf_channel ch, bladerf_timestamp timestamp,
    bladerf_frequency frequency, const struct bladerf_quick_tune *quick_tune,
    uint32_t sweep_sequence)
{
    struct bladerf2_board_data *board_data = dev->board_data;
    unsigned int i;

    (void)ch;
    if (schedule_status == BLADERF_ERR_INVAL) {
        return schedule_status; /* rejected before the ledger */
    }
    for (i = 0; i < SWEEP_DEPTH; i++) {
        if (!board_data->rx_retune2_pending[i].valid) {
            break;
        }
    }
    if (i == SWEEP_DEPTH) {
        return BLADERF_ERR_WOULD_BLOCK;
    }
    board_data->rx_retune2_pending[i].timestamp = timestamp;
    board_data->rx_retune2_pending[i].requested_frequency_hz = frequency;
    board_data->rx_retune2_pending[i].nios_profile = quick_tune->nios_profile;
    board_data->rx_retune2_pending[i].rffe_profile = quick_tune->rffe_profile;
    board_data->rx_retune2_pending[i].request_token = ++next_token;
    board_data->rx_retune2_pending[i].session_generation = 5;
    board_data->rx_retune2_pending[i].sweep_sequence = sweep_sequence;
    board_data->rx_retune2_pending[i].valid = true;
    board_data->rx_retune2_pending_count++;

    requests[request_count].timestamp = timestamp;
    requests[request_count].nios_profile = quick_tune->nios_profile;
    requests[request_count].token = next_token;
    request_count++;
    /* An ambiguous transport error keeps the ledger entry. */
    return schedule_status;
}

void ad9361_fastlock_invalidate_current_lo(struct ad9361_rf_phy *phy,
                                           bool tx)
{
    (void)phy;
    (void)tx;
}

static int mock_cancel(struct bladerf *dev, bladerf_channel ch)
{
    struct bladerf2_board_data *board_data = dev->board_data;
    unsigned int i;

    (void)ch;
    cancelled = true;
    for (i = 0; i < SWEEP_DEPTH; i++) {
        board_data->rx_retune2_pending[i].valid = false;
    }
    board_data->rx_retune2_pending_count = 0;
    board_data->rx_retune2_outcome_unknown = false;
    return 0;
}

static int mock_get_timestamp(struct bladerf *dev, bladerf_direction dir,
                              bladerf_timestamp *timestamp)
{
    (void)dev;
    (void)dir;
    *timestamp = now_timestamp;
    return 0;
}

static struct bladerf_board_fns_mock {
    struct board_fns fns;
} mock_board;

static struct bladerf2_board_data board_data;
static struct bladerf dev;

static int failures;

#define CHECK(cond, ...)                         \
    do {                                         \
        if (!(cond)) {                           \
            fprintf(stderr, __VA_ARGS__);        \
            fputc('\n', stderr);                 \
            failures++;                          \
        }                                        \
    } while (0)

static void setup(void)
{
    memset(&board_data, 0, sizeof(board_data));
    memset(requests, 0, sizeof(requests));
    request_count = 0;
    successes_published = 0;
    failures_published = 0;
    success_read_fails = false;
    schedule_status = 0;
    cancelled = false;
    MUTEX_INIT(&board_data.rx_async_epoch_lock);
    board_data.capabilities = BLADERF_CAP_SCHEDULED_RETUNE |
                              BLADERF_CAP_RETUNE2_REQUEST_TOKEN |
                              BLADERF_CAP_RETUNE2_EXECUTION_RECORD;
    mock_board.fns.cancel_scheduled_retunes = mock_cancel;
    mock_board.fns.get_timestamp = mock_get_timestamp;
    dev.board = &mock_board.fns;
    dev.board_data = &board_data;
}

static uint32_t schedule(uint64_t timestamp, uint64_t frequency,
                         uint16_t profile)
{
    struct bladerf_quick_tune qt;
    uint32_t sequence = 0;

    memset(&qt, 0, sizeof(qt));
    qt.nios_profile = profile;
    qt.rffe_profile = (uint8_t)(profile % 8);
    (void)bladerf2_rx_sweep_schedule(&dev, BLADERF_CHANNEL_RX(0), timestamp,
                                     frequency, &qt, &sequence);
    return sequence;
}

static unsigned int drain(struct bladerf_rx_sweep_record *out,
                          unsigned int max)
{
    unsigned int count = 0;
    int status = bladerf2_rx_sweep_completions(&dev, BLADERF_CHANNEL_RX(0),
                                               out, max, &count);
    CHECK(status == 0, "completions failed: %d", status);
    return count;
}

#define OK_FLAGS (NIOS_PKT_8x32_RETUNE2_EXEC_TS_VALID | \
                  NIOS_PKT_8x32_RETUNE2_EXEC_PLL_LOCKED)

static void check_mixed_outcomes(void)
{
    struct bladerf_rx_sweep_record r[16];
    unsigned int n;
    unsigned int index = 0;

    setup();
    CHECK(bladerf2_rx_sweep_begin(&dev, BLADERF_CHANNEL_RX(0), 10) == 0,
          "begin failed");
    CHECK(schedule(1000, 100000000, 0) == 1, "first sequence");
    CHECK(schedule(2000, 200000000, 1) == 2, "second sequence");
    CHECK(schedule(3000, 300000000, 2) == 3, "third sequence");

    /* Request 1 succeeds, 2 fails at stage 9, 3 succeeds. NIOS publishes
     * the failure before the successes; records still come out in order. */
    requests[0].executed = 1100;
    requests[0].flags = OK_FLAGS;
    requests[0].readback = 100000000;
    requests[2].executed = 3050;
    requests[2].flags = OK_FLAGS;
    requests[1].failure_stage = 9;
    failure_order[0] = 1;
    failures_published = 1;
    success_order[0] = 0;
    success_order[1] = 2;
    successes_published = 2;

    /* Scheduling collects completed records and frees their ledger slots. */
    CHECK(schedule(4000, 400000000, 3) == 4, "fourth sequence");
    CHECK(board_data.rx_retune2_pending_count == 1,
          "ledger not drained: %u", board_data.rx_retune2_pending_count);

    n = drain(r, 16);
    CHECK(n == 3, "expected 3 records, got %u", n);
    if (n == 3) {
        CHECK(r[0].sequence == 1 && r[0].status == BLADERF_RX_SWEEP_SUCCESS &&
              (r[0].flags & BLADERF_RX_SWEEP_F_CERTIFIED) &&
              r[0].certified_start == 1110 && r[0].frequency == 100000000,
              "record 1 wrong");
        CHECK(r[1].sequence == 2 && r[1].status == BLADERF_RX_SWEEP_FAILED &&
              r[1].failure_stage == 9 && r[1].scheduled_timestamp == 2000,
              "record 2 wrong");
        CHECK(r[2].sequence == 3 && r[2].status == BLADERF_RX_SWEEP_SUCCESS &&
              r[2].executed_timestamp == 3050, "record 3 wrong");
        CHECK(bladerf2_rx_sweep_label(r, n, 1500, &index) && index == 0,
              "sample 1500 not certified at record 1");
        CHECK(!bladerf2_rx_sweep_label(r, n, 2500, &index),
              "sample after failed request certified");
        CHECK(!bladerf2_rx_sweep_label(r, n, 3500, &index),
              "last record certified without successor");
    }

    /* End: request 4 never completed and is reported cancelled with its
     * schedule clipped to the end time; END follows in sequence. */
    now_timestamp = 3900;
    CHECK(bladerf2_rx_sweep_end(&dev, BLADERF_CHANNEL_RX(0)) == 0 &&
          cancelled && !board_data.rx_sweep.active, "end failed");
    n = drain(r, 16);
    CHECK(n == 2, "expected CANCELLED + END, got %u", n);
    if (n == 2) {
        CHECK(r[0].sequence == 4 && r[0].status == BLADERF_RX_SWEEP_CANCELLED &&
              r[0].scheduled_timestamp == 3900, "cancelled record wrong");
        CHECK(r[1].sequence == 5 && r[1].status == BLADERF_RX_SWEEP_END &&
              r[1].scheduled_timestamp == 3900, "end record wrong");
    }
}

static void check_lost_and_rejected(void)
{
    struct bladerf_rx_sweep_record r[16];
    unsigned int n;

    setup();
    CHECK(bladerf2_rx_sweep_begin(&dev, BLADERF_CHANNEL_RX(0), 0) == 0,
          "begin failed");

    /* A request rejected before the ledger does not consume a sequence. */
    schedule_status = BLADERF_ERR_INVAL;
    CHECK(schedule(500, 1, 0) == 0, "rejected request got a sequence");
    schedule_status = 0;

    CHECK(schedule(1000, 100000000, 0) == 1, "sequence after rejection");
    /* Ambiguous transport failure: ledger kept, sequence consumed. */
    schedule_status = BLADERF_ERR_TIMEOUT;
    CHECK(schedule(2000, 200000000, 1) == 2, "ambiguous request sequence");
    schedule_status = 0;

    /* The success record for request 1 cannot be read: both pending
     * requests become LOST and further scheduling is blocked. */
    success_order[0] = 0;
    successes_published = 1;
    success_read_fails = true;
    n = drain(r, 16);
    CHECK(n == 2 && r[0].sequence == 1 && r[0].status == BLADERF_RX_SWEEP_LOST &&
          r[1].sequence == 2 && r[1].status == BLADERF_RX_SWEEP_LOST,
          "lost records wrong (n=%u)", n);
    CHECK(board_data.rx_retune2_outcome_unknown &&
          board_data.rx_retune2_pending_count == 0,
          "lost completion did not mark outcome unknown");
    success_read_fails = false;

    /* Transitions and a second session are refused while active. */
    CHECK(bladerf2_rx_sweep_begin(&dev, BLADERF_CHANNEL_RX(0), 0) ==
              BLADERF_ERR_WOULD_BLOCK, "second begin accepted");
    now_timestamp = 5000;
    CHECK(bladerf2_rx_sweep_end(&dev, BLADERF_CHANNEL_RX(0)) == 0,
          "end after loss failed");
    n = drain(r, 16);
    CHECK(n == 1 && r[0].sequence == 3 && r[0].status == BLADERF_RX_SWEEP_END,
          "end record after loss wrong");
}

static void check_begin_preconditions(void)
{
    setup();
    board_data.rf_transition_epoch_contract_enabled = true;
    CHECK(bladerf2_rx_sweep_begin(&dev, BLADERF_CHANNEL_RX(0), 0) ==
              BLADERF_ERR_UNSUPPORTED, "epoch contract accepted");
    setup();
    board_data.capabilities &= ~BLADERF_CAP_RETUNE2_EXECUTION_RECORD;
    CHECK(bladerf2_rx_sweep_begin(&dev, BLADERF_CHANNEL_RX(0), 0) ==
              BLADERF_ERR_UNSUPPORTED, "old FPGA accepted");
    setup();
    board_data.rx_retune2_pending_count = 1;
    CHECK(bladerf2_rx_sweep_begin(&dev, BLADERF_CHANNEL_RX(0), 0) ==
              BLADERF_ERR_WOULD_BLOCK, "pending retune accepted");
    setup();
    CHECK(bladerf2_rx_sweep_begin(&dev, BLADERF_CHANNEL_TX(0), 0) ==
              BLADERF_ERR_INVAL, "TX channel accepted");
}

int main(void)
{
    check_begin_preconditions();
    check_mixed_outcomes();
    check_lost_and_rejected();
    if (failures != 0) {
        fprintf(stderr, "%d RX sweep session failure(s)\n", failures);
        return 1;
    }
    puts("RX sweep session records: PASS");
    return 0;
}
