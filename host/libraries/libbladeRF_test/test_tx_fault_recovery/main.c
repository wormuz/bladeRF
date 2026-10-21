/* Order and fail-closed exits of the stale-TX recovery coordinator. The
 * hardware steps are replaced by a model of the board latch, its fault
 * generation, the submit-lease count and the FX3/FPGA/RFIC operations. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <libbladeRF.h>

#include "tx_fault_recovery.h"

#define TX_SAMPLE_ENDPOINT 0x01

enum step {
    STEP_DRAIN = 1,
    STEP_LOCK,
    STEP_BEGIN,
    STEP_FLUSH_START,
    STEP_FLUSH_WAIT,
    STEP_CLEAR_HALT,
    STEP_RFIC_READY,
    STEP_FLUSH_RELEASE,
    STEP_COMMIT,
    STEP_UNLOCK,
};

struct mock {
    /* Board model */
    bool latch;
    uint64_t fault_generation;
    unsigned int leases;
    bool locked;
    bool fifo_draining;
    bool fifo_released;
    unsigned int endpoint_resets;
    uint8_t reset_endpoint;
    unsigned int flush_events;

    /* Injected outcomes */
    int drain_status;
    int flush_wait_status;
    int clear_halt_status;
    int rfic_status;
    int release_status;
    bool fault_during_clear_halt;

    enum step trace[32];
    unsigned int trace_len;
};

static void record(struct mock *m, enum step s)
{
    assert(m->trace_len < sizeof(m->trace) / sizeof(m->trace[0]));
    m->trace[m->trace_len++] = s;
}

static int drain(void *ctx, unsigned int timeout_ms)
{
    struct mock *m = ctx;
    record(m, STEP_DRAIN);
    /* USB callbacks retire leases; holding the board lock would block them. */
    assert(!m->locked);
    assert(timeout_ms == 250);
    if (!m->latch) {
        return BLADERF_ERR_INVAL;
    }
    if (m->drain_status != 0) {
        return m->drain_status;
    }
    m->leases = 0;
    return 0;
}

static void lock(void *ctx)
{
    struct mock *m = ctx;
    record(m, STEP_LOCK);
    assert(!m->locked);
    m->locked = true;
}

static void unlock(void *ctx)
{
    struct mock *m = ctx;
    record(m, STEP_UNLOCK);
    assert(m->locked);
    m->locked = false;
}

static int begin(void *ctx, uint64_t *generation)
{
    struct mock *m = ctx;
    record(m, STEP_BEGIN);
    assert(m->locked);
    if (!m->latch) {
        return BLADERF_ERR_INVAL;
    }
    if (m->leases != 0) {
        return BLADERF_ERR_WOULD_BLOCK;
    }
    *generation = m->fault_generation;
    return 0;
}

static int flush_start(void *ctx)
{
    struct mock *m = ctx;
    record(m, STEP_FLUSH_START);
    assert(m->locked && m->leases == 0);
    m->fifo_draining = true;
    m->fifo_released = false;
    return 0;
}

static int flush_wait(void *ctx, unsigned int timeout_ms)
{
    struct mock *m = ctx;
    record(m, STEP_FLUSH_WAIT);
    assert(m->locked && m->fifo_draining);
    assert(timeout_ms == 250);
    return m->flush_wait_status;
}

static int clear_halt(void *ctx)
{
    struct mock *m = ctx;
    record(m, STEP_CLEAR_HALT);
    /* FX3 endpoint reset only behind a drained FPGA writer and idle GPIF. */
    assert(m->locked && m->fifo_draining && m->leases == 0);
    if (m->fault_during_clear_halt) {
        m->fault_during_clear_halt = false;
        m->latch = true;
        m->fault_generation++;
    }
    if (m->clear_halt_status != 0) {
        return m->clear_halt_status;
    }
    m->endpoint_resets++;
    m->reset_endpoint = TX_SAMPLE_ENDPOINT;
    return 0;
}

static int rfic_ready(void *ctx)
{
    struct mock *m = ctx;
    record(m, STEP_RFIC_READY);
    assert(m->locked);
    return m->rfic_status;
}

static int flush_release(void *ctx, unsigned int timeout_ms)
{
    struct mock *m = ctx;
    record(m, STEP_FLUSH_RELEASE);
    assert(m->locked && m->fifo_draining && m->endpoint_resets != 0);
    assert(timeout_ms == 250);
    if (m->release_status != 0) {
        return m->release_status;
    }
    m->fifo_draining = false;
    m->fifo_released = true;
    return 0;
}

static int commit(void *ctx, uint64_t generation)
{
    struct mock *m = ctx;
    record(m, STEP_COMMIT);
    assert(m->locked && m->fifo_released);
    if (generation != m->fault_generation) {
        return BLADERF_ERR_WOULD_BLOCK;
    }
    m->latch = false;
    m->flush_events++;
    return 0;
}

static const struct bladerf2_tx_fault_recovery_ops ops = {
    .drain_submissions = drain,
    .lock = lock,
    .unlock = unlock,
    .begin = begin,
    .fifo_flush_start = flush_start,
    .fifo_flush_wait = flush_wait,
    .clear_tx_endpoint_halt = clear_halt,
    .rfic_ready = rfic_ready,
    .fifo_flush_release = flush_release,
    .commit = commit,
};

static void faulted(struct mock *m)
{
    memset(m, 0, sizeof(*m));
    m->latch = true;
    m->fault_generation = 7;
    m->leases = 3;
}

static void expect_trace(const struct mock *m, const enum step *steps,
                         unsigned int count)
{
    unsigned int i;
    if (m->trace_len != count) {
        fprintf(stderr, "trace length %u, expected %u\n", m->trace_len, count);
    }
    assert(m->trace_len == count);
    for (i = 0; i < count; i++) {
        assert(m->trace[i] == steps[i]);
    }
}

#define EXPECT(m, ...)                                                    \
    do {                                                                  \
        const enum step _s[] = {__VA_ARGS__};                             \
        expect_trace((m), _s, sizeof(_s) / sizeof(_s[0]));                \
    } while (0)

static void test_full_sequence(void)
{
    struct mock m;
    faulted(&m);
    assert(bladerf2_tx_fault_recovery_run(&ops, &m, 250) == 0);
    EXPECT(&m, STEP_DRAIN, STEP_LOCK, STEP_BEGIN, STEP_FLUSH_START,
           STEP_FLUSH_WAIT, STEP_CLEAR_HALT, STEP_RFIC_READY,
           STEP_FLUSH_RELEASE, STEP_COMMIT, STEP_UNLOCK);
    assert(!m.latch && !m.locked && m.leases == 0);
    assert(m.endpoint_resets == 1 && m.reset_endpoint == TX_SAMPLE_ENDPOINT);
    assert(m.fifo_released && m.flush_events == 1);
}

static void test_no_fault_latched(void)
{
    struct mock m;
    memset(&m, 0, sizeof(m));
    assert(bladerf2_tx_fault_recovery_run(&ops, &m, 250) ==
           BLADERF_ERR_INVAL);
    EXPECT(&m, STEP_DRAIN);
    assert(m.endpoint_resets == 0 && m.flush_events == 0);
}

static void test_drain_timeout_touches_no_hardware(void)
{
    struct mock m;
    faulted(&m);
    m.drain_status = BLADERF_ERR_TIMEOUT;
    assert(bladerf2_tx_fault_recovery_run(&ops, &m, 250) ==
           BLADERF_ERR_TIMEOUT);
    EXPECT(&m, STEP_DRAIN);
    assert(m.latch && !m.locked && !m.fifo_draining);
}

static void test_flush_wait_timeout_keeps_fx3_untouched(void)
{
    struct mock m;
    faulted(&m);
    m.flush_wait_status = BLADERF_ERR_TIMEOUT;
    assert(bladerf2_tx_fault_recovery_run(&ops, &m, 250) ==
           BLADERF_ERR_TIMEOUT);
    EXPECT(&m, STEP_DRAIN, STEP_LOCK, STEP_BEGIN, STEP_FLUSH_START,
           STEP_FLUSH_WAIT, STEP_UNLOCK);
    assert(m.latch && m.endpoint_resets == 0 && !m.fifo_released);
}

static void test_clear_halt_failure_stops_before_release(void)
{
    struct mock m;
    faulted(&m);
    m.clear_halt_status = BLADERF_ERR_IO;
    assert(bladerf2_tx_fault_recovery_run(&ops, &m, 250) == BLADERF_ERR_IO);
    EXPECT(&m, STEP_DRAIN, STEP_LOCK, STEP_BEGIN, STEP_FLUSH_START,
           STEP_FLUSH_WAIT, STEP_CLEAR_HALT, STEP_UNLOCK);
    assert(m.latch && !m.fifo_released);
}

static void test_poisoned_rfic_then_retry(void)
{
    struct mock m;
    faulted(&m);
    m.rfic_status = BLADERF_ERR_WOULD_BLOCK;
    assert(bladerf2_tx_fault_recovery_run(&ops, &m, 250) ==
           BLADERF_ERR_WOULD_BLOCK);
    EXPECT(&m, STEP_DRAIN, STEP_LOCK, STEP_BEGIN, STEP_FLUSH_START,
           STEP_FLUSH_WAIT, STEP_CLEAR_HALT, STEP_RFIC_READY, STEP_UNLOCK);
    assert(m.latch && m.fifo_draining && !m.fifo_released);

    /* RFIC reinitialized by its own path; the whole pass is repeatable. */
    m.rfic_status = 0;
    m.trace_len = 0;
    assert(bladerf2_tx_fault_recovery_run(&ops, &m, 250) == 0);
    EXPECT(&m, STEP_DRAIN, STEP_LOCK, STEP_BEGIN, STEP_FLUSH_START,
           STEP_FLUSH_WAIT, STEP_CLEAR_HALT, STEP_RFIC_READY,
           STEP_FLUSH_RELEASE, STEP_COMMIT, STEP_UNLOCK);
    assert(!m.latch && m.endpoint_resets == 2 && m.flush_events == 1);
}

static void test_release_timeout_keeps_latch(void)
{
    struct mock m;
    faulted(&m);
    m.release_status = BLADERF_ERR_TIMEOUT;
    assert(bladerf2_tx_fault_recovery_run(&ops, &m, 250) ==
           BLADERF_ERR_TIMEOUT);
    EXPECT(&m, STEP_DRAIN, STEP_LOCK, STEP_BEGIN, STEP_FLUSH_START,
           STEP_FLUSH_WAIT, STEP_CLEAR_HALT, STEP_RFIC_READY,
           STEP_FLUSH_RELEASE, STEP_UNLOCK);
    assert(m.latch && m.flush_events == 0);
}

static void test_newer_fault_not_retired(void)
{
    struct mock m;
    faulted(&m);
    m.fault_during_clear_halt = true;
    assert(bladerf2_tx_fault_recovery_run(&ops, &m, 250) ==
           BLADERF_ERR_WOULD_BLOCK);
    EXPECT(&m, STEP_DRAIN, STEP_LOCK, STEP_BEGIN, STEP_FLUSH_START,
           STEP_FLUSH_WAIT, STEP_CLEAR_HALT, STEP_RFIC_READY,
           STEP_FLUSH_RELEASE, STEP_COMMIT, STEP_UNLOCK);
    assert(m.latch && m.flush_events == 0);

    m.trace_len = 0;
    assert(bladerf2_tx_fault_recovery_run(&ops, &m, 250) == 0);
    assert(!m.latch && m.flush_events == 1);
}

static int drain_leaving_lease(void *ctx, unsigned int timeout_ms)
{
    struct mock *m = ctx;
    int status = drain(ctx, timeout_ms);
    m->leases = 1;
    return status;
}

static void test_unretired_lease_blocks_flush(void)
{
    struct bladerf2_tx_fault_recovery_ops o = ops;
    struct mock m;
    faulted(&m);
    o.drain_submissions = drain_leaving_lease;
    assert(bladerf2_tx_fault_recovery_run(&o, &m, 250) ==
           BLADERF_ERR_WOULD_BLOCK);
    EXPECT(&m, STEP_DRAIN, STEP_LOCK, STEP_BEGIN, STEP_UNLOCK);
    assert(m.latch && !m.fifo_draining && m.endpoint_resets == 0);
}

int main(void)
{
    test_full_sequence();
    test_no_fault_latched();
    test_drain_timeout_touches_no_hardware();
    test_flush_wait_timeout_keeps_fx3_untouched();
    test_clear_halt_failure_stops_before_release();
    test_poisoned_rfic_then_retry();
    test_release_timeout_keeps_latch();
    test_newer_fault_not_retired();
    test_unretired_lease_blocks_flush();
    assert(bladerf2_tx_fault_recovery_run(NULL, NULL, 250) ==
           BLADERF_ERR_INVAL);
    printf("tx fault recovery: all tests passed\n");
    return 0;
}
