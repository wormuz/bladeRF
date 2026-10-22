/* RX ring ordering through the real sync worker callbacks and sync_rx().
 *
 * The sync worker source is included so the static rx_callback() is driven
 * exactly as libusb/async.c drives it. A FIFO models the USB transfer queue;
 * every completed transfer carries a monotonically increasing tag, so any
 * data returned out of hardware order is visible as a tag going backwards.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "streaming/sync_worker.c"

#define NUM_BUFFERS 12u
#define NUM_XFERS 4u
#define SPB 512u
#define MAX_FIFO 64u

/* Linked board helpers reference the bladeRF2 identity token only. */
const struct board_fns bladerf2_board_fns = { .name = "bladerf2" };
static const struct board_fns test_board = { .name = "ring-order-test" };

struct harness {
    struct bladerf dev;
    struct bladerf_sync s;
    struct sync_worker w;
    void *buffers[NUM_BUFFERS];
    size_t lengths[NUM_BUFFERS];
    sync_buffer_status states[NUM_BUFFERS];
    uint32_t seq[NUM_BUFFERS];
    bool dropped[NUM_BUFFERS];
    void *fifo[MAX_FIFO];
    unsigned int head, count;
    uint32_t tag;
};

static void fifo_push(struct harness *h, void *buf)
{
    assert(h->count < MAX_FIFO);
    h->fifo[(h->head + h->count) % MAX_FIFO] = buf;
    h->count++;
}

static void *fifo_pop(struct harness *h)
{
    void *buf;
    assert(h->count > 0);
    buf = h->fifo[h->head];
    h->head = (h->head + 1) % MAX_FIFO;
    h->count--;
    return buf;
}

static void harness_init(struct harness *h, unsigned int num_buffers,
                         unsigned int num_xfers)
{
    struct buffer_mgmt *b = &h->s.buf_mgmt;

    memset(h, 0, sizeof(*h));
    assert(num_buffers <= NUM_BUFFERS && num_xfers < num_buffers);
    for (unsigned int i = 0; i < num_buffers; ++i) {
        h->buffers[i] = calloc(SPB, 4);
        assert(h->buffers[i] != NULL);
    }

    h->dev.board = &test_board;
    h->s.dev = &h->dev;
    h->s.worker = &h->w;
    assert(MUTEX_INIT(&h->s.lock) == 0);
    assert(MUTEX_INIT(&h->s.rx_reader_lock) == 0);
    assert(MUTEX_INIT(&h->s.rx_epoch_generation_lock) == 0);
    assert(MUTEX_INIT(&b->lock) == 0);
    assert(COND_INIT(&b->buf_ready) == 0);
    assert(MUTEX_INIT(&h->w.state_lock) == 0);
    assert(MUTEX_INIT(&h->w.request_lock) == 0);
    assert(COND_INIT(&h->w.state_changed) == 0);
    assert(COND_INIT(&h->w.requests_pending) == 0);
    h->w.state = SYNC_WORKER_STATE_RUNNING;

    h->s.initialized = true;
    h->s.state = SYNC_STATE_WAIT_FOR_BUFFER;
    h->s.stream_config.format = BLADERF_FORMAT_SC16_Q11;
    h->s.stream_config.layout = BLADERF_RX_X1;
    h->s.stream_config.samples_per_buffer = SPB;
    h->s.stream_config.bytes_per_sample = 4;
    h->s.stream_config.num_xfers = num_xfers;
    h->s.meta.samples_per_ts = 1;

    b->buffers = h->buffers;
    b->actual_lengths = h->lengths;
    b->status = h->states;
    b->buffer_seq = h->seq;
    b->buffer_dropped = h->dropped;
    b->num_buffers = num_buffers;
    b->submitter = SYNC_TX_SUBMITTER_INVALID;

    /* Same ring start state as exec_idle_state() for RX. */
    b->cons_i = 0;
    b->prod_i = num_xfers;
    for (unsigned int i = 0; i < num_buffers; ++i) {
        h->states[i] = i < num_xfers ? SYNC_BUFFER_IN_FLIGHT
                                     : SYNC_BUFFER_EMPTY;
    }
    sync_reset_sequence_tracking(b, num_xfers);
    for (unsigned int i = 0; i < num_xfers; ++i) {
        fifo_push(h, h->buffers[i]);
    }
}

static void harness_destroy(struct harness *h)
{
    for (unsigned int i = 0; i < h->s.buf_mgmt.num_buffers; ++i) {
        free(h->buffers[i]);
    }
}

/* One transfer completed with application-valid IQ. */
static void complete_valid(struct harness *h)
{
    struct bladerf_metadata meta = {0};
    uint32_t *samples = fifo_pop(h);
    void *next;

    for (unsigned int i = 0; i < SPB; ++i) {
        samples[i] = h->tag;
    }
    h->tag++;
    next = rx_callback(&h->dev, NULL, &meta, samples, SPB, &h->s);
    assert(next != NULL && next != BLADERF_STREAM_NO_DATA &&
           next != BLADERF_STREAM_REUSE_BUFFER);
    fifo_push(h, next);
}

/* One transfer rejected by the async epoch fence (async_rx_process_buffer). */
static void complete_rejected(struct harness *h)
{
    struct bladerf_metadata meta = {0};
    void *buf = fifo_pop(h);
    void *replacement;

    h->tag++; /* These samples are lost; time still advances. */
    replacement = sync_worker_rx_buffer_rejected(&h->s, buf);
    assert(rx_callback(&h->dev, NULL, &meta, NULL, 0, &h->s) ==
           BLADERF_STREAM_REUSE_BUFFER);
    fifo_push(h, replacement);
}

/* Returns true and the buffer tag when one complete buffer was read. */
static bool read_one_timeout(struct harness *h, uint32_t *tag,
                             unsigned int timeout_ms)
{
    static uint32_t out[SPB];
    int status = sync_rx(&h->s, out, SPB, NULL, timeout_ms);

    if (status != 0) {
        assert(status == BLADERF_ERR_TIMEOUT);
        return false;
    }
    for (unsigned int i = 1; i < SPB; ++i) {
        assert(out[i] == out[0]);
    }
    *tag = out[0];
    return true;
}

/* Data is known to be released: a generous deadline only bounds a hang. */
static bool read_one(struct harness *h, uint32_t *tag)
{
    return read_one_timeout(h, tag, 1000);
}

/* Nothing is released: exercise the consumer's wait path briefly. */
static void read_nothing(struct harness *h)
{
    uint32_t tag;
    assert(!read_one_timeout(h, &tag, 1));
}

/* An epoch fence rejects a ring lap of completions while the application is
 * not reading, so the producer re-issues the consumer's slot for a later
 * sequence. The consumer must still read the oldest released buffer first
 * and must never return an older buffer after a newer one. */
static void test_consumer_follows_sequence_after_rejected_lap(void)
{
    struct harness h;
    const unsigned int rejected = NUM_BUFFERS - NUM_XFERS + 1;
    uint32_t tag, expect;

    harness_init(&h, NUM_BUFFERS, NUM_XFERS);
    for (unsigned int i = 0; i < rejected; ++i) {
        complete_rejected(&h);
    }
    /* Precondition of the live failure: slot 0 now carries a later live
     * sequence while expected_seq lives in another slot. */
    assert(h.states[0] == SYNC_BUFFER_IN_FLIGHT && !h.dropped[0]);
    assert(h.seq[0] != h.s.buf_mgmt.expected_seq);

    expect = h.tag;
    for (unsigned int lap = 0; lap < 4 * NUM_BUFFERS; ++lap) {
        complete_valid(&h);
        complete_valid(&h);
        assert(read_one(&h, &tag));
        assert(tag == expect);
        expect++;
        assert(read_one(&h, &tag));
        assert(tag == expect);
        expect++;
    }
    read_nothing(&h);
    harness_destroy(&h);
}

static uint32_t rng_state = 0x12345678u;
static uint32_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static void dump_ring(struct harness *h, unsigned int run, unsigned int step)
{
    struct buffer_mgmt *b = &h->s.buf_mgmt;
    fprintf(stderr, "run=%u step=%u cons=%u prod=%u expected=%u next=%u "
            "state=%d reorder_len=%zu stale=%u overrun=%u\n", run, step,
            b->cons_i, b->prod_i, b->expected_seq, b->next_seq,
            (int)h->s.state, b->reorder_len, b->stale_pending,
            b->overrun_pending);
    for (unsigned int i = 0; i < b->num_buffers; ++i) {
        fprintf(stderr, "  [%u] st=%u seq=%u drop=%u tag=%u\n", i,
                (unsigned)b->status[i], b->buffer_seq[i], b->buffer_dropped[i],
                ((uint32_t *)b->buffers[i])[0]);
    }
}

/* Random fence episodes, completions and reads. Whenever the ring cannot
 * overflow (reads keep up), every valid buffer is read exactly once and in
 * hardware order. */
static void test_random_fences_preserve_order(void)
{
    for (unsigned int run = 0; run < 2000; ++run) {
        struct harness h;
        uint32_t tag, last = 0;
        bool have_last = false;
        unsigned int unread = 0;
        bool fence = false;

        harness_init(&h, NUM_BUFFERS, NUM_XFERS);
        for (unsigned int step = 0; step < 400; ++step) {
            const uint32_t r = rng() % 100;
            if (r < 4) {
                fence = !fence;
            }
            if (r < 60) {
                /* The ring holds at most NUM_BUFFERS - NUM_XFERS released
                 * buffers; stay below so no overrun path discards data. */
                if (!fence && unread >= NUM_BUFFERS - NUM_XFERS - 1) {
                    continue;
                }
                if (fence) {
                    complete_rejected(&h);
                } else {
                    complete_valid(&h);
                    unread++;
                }
            } else if (!fence || r > 90) {
                if (unread == 0) {
                    if (r > 97) {
                        read_nothing(&h);
                    }
                    continue;
                }
                if (!read_one(&h, &tag)) {
                    dump_ring(&h, run, step);
                    assert(!"released RX buffer was not delivered");
                }
                unread--;
                if (have_last) {
                    assert(tag > last);
                }
                last = tag;
                have_last = true;
            }
        }
        while (unread > 0) {
            assert(read_one(&h, &tag));
            assert(!have_last || tag > last);
            last = tag;
            have_last = true;
            unread--;
        }
        assert(!h.s.buf_mgmt.overrun_pending);
        harness_destroy(&h);
    }
}

#ifdef ENABLE_LIBBLADERF_SYNC_LOG_VERBOSE
/* dump_buf_states() runs on every sync_rx() iteration. Its state string must
 * be sized for the ring being dumped, also when a later stream in the same
 * process uses a larger ring (run under AddressSanitizer). */
static void test_state_dump_follows_ring_size(void)
{
    struct harness small, large;
    uint32_t tag;

    log_set_verbosity(BLADERF_LOG_LEVEL_VERBOSE);
    harness_init(&small, 2, 1);
    complete_valid(&small);
    assert(read_one(&small, &tag));
    harness_destroy(&small);

    harness_init(&large, NUM_BUFFERS, NUM_XFERS);
    complete_valid(&large);
    assert(read_one(&large, &tag));
    harness_destroy(&large);
    log_set_verbosity(BLADERF_LOG_LEVEL_ERROR);
}
#endif

int main(void)
{
    log_set_verbosity(BLADERF_LOG_LEVEL_ERROR);
#ifdef ENABLE_LIBBLADERF_SYNC_LOG_VERBOSE
    test_state_dump_follows_ring_size();
#endif
    test_consumer_follows_sequence_after_rejected_lap();
    test_random_fences_preserve_order();
    printf("sync RX ring order tests passed\n");
    return 0;
}
