/* NIOS RETUNE2 packet callback (pkt_retune2) against the production queue
 * worker (pkt_retune2_work), reusing the simulated RFIC/fastlock/time-tamer of
 * test_nios_retune2_queue.c.
 *
 * Contract: a request the callback rejects is answered without SUCCESS and is
 * never executed afterwards, however many worker turns follow (no profile
 * upload, no recall, no timer arm, no completion or failure record); a
 * failing accepted request produces exactly one terminal failure and is not
 * replayed; an accepted request retires exactly once. */
#define main retune2_queue_main
#include "test_nios_retune2_queue.c"
#undef main

#define TOKEN_A 0x41
#define TOKEN_B 0x42
#define SESSION 0x12345678u
#define RX_PORT 0x83
#define IDLE_TURNS 100u

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; \
    printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void reset_world(void)
{
    pkt_retune2_init();
    simulated_fastlock_active = false;
    simulated_rfic_worker_active = false;
    simulated_rfic_device_owned = false;
    simulated_rfic_phy_present = true;
    simulated_rfic_faulted = false;
    simulated_fastlock_terminal_status = 0;
    simulated_txq_owner = false;
    simulate_txq_yield = false;
    recall_count = 0;
    schedule_count = 0;
    port_count = 0;
    spdt_count = 0;
    load_count = 0;
    staged_profile_fail_after_write = -1;
    staged_profile_write_count = 0;
    staged_profile_load_active = false;
    fail_profile_load = false;
    memset(fastlocks_rx, 0, sizeof(fastlocks_rx));
    memset(fastlocks_tx, 0, sizeof(fastlocks_tx));
    memset(simulated_rffe_rx_valid, 0, sizeof(simulated_rffe_rx_valid));
    fastlocks_rx[0].profile_num = 1;
    fastlocks_rx[0].state = FASTLOCK_STATE_BBP;
    tamer_timestamp = 0;
}

static uint8_t submit(bladerf_module module, uint64_t timestamp,
                      uint16_t nios_profile, uint8_t rffe_profile,
                      uint8_t port, uint8_t spdt, uint16_t token,
                      uint8_t *failure_stage)
{
    struct pkt_buf request = { 0 };

    request.session_generation = SESSION;
    nios_pkt_retune2_pack_token(request.req, module, timestamp, nios_profile,
                                rffe_profile, port, spdt, token);
    pkt_retune2(&request);
    if (failure_stage != NULL)
        *failure_stage = nios_pkt_retune2_resp_failure(request.resp);
    return request.resp[NIOS_PKT_RETUNE2_RESP_IDX_FLAGS];
}

struct snapshot {
    unsigned loads, writes, recalls, schedules, ports, spdts;
    uint32_t rx_ok, rx_fail, tx_ok, tx_fail;
    uint8_t rx_count, tx_count;
};

static struct snapshot snap(void)
{
    struct snapshot s = {
        load_count, staged_profile_write_count, recall_count, schedule_count,
        port_count, spdt_count,
        pkt_retune2_success_count(BLADERF_MODULE_RX),
        pkt_retune2_failure_count(BLADERF_MODULE_RX),
        pkt_retune2_success_count(BLADERF_MODULE_TX),
        pkt_retune2_failure_count(BLADERF_MODULE_TX),
        rx_queue.count, tx_queue.count,
    };
    return s;
}

static bool same(const struct snapshot *a, const struct snapshot *b)
{
    return memcmp(a, b, sizeof(*a)) == 0;
}

/* The rejected request must leave no trace, however long the worker runs. */
static void expect_rejected(const char *what, uint8_t flags, uint8_t stage,
                            uint8_t want_stage, const struct snapshot *before,
                            uint64_t unique_timestamp)
{
    unsigned i;
    bool timer_armed_for_it = false;

    CHECK((flags & NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS) == 0,
          "%s: answered SUCCESS", what);
    CHECK((flags & NIOS_PKT_RETUNE2_RESP_FLAG_ASYNC_PENDING) == 0,
          "%s: answered async acceptance", what);
    CHECK(stage == want_stage, "%s: failure stage %u, want %u", what, stage,
          want_stage);
    for (i = 0; i < IDLE_TURNS; i++) {
        pkt_retune2_work();
        if (unique_timestamp != 0 && scheduled_timestamp == unique_timestamp)
            timer_armed_for_it = true;
    }
    {
        struct snapshot after = snap();
        CHECK(!timer_armed_for_it, "%s: its timestamp was armed", what);
        CHECK(after.loads == before->loads && after.writes == before->writes,
              "%s: profile upload ran after rejection", what);
        CHECK(after.recalls == before->recalls, "%s: recall ran after rejection",
              what);
        CHECK(after.ports == before->ports && after.spdts == before->spdts,
              "%s: RF path changed after rejection", what);
        CHECK(after.rx_ok == before->rx_ok && after.rx_fail == before->rx_fail &&
                  after.tx_ok == before->tx_ok && after.tx_fail == before->tx_fail,
              "%s: a completion/failure record was published", what);
        CHECK(after.rx_count == before->rx_count && after.tx_count == before->tx_count,
              "%s: queue depth changed", what);
    }
}

static void test_rejected_requests_never_execute(void)
{
    struct snapshot before;
    uint8_t flags, stage;

    reset_world();
    before = snap();
    flags = submit(BLADERF_MODULE_RX, NIOS_PKT_RETUNE2_NOW, 0, 1, 0x7e, 0,
                   TOKEN_A, &stage);
    expect_rejected("invalid route", flags, stage,
                    NIOS_PKT_RETUNE2_FAILURE_INVALID_REQUEST, &before, 0);

    reset_world();
    before = snap();
    flags = submit(BLADERF_MODULE_RX, NIOS_PKT_RETUNE2_NOW,
                   NUM_BBP_FASTLOCK_PROFILES, 1, RX_PORT, 0, TOKEN_A, &stage);
    expect_rejected("profile index out of range", flags, stage,
                    NIOS_PKT_RETUNE2_FAILURE_INVALID_REQUEST, &before, 0);

    reset_world();
    simulated_rfic_faulted = true;
    before = snap();
    flags = submit(BLADERF_MODULE_RX, NIOS_PKT_RETUNE2_NOW, 0, 1, RX_PORT, 0,
                   TOKEN_A, &stage);
    expect_rejected("RFIC faulted", flags, stage,
                    NIOS_PKT_RETUNE2_FAILURE_RFIC_FAULTED, &before, 0);
    simulated_rfic_faulted = false;

    /* a tokened NOW while another request owns the queue */
    reset_world();
    flags = submit(BLADERF_MODULE_RX, NIOS_PKT_RETUNE2_NOW, 0, 1, RX_PORT, 0,
                   TOKEN_A, &stage);
    CHECK((flags & NIOS_PKT_RETUNE2_RESP_FLAG_ASYNC_PENDING) != 0,
          "first tokened NOW accepted");
    before = snap();
    flags = submit(BLADERF_MODULE_RX, NIOS_PKT_RETUNE2_NOW, 0, 2, RX_PORT, 0,
                   TOKEN_B, &stage);
    CHECK((flags & NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS) == 0, "busy NOW refused");
    CHECK(stage == NIOS_PKT_RETUNE2_FAILURE_PROFILE_SLOT_BUSY,
          "busy NOW failure stage %u", stage);
    CHECK(run_until_queue_empty(&rx_queue, 64), "first request completes");
    CHECK(pkt_retune2_success_count(BLADERF_MODULE_RX) == before.rx_ok + 1 &&
              pkt_retune2_failure_count(BLADERF_MODULE_RX) == before.rx_fail,
          "exactly the accepted request retired");
    CHECK(rx_retune2_completions[0].request_token == TOKEN_A,
          "the retired request is the accepted one");
    for (unsigned i = 0; i < IDLE_TURNS; i++)
        pkt_retune2_work();
    CHECK(pkt_retune2_success_count(BLADERF_MODULE_RX) == before.rx_ok + 1,
          "accepted request retires exactly once");

    /* scheduled request into a full queue */
    reset_world();
    {
        fastlock_profile profile = { 0 };
        unsigned i;

        profile.profile_num = 2;
        for (i = 0; i < RETUNE2_QUEUE_MAX; i++)
            CHECK(enqueue_retune_resident(&rx_queue, &profile, 2,
                                          1000 + 100 * i) != QUEUE_FULL,
                  "fill entry %u", i);
        before = snap();
        flags = submit(BLADERF_MODULE_RX, 900000, 2, 2, RX_PORT, 0, TOKEN_B,
                       &stage);
        CHECK((flags & NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS) == 0,
              "full queue refused the request");
        CHECK(rx_queue.count == RETUNE2_QUEUE_MAX, "queue depth unchanged");
        {
            bool armed = false;
            for (i = 0; i < IDLE_TURNS; i++) {
                pkt_retune2_work();
                if (scheduled_timestamp == 900000)
                    armed = true;
            }
            CHECK(!armed, "rejected scheduled request was armed");
        }
    }

    /* scheduled request not strictly after the last queued one */
    reset_world();
    {
        fastlock_profile profile = { 0 };

        profile.profile_num = 2;
        CHECK(enqueue_retune_resident(&rx_queue, &profile, 2, 5000) == 1,
              "seed entry");
        before = snap();
        flags = submit(BLADERF_MODULE_RX, 4000, 2, 2, RX_PORT, 0, TOKEN_B,
                       &stage);
        CHECK((flags & NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS) == 0,
              "stale timestamp refused");
        {
            bool armed = false;
            unsigned i;
            for (i = 0; i < IDLE_TURNS; i++) {
                pkt_retune2_work();
                if (scheduled_timestamp == 4000)
                    armed = true;
            }
            CHECK(!armed, "stale-timestamp request was armed");
            CHECK(rx_queue.count <= before.rx_count, "stale request queued");
        }
    }
}

static void test_failing_request_is_not_replayed(void)
{
    uint8_t flags, stage;
    unsigned i, writes;

    reset_world();
    staged_profile_fail_after_write = 3;
    flags = submit(BLADERF_MODULE_RX, NIOS_PKT_RETUNE2_NOW, 0, 1, RX_PORT, 0,
                   TOKEN_A, &stage);
    CHECK((flags & NIOS_PKT_RETUNE2_RESP_FLAG_ASYNC_PENDING) != 0, "accepted");
    for (i = 0; i < 8; i++)
        pkt_retune2_work();
    staged_profile_fail_after_write = -1;
    writes = staged_profile_write_count;
    CHECK(rx_queue.count == 0, "failed request left the queue");
    CHECK(pkt_retune2_failure_count(BLADERF_MODULE_RX) == 1 &&
              pkt_retune2_success_count(BLADERF_MODULE_RX) == 0,
          "exactly one terminal failure");
    for (i = 0; i < IDLE_TURNS; i++)
        pkt_retune2_work();
    CHECK(pkt_retune2_failure_count(BLADERF_MODULE_RX) == 1,
          "failure record published once, not per turn");
    CHECK(staged_profile_write_count == writes && !staged_profile_load_active,
          "failed upload was not retried");
    CHECK(pkt_retune2_success_count(BLADERF_MODULE_RX) == 0,
          "failed request never reports success");
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    test_rejected_requests_never_execute();
    test_failing_request_is_not_replayed();
    if (failures != 0) {
        printf("NIOS RETUNE2 rejected/failed requests: %d FAILURES\n", failures);
        return 1;
    }
    puts("NIOS RETUNE2 rejected/failed requests: PASS");
    return 0;
}
