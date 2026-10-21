/* NIOS RFIC command lifecycle, executed through the production worker
 * (devices_rfic.c / devices_rfic_cmds.c / devices_rfic_queue.c) with only the
 * AD9361 transaction layer replaced (nios_rfic_harness.h).
 *
 * Covers: callback failure (primary error, cleanup error, no replay), rejected
 * requests that must never execute, lost ACK against a host-side terminal
 * timeout, exactly-once retirement, and the cold-INIT tail phase boundaries
 * including failure at each boundary. */
#include "nios_rfic_harness.h"

#define TX0 BLADERF_CHANNEL_TX(0)
#define MUTE BLADERF_RFIC_COMMAND_TXMUTE
#define INIT BLADERF_RFIC_COMMAND_INIT
#define ON BLADERF_RFIC_INIT_STATE_ON

static bool ack(uint16_t tid, uint32_t seq)
{
    return rq(BLADERF_RFIC_COMMAND_RESULT_ACK, SYS_CH, seq, tid);
}

static void expect_events(const char *label, const char *const *want,
                          unsigned n)
{
    unsigned i;
    bool ok = event_count == n;

    for (i = 0; ok && i < n; i++)
        ok = strcmp(events[i], want[i]) == 0;
    if (!ok) {
        printf("%s: events differ, got:", label);
        for (i = 0; i < event_count; i++)
            printf(" %s", events[i]);
        printf("\n");
    }
    CHECK(ok, "%s: event order", label);
}

/* Cold INIT to ON, acknowledged, with counters cleared. Next tid is 2. */
static void init_ready(void)
{
    harness_reset();
    CHECK(rq(INIT, SYS_CH, ON, 1), "INIT accepted");
    CHECK(drain(3000) >= 0, "INIT drains");
    CHECK(state.init_state == ON, "INIT reached ON");
    CHECK(ack(1, retired()), "INIT ack");
    event_count = 0;
    txmute_calls = 0;
}

/* ---- callback failure --------------------------------------------------- */

static void test_callback_error(void)
{
    uint32_t base;

    init_ready();
    base = retired();
    txmute_fail = true;
    txmute_status = -ETIMEDOUT;
    CHECK(rq(MUTE, TX0, 1, 2), "mute accepted");
    CHECK(drain(100) >= 0, "failed callback drains");
    CHECK(txmute_calls == 1, "failed callback ran once, not replayed (%u)",
          txmute_calls);
    CHECK(retired() == base + 1, "failed callback retired once");
    CHECK(!state.write_queue.last_rv, "rv reports failure");
    CHECK(state.write_queue.last_result_code == BLADERF_RFIC_RESULT_TIMEOUT,
          "callback errno maps to its protocol result, got %u",
          state.write_queue.last_result_code);
    for (int i = 0; i < 200; i++)
        rfic_command_work();
    CHECK(txmute_calls == 1, "idle passes do not rerun callback");
    CHECK(ack(2, retired()), "failure result is ackable");

    /* A concrete error recorded inside the callback is the primary result;
     * the later cleanup error is kept separately and never replaces it. */
    init_ready();
    txmute_fail = true;
    txmute_record_status = -ETIMEDOUT;
    txmute_record_cleanup = -EIO;
    CHECK(rq(MUTE, TX0, 1, 2), "mute accepted");
    CHECK(drain(100) >= 0, "drains");
    CHECK(state.write_queue.last_result_code == BLADERF_RFIC_RESULT_TIMEOUT,
          "primary error preserved, got %u", state.write_queue.last_result_code);
    CHECK(state.write_queue.last_cleanup_result_code == BLADERF_RFIC_RESULT_IO,
          "cleanup error kept separately, got %u",
          state.write_queue.last_cleanup_result_code);
}

/* ---- rejected requests never execute ------------------------------------ */

static void test_rejected_never_executes(void)
{
    unsigned i;

    /* untagged write after the tagged protocol is on */
    init_ready();
    CHECK(rq(MUTE, TX0, 1, 2), "tagged mute accepted");
    CHECK(!rq(MUTE, TX0, 1, 0), "untagged refused once tagged protocol is on");
    CHECK(drain(100) >= 0, "drains");
    CHECK(txmute_calls == 1, "only the accepted command ran (%u)", txmute_calls);

    /* next tagged command while the previous result is still unacknowledged */
    CHECK(!rq(MUTE, TX0, 0, 3), "tagged refused while result unacknowledged");
    CHECK(drain(100) >= 0, "drains");
    CHECK(txmute_calls == 1, "refused tagged command never ran (%u)",
          txmute_calls);
    CHECK(ack(2, retired()), "ack frees the slot");
    CHECK(rq(MUTE, TX0, 0, 3), "accepted after ack");

    /* same tid, different payload, while queued: conflict, refused */
    CHECK(!rq(MUTE, TX0, 1, 3), "tid reuse with another payload refused");
    CHECK(drain(100) >= 0, "drains");
    CHECK(txmute_calls == 2, "conflicting tid did not run (%u)", txmute_calls);

    /* queue full (untagged traffic): the 17th request is refused and never runs */
    harness_reset();
    state.init_state = ON;
    state.phy = &fake_phy;
    fake_phy.pdata = &fake_pdata;
    for (i = 0; i < COMMAND_QUEUE_MAX; i++)
        CHECK(rq(MUTE, TX0, 1, 0), "untagged request %u accepted", i);
    CHECK(!rq(MUTE, TX0, 1, 0), "17th request refused: queue full");
    CHECK(drain(1000) >= 0, "queue drains");
    CHECK(txmute_calls == COMMAND_QUEUE_MAX,
          "exactly the accepted requests ran (%u)", txmute_calls);

    /* INIT-required command while OFF is refused at enqueue */
    harness_reset();
    CHECK(!rq(MUTE, TX0, 1, 1), "mute refused while uninitialized");
    CHECK(drain(100) >= 0, "drains");
    CHECK(txmute_calls == 0, "nothing executed");

    /* accepted while ON, state drops to OFF before the worker reaches it:
     * revalidation fails and the callback is not entered */
    init_ready();
    CHECK(rq(MUTE, TX0, 1, 2), "accepted while ON");
    state.init_state = BLADERF_RFIC_INIT_STATE_OFF;
    CHECK(drain(100) >= 0, "drains");
    CHECK(txmute_calls == 0, "callback skipped after revalidation (%u)",
          txmute_calls);
    CHECK(!state.write_queue.last_rv, "rejected command reports failure");
    CHECK(state.write_queue.last_result_code != BLADERF_RFIC_RESULT_SUCCESS,
          "rejected command is not SUCCESS");
}

/* ---- lost ACK vs host terminal timeout ----------------------------------- */

static void test_lost_ack_and_terminal_timeout(void)
{
    uint32_t seq;
    unsigned i;

    harness_reset();
    CHECK(rq(INIT, SYS_CH, ON, 1), "INIT accepted");
    for (i = 0; i < 6; i++)
        rfic_command_work();
    CHECK(rfic_command_worker_active() && state.write_queue.count == 1,
          "INIT still running");

    /* Host terminal timeout: it never ACKs a running transaction. */
    CHECK(!ack(1, retired() + 1), "ack of a running transaction refused");
    CHECK(!ack(1, retired()), "ack with a stale sequence refused");
    /* Host retries the same command after giving up: duplicate, not replay. */
    CHECK(rq(INIT, SYS_CH, ON, 1), "identical retry recognised as duplicate");
    CHECK(!rq(MUTE, TX0, 1, 2), "a different command cannot overtake it");
    CHECK(drain(3000) >= 0, "INIT completes");
    CHECK(starts[J_INIT] == 1, "INIT constructor started once (%u)",
          starts[J_INIT]);
    CHECK(state.init_state == ON, "INIT result survives the host timeout");
    seq = retired();
    CHECK(state.write_queue.last_rv_valid, "terminal result still held");

    CHECK(!ack(1, seq + 1), "ack with a wrong sequence refused");
    CHECK(!ack(2, seq), "ack with a wrong id refused");
    CHECK(state.write_queue.last_rv_valid, "refused acks consume nothing");
    CHECK(ack(1, seq), "ack accepted");
    CHECK(!state.write_queue.last_rv_valid, "ack retires the result");

    /* The ACK took effect but its USB response was lost: identical retry. */
    CHECK(ack(1, seq), "lost-ack retry is idempotent");

    /* A later request does not invalidate the retry, and the retry cannot
     * consume the later request's result. */
    CHECK(rq(MUTE, TX0, 1, 2), "next command accepted");
    CHECK(drain(100) >= 0, "next command drains");
    CHECK(ack(1, seq), "old ack retry still harmless");
    CHECK(state.write_queue.last_rv_valid &&
              state.write_queue.last_rv_transaction_id == 2,
          "old ack retry did not consume the newer result");
    CHECK(ack(2, retired()), "newer ack accepted");

    /* NIOS reset forgets the ACK record. */
    rfic_command_init();
    CHECK(!ack(1, seq), "ack record cleared by queue reset");
}

/* ---- exactly-once retirement --------------------------------------------- */

static void test_exactly_once_retirement(void)
{
    uint32_t base;
    unsigned i;

    init_ready();
    base = retired();
    for (i = 0; i < 5; i++) {
        CHECK(rq(MUTE, TX0, i & 1, (uint16_t)(2 + i)), "command %u accepted", i);
        CHECK(drain(100) >= 0, "command %u drains", i);
        CHECK(retired() == base + i + 1, "command %u retired once (%u)", i,
              retired() - base);
        CHECK(state.write_queue.count == 0 &&
                  state.write_queue.rem_idx == state.write_queue.ins_idx,
              "queue empty after command %u", i);
        CHECK(ack((uint16_t)(2 + i), retired()), "command %u ack", i);
    }
    CHECK(txmute_calls == 5, "each command executed once (%u)", txmute_calls);
    for (i = 0; i < 300; i++)
        rfic_command_work();
    CHECK(retired() == base + 5, "idle passes never retire again");

    /* A retry of an already retired transaction is a duplicate, not a rerun. */
    CHECK(rq(MUTE, TX0, 0, 6), "retry of retired tid accepted as duplicate");
    CHECK(drain(100) >= 0, "drains");
    CHECK(txmute_calls == 5 && retired() == base + 5,
          "retired tid was not executed or retired again");
}

/* ---- cold INIT phase boundaries ------------------------------------------ */

static const int expected_phases[] = {
    RFIC_INIT_TAIL_FILTER, RFIC_INIT_TAIL_SAMPLE_RATE,
    RFIC_INIT_TAIL_FINAL_LO, RFIC_INIT_TAIL_TX_GAIN, RFIC_INIT_TAIL_TX_MUTE,
    RFIC_INIT_TAIL_TX_GAIN, RFIC_INIT_TAIL_TX_MUTE, RFIC_INIT_TAIL_DONE,
};

static bool job_in_flight(void)
{
    unsigned j;

    for (j = 0; j < J_COUNT; j++)
        if (poll_left[j] > 0)
            return true;
    return state.fir_load_job_pending || state.fir_enable_job_pending ||
           state.sample_rate_job_pending || state.rfpll_job_pending ||
           state.rfdc_job_pending || state.tx_quad_job_pending ||
           state.bandwidth_job_pending;
}

static void test_cold_init_phases(void)
{
    static const char *const want[] = {
        "init", "fir_load", "fir_load", "fir_enable", "sample_rate", "rfpll",
        "txq", "txmute_set_cached", "txmute_set", "txmute_set_cached",
        "txmute_set",
    };
    int seen[16], n_seen = 0, last = -1;
    unsigned pass, n;
    bool on_before_done = false;

    harness_reset();
    CHECK(rq(INIT, SYS_CH, ON, 1), "INIT accepted");
    for (pass = 0; pass < 3000 && rfic_command_worker_active(); pass++) {
        rfic_command_work();
        if (state.init_state == ON && state.init_tail_phase != RFIC_INIT_TAIL_IDLE)
            on_before_done = true;
        if ((int)state.init_tail_phase != last) {
            last = (int)state.init_tail_phase;
            if (last != RFIC_INIT_TAIL_IDLE && n_seen < 16)
                seen[n_seen++] = last;
            /* a phase advances only once the previous phase's job is done */
            if (last != RFIC_INIT_TAIL_IDLE && job_in_flight() &&
                !(last == RFIC_INIT_TAIL_FILTER))
                CHECK(false, "phase %d entered with a job in flight", last);
        }
    }
    CHECK(pass < 3000, "INIT terminates");
    /* A pass can run a whole phase (TX_GAIN executes inside the call that
     * leaves FINAL_LO), so sampled phases are an ordered subset of the
     * contract; the event trace below pins the work done in every phase. */
    n = (unsigned)(sizeof(expected_phases) / sizeof(expected_phases[0]));
    {
        unsigned e = 0, s;

        for (s = 0; s < (unsigned)n_seen; s++) {
            while (e < n && expected_phases[e] != seen[s])
                e++;
            CHECK(e < n, "sampled phase %d is out of order", seen[s]);
            e++;
        }
        CHECK(n_seen > 0 && seen[n_seen - 1] == RFIC_INIT_TAIL_DONE,
              "DONE is the last phase");
    }
    for (pass = 0; pass < (unsigned)n_seen; pass++)
        CHECK(seen[pass] != RFIC_INIT_TAIL_RESET_LO, "cold INIT skips RESET_LO");
    CHECK(!on_before_done, "ON is not published before the DONE phase");
    expect_events("cold INIT", want, sizeof(want) / sizeof(want[0]));
    CHECK(state.init_state == ON, "INIT ends ON");
    CHECK(retired() == 1, "one retirement for the whole INIT (%u)", retired());
    CHECK(state.init_timing.valid &&
              state.init_timing.retirement_sequence == retired(),
          "INIT timing record carries the retirement sequence");
    CHECK(state.write_queue.last_result_code == BLADERF_RFIC_RESULT_SUCCESS,
          "INIT result SUCCESS");
    CHECK(rfic_command_pending_mask() == 0, "no pending job after INIT");
    CHECK(unexpected_calls == 0, "unexpected AD9361 call: %s", unexpected_last);
}

struct init_fault {
    enum job_id job;
    bool at_start;
    unsigned nth;
    const char *last_job_event;
};

static void test_cold_init_fault_at_each_boundary(void)
{
    static const struct init_fault faults[] = {
        {J_INIT, true, 1, "init"},
        {J_INIT, false, 1, "init"},
        {J_FIR_LOAD, true, 1, "fir_load"},
        {J_FIR_LOAD, false, 1, "fir_load"},
        {J_FIR_LOAD, true, 2, "fir_load"},
        {J_FIR_LOAD, false, 2, "fir_load"},
        {J_FIR_ENABLE, true, 1, "fir_enable"},
        {J_FIR_ENABLE, false, 1, "fir_enable"},
        {J_SAMPLE_RATE, true, 1, "sample_rate"},
        {J_SAMPLE_RATE, false, 1, "sample_rate"},
    };
    unsigned i;

    for (i = 0; i < sizeof(faults) / sizeof(faults[0]); i++) {
        const struct init_fault *f = &faults[i];

        harness_reset();
        if (f->at_start)
            fail_start_n[f->job] = f->nth;
        else
            fail_poll_n[f->job] = f->nth;
        CHECK(rq(INIT, SYS_CH, ON, 1), "INIT accepted");
        CHECK(drain(3000) >= 0, "case %u drains", i);
        CHECK(retired() == 1, "case %u retired once (%u)", i, retired());
        CHECK(state.init_state != ON, "case %u not ON", i);
        CHECK(!state.write_queue.last_rv &&
                  state.write_queue.last_result_code == BLADERF_RFIC_RESULT_IO,
              "case %u result IO, got %u", i, state.write_queue.last_result_code);
        CHECK(rfic_command_pending_mask() == 0, "case %u leaves no job pending", i);
        CHECK(event_count >= 2 &&
                  strcmp(events[event_count - 1], "remove") == 0 &&
                  strcmp(events[event_count - 2], f->last_job_event) == 0,
              "case %u: failing job is the last started, then PHY teardown", i);
        CHECK(ev_count("txmute_set") + ev_count("txmute_set_cached") == 0,
              "case %u: no later phase ran", i);
        CHECK(state.init_timing.valid &&
                  state.init_timing.primary_error_status == -EIO,
              "case %u: primary error recorded", i);
        CHECK(unexpected_calls == 0, "case %u unexpected call %s", i,
              unexpected_last);
    }
}

/* INIT's final LO may leave a retained RFPLL job behind; the tail must wait
 * for it and re-enter the same phase instead of advancing past it. */
static void test_init_final_lo_waits_for_rfpll(void)
{
    unsigned pass;
    bool advanced_early = false;

    harness_reset();
    rfpll_force_active = true;
    CHECK(rq(INIT, SYS_CH, ON, 1), "INIT accepted");
    for (pass = 0; pass < 3000 && rfic_command_worker_active(); pass++) {
        rfic_command_work();
        if (state.rfpll_job_pending &&
            state.init_tail_phase != RFIC_INIT_TAIL_FINAL_LO)
            advanced_early = true;
    }
    CHECK(!advanced_early, "tail advanced while RFPLL job was pending");
    CHECK(state.init_state == ON, "INIT ON");
    CHECK(!state.frequency_rfpll_resume_command,
          "RFPLL continuation consumed, not left armed");
    CHECK(ev_count("txq") == 1, "TX quadrature calibration ran after the LO");
    CHECK(!state.frequency_invalid[BLADERF_TX],
          "TX LO readback valid after INIT");

    harness_reset();
    rfpll_force_active = true;
    fail_poll_n[J_RFPLL] = 1;
    CHECK(rq(INIT, SYS_CH, ON, 1), "INIT accepted");
    CHECK(drain(3000) >= 0, "drains");
    CHECK(state.init_state != ON && !state.write_queue.last_rv,
          "RFPLL failure fails INIT");
    CHECK(ev_count("txmute_set") + ev_count("txmute_set_cached") == 0,
          "no tail phase after the RFPLL failure");
    CHECK(!state.frequency_rfpll_resume_command, "no stale continuation");
}

/* A failed INIT leaves the context faulted; the next INIT recovers. */
static void test_init_recovers_after_failure(void)
{
    harness_reset();
    fail_poll_n[J_FIR_ENABLE] = 1;
    CHECK(rq(INIT, SYS_CH, ON, 1), "INIT accepted");
    CHECK(drain(3000) >= 0, "drains");
    CHECK(state.init_state != ON, "first INIT failed");
    CHECK(ack(1, retired()), "failure acked");
    event_count = 0;
    fail_poll_n[J_FIR_ENABLE] = 0;
    CHECK(rq(INIT, SYS_CH, ON, 2), "second INIT accepted");
    CHECK(drain(3000) >= 0, "drains");
    CHECK(state.init_state == ON, "second INIT reached ON");
    CHECK(retired() == 2, "two retirements in total (%u)", retired());
}

int main(void)
{
    freopen("/dev/null", "w", stderr);
    test_callback_error();
    test_rejected_never_executes();
    test_lost_ack_and_terminal_timeout();
    test_exactly_once_retirement();
    test_cold_init_phases();
    test_cold_init_fault_at_each_boundary();
    test_init_final_lo_waits_for_rfpll();
    test_init_recovers_after_failure();
    if (failures != 0) {
        printf("NIOS RFIC lifecycle: %d FAILURES\n", failures);
        return 1;
    }
    puts("NIOS RFIC lifecycle: PASS");
    return 0;
}
