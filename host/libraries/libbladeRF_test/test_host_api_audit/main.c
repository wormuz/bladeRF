/* Public-API audit (BL-HOST-API-AUDIT-001).
 *
 * Every entry point that takes a channel must reject an out-of-range channel
 * with BLADERF_ERR_INVAL before any side effect: RFIC/NIOS/GPIO access, array
 * indexing or board-state mutation. The fixture has no usable backend and no
 * RFIC controller, so reaching any of them dereferences NULL; that is caught
 * and reported as a failure naming the entry point. The same calls with a
 * valid channel must get past channel validation (control), otherwise the
 * fixture would make the check vacuous.
 *
 * The second half pins the NIOS 8x16 write BUSY retry to a deadline against a
 * scripted USB transport.
 */
#include <assert.h>
#include <limits.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "host_config.h"
#include "board/board.h"
#include "board/bladerf2/common.h"
#include "backend/usb/usb.h"
#include "backend/usb/nios_access.h"
#include "nios_pkt_formats.h"
#include "bladeRF2.h"

static struct bladerf dev;
static struct bladerf2_board_data bd;

static sigjmp_buf jump;
static volatile sig_atomic_t armed;

static void on_fault(int sig)
{
    (void)sig;
    if (armed) {
        siglongjmp(jump, 1);
    }
    _exit(70);
}

static struct bladerf_quick_tune qt;

static void reset_fixture(void)
{
    memset(&dev, 0, sizeof(dev));
    memset(&bd, 0, sizeof(bd));
    assert(MUTEX_INIT(&dev.lock) == 0);
    assert(MUTEX_INIT(&bd.rx_async_epoch_lock) == 0);
    bd.state = STATE_INITIALIZED;
    bd.rfic_reference_clock_known = true;
    bd.rfic_frequency_model_known = true;
    dev.board = &bladerf2_board_fns;
    dev.board_data = &bd;
    memset(&qt, 0, sizeof(qt));
    qt.port = 0x83;
}

extern struct controller_fns const rfic_host_control;

typedef int (*entry_fn)(struct bladerf *, bladerf_channel);

#define NEEDS_SWEEP 1u
#define NEEDS_EPOCH 2u
#define NEEDS_CALFAULT 4u

struct entry {
    const char *name;
    entry_fn fn;
    unsigned int needs;
};

static char const *stage_name = "rf";
static char const *name_out[8];
static struct bladerf_range const *range_out;
static struct bladerf_gain_modes const *modes_out;
static struct bladerf_trigger trig;
static uint8_t profile[64];
static uint8_t u8;
static int16_t s16;
static int ival;
static int32_t pre, sym;
static bool flag;
static bladerf_gain_mode gmode;
static bladerf_sample_rate srate;
static bladerf_bandwidth bw;
static bladerf_frequency freq;
static struct bladerf_rational_rate rr = { .integer = 1000000, .num = 0, .den = 1 };
static const struct bladerf_gain_cal_tbl *cal_tbl;
static struct bladerf_rx_sweep_record records[2];
static unsigned int count_out;
static uint32_t seq, txid;
static uint16_t w1, w2;
static struct bladerf_rx_transition_request tr_req;

#define B(n, call) \
    static int b_##n(struct bladerf *d, bladerf_channel ch) { return call; }
#define P(n, call) \
    static int p_##n(struct bladerf *d, bladerf_channel ch) { return call; }

B(enable_module, d->board->enable_module(d, ch, true))
B(get_gain_range, d->board->get_gain_range(d, ch, &range_out))
B(get_gain_modes, d->board->get_gain_modes(d, ch, &modes_out))
B(get_gain_stages, d->board->get_gain_stages(d, ch, name_out, 8))
B(get_gain_mode, d->board->get_gain_mode(d, ch, &gmode))
B(set_gain_mode, d->board->set_gain_mode(d, ch, BLADERF_GAIN_MGC))
B(get_gain, d->board->get_gain(d, ch, &ival))
B(set_gain, d->board->set_gain(d, ch, 10))
B(get_gain_stage, d->board->get_gain_stage(d, ch, stage_name, &ival))
B(set_gain_stage, d->board->set_gain_stage(d, ch, stage_name, 10))
B(get_gain_stage_range,
  d->board->get_gain_stage_range(d, ch, stage_name, &range_out))
B(get_sample_rate_range, d->board->get_sample_rate_range(d, ch, &range_out))
B(get_rational_sample_rate, d->board->get_rational_sample_rate(d, ch, &rr))
B(set_rational_sample_rate, d->board->set_rational_sample_rate(d, ch, &rr, NULL))
B(get_sample_rate, d->board->get_sample_rate(d, ch, &srate))
B(set_sample_rate, d->board->set_sample_rate(d, ch, 1000000, NULL))
B(get_bandwidth_range, d->board->get_bandwidth_range(d, ch, &range_out))
B(get_bandwidth, d->board->get_bandwidth(d, ch, &bw))
B(set_bandwidth, d->board->set_bandwidth(d, ch, 1000000, NULL))
B(get_frequency_range, d->board->get_frequency_range(d, ch, &range_out))
B(get_frequency, d->board->get_frequency(d, ch, &freq))
B(set_frequency, d->board->set_frequency(d, ch, 1000000000))
B(select_band, d->board->select_band(d, ch, 1000000000))
B(set_rf_port, d->board->set_rf_port(d, ch, "rx1"))
B(get_rf_port, d->board->get_rf_port(d, ch, name_out))
B(get_rf_ports, d->board->get_rf_ports(d, ch, name_out, 8))
B(get_quick_tune, d->board->get_quick_tune(d, ch, &qt))
B(get_quick_tune_profile,
  d->board->get_quick_tune_profile(d, ch, &qt, profile))
B(load_quick_tune_profile,
  d->board->load_quick_tune_profile(d, ch, profile, 0, &qt))
B(schedule_retune, d->board->schedule_retune(d, ch, 0, 1000000000, &qt))
B(cancel_scheduled_retunes, d->board->cancel_scheduled_retunes(d, ch))
B(get_correction, d->board->get_correction(d, ch, BLADERF_CORR_PHASE, &s16))
B(set_correction, d->board->set_correction(d, ch, BLADERF_CORR_PHASE, 0))
B(trigger_init,
  d->board->trigger_init(d, ch, BLADERF_TRIGGER_J51_1, &trig))
B(read_trigger, d->board->read_trigger(d, ch, BLADERF_TRIGGER_J51_1, &u8))
B(write_trigger, d->board->write_trigger(d, ch, BLADERF_TRIGGER_J51_1, 0))
B(invalidate_rx_data, d->board->invalidate_rx_data(d, ch, 1))
B(tx_frequency_reconfigure_begin,
  d->board->tx_frequency_reconfigure_begin(d, ch, 1000000000))
B(rx_sweep_begin, d->board->rx_sweep_begin(d, ch, 0))
B(rx_sweep_schedule,
  d->board->rx_sweep_schedule(d, ch, 1000, 1000000000, &qt, &seq))
B(rx_sweep_completions,
  d->board->rx_sweep_completions(d, ch, records, 2, &count_out))
B(rx_sweep_end, d->board->rx_sweep_end(d, ch))
B(rfic_host_set_sample_rate, rfic_host_control.set_sample_rate(d, ch, 1000000))

P(set_gain, bladerf_set_gain(d, ch, 10))
P(get_gain, bladerf_get_gain(d, ch, &ival))
P(set_gain_mode, bladerf_set_gain_mode(d, ch, BLADERF_GAIN_MGC))
P(get_gain_mode, bladerf_get_gain_mode(d, ch, &gmode))
P(get_gain_modes, bladerf_get_gain_modes(d, ch, &modes_out))
P(get_gain_range, bladerf_get_gain_range(d, ch, &range_out))
P(set_gain_stage, bladerf_set_gain_stage(d, ch, stage_name, 10))
P(get_gain_stage, bladerf_get_gain_stage(d, ch, stage_name, &ival))
P(get_gain_stage_range,
  bladerf_get_gain_stage_range(d, ch, stage_name, &range_out))
P(get_gain_stages, bladerf_get_gain_stages(d, ch, name_out, 8))
P(set_sample_rate, bladerf_set_sample_rate(d, ch, 1000000, NULL))
P(set_rational_sample_rate, bladerf_set_rational_sample_rate(d, ch, &rr, NULL))
P(get_sample_rate, bladerf_get_sample_rate(d, ch, &srate))
P(get_sample_rate_range, bladerf_get_sample_rate_range(d, ch, &range_out))
P(get_rational_sample_rate, bladerf_get_rational_sample_rate(d, ch, &rr))
P(set_bandwidth, bladerf_set_bandwidth(d, ch, 1000000, NULL))
P(get_bandwidth, bladerf_get_bandwidth(d, ch, &bw))
P(get_bandwidth_range, bladerf_get_bandwidth_range(d, ch, &range_out))
P(select_band, bladerf_select_band(d, ch, 1000000000))
P(set_frequency, bladerf_set_frequency(d, ch, 1000000000))
P(get_frequency, bladerf_get_frequency(d, ch, &freq))
P(get_frequency_range, bladerf_get_frequency_range(d, ch, &range_out))
P(trigger_init, bladerf_trigger_init(d, ch, BLADERF_TRIGGER_J51_1, &trig))
P(schedule_retune, bladerf_schedule_retune(d, ch, 0, 1000000000, &qt))
P(cancel_scheduled_retunes, bladerf_cancel_scheduled_retunes(d, ch))
P(get_quick_tune, bladerf_get_quick_tune(d, ch, &qt))
P(get_quick_tune_profile, bladerf_get_quick_tune_profile(d, ch, &qt, profile))
P(load_quick_tune_profile,
  bladerf_load_quick_tune_profile(d, ch, profile, 0, &qt))
P(rx_sweep_begin, bladerf_rx_sweep_begin(d, ch, 0))
P(rx_sweep_schedule, bladerf_rx_sweep_schedule(d, ch, 1000, 1000000000, &qt, &seq))
P(rx_sweep_completions,
  bladerf_rx_sweep_completions(d, ch, records, 2, &count_out))
P(rx_sweep_end, bladerf_rx_sweep_end(d, ch))
P(set_correction, bladerf_set_correction(d, ch, BLADERF_CORR_PHASE, 0))
P(get_correction, bladerf_get_correction(d, ch, BLADERF_CORR_PHASE, &s16))
P(enable_module, bladerf_enable_module(d, ch, true))
P(read_trigger, bladerf_read_trigger(d, ch, BLADERF_TRIGGER_J51_1, &u8))
P(write_trigger, bladerf_write_trigger(d, ch, BLADERF_TRIGGER_J51_1, 0))
P(set_rf_port, bladerf_set_rf_port(d, ch, "rx1"))
P(get_rf_port, bladerf_get_rf_port(d, ch, name_out))
P(get_rf_ports, bladerf_get_rf_ports(d, ch, name_out, 8))
P(enable_gain_calibration, bladerf_enable_gain_calibration(d, ch, true))
P(print_gain_calibration, bladerf_print_gain_calibration(d, ch, false))
P(get_gain_calibration, bladerf_get_gain_calibration(d, ch, &cal_tbl))
P(get_gain_target, bladerf_get_gain_target(d, ch, &ival))
P(get_bias_tee, bladerf_get_bias_tee(d, ch, &flag))
P(set_bias_tee, bladerf_set_bias_tee(d, ch, true))
P(set_rx_qec_tracking, bladerf_set_rx_qec_tracking(d, ch, true))
P(get_rx_qec_tracking, bladerf_get_rx_qec_tracking(d, ch, &flag))
P(get_rx_bbdc_tracking_words, bladerf_get_rx_bbdc_tracking_words(d, ch, &w1, &w2))
P(get_rfic_rssi, bladerf_get_rfic_rssi(d, ch, &pre, &sym))
P(rx_transition_begin, bladerf_rx_transition_begin(d, ch, &tr_req, &txid))
P(rx_capture_close, bladerf_rx_capture_close(d, ch))

#define BE(n) { "board." #n, b_##n, 0 }
#define BES(n) { "board." #n, b_##n, NEEDS_SWEEP }
#define PE(n) { "public." #n, p_##n, 0 }
#define PES(n) { "public." #n, p_##n, NEEDS_SWEEP }

static const struct entry entries[] = {
    BE(enable_module), BE(get_gain_range), BE(get_gain_modes),
    BE(get_gain_stages), BE(get_gain_mode), BE(set_gain_mode), BE(get_gain),
    BE(set_gain), BE(get_gain_stage), BE(set_gain_stage),
    BE(get_gain_stage_range), BE(get_sample_rate_range),
    BE(get_rational_sample_rate), BE(set_rational_sample_rate),
    BE(get_sample_rate), BE(set_sample_rate), BE(get_bandwidth_range),
    BE(get_bandwidth), BE(set_bandwidth), BE(get_frequency_range),
    BE(get_frequency), BE(set_frequency), BE(select_band), BE(set_rf_port),
    BE(get_rf_port), BE(get_rf_ports), BE(get_quick_tune),
    BE(get_quick_tune_profile), BE(load_quick_tune_profile),
    BE(schedule_retune), BE(cancel_scheduled_retunes), BE(get_correction),
    BE(set_correction), BE(trigger_init), BE(read_trigger), BE(write_trigger),
    BE(invalidate_rx_data), BE(tx_frequency_reconfigure_begin),
    BE(rx_sweep_begin), BES(rx_sweep_schedule), BE(rx_sweep_completions),
    BES(rx_sweep_end), { "board.rfic_host_set_sample_rate", b_rfic_host_set_sample_rate, NEEDS_CALFAULT },
    PE(set_gain), PE(get_gain), PE(set_gain_mode), PE(get_gain_mode),
    PE(get_gain_modes), PE(get_gain_range), PE(set_gain_stage),
    PE(get_gain_stage), PE(get_gain_stage_range), PE(get_gain_stages),
    PE(set_sample_rate), PE(set_rational_sample_rate), PE(get_sample_rate),
    PE(get_sample_rate_range), PE(get_rational_sample_rate),
    PE(set_bandwidth), PE(get_bandwidth), PE(get_bandwidth_range),
    PE(select_band), PE(set_frequency), PE(get_frequency),
    PE(get_frequency_range), PE(trigger_init), PE(schedule_retune),
    PE(cancel_scheduled_retunes), PE(get_quick_tune),
    PE(get_quick_tune_profile), PE(load_quick_tune_profile),
    PE(rx_sweep_begin), PES(rx_sweep_schedule), PE(rx_sweep_completions),
    PES(rx_sweep_end), PE(set_correction), PE(get_correction),
    PE(enable_module), PE(read_trigger), PE(write_trigger), PE(set_rf_port),
    PE(get_rf_port), PE(get_rf_ports), PE(enable_gain_calibration),
    PE(print_gain_calibration), PE(get_gain_calibration), PE(get_gain_target),
    PE(get_bias_tee), PE(set_bias_tee), PE(set_rx_qec_tracking),
    PE(get_rx_qec_tracking), PE(get_rx_bbdc_tracking_words),
    PE(get_rfic_rssi), PE(rx_transition_begin),
    { "public.rx_capture_close", p_rx_capture_close, NEEDS_EPOCH },
};

static void apply_needs(const struct entry *e)
{
    if (e->needs & NEEDS_SWEEP) {
        bd.rx_sweep.active = true;
        bd.rx_sweep.ch = BLADERF_CHANNEL_RX(0);
    }
    if (e->needs & NEEDS_CALFAULT) {
        bd.calibration_faulted = true;
    }
    if (e->needs & NEEDS_EPOCH) {
        bd.rf_transition_epoch_contract_enabled = true;
        bd.rf_transition_epoch_certified = true;
    }
}

/* Returns true when the call returned (or faulted) past channel validation. */
static bool call_entry(const struct entry *e, bladerf_channel ch, int *status,
                       bool *faulted)
{
    reset_fixture();
    apply_needs(e);
    *faulted = false;
    armed = 1;
    alarm(5);
    if (sigsetjmp(jump, 1) != 0) {
        armed = 0;
        alarm(0);
        *faulted = true;
        *status = 0;
        return true;
    }
    *status = e->fn(&dev, ch);
    alarm(0);
    armed = 0;
    return *status != BLADERF_ERR_INVAL;
}

static int test_channel_validation(void)
{
    static const bladerf_channel invalid[] = {
        4, 5, 6, 7, 8, 100, -1, -2, INT_MAX, INT_MIN, BLADERF_CHANNEL_INVALID,
    };
    static const bladerf_channel valid[] = {
        BLADERF_CHANNEL_RX(0), BLADERF_CHANNEL_RX(1),
        BLADERF_CHANNEL_TX(0), BLADERF_CHANNEL_TX(1),
    };
    int failures = 0;

    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); ++i) {
        const struct entry *e = &entries[i];
        bool any_valid_passes = false;

        for (size_t c = 0; c < sizeof(invalid) / sizeof(invalid[0]); ++c) {
            int status;
            bool faulted;
            bool passed = call_entry(e, invalid[c], &status, &faulted);
            bool dirty = bd.rf_transition_setter_active ||
                         !bd.rfic_reference_clock_known;

            if (passed || dirty) {
                printf("FAIL %s ch=%d: status=%d faulted=%d state_dirty=%d\n",
                       e->name, invalid[c], status, faulted, dirty);
                failures++;
                break;
            }
        }
        for (size_t c = 0; c < sizeof(valid) / sizeof(valid[0]); ++c) {
            int status;
            bool faulted;
            if (call_entry(e, valid[c], &status, &faulted)) {
                any_valid_passes = true;
            }
        }
        if (!any_valid_passes) {
            printf("FAIL %s: no valid channel gets past validation "
                   "(fixture is vacuous)\n", e->name);
            failures++;
        }
    }
    return failures;
}

/* fpga ADC channel index is a raw register-bank selector, not a bladerf_channel */
static int test_adc_channel_index(void)
{
    uint32_t regs[4];
    int failures = 0;

    for (unsigned int ch = 4; ch < 256; ch += 17) {
        int status;
        reset_fixture();
        status = bladerf_get_rx_fpga_adc_chan_regs(&dev, (uint8_t)ch, regs);
        if (status != BLADERF_ERR_INVAL) {
            printf("FAIL get_rx_fpga_adc_chan_regs ch=%u status=%d\n", ch,
                   status);
            failures++;
        }
    }
    return failures;
}

/* ---- NIOS 8x16 BUSY retry deadline ---- */

struct busy_usb {
    int busy_responses_left; /* -1: BUSY forever */
    int in_calls;
    uint8_t last_request[NIOS_PKT_LEN];
};

static int busy_bulk(void *driver, uint8_t endpoint, void *buffer,
                     uint32_t length, uint32_t timeout_ms,
                     uint32_t *transferred, bool *transferred_valid)
{
    struct busy_usb *u = driver;
    (void)timeout_ms;
    if (transferred_valid != NULL) {
        *transferred_valid = true;
    }
    if (transferred != NULL) {
        *transferred = length;
    }
    if (endpoint == PERIPHERAL_EP_OUT) {
        memcpy(u->last_request, buffer, NIOS_PKT_LEN);
        return 0;
    }
    u->in_calls++;
    memcpy(buffer, u->last_request, NIOS_PKT_LEN);
    uint8_t *flags = (uint8_t *)buffer + NIOS_PKT_8x16_IDX_FLAGS;
    *flags &= (uint8_t)~NIOS_PKT_8x16_FLAG_SUCCESS;
    if (u->busy_responses_left != 0) {
        if (u->busy_responses_left > 0) {
            u->busy_responses_left--;
        }
        *flags |= NIOS_PKT_8x16_FLAG_BUSY;
    } else {
        *flags |= NIOS_PKT_8x16_FLAG_SUCCESS;
    }
    return 0;
}

static int run_nios_write(int busy_responses, double *elapsed, int *in_calls)
{
    struct bladerf_usb usb;
    struct usb_fns fns;
    struct busy_usb fake;
    struct timespec t0, t1;
    int status;

    memset(&usb, 0, sizeof(usb));
    memset(&fns, 0, sizeof(fns));
    memset(&fake, 0, sizeof(fake));
    fake.busy_responses_left = busy_responses;
    fns.bulk_transfer = busy_bulk;
    usb.fn = &fns;
    usb.driver = &fake;
    usb.peripheral = &usb.peripheral_local;
    assert(MUTEX_INIT(&usb.peripheral->lock) == 0);

    memset(&dev, 0, sizeof(dev));
    dev.backend_data = &usb;

    clock_gettime(CLOCK_MONOTONIC, &t0);
    status = nios_ina219_write(&dev, 0, 0x1234);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    *elapsed = (double)(t1.tv_sec - t0.tv_sec) +
               (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    *in_calls = fake.in_calls;
    MUTEX_DESTROY(&usb.peripheral->lock);
    return status;
}

static int test_nios_busy_deadline(void)
{
    int failures = 0;
    double elapsed;
    int in_calls;
    int status;

    status = run_nios_write(2, &elapsed, &in_calls);
    if (status != 0 || in_calls != 3) {
        printf("FAIL nios 8x16 BUSY x2 then ACK: status=%d in_calls=%d\n",
               status, in_calls);
        failures++;
    }

    alarm(10);
    status = run_nios_write(-1, &elapsed, &in_calls);
    alarm(0);
    if (status != BLADERF_ERR_TIMEOUT || elapsed < 0.9 || elapsed > 3.0) {
        printf("FAIL nios 8x16 BUSY forever: status=%d elapsed=%.3f s "
               "in_calls=%d\n", status, elapsed, in_calls);
        failures++;
    }
    return failures;
}

/* A pending TX certificate that never arrives must time out, not block. */
static int test_tx_validity_deadline(void)
{
    struct timespec t0, t1;
    double elapsed;
    int status;

    reset_fixture();
    assert(COND_INIT(&bd.rx_async_epoch_cond) == 0);
    bd.rx_async_epoch_cond_initialized = true;
    bd.tx_async_link_certified = true;
    alarm(10);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    status = bladerf2_tx_wait_for_validity(&dev);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    alarm(0);
    elapsed = (double)(t1.tv_sec - t0.tv_sec) +
              (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    if (status != BLADERF_ERR_TIMEOUT || elapsed < 1.9 || elapsed > 4.0) {
        printf("FAIL tx_wait_for_validity: status=%d elapsed=%.3f s\n",
               status, elapsed);
        return 1;
    }
    return 0;
}

int main(void)
{
    struct sigaction sa;
    int failures;

    setvbuf(stdout, NULL, _IONBF, 0);
    bladerf_log_set_verbosity(BLADERF_LOG_LEVEL_SILENT);
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_fault;
    sa.sa_flags = SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGALRM, &sa, NULL);

    failures = test_channel_validation();
    failures += test_adc_channel_index();
    failures += test_nios_busy_deadline();
    failures += test_tx_validity_deadline();
    if (failures != 0) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("test_host_api_audit: PASS\n");
    return 0;
}
