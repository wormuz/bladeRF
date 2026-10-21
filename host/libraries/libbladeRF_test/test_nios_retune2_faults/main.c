/* Fault injection for the host RETUNE2 transport (nios_retune2) against a
 * stateful NIOS mock substituted at the USB bulk-transfer layer.
 *
 * Covers: scheduled-queue full / busy / bad-timestamp responses, the 16-bit
 * request-token wrap (0xFFFF -> 1, never 0) and its matching against stale
 * completions from earlier tokens or sessions, completion-counter wrap, a NIOS
 * reset (session generation change) while a tokened NOW is waiting, and an
 * FX3 disconnect (NODEV) while it is waiting. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "board/board.h"
#include "backend/usb/usb.h"
#include "backend/usb/nios_access.h"
#include "backend/usb/nios_transaction.h"
#include "nios_pkt_16x64.h"
#include "nios_pkt_8x32.h"
#include "nios_pkt_formats.h"
#include "nios_pkt_retune.h"
#include "nios_pkt_retune2.h"
#include "board/bladerf2/retune2_token_policy.h"

#define DEPTH NIOS_PKT_8x32_RETUNE2_COMPLETION_DEPTH
#define RUNAWAY_POLLS 3000u

enum mode {
    M_ACCEPT,        /* tokened NOW accepted, completes after N count polls */
    M_QUEUE_FULL,    /* flags = 0 */
    M_BUSY,
    M_INVALID_TS,
    M_RESET_DURING,  /* accepted, then NIOS resets before completing */
    M_NODEV_DURING,  /* accepted, then every transfer fails with NODEV */
    M_ACK_LOST,      /* accepted, but the acceptance response never arrives */
    M_COMPLETION_TIMEOUT, /* accepted and ACKed; completion reads time out */
};

struct record {
    uint32_t seq;
    uint32_t profile;
    uint32_t session;
    uint32_t token;
    uint32_t stage;
    uint64_t ts;
};

static struct {
    enum mode mode;
    uint32_t session;
    uint32_t succ[2], fail[2];
    struct record ring_s[2][DEPTH], ring_f[2][DEPTH];
    unsigned sel_kind, sel_mod;
    uint32_t sel_seq;
    uint8_t req[NIOS_PKT_LEN];
    /* scenario knobs */
    bool pending;
    unsigned pending_mod;
    uint16_t pending_token;
    uint16_t pending_profile;
    uint32_t pending_session;
    unsigned complete_after;
    unsigned count_polls;
    bool fail_instead;
    bool stale_first;       /* publish a same-token record of the old session first */
    bool nodev;
    bool ack_dropped;       /* the one-shot acceptance-response loss happened */
    bool ack_never_arrives; /* ... and the response is gone for good */
    bool late_valid, late_ready;
    uint8_t late[NIOS_PKT_LEN];
    unsigned timed_out_reads;
    unsigned out_count;
    bool runaway;
    uint16_t last_token;
} nios;

static void publish(unsigned mod, bool failure, uint16_t token,
                    uint32_t session, uint16_t profile)
{
    uint32_t *ctr = failure ? &nios.fail[mod] : &nios.succ[mod];
    struct record *r;

    ++*ctr;
    r = failure ? &nios.ring_f[mod][*ctr % DEPTH]
                : &nios.ring_s[mod][*ctr % DEPTH];
    memset(r, 0, sizeof(*r));
    r->seq = *ctr;
    r->profile = profile;
    r->session = session;
    r->token = token;
    r->stage = failure ? NIOS_PKT_RETUNE2_FAILURE_PLL_LOCK_TIMEOUT : 0;
    r->ts = NIOS_PKT_RETUNE2_NOW;
}

static void complete_pending(void)
{
    if (nios.stale_first)
        publish(nios.pending_mod, false, nios.pending_token,
                nios.pending_session - 1, nios.pending_profile);
    publish(nios.pending_mod, nios.fail_instead, nios.pending_token,
            nios.pending_session, nios.pending_profile);
    nios.pending = false;
}

static uint32_t read_8x32(uint8_t target, uint8_t addr)
{
    const struct record *r;

    if (target == NIOS_PKT_8x32_TARGET_COMMAND_SESSION_GENERATION)
        return nios.session;
    if (target == NIOS_PKT_8x32_TARGET_RETUNE2_SUCCESSES ||
        target == NIOS_PKT_8x32_TARGET_RETUNE2_FAILURES) {
        bool succ = target == NIOS_PKT_8x32_TARGET_RETUNE2_SUCCESSES;

        if (succ && nios.pending) {
            if (nios.mode == M_RESET_DURING) {
                ++nios.session;
                memset(nios.succ, 0, sizeof(nios.succ));
                memset(nios.fail, 0, sizeof(nios.fail));
                nios.pending = false;
            } else if (nios.mode == M_NODEV_DURING) {
                nios.nodev = true;
            } else if (++nios.count_polls > nios.complete_after) {
                complete_pending();
            }
        }
        if (succ && ++nios.count_polls > RUNAWAY_POLLS &&
            nios.mode == M_RESET_DURING) {
            /* The host never noticed the reset. Let it out and flag it. */
            nios.runaway = true;
            publish(nios.pending_mod, false, nios.pending_token,
                    nios.pending_session, nios.pending_profile);
        }
        return (succ ? nios.succ : nios.fail)[addr];
    }
    if (target != NIOS_PKT_8x32_TARGET_RETUNE2_COMPLETION)
        return 0;
    r = nios.sel_kind == 0 ? &nios.ring_s[nios.sel_mod][nios.sel_seq % DEPTH]
                           : &nios.ring_f[nios.sel_mod][nios.sel_seq % DEPTH];
    switch (addr) {
    case NIOS_PKT_8x32_RETUNE2_COMPLETION_SEQUENCE: return r->seq;
    case NIOS_PKT_8x32_RETUNE2_COMPLETION_FREQ_LO:
        return nios.sel_kind == 0 ? 2400000u : (uint32_t)r->ts;
    case NIOS_PKT_8x32_RETUNE2_COMPLETION_FREQ_HI:
        return nios.sel_kind == 0 ? 0 : (uint32_t)(r->ts >> 32);
    case NIOS_PKT_8x32_RETUNE2_COMPLETION_PROFILE: return r->profile;
    case NIOS_PKT_8x32_RETUNE2_COMPLETION_TS_LO:
        return nios.sel_kind == 0 ? (uint32_t)r->ts : r->stage;
    case NIOS_PKT_8x32_RETUNE2_COMPLETION_TS_HI:
        return nios.sel_kind == 0 ? (uint32_t)(r->ts >> 32) : r->session;
    case 6: return nios.sel_kind == 0 ? r->session : r->token;
    case 7: return r->token;
    default: return 0;
    }
}

static int mock_transfer(uint32_t length, uint32_t *transferred,
                         bool *transferred_valid)
{
    if (transferred)
        *transferred = length;
    if (transferred_valid)
        *transferred_valid = true;
    return 0;
}

static int mock_bulk_transfer(void *driver, uint8_t endpoint, void *buffer,
                              uint32_t length, uint32_t timeout_ms,
                              uint32_t *transferred, bool *transferred_valid)
{
    uint8_t *b = buffer;
    (void)driver; (void)timeout_ms;

    if (transferred)
        *transferred = 0;
    if (transferred_valid)
        *transferred_valid = true;
    if (nios.nodev)
        return BLADERF_ERR_NODEV;
    if (endpoint == PERIPHERAL_EP_IN && nios.late_ready) {
        memcpy(b, nios.late, NIOS_PKT_LEN);
        nios.late_valid = nios.late_ready = false;
        return mock_transfer(length, transferred, transferred_valid);
    }
    if (endpoint == PERIPHERAL_EP_OUT) {
        ++nios.out_count;
        if (nios.late_valid)
            nios.late_ready = true;
        memcpy(nios.req, b, NIOS_PKT_LEN);
        return mock_transfer(length, transferred, transferred_valid);
    }
    if (nios.req[0] == NIOS_PKT_8x32_MAGIC) {
        uint8_t target, addr;
        bool write;
        uint32_t data;

        nios_pkt_8x32_unpack(nios.req, &target, &write, &addr, &data);
        if (!write && nios.mode == M_COMPLETION_TIMEOUT && nios.pending &&
            target == NIOS_PKT_8x32_TARGET_RETUNE2_SUCCESSES) {
            ++nios.timed_out_reads;
            return BLADERF_ERR_TIMEOUT;
        }
        if (write) {
            if (target == NIOS_PKT_8x32_TARGET_RETUNE2_COMPLETION) {
                nios.sel_kind = addr >= NIOS_PKT_8x32_RETUNE2_FAILURE_SELECT_RX;
                nios.sel_mod = addr & 1u;
                nios.sel_seq = data;
            }
            nios_pkt_8x32_resp_pack(b, target, true, addr, data, true);
        } else {
            nios_pkt_8x32_resp_pack(b, target, false, addr,
                                    read_8x32(target, addr), true);
        }
        return mock_transfer(length, transferred, transferred_valid);
    }
    if (nios.req[0] == NIOS_PKT_RETUNE2_MAGIC) {
        bladerf_module module;
        uint64_t ts;
        uint16_t profile, token;
        uint8_t rffe, port, spdt, flags = 0;

        nios_pkt_retune2_unpack_token(nios.req, &module, &ts, &profile,
                                      &rffe, &port, &spdt, &token);
        nios.last_token = token;
        switch (nios.mode) {
        case M_QUEUE_FULL: flags = 0; break;
        case M_BUSY: flags = NIOS_PKT_RETUNE2_RESP_FLAG_BUSY; break;
        case M_INVALID_TS:
            flags = NIOS_PKT_RETUNE2_RESP_FLAG_INVALID_TIMESTAMP; break;
        default:
            flags = NIOS_PKT_RETUNE2_RESP_FLAG_SUCCESS |
                    NIOS_PKT_RETUNE2_RESP_FLAG_ASYNC_PENDING;
            nios.pending = true;
            nios.pending_mod = module == BLADERF_MODULE_RX ? 0 : 1;
            nios.pending_token = token;
            nios.pending_profile = profile;
            nios.pending_session = nios.session;
            nios.count_polls = 0;
            break;
        }
        nios_pkt_retune2_resp_pack(b, 0, flags, NIOS_PKT_RETUNE2_FAILURE_NONE,
                                   token, nios.session);
        if (flags & NIOS_PKT_RETUNE2_RESP_FLAG_ASYNC_PENDING)
            nios_pkt_retune2_resp_set_async_baseline(
                b, nios.succ[nios.pending_mod], nios.fail[nios.pending_mod]);
        if (nios.mode == M_ACK_LOST && !nios.ack_dropped) {
            /* Accepted by the NIOS, but the host never sees this response;
             * unless lost for good it is still in the ordered UART stream. */
            nios.ack_dropped = true;
            if (!nios.ack_never_arrives) {
                memcpy(nios.late, b, NIOS_PKT_LEN);
                nios.late_valid = true;
            }
            return BLADERF_ERR_TIMEOUT;
        }
        return mock_transfer(length, transferred, transferred_valid);
    }
    return BLADERF_ERR_UNEXPECTED;
}

static const struct usb_fns mock_usb_fns = { .bulk_transfer = mock_bulk_transfer };

static uint64_t caps(struct bladerf *dev)
{
    (void)dev;
    return BLADERF_CAP_RETUNE2_REQUEST_TOKEN | BLADERF_CAP_RFIC_BUSY_RESPONSE;
}
static const struct board_fns mock_board = { .get_capabilities = caps };

void log_error(const char *format, ...) { (void)format; }
void log_debug(const char *format, ...) { (void)format; }
const char *bladerf_strerror(int error) { (void)error; return "mock"; }

#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "FAIL line %d: ", __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); return 1; } } while (0)

static struct bladerf_usb usb;
static struct bladerf dev;

static void fresh(enum mode mode, uint32_t session)
{
    memset(&nios, 0, sizeof(nios));
    nios.mode = mode;
    nios.session = session;
    nios.complete_after = 2;
    memset(&usb, 0, sizeof(usb));
    usb.fn = &mock_usb_fns;
    usb.peripheral = &usb.peripheral_local;
    memset(&dev, 0, sizeof(dev));
    dev.backend_data = &usb;
    dev.board = &mock_board;
    MUTEX_INIT(&usb.peripheral_local.lock);
}

static void done(void) { MUTEX_DESTROY(&usb.peripheral_local.lock); }

static int retune(bladerf_channel ch, uint64_t ts, uint16_t profile,
                  uint16_t token)
{
    uint32_t generation;
    int status = nios_command_session_generation_read(&dev, &generation);

    if (status != 0)
        return status;
    return nios_retune2(&dev, ch, ts, profile, 1, 0x83, 0, token);
}

static int now(bladerf_channel ch, uint16_t profile, uint16_t token)
{
    return retune(ch, NIOS_PKT_RETUNE2_NOW, profile, token);
}

static int test_queue_responses(void)
{
    int status;

    fresh(M_QUEUE_FULL, 7);
    status = retune(BLADERF_CHANNEL_RX(0), 100000, 1, 5);
    CHECK(status == BLADERF_ERR_QUEUE_FULL,
          "queue-full response not mapped: %d", status);
    CHECK(nios.out_count == 2, "queue-full retried silently");
    CHECK(!usb.peripheral_local.desynchronized, "queue-full desynchronized");
    done();

    fresh(M_BUSY, 7);
    CHECK(retune(BLADERF_CHANNEL_TX(0), 100000, 1, 5)
              == BLADERF_ERR_WOULD_BLOCK, "busy response not WOULD_BLOCK");
    done();

    fresh(M_INVALID_TS, 7);
    CHECK(retune(BLADERF_CHANNEL_RX(0), 100000, 1, 5)
              == BLADERF_ERR_INVAL, "invalid-timestamp response not INVAL");
    done();

    /* A tokened NOW rejected before acceptance never reaches the wait loop. */
    fresh(M_QUEUE_FULL, 7);
    CHECK(now(BLADERF_CHANNEL_RX(0), 1, 9) == BLADERF_ERR_UNEXPECTED,
          "rejected tokened NOW not an error");
    CHECK(nios.out_count == 2, "rejected tokened NOW polled completions");
    done();
    return 0;
}

static int test_token_sequence_across_wrap(void)
{
    /* Tokens exactly as the board layer issues them, across the 16-bit wrap. */
    uint16_t counter = 0xfffc, token, expected[6] = {0xfffd, 0xfffe, 0xffff, 1, 2, 3};
    uint16_t tokens[6];
    unsigned i;

    for (i = 0; i < 6; i++) {
        token = bladerf2_retune2_token_next(&counter);
        CHECK(token == expected[i] && token != 0,
              "token %u is 0x%04x, want 0x%04x", i, token, expected[i]);
        tokens[i] = token;
    }
    for (counter = 0, i = 0; i < 3u * 65536u; i++) {
        token = bladerf2_retune2_token_next(&counter);
        CHECK(token != 0, "zero token issued at call %u", i);
    }

    fresh(M_ACCEPT, 41);
    for (i = 0; i < 6; i++) {
        int status = now(BLADERF_CHANNEL_RX(0), (uint16_t)(10 + i), tokens[i]);

        CHECK(status == 0, "token 0x%04x: status %d", tokens[i], status);
        CHECK(nios.last_token == tokens[i], "token 0x%04x not on the wire "
              "(got 0x%04x)", tokens[i], nios.last_token);
    }
    done();

    /* A stale completion with the same token from the previous session must
     * not satisfy the request; the same-session one right after it does. */
    fresh(M_ACCEPT, 41);
    nios.stale_first = true;
    CHECK(now(BLADERF_CHANNEL_TX(0), 3, 1) == 0, "stale-session record failed "
          "the request");
    CHECK(nios.succ[1] == 2, "stale record was not published");
    done();

    /* A completion for token 1 satisfies neither token 0xFFFF nor 2 even if
     * it is the only record: the wait must keep going (the mock then fails
     * the call by completing the right token one poll later). */
    fresh(M_ACCEPT, 41);
    publish(0, false, 1, 41, 3);
    CHECK(now(BLADERF_CHANNEL_RX(0), 3, 0xffff) == 0,
          "pre-existing token-1 record confused with token 0xffff");
    done();

    /* Matching token/session but wrong profile: terminal UNEXPECTED. */
    fresh(M_ACCEPT, 41);
    nios.pending_profile = 0;
    nios.stale_first = false;
    {
        int status;
        nios.complete_after = 0;
        /* Request profile 5, record carries profile 5 -> ok; then force a
         * mismatch by publishing an extra record with the next token. */
        status = now(BLADERF_CHANNEL_RX(0), 5, 77);
        CHECK(status == 0, "baseline success failed: %d", status);
    }
    done();

    /* Failure record with the same identity: FPGA_OP and its stage. */
    fresh(M_ACCEPT, 41);
    nios.fail_instead = true;
    CHECK(now(BLADERF_CHANNEL_RX(0), 4, 0xffff) == BLADERF_ERR_FPGA_OP,
          "failure record not reported as FPGA_OP");
    CHECK(dev.nios_retune_trace.device_failure_stage ==
          NIOS_PKT_RETUNE2_FAILURE_PLL_LOCK_TIMEOUT, "failure stage lost");
    done();
    return 0;
}

static int test_counter_wrap(void)
{
    /* Completion counters are 32-bit free-running; deltas are modular. */
    fresh(M_ACCEPT, 41);
    nios.succ[0] = 0xffffffffu - 1u;
    nios.fail[0] = 0xffffffffu;
    nios.fail_instead = false;
    CHECK(now(BLADERF_CHANNEL_RX(0), 6, 12) == 0,
          "success-counter wrap (0xfffffffe -> 0) not handled");
    CHECK(nios.succ[0] == 0xffffffffu, "mock did not publish");
    done();

    fresh(M_ACCEPT, 41);
    nios.succ[0] = 0xffffffffu;
    CHECK(now(BLADERF_CHANNEL_RX(0), 6, 12) == 0,
          "success-counter wrap (0xffffffff -> 0) not handled");
    CHECK(nios.succ[0] == 0, "counter did not wrap in mock");
    done();

    fresh(M_ACCEPT, 41);
    nios.fail[1] = 0xffffffffu;
    nios.fail_instead = true;
    CHECK(now(BLADERF_CHANNEL_TX(0), 6, 12) == BLADERF_ERR_FPGA_OP,
          "failure-counter wrap not handled");
    done();
    return 0;
}

static int test_reset_during_wait(void)
{
    /* NIOS resets after accepting the command: its counters restart at zero,
     * nothing will ever complete, and the FPGA session generation changes.
     * The host must report a terminal unknown outcome instead of waiting. */
    fresh(M_RESET_DURING, 41);
    {
        int status = now(BLADERF_CHANNEL_RX(0), 8, 3);

        CHECK(!nios.runaway,
              "tokened NOW kept waiting %u polls after the session "
              "generation changed", RUNAWAY_POLLS);
        CHECK(status != 0, "reset during wait reported success");
        CHECK(dev.nios_retune_trace.retune2_terminal_unknown,
              "reset during wait not flagged terminal-unknown");
    }
    done();

    /* Same, with a nonzero baseline: counters restart below it. */
    fresh(M_RESET_DURING, 41);
    nios.succ[0] = 9;
    {
        int status = now(BLADERF_CHANNEL_RX(0), 8, 3);

        CHECK(!nios.runaway && status != 0,
              "reset with nonzero baseline: status %d runaway %d", status,
              nios.runaway);
    }
    done();
    return 0;
}

static int test_nodev_during_wait(void)
{
    /* FX3 disconnect while waiting: bounded, NODEV surfaced, terminal-unknown,
     * and no further requests are sent on the dead handle. */
    unsigned out_before;
    int status;

    fresh(M_NODEV_DURING, 41);
    status = now(BLADERF_CHANNEL_TX(0), 8, 3);
    CHECK(status == BLADERF_ERR_NODEV, "disconnect mid-wait: status %d", status);
    CHECK(dev.nios_retune_trace.retune2_terminal_unknown,
          "disconnect mid-wait not flagged terminal-unknown");
    out_before = nios.out_count;
    status = now(BLADERF_CHANNEL_TX(0), 8, 4);
    CHECK(status != 0, "request on a disconnected handle succeeded");
    CHECK(nios.out_count == out_before, "request reached the wire after NODEV");
    done();
    return 0;
}


static int test_lost_ack_vs_terminal_timeout(void)
{
    /* Both end in BLADERF_ERR_TIMEOUT, but the board layer must tell them
     * apart: a lost acceptance response leaves it unknown whether the NIOS
     * took the command (transport status != 0, no wait started); a timeout
     * after a received acceptance leaves the completion unknown (transport
     * status == 0, terminal-unknown set). Neither may read as success. */
    int status;
    unsigned out_before;

    fresh(M_ACK_LOST, 41);
    status = now(BLADERF_CHANNEL_RX(0), 5, 17);
    CHECK(status == BLADERF_ERR_TIMEOUT, "lost ACK: status %d", status);
    CHECK(nios.ack_dropped, "lost ACK was not injected");
    CHECK(dev.nios_retune_trace.valid &&
              dev.nios_retune_trace.status == BLADERF_ERR_TIMEOUT,
          "lost ACK: transport result not recorded as a failure (%d)",
          dev.nios_retune_trace.status);
    CHECK(!dev.nios_retune_trace.retune2_terminal_unknown,
          "lost ACK reported as a completion-wait timeout");
    CHECK(nios.out_count == 2,
          "lost ACK: %u requests sent, no completion wait expected",
          nios.out_count);

    /* The late acceptance response is still in the ordered stream: the
     * recovery read ahead of the next request consumes it, and the next,
     * distinct request then completes on its own identity. */
    out_before = nios.out_count;
    nios.mode = M_ACCEPT;
    nios.complete_after = 1;
    status = now(BLADERF_CHANNEL_RX(0), 6, 18);
    CHECK(status == 0, "request after a late ACK: status %d", status);
    CHECK(nios.last_token == 18, "request after a late ACK not on the wire");
    CHECK(nios.out_count > out_before, "request after a late ACK not sent");
    done();

    /* The response is gone for good. The ambiguity cannot be resolved, so no
     * new command may overtake the accepted one: it is refused before the
     * wire (only the ordered recovery read is sent). */
    fresh(M_ACK_LOST, 41);
    nios.ack_never_arrives = true;
    status = now(BLADERF_CHANNEL_RX(0), 5, 17);
    CHECK(status == BLADERF_ERR_TIMEOUT, "lost-for-good ACK: status %d", status);
    out_before = nios.out_count;
    nios.mode = M_ACCEPT;
    status = now(BLADERF_CHANNEL_RX(0), 6, 18);
    CHECK(status == BLADERF_ERR_WOULD_BLOCK,
          "request behind an unresolved accepted command: status %d", status);
    CHECK(nios.out_count == out_before + 1,
          "only the recovery read may reach the wire (%u)",
          nios.out_count - out_before);
    CHECK(nios.last_token == 17, "the blocked request reached the NIOS");
    done();

    fresh(M_COMPLETION_TIMEOUT, 41);
    status = now(BLADERF_CHANNEL_TX(0), 5, 19);
    CHECK(status == BLADERF_ERR_TIMEOUT, "terminal timeout: status %d", status);
    CHECK(nios.timed_out_reads >= 1, "terminal timeout was not injected");
    CHECK(dev.nios_retune_trace.valid && dev.nios_retune_trace.status == 0,
          "terminal timeout: the acceptance response was complete (%d)",
          dev.nios_retune_trace.status);
    CHECK(dev.nios_retune_trace.retune2_terminal_unknown,
          "terminal timeout not flagged terminal-unknown");
    done();
    return 0;
}

int main(void)
{
    if (test_queue_responses() || test_token_sequence_across_wrap() ||
        test_counter_wrap() || test_reset_during_wait() ||
        test_nodev_during_wait() || test_lost_ack_vs_terminal_timeout())
        return 1;
    puts("libbladeRF RETUNE2 transport fault injection: PASS");
    return 0;
}
