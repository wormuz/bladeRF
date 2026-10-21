#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Host RX sweep session against a scripted NIOS: sequence assignment,
 * record assembly from success and failure records, LOST on an unreadable
 * record, CANCELLED and END on session end. */
struct gain_table_info;
int32_t ad9361_gain_table_index(const struct gain_table_info *info,
                                int32_t gain_db);
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
    bool synth_corrupt;
    uint8_t gain_table;
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

int nios_retune2_record_counts(struct bladerf *dev, bladerf_module module,
                               uint32_t *success_count,
                               uint32_t *failure_count)
{
    (void)dev;
    (void)module;
    *success_count = success_base + successes_published;
    *failure_count = failure_base + failures_published;
    return 0;
}

int nios_retune2_record_read(struct bladerf *dev, bladerf_module module,
                             bool failure, uint32_t sequence,
                             bool read_synth,
                             struct nios_retune2_record *record)
{
    const struct scripted_request *r;

    (void)module;
    memset(record, 0, sizeof(*record));
    if (failure) {
        if (sequence <= failure_base ||
            sequence > failure_base + failures_published) {
            return BLADERF_ERR_WOULD_BLOCK;
        }
        r = &requests[failure_order[sequence - failure_base - 1u]];
        record->failure_stage = r->failure_stage;
    } else {
        if (success_read_fails || sequence <= success_base ||
            sequence > success_base + successes_published) {
            return BLADERF_ERR_WOULD_BLOCK;
        }
        r = &requests[success_order[sequence - success_base - 1u]];
        record->execution_flags = r->flags;
        record->executed_delta = (uint32_t)(r->executed - r->timestamp);
        record->readback_frequency_hz = r->readback;
        if (read_synth) {
            const struct bladerf2_nios_fastlock_words *words =
                &((struct bladerf2_board_data *)dev->board_data)
                      ->nios_fastlock_words[0][r->nios_profile];

            bladerf2_rx_sweep_synth_expected(words->words,
                                             (uint8_t)(r->nios_profile % 8),
                                             record->synth);
            record->synth[2] ^= r->synth_corrupt ? 0x01 : 0x00;
            record->synth_read = true;
        }
    }
    record->request_token = r->token;
    record->nios_profile = (uint8_t)r->nios_profile;
    record->session_generation_low = 5;
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
    board_data->rx_retune2_pending[i].gain_table =
        bladerf2_rx_retune_gain_table(dev, frequency, sweep_sequence != 0,
                                      board_data->rx_retune2_pending_count ==
                                          0);
    requests[request_count].gain_table =
        board_data->rx_retune2_pending[i].gain_table;
    board_data->rx_retune2_pending[i].valid = true;
    board_data->rx_retune2_pending_count++;

    requests[request_count].timestamp = timestamp;
    requests[request_count].nios_profile = quick_tune->nios_profile;
    requests[request_count].token = next_token;
    request_count++;
    /* An ambiguous transport error keeps the ledger entry. */
    return schedule_status;
}

static unsigned int lo_invalidated;
static int32_t fastlock_setup_reg;
static uint32_t lo_synced_profile;
static uint64_t lo_synced_expected;

void ad9361_fastlock_invalidate_current_lo(struct ad9361_rf_phy *phy,
                                           bool tx)
{
    (void)phy;
    (void)tx;
    lo_invalidated++;
}

int32_t ad9361_spi_read(struct no_os_spi_desc *spi, uint32_t reg)
{
    (void)spi;
    return reg == REG_RX_FAST_LOCK_SETUP ? fastlock_setup_reg : -EIO;
}

int32_t ad9361_fastlock_sync_current_lo(struct ad9361_rf_phy *phy, bool tx,
                                        uint32_t profile,
                                        uint64_t expected_freq_hz,
                                        uint64_t *freq_hz)
{
    (void)tx;
    lo_synced_expected = expected_freq_hz;
    (void)freq_hz;
    lo_synced_profile = profile;
    phy->fastlock.current_profile[0] = profile + 1;
    return 0;
}

/* RX RFPLL reference rate; 100 MHz is integer word 80 at VCO divider 5. */
static int32_t refclk_status;

int32_t clk_get_rate_checked(struct ad9361_rf_phy *phy,
                             struct refclk_scale *clk_priv, uint32_t *rate)
{
    (void)phy;
    (void)clk_priv;
    *rate = 80000000;
    return refclk_status;
}

int errno_ad9361_to_bladerf(int err)
{
    return err == 0 ? 0 : BLADERF_ERR_UNEXPECTED;
}

static struct ad9361_rf_phy mock_phy;

static unsigned int events_appended;
static struct bladerf_rf_event last_event;
static bool cancel_saw_internal_flag;

void bladerf2_rf_event_append(struct bladerf2_board_data *board_data,
                              const struct bladerf_rf_event *event)
{
    (void)board_data;
    events_appended++;
    last_event = *event;
}

static int mock_cancel(struct bladerf *dev, bladerf_channel ch)
{
    struct bladerf2_board_data *board_data = dev->board_data;
    unsigned int i;

    (void)ch;
    cancelled = true;
    cancel_saw_internal_flag =
        board_data->rf_transition_cancelling_scheduled_retunes;
    for (i = 0; i < SWEEP_DEPTH; i++) {
        board_data->rx_retune2_pending[i].valid = false;
    }
    board_data->rx_retune2_pending_count = 0;
    board_data->rx_retune2_outcome_unknown = false;
    return 0;
}


static bladerf_frequency lo_frequency;

static int mock_get_frequency(struct bladerf *dev, bladerf_channel ch,
                              bladerf_frequency *frequency)
{
    (void)dev;
    (void)ch;
    *frequency = lo_frequency;
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

/* Index = full-table dB, so the upload exposes the dB it asked for. */
struct gain_table_info ad9361_adi_gt_info[1];
int32_t ad9361_gain_table_index(const struct gain_table_info *info,
                                int32_t gain_db)
{
    (void)info;
    return gain_db;
}

static uint32_t uploaded[3];
static unsigned int uploads;

int nios_rx_gain_table_index_write(struct bladerf *dev, uint8_t region,
                                   uint32_t indices)
{
    (void)dev;
    uploaded[region] = indices;
    uploads++;
    return 0;
}

static bladerf_gain_mode lane_mode[2];

static int mock_get_gain_mode(struct bladerf *dev, bladerf_channel ch,
                              bladerf_gain_mode *mode)
{
    (void)dev;
    *mode = lane_mode[ch >> 1];
    return 0;
}

static struct controller_fns mock_rfic;

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
    memset(&mock_phy, 0, sizeof(mock_phy));
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
    board_data.capabilities |= BLADERF_CAP_RETUNE2_SYNTH_RECORD;
    /* HOST tuning mode; NIOS slot 0 holds the 100 MHz profile. */
    refclk_status = 0;
    board_data.phy = &mock_phy;
    board_data.nios_fastlock_words[0][0].words[0] = 80;
    board_data.nios_fastlock_words[0][0].words[12] = 5;
    board_data.nios_fastlock_words[0][0].valid = true;
    mock_board.fns.cancel_scheduled_retunes = mock_cancel;
    mock_board.fns.get_timestamp = mock_get_timestamp;
    mock_board.fns.get_frequency = mock_get_frequency;
    lo_frequency = 925000000;
    mock_rfic.get_gain_mode = mock_get_gain_mode;
    lane_mode[0] = lane_mode[1] = BLADERF_GAIN_MGC;
    board_data.rfic = &mock_rfic;
    uploads = 0;
    dev.board = &mock_board.fns;
    dev.board_data = &board_data;
}

static uint32_t schedule_on(bladerf_channel ch, uint64_t timestamp,
                              uint64_t frequency, uint16_t profile)
{
    struct bladerf_quick_tune qt;
    uint32_t sequence = 0;

    memset(&qt, 0, sizeof(qt));
    qt.nios_profile = profile;
    qt.rffe_profile = (uint8_t)(profile % 8);
    (void)bladerf2_rx_sweep_schedule(&dev, ch, timestamp, frequency, &qt,
                                     &sequence);
    return sequence;
}

static uint32_t schedule(uint64_t timestamp, uint64_t frequency,
                         uint16_t profile)
{
    return schedule_on(BLADERF_CHANNEL_RX(0), timestamp, frequency, profile);
}

static unsigned int drain_on(bladerf_channel ch,
                             struct bladerf_rx_sweep_record *out,
                             unsigned int max)
{
    unsigned int count = 0;
    int status = bladerf2_rx_sweep_completions(&dev, ch, out, max, &count);
    CHECK(status == 0, "completions failed: %d", status);
    return count;
}

static unsigned int drain(struct bladerf_rx_sweep_record *out,
                          unsigned int max)
{
    return drain_on(BLADERF_CHANNEL_RX(0), out, max);
}

#define OK_FLAGS (NIOS_PKT_RETUNE2_EXEC_TS_VALID | \
                  NIOS_PKT_RETUNE2_EXEC_PLL_LOCKED)

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

/* The record certifies only an LO read back from the words the host
 * uploaded for the slot. */
static void check_lo_readback(void)
{
    struct bladerf_rx_sweep_record r[8];
    unsigned int n;
    unsigned int i;

    setup();
    CHECK(bladerf2_rx_sweep_begin(&dev, BLADERF_CHANNEL_RX(0), 10) == 0 &&
              board_data.rx_sweep.refclk_hz == 80000000,
          "begin failed or reference rate not taken");
    CHECK(schedule(1000, 100000000, 0) == 1, "sequence 1");
    CHECK(schedule(2000, 100000000, 0) == 2, "sequence 2");
    CHECK(schedule(3000, 100000000, 1) == 3, "sequence 3");
    CHECK(schedule(4000, 100002000, 0) == 4, "sequence 4");
    for (i = 0; i < 4; i++) {
        requests[i].executed = requests[i].timestamp + 50;
        requests[i].flags = OK_FLAGS;
        success_order[i] = i;
    }
    requests[1].synth_corrupt = true;
    successes_published = 4;
    n = drain(r, 8);
    CHECK(n == 4, "expected 4 records, got %u", n);
    if (n == 4) {
        CHECK((r[0].flags & BLADERF_RX_SWEEP_F_CERTIFIED) &&
                  (r[0].flags & BLADERF_RX_SWEEP_F_READBACK_VALID) &&
                  r[0].readback_frequency == 100000000,
              "matching words not certified (flags 0x%x, %llu Hz)",
              r[0].flags, (unsigned long long)r[0].readback_frequency);
        CHECK(!(r[1].flags & BLADERF_RX_SWEEP_F_CERTIFIED) &&
                  (r[1].flags & BLADERF_RX_SWEEP_F_READBACK_MISMATCH),
              "corrupted words certified (flags 0x%x)", r[1].flags);
        CHECK(!(r[2].flags & (BLADERF_RX_SWEEP_F_CERTIFIED |
                              BLADERF_RX_SWEEP_F_READBACK_VALID)),
              "slot with unknown words certified (flags 0x%x)", r[2].flags);
        CHECK(!(r[3].flags & BLADERF_RX_SWEEP_F_CERTIFIED) &&
                  (r[3].flags & BLADERF_RX_SWEEP_F_READBACK_MISMATCH),
              "LO 2 kHz off the request certified (flags 0x%x)",
              r[3].flags);
    }
    now_timestamp = 5000;
    CHECK(bladerf2_rx_sweep_end(&dev, BLADERF_CHANNEL_RX(0)) == 0, "end");

    /* FPGA 0.32.0 has no synthesizer word: HOST tuning mode certifies
     * nothing. */
    setup();
    board_data.capabilities &= ~BLADERF_CAP_RETUNE2_SYNTH_RECORD;
    CHECK(bladerf2_rx_sweep_begin(&dev, BLADERF_CHANNEL_RX(0), 10) == 0,
          "begin on 0.32.0 failed");
    CHECK(schedule(1000, 100000000, 0) == 1, "0.32.0 sequence 1");
    requests[0].executed = 1050;
    requests[0].flags = OK_FLAGS;
    success_order[0] = 0;
    successes_published = 1;
    n = drain(r, 8);
    CHECK(n == 1 && !(r[0].flags & BLADERF_RX_SWEEP_F_CERTIFIED) &&
              r[0].readback_frequency == 0,
          "record without readback certified");
    now_timestamp = 5000;
    CHECK(bladerf2_rx_sweep_end(&dev, BLADERF_CHANNEL_RX(0)) == 0, "end");

    /* An unreadable reference rate refuses the session. */
    setup();
    refclk_status = -5;
    CHECK(bladerf2_rx_sweep_begin(&dev, BLADERF_CHANNEL_RX(0), 10) != 0 &&
              !board_data.rx_sweep.active,
          "session began without a reference rate");
}

static void check_begin_preconditions(void)
{
    setup();
    board_data.rf_transition_epoch_contract_enabled = true;
    board_data.rf_transition_epoch_certified = false;
    CHECK(bladerf2_rx_sweep_begin(&dev, BLADERF_CHANNEL_RX(0), 0) ==
              BLADERF_ERR_WOULD_BLOCK, "closed epoch accepted");
    setup();
    board_data.rf_transition_epoch_contract_enabled = true;
    board_data.rf_transition_epoch_certified = true;
    board_data.rf_transition_readback_frequency_hz = 925000000;
    events_appended = 0;
    CHECK(bladerf2_rx_sweep_begin(&dev, BLADERF_CHANNEL_RX(0), 0) == 0 &&
              events_appended == 1 &&
              last_event.event_type == BLADERF_RF_EVT_RX_SWEEP_BEGIN &&
              last_event.requested_rx_lo_hz == 925000000,
          "open certified epoch refused or SWEEP_BEGIN missing");
    now_timestamp = 10;
    CHECK(bladerf2_rx_sweep_end(&dev, BLADERF_CHANNEL_RX(0)) == 0 &&
              cancel_saw_internal_flag &&
              !board_data.rf_transition_cancelling_scheduled_retunes &&
              last_event.event_type == BLADERF_RF_EVT_RX_SWEEP_END,
          "contract-mode end did not own the queue clear");
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


static void check_gain_table_region(void)
{
    struct bladerf_quick_tune qt;
    uint32_t sequence = 99;

    CHECK(bladerf2_rx_gain_table_region(1300000000) == 0 &&
              bladerf2_rx_gain_table_region(1300000001) == 1 &&
              bladerf2_rx_gain_table_region(4000000000ULL) == 1 &&
              bladerf2_rx_gain_table_region(4000000001ULL) == 2 &&
              bladerf2_rx_gain_table_region(70000000) == 0,
          "gain-table region boundaries");

    setup();
    lo_frequency = 2500000000ULL;
    CHECK(bladerf2_rx_sweep_begin(&dev, BLADERF_CHANNEL_RX(0), 0) == 0 &&
              board_data.rx_sweep.gain_table_region == 1,
          "session region not taken from the LO");
    memset(&qt, 0, sizeof(qt));
    CHECK(bladerf2_rx_sweep_schedule(&dev, BLADERF_CHANNEL_RX(0), 1000,
                                     925000000, &qt, &sequence) ==
              BLADERF_ERR_RANGE &&
              sequence == 0 && request_count == 0,
          "retune into another gain-table region was queued");
    CHECK(bladerf2_rx_sweep_schedule(&dev, BLADERF_CHANNEL_RX(0), 1000,
                                     1903000000, &qt, &sequence) == 0 &&
              sequence == 1,
          "retune inside the region was refused");
}
/* Full ADI tables of the three regions, as ad9361_adi_gt_info lays them
 * out; only the bounds and the terminator matter here. */
static uint8_t mock_table_rows[1][3];
static struct gain_table_info mock_gt_info[] = {
    { 0, 1300000000ULL, 77, 0, NULL, mock_table_rows },
    { 1300000000ULL, 4000000000ULL, 77, 0, NULL, mock_table_rows },
    { 4000000000ULL, 6000000000ULL, 77, 0, NULL, mock_table_rows },
    { 0, 0, 0, 0, NULL, NULL },
};
static struct ad9361_phy_platform_data mock_pdata;

static void setup_gain_table(void)
{
    setup();
    board_data.capabilities |= BLADERF_CAP_RETUNE2_GAIN_TABLE;
    memset(&mock_pdata, 0, sizeof(mock_pdata));
    mock_phy.pdata = &mock_pdata;
    mock_phy.gt_info = mock_gt_info;
    mock_phy.current_table = 1;
    lo_frequency = 2500000000ULL;
}

static void complete(unsigned int request, uint64_t executed, uint64_t lo,
                     unsigned int region, bool loaded)
{
    requests[request].executed = executed;
    requests[request].readback = lo;
    requests[request].flags = (uint8_t)(
        OK_FLAGS | NIOS_PKT_RETUNE2_EXEC_GT_VALID |
        (loaded ? NIOS_PKT_RETUNE2_EXEC_GT_LOADED : 0u) |
        (region << NIOS_PKT_RETUNE2_EXEC_GT_SHIFT));
    success_order[successes_published++] = request;
}

/* FPGA >= 0.36.0: the session crosses regions; each request names the
 * table it needs and the one the previous request leaves, the host driver's
 * table is unknown while NIOS may load one, and comes back at the end. */
static void check_gain_table_crossing(void)
{
    struct bladerf_rx_sweep_record r[16];
    unsigned int n;

    setup_gain_table();
    CHECK(bladerf2_rx_sweep_begin(&dev, BLADERF_CHANNEL_RX(0), 10) == 0,
          "begin failed");
    CHECK(schedule(1000, 925000000, 1) == 1 &&
              schedule(2000, 930000000, 2) == 2 &&
              schedule(3000, 1310000000, 3) == 3,
          "crossing retunes refused");
    CHECK(requests[0].gain_table ==
                  nios_pkt_retune2_gain_table(false, false, 1, 0) &&
              requests[1].gain_table ==
                  nios_pkt_retune2_gain_table(false, false, 0, 0) &&
              requests[2].gain_table ==
                  nios_pkt_retune2_gain_table(false, false, 0, 1),
          "gain-table bytes %02x %02x %02x", requests[0].gain_table,
          requests[1].gain_table, requests[2].gain_table);
    CHECK(mock_phy.current_table == NO_GAIN_TABLE,
          "driver table still claimed while NIOS may load another");

    complete(0, 1100, 925000000, 0, true);
    complete(1, 2100, 930000000, 0, false);
    complete(2, 3100, 1310000000, 1, true);
    n = drain(r, 16);
    CHECK(n == 3, "expected 3 records, got %u", n);
    if (n == 3) {
        CHECK((r[0].flags & BLADERF_RX_SWEEP_F_CERTIFIED) &&
                  (r[0].flags & BLADERF_RX_SWEEP_F_GAIN_TABLE_LOADED) &&
                  r[0].gain_table == 0,
              "crossing down not certified: %02x", r[0].flags);
        CHECK((r[1].flags & BLADERF_RX_SWEEP_F_CERTIFIED) &&
                  !(r[1].flags & BLADERF_RX_SWEEP_F_GAIN_TABLE_LOADED),
              "stop inside the new region not certified: %02x",
              r[1].flags);
        CHECK((r[2].flags & BLADERF_RX_SWEEP_F_CERTIFIED) &&
                  r[2].gain_table == 1,
              "crossing up not certified: %02x", r[2].flags);
    }
    CHECK(bladerf2_rx_sweep_end(&dev, BLADERF_CHANNEL_RX(0)) == 0 &&
              mock_phy.current_table == 1,
          "driver table after the session: %u", mock_phy.current_table);

    /* A record reporting another region's table is not certified, and the
     * driver is not handed a table after it. */
    setup_gain_table();
    CHECK(bladerf2_rx_sweep_begin(&dev, BLADERF_CHANNEL_RX(0), 10) == 0,
          "begin failed");
    CHECK(schedule(1000, 925000000, 1) == 1, "crossing refused");
    complete(0, 1100, 925000000, 1, false);
    n = drain(r, 16);
    CHECK(n == 1 && !(r[0].flags & BLADERF_RX_SWEEP_F_CERTIFIED) &&
              !(r[0].flags & BLADERF_RX_SWEEP_F_GAIN_TABLE_VALID),
          "record with the old region's table certified");
    CHECK(bladerf2_rx_sweep_end(&dev, BLADERF_CHANNEL_RX(0)) == 0 &&
              mock_phy.current_table == NO_GAIN_TABLE,
          "driver handed a table after a wrong one was reported");

    /* A crossing still pending at the end may or may not have run. */
    setup_gain_table();
    CHECK(bladerf2_rx_sweep_begin(&dev, BLADERF_CHANNEL_RX(0), 10) == 0,
          "begin failed");
    CHECK(schedule(1000, 1900000000, 1) == 1 &&
              schedule(2000, 5000000000ULL, 2) == 2,
          "retunes refused");
    complete(0, 1100, 1900000000, 1, false);
    CHECK(bladerf2_rx_sweep_end(&dev, BLADERF_CHANNEL_RX(0)) == 0 &&
              mock_phy.current_table == NO_GAIN_TABLE,
          "driver handed a table after a cancelled crossing");

    /* Within one region the driver keeps its table throughout. */
    setup_gain_table();
    CHECK(bladerf2_rx_sweep_begin(&dev, BLADERF_CHANNEL_RX(0), 10) == 0 &&
              schedule(1000, 1900000000, 1) == 1 &&
              mock_phy.current_table == 1 &&
              requests[0].gain_table ==
                  nios_pkt_retune2_gain_table(false, false, 1, 1),
          "retune inside the region disturbed the driver table");
    complete(0, 1100, 1900000000, 1, false);
    CHECK(bladerf2_rx_sweep_end(&dev, BLADERF_CHANNEL_RX(0)) == 0 &&
              mock_phy.current_table == 1,
          "driver table lost inside one region");
    mock_phy.pdata = NULL;
    mock_phy.gt_info = NULL;
    mock_phy.current_table = 0;
}

/* Session begin hands NIOS the manual index bladerf_set_gain() would write
 * in each region: API gain minus the region offset, clamped to "full". */
static void check_gain_table_index_upload(void)
{
    setup_gain_table();
    board_data.rx_gain_api_known[0] = true;
    board_data.rx_gain_api_db[0] = 20;
    board_data.rx_gain_api_known[1] = true;
    board_data.rx_gain_api_db[1] = 70;
    CHECK(bladerf2_rx_sweep_begin(&dev, BLADERF_CHANNEL_RX(0), 10) == 0,
          "begin failed");
    CHECK(uploads == 3 && uploaded[0] == (37u | 73u << 8) &&
              uploaded[1] == (31u | 71u << 8) &&
              uploaded[2] == (22u | 62u << 8),
          "indices %u: %04x %04x %04x", uploads, uploaded[0], uploaded[1],
          uploaded[2]);
    (void)bladerf2_rx_sweep_end(&dev, BLADERF_CHANNEL_RX(0));

    /* NIOS still holds them: an unchanged gain sends nothing, which also
     * keeps the write off a queue whose RFIC work refuses it (FPGA_OP). */
    CHECK(bladerf2_rx_sweep_begin(&dev, BLADERF_CHANNEL_RX(0), 10) == 0 &&
              uploads == 3,
          "unchanged indices re-sent: %u writes", uploads);
    (void)bladerf2_rx_sweep_end(&dev, BLADERF_CHANNEL_RX(0));
    board_data.rx_gain_api_db[0] = 21;
    CHECK(bladerf2_rx_sweep_begin(&dev, BLADERF_CHANNEL_RX(0), 10) == 0 &&
              uploads == 6 && uploaded[0] == (38u | 73u << 8),
          "changed gain not re-sent: %u writes, %04x", uploads, uploaded[0]);
    (void)bladerf2_rx_sweep_end(&dev, BLADERF_CHANNEL_RX(0));

    /* AGC owns lane 1's index; an unset gain leaves lane 0's. */
    setup_gain_table();
    board_data.rx_gain_api_known[1] = true;
    board_data.rx_gain_api_db[1] = 20;
    lane_mode[1] = BLADERF_GAIN_DEFAULT;
    CHECK(bladerf2_rx_sweep_begin(&dev, BLADERF_CHANNEL_RX(0), 10) == 0 &&
              uploads == 3 && uploaded[0] == 0x7f7f &&
              uploaded[2] == 0x7f7f,
          "kept indices %u: %04x %04x", uploads, uploaded[0], uploaded[2]);
    (void)bladerf2_rx_sweep_end(&dev, BLADERF_CHANNEL_RX(0));

    setup();
    board_data.rx_gain_api_known[0] = true;
    CHECK(bladerf2_rx_sweep_begin(&dev, BLADERF_CHANNEL_RX(0), 10) == 0 &&
              uploads == 0,
          "index upload without the gain-table capability");
    (void)bladerf2_rx_sweep_end(&dev, BLADERF_CHANNEL_RX(0));
}

/* A session on RX2 alone: its index goes up, the unused RX1 lane keeps the
 * re-map (0x7f), and the session on RX2 is the same state machine. */
static void check_rx2_only_session(void)
{
    struct bladerf_rx_sweep_record r[16];
    unsigned int n;

    setup_gain_table();
    board_data.rx_gain_api_known[1] = true;
    board_data.rx_gain_api_db[1] = 20;
    CHECK(bladerf2_rx_sweep_begin(&dev, BLADERF_CHANNEL_RX(1), 10) == 0,
          "RX2 begin failed");
    CHECK(uploads == 3 && uploaded[0] == (0x7fu | 37u << 8) &&
              uploaded[1] == (0x7fu | 31u << 8) &&
              uploaded[2] == (0x7fu | 22u << 8),
          "RX2 indices %u: %04x %04x %04x", uploads, uploaded[0],
          uploaded[1], uploaded[2]);
    CHECK(bladerf2_rx_sweep_active(&dev, BLADERF_CHANNEL_RX(1)),
          "RX2 session not reported active");
    CHECK(bladerf2_rx_sweep_begin(&dev, BLADERF_CHANNEL_RX(0), 10) != 0,
          "second session opened on the other lane");
    CHECK(schedule_on(BLADERF_CHANNEL_RX(0), 1000, 925000000, 1) != 1,
          "schedule on the RX1 lane accepted in an RX2 session");
    CHECK(schedule_on(BLADERF_CHANNEL_RX(1), 1000, 925000000, 1) == 1,
          "RX2 retune refused");
    complete(0, 1100, 925000000, 0, false);
    CHECK(bladerf2_rx_sweep_completions(&dev, BLADERF_CHANNEL_RX(0), r, 16,
                                        &n) != 0,
          "completions of the RX1 lane read an RX2 session");
    n = drain_on(BLADERF_CHANNEL_RX(1), r, 16);
    CHECK(n == 1 && (r[0].flags & BLADERF_RX_SWEEP_F_CERTIFIED),
          "RX2 record not certified: %u", n);
    CHECK(bladerf2_rx_sweep_end(&dev, BLADERF_CHANNEL_RX(1)) == 0,
          "RX2 end failed");
}


/* Session end hands the AD9361 driver the LO of the last executed NIOS
 * record (slot 3 at 300 MHz here), checked against the chip; without it a
 * later sample-rate change hits profile 0xff (-EINVAL). Unknown when the
 * last record failed or the chip has another slot recalled. */
static void end_session(bool last_fails, uint8_t chip_slot)
{
    static uint8_t spi_dummy;

    setup();
    mock_phy.spi = (struct no_os_spi_desc *)&spi_dummy;
    mock_phy.fastlock.entry[0][3].flags = FASTLOOK_INIT;
    fastlock_setup_reg =
        (int32_t)(RX_FAST_LOCK_PROFILE(chip_slot) | RX_FAST_LOCK_MODE_ENABLE);
    lo_invalidated = 0;
    lo_synced_profile = 0xff;
    CHECK(bladerf2_rx_sweep_begin(&dev, BLADERF_CHANNEL_RX(0), 10) == 0,
          "begin failed");
    CHECK(schedule(1000, 200000000, 2) == 1, "first sequence");
    CHECK(schedule(2000, 300000000, 3) == 2, "second sequence");
    requests[0].executed = 1050;
    requests[0].flags = OK_FLAGS;
    success_order[0] = 0;
    if (last_fails) {
        requests[1].failure_stage = 9;
        failure_order[0] = 1;
        failures_published = 1;
        successes_published = 1;
    } else {
        requests[1].executed = 2050;
        requests[1].flags = OK_FLAGS;
        success_order[1] = 1;
        successes_published = 2;
    }
    now_timestamp = 3000;
    CHECK(bladerf2_rx_sweep_end(&dev, BLADERF_CHANNEL_RX(0)) == 0, "end");
}

static void check_end_syncs_host_lo(void)
{
    end_session(false, 3);
    CHECK(lo_invalidated == 0 && lo_synced_profile == 3 &&
              lo_synced_expected == 300000000 &&
              mock_phy.fastlock.current_profile[0] == 4 &&
              mock_phy.fastlock.entry[0][3].flags == 0,
          "end did not sync the last executed record (slot %u, %llu Hz, "
          "profile byte %u, invalidated %u)", lo_synced_profile,
          (unsigned long long)lo_synced_expected,
          mock_phy.fastlock.current_profile[0], lo_invalidated);
    end_session(true, 3);
    CHECK(lo_invalidated == 1 && lo_synced_profile == 0xff,
          "failed last record left a known LO");
    end_session(false, 2);
    CHECK(lo_invalidated == 1 && lo_synced_profile == 0xff,
          "chip slot 2 against record slot 3 left a known LO");
}

int main(void)
{
    check_begin_preconditions();
    check_mixed_outcomes();
    check_lost_and_rejected();
    check_gain_table_region();
    check_gain_table_crossing();
    check_gain_table_index_upload();
    check_rx2_only_session();
    check_lo_readback();
    check_end_syncs_host_lo();
    if (failures != 0) {
        fprintf(stderr, "%d RX sweep session failure(s)\n", failures);
        return 1;
    }
    puts("RX sweep session records: PASS");
    return 0;
}
