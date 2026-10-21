/* Host harness for the production NIOS RFIC command worker.
 *
 * devices_rfic_queue.c, devices_rfic_cmds.c and devices_rfic.c are compiled
 * unmodified. Only the AD9361 transaction layer and the board registers
 * underneath them are replaced: every retained ADI job takes POLL_STEPS worker
 * passes (poll() == 1) before it reports its scripted terminal status, so the
 * worker's real continuation logic runs between those passes. */
#define DEVICES_INLINE_H_
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "devices.h"
#include "rffe_csr_policy.h"
#define CONTROL_AD9361_RX_IF_CLOCK_ALIVE (1u << 27)

#include "devices_rfic_queue.c"
#include "devices_rfic_cmds.c"
#include "devices_rfic.c"

#define POLL_STEPS 2
#define MAX_EVENTS 4096

enum job_id {
    J_INIT, J_FIR_LOAD, J_FIR_ENABLE, J_SAMPLE_RATE, J_BANDWIDTH, J_RFPLL,
    J_RFDC, J_TXQ, J_COUNT
};
static const char *const job_name[J_COUNT] = {
    "init", "fir_load", "fir_enable", "sample_rate", "bandwidth", "rfpll",
    "rfdc", "txq",
};

static struct ad9361_rf_phy fake_phy;
static struct ad9361_phy_platform_data fake_pdata;
static AD9361_InitParam *const init_params = &bladerf2_rfic_init_params;

static const char *events[MAX_EVENTS];
static unsigned event_count;
static unsigned starts[J_COUNT];
static unsigned polls[J_COUNT];
static int poll_left[J_COUNT];
static unsigned fail_start_n[J_COUNT];  /* k-th start of this job fails (1-based) */
static unsigned fail_poll_n[J_COUNT];   /* k-th started job ends with failure */
static int32_t fail_status;
static unsigned txmute_calls, txmute_fail_after;
static int32_t txmute_record_status, txmute_record_cleanup, txmute_status;
static bool rfpll_force_active; /* real ADI start is inactive when the LO was already requested */
static unsigned unexpected_calls;
static const char *unexpected_last;
static bool txmute_fail;
static unsigned long fake_ticks;
static uint32_t fake_rffe;

AD9361_InitParam bladerf2_rfic_init_params;
AD9361_RXFIRConfig bladerf2_rfic_rx_fir_config, bladerf2_rfic_rx_fir_config_dec2,
    bladerf2_rfic_rx_fir_config_dec4;
AD9361_TXFIRConfig bladerf2_rfic_tx_fir_config, bladerf2_rfic_tx_fir_config_int2,
    bladerf2_rfic_tx_fir_config_int4;

static void ev(const char *name)
{
    if (event_count < MAX_EVENTS)
        events[event_count++] = name;
}

static unsigned ev_count(const char *name)
{
    unsigned n = 0, i;
    for (i = 0; i < event_count; i++)
        n += strcmp(events[i], name) == 0;
    return n;
}

static int ev_index(const char *name, unsigned nth)
{
    unsigned i;
    for (i = 0; i < event_count; i++)
        if (strcmp(events[i], name) == 0 && nth-- == 0)
            return (int)i;
    return -1;
}

unsigned long alt_timestamp(void) { return ++fake_ticks; }
void no_os_mdelay(uint32_t ms) { (void)ms; }
void no_os_udelay(uint32_t us) { (void)us; }
uint32_t control_reg_read(void) { return CONTROL_AD9361_RX_IF_CLOCK_ALIVE; }
bool rffe_csr_read_checked(uint32_t *v) { *v = fake_rffe; return true; }
bool rffe_csr_write_checked(uint32_t v, uint32_t m)
{
    (void)m; fake_rffe = v; return true;
}

static int32_t job_start(enum job_id id)
{
    ev(job_name[id]);
    starts[id]++;
    poll_left[id] = POLL_STEPS;
    if (fail_start_n[id] == starts[id])
        return fail_status;
    return 0;
}

static int32_t job_poll(enum job_id id)
{
    polls[id]++;
    if (poll_left[id] > 0 && --poll_left[id] > 0)
        return 1;
    return fail_poll_n[id] == starts[id] ? fail_status : 0;
}

int32_t ad9361_init_transaction_start(struct ad9361_init_transaction *t,
                                      struct ad9361_rf_phy **phy,
                                      AD9361_InitParam *p)
{
    int32_t s = job_start(J_INIT);
    (void)t; (void)p;
    fake_phy.pdata = &fake_pdata;
    fake_pdata.dig_interface_tune_skipmode = 2;
    *phy = &fake_phy;
    return s;
}
int32_t ad9361_init_transaction_poll(struct ad9361_init_transaction *t)
{
    (void)t; return job_poll(J_INIT);
}
int32_t ad9361_set_rx_fir_config_transaction_start(
    struct ad9361_rf_phy *p, const AD9361_RXFIRConfig *c,
    struct ad9361_fir_load_transaction *t)
{ (void)p; (void)c; (void)t; return job_start(J_FIR_LOAD); }
int32_t ad9361_set_tx_fir_config_transaction_start(
    struct ad9361_rf_phy *p, const AD9361_TXFIRConfig *c,
    struct ad9361_fir_load_transaction *t)
{ (void)p; (void)c; (void)t; return job_start(J_FIR_LOAD); }
int32_t ad9361_fir_load_transaction_poll(struct ad9361_rf_phy *p,
                                         struct ad9361_fir_load_transaction *t)
{ (void)p; (void)t; return job_poll(J_FIR_LOAD); }
int32_t ad9361_fir_enable_pair_transaction_start(
    struct ad9361_rf_phy *p, uint8_t rx, uint8_t tx,
    struct ad9361_fir_enable_transaction *t)
{ (void)p; (void)rx; (void)tx; (void)t; return job_start(J_FIR_ENABLE); }
int32_t ad9361_fir_enable_transaction_start(
    struct ad9361_rf_phy *p, bool tx, uint8_t en,
    struct ad9361_fir_enable_transaction *t)
{ (void)p; (void)tx; (void)en; (void)t; return job_start(J_FIR_ENABLE); }
int32_t ad9361_fir_enable_transaction_poll(
    struct ad9361_rf_phy *p, struct ad9361_fir_enable_transaction *t)
{ (void)p; (void)t; return job_poll(J_FIR_ENABLE); }
int32_t ad9361_sample_rate_transaction_start(
    struct ad9361_rf_phy *p, uint32_t hz, bool tx,
    struct ad9361_sample_rate_transaction *t)
{
    int32_t s = job_start(J_SAMPLE_RATE);
    (void)p; (void)hz; (void)tx;
    t->active = s >= 0;
    return s;
}
int32_t ad9361_sample_rate_transaction_poll(
    struct ad9361_rf_phy *p, struct ad9361_sample_rate_transaction *t)
{ (void)p; (void)t; return job_poll(J_SAMPLE_RATE); }
int32_t ad9361_rf_bandwidth_transaction_start(
    struct ad9361_rf_phy *p, uint32_t rx, uint32_t tx,
    struct ad9361_rf_bandwidth_transaction *t)
{ (void)p; (void)rx; (void)tx; (void)t; return job_start(J_BANDWIDTH); }
int32_t ad9361_rf_bandwidth_transaction_poll(
    struct ad9361_rf_phy *p, struct ad9361_rf_bandwidth_transaction *t)
{ (void)p; (void)t; return job_poll(J_BANDWIDTH); }
int32_t ad9361_rfpll_clock_transaction_start(
    struct ad9361_rf_phy *p, struct refclk_scale *c, uint32_t hz,
    struct ad9361_rfpll_clock_transaction *t)
{
    int32_t s = job_start(J_RFPLL);
    (void)p; (void)c; (void)hz;
    t->active = s >= 0 && rfpll_force_active;
    if (!t->active && s >= 0)
        poll_left[J_RFPLL] = 0;
    return s;
}
int32_t ad9361_rfpll_clock_transaction_poll(
    struct ad9361_rf_phy *p, struct ad9361_rfpll_clock_transaction *t)
{ (void)p; (void)t; return job_poll(J_RFPLL); }
int32_t ad9361_rfdc_calibration_start(struct ad9361_rf_phy *p,
                                      struct ad9361_rfdc_calibration_job *j)
{ (void)p; (void)j; return job_start(J_RFDC); }
int32_t ad9361_rfdc_calibration_poll(struct ad9361_rf_phy *p,
                                     struct ad9361_rfdc_calibration_job *j)
{ (void)p; (void)j; return job_poll(J_RFDC); }
int32_t ad9361_ensure_tx_quad_calib_start(
    struct ad9361_rf_phy *p, struct ad9361_tx_quad_calibration_transaction *t)
{ (void)p; (void)t; return job_start(J_TXQ); }
int32_t ad9361_tx_quad_calibration_transaction_poll(
    struct ad9361_rf_phy *p, struct ad9361_tx_quad_calibration_transaction *t)
{ (void)p; (void)t; return job_poll(J_TXQ); }

bool ad9361_calibration_state_unknown(const struct ad9361_rf_phy *p)
{ (void)p; return false; }
void ad9361_mark_calibration_state_unknown(struct ad9361_rf_phy *p)
{ (void)p; ev("cal_unknown"); }
int32_t ad9361_remove_ex(struct ad9361_rf_phy *p, bool reset, bool *consumed)
{ (void)p; (void)reset; ev("remove"); *consumed = true; return 0; }
void ad9361_fastlock_invalidate_cache(struct ad9361_rf_phy *p, bool tx)
{ (void)p; (void)tx; }
void ad9361_fastlock_invalidate_current_lo(struct ad9361_rf_phy *p, bool tx)
{ (void)p; (void)tx; }
int32_t ad9361_fastlock_exit_foreign(struct ad9361_rf_phy *p, bool tx)
{ (void)p; (void)tx; return 0; }
int32_t ad9361_fastlock_sync_current_lo(struct ad9361_rf_phy *p, bool tx,
                                        uint32_t prof, uint64_t exp,
                                        uint64_t *hz)
{ (void)p; (void)tx; (void)prof; (void)exp; (void)hz; return 0; }
bool adi_fastlock_load_active(void) { return false; }
bool adi_fastlock_recall_active(void) { return false; }
uint32_t ad9361_to_clk(uint64_t f) { return (uint32_t)f; }
int32_t ad9361_get_rx_rf_port_input(struct ad9361_rf_phy *p, uint32_t *m)
{ (void)p; *m = 0; return 0; }
int set_ad9361_port_by_freq(struct ad9361_rf_phy *p, bladerf_channel ch,
                            bool enabled, bladerf_frequency f)
{ (void)p; (void)ch; (void)enabled; (void)f; return 0; }

static int txmute_hook(const char *name)
{
    ev(name);
    txmute_calls++;
    if (txmute_fail && txmute_calls > txmute_fail_after) {
        if (txmute_record_status < 0)
            rfic_command_record_error(txmute_record_status);
        if (txmute_record_cleanup < 0)
            rfic_command_record_cleanup_error(txmute_record_cleanup);
        return txmute_status;
    }
    return 0;
}
int txmute_set(struct ad9361_rf_phy *p, bladerf_channel ch, bool s)
{ (void)p; (void)ch; (void)s; return txmute_hook("txmute_set"); }
int txmute_set_cached(struct ad9361_rf_phy *p, bladerf_channel ch, uint32_t a)
{ (void)p; (void)ch; (void)a; return txmute_hook("txmute_set_cached"); }
int txmute_get(struct ad9361_rf_phy *p, bladerf_channel ch, bool *s)
{ (void)p; (void)ch; *s = false; return 0; }
uint32_t txmute_get_cached(struct ad9361_rf_phy *p, bladerf_channel ch)
{ (void)p; (void)ch; return 0; }
int32_t ad9361_tx_fastlock_store(struct ad9361_rf_phy *p, uint32_t prof)
{ (void)p; (void)prof; return 0; }
int32_t ad9361_rx_fastlock_store(struct ad9361_rf_phy *p, uint32_t prof)
{ (void)p; (void)prof; return 0; }

/* ---- driver helpers ----------------------------------------------------- */
#define RFIC_ADDR(cmd, ch) ((uint16_t)((((ch) & 0xF) << 8) | (cmd)))
#define SYS_CH BLADERF_CHANNEL_INVALID

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
    printf("\n"); } } while (0)

static void harness_reset(void)
{
    memset(starts, 0, sizeof(starts));
    memset(polls, 0, sizeof(polls));
    memset(poll_left, 0, sizeof(poll_left));
    memset(fail_start_n, 0, sizeof(fail_start_n));
    memset(fail_poll_n, 0, sizeof(fail_poll_n));
    memset(&fake_phy, 0, sizeof(fake_phy));
    memset(&fake_pdata, 0, sizeof(fake_pdata));
    event_count = 0;
    fail_status = -EIO;
    txmute_calls = 0;
    txmute_fail = false;
    txmute_fail_after = 0;
    txmute_status = -EIO;
    txmute_record_status = 0;
    txmute_record_cleanup = 0;
    rfpll_force_active = false;
    unexpected_calls = 0;
    fake_rffe = 0;
    init_params->digital_interface_tune_skip_mode = 2;
    init_params->digital_interface_tune_fir_disable = 1;
    init_params->rx_synthesizer_frequency_hz = 1000000000ull;
    init_params->tx_synthesizer_frequency_hz = 1000000000ull;
    init_params->rx_path_clock_frequencies[5] = 1000000u;
    init_params->tx_path_clock_frequencies[5] = 1000000u;
    rfic_command_init();
}

static bool rq(uint8_t cmd, int ch, uint64_t value, uint16_t tid)
{
    return rfic_command_write_with_transaction_id(RFIC_ADDR(cmd, ch), value,
                                                  tid);
}

/* Run worker passes until nothing is queued or pending. Returns the pass
 * count, or -1 if the bound is hit (a runaway worker). */
static int drain(unsigned max_passes)
{
    unsigned i;
    for (i = 0; i < max_passes; i++) {
        if (!rfic_command_worker_active())
            return (int)i;
        rfic_command_work();
    }
    return -1;
}

static uint32_t retired(void) { return state.write_queue.last_rv_sequence; }

/* Entry points the tested paths must not reach. Reaching one is a failure. */
#define UNEXPECTED(ret, name, params) \
    ret name params { unexpected_calls++; unexpected_last = #name; return (ret)-ENOTSUP; }
UNEXPECTED(int32_t, ad9361_dig_tune_with_cleanup,
           (struct ad9361_rf_phy *p, uint32_t f, enum dig_tune_flags fl, int32_t *c))
UNEXPECTED(int32_t, ad9361_get_rx_gain, (struct ad9361_rf_phy *p, uint32_t i, struct rf_rx_gain *g))
UNEXPECTED(int32_t, ad9361_get_rx_gain_control_mode, (struct ad9361_rf_phy *p, uint8_t c, uint8_t *m))
UNEXPECTED(int32_t, ad9361_get_rx_rf_bandwidth, (struct ad9361_rf_phy *p, uint32_t *b))
UNEXPECTED(int32_t, ad9361_get_rx_rssi, (struct ad9361_rf_phy *p, uint8_t c, struct rf_rssi *r))
UNEXPECTED(int32_t, ad9361_get_tx_attenuation, (struct ad9361_rf_phy *p, uint8_t c, uint32_t *a))
UNEXPECTED(int32_t, ad9361_get_tx_rf_bandwidth, (struct ad9361_rf_phy *p, uint32_t *b))
UNEXPECTED(int32_t, ad9361_get_tx_rssi, (struct ad9361_rf_phy *p, uint8_t c, uint32_t *r))
UNEXPECTED(int32_t, ad9361_set_rx_fir_config_with_cleanup,
           (struct ad9361_rf_phy *p, AD9361_RXFIRConfig c, int32_t *s))
UNEXPECTED(int32_t, ad9361_set_tx_fir_config_with_cleanup,
           (struct ad9361_rf_phy *p, AD9361_TXFIRConfig c, int32_t *s))
UNEXPECTED(int32_t, ad9361_set_rx_gain_control_mode, (struct ad9361_rf_phy *p, uint8_t c, uint8_t m))
UNEXPECTED(int32_t, ad9361_set_rx_lo_freq, (struct ad9361_rf_phy *p, uint64_t f))
UNEXPECTED(int32_t, ad9361_set_tx_lo_freq, (struct ad9361_rf_phy *p, uint64_t f))
UNEXPECTED(int32_t, ad9361_set_rx_rf_gain, (struct ad9361_rf_phy *p, uint8_t c, int32_t g))
UNEXPECTED(int32_t, ad9361_set_rx_rf_port_input, (struct ad9361_rf_phy *p, uint32_t m))
UNEXPECTED(int32_t, ad9361_set_tx_attenuation, (struct ad9361_rf_phy *p, uint8_t c, uint32_t a))
UNEXPECTED(int32_t, ad9361_set_tx_rf_port_output, (struct ad9361_rf_phy *p, uint32_t m))
UNEXPECTED(int32_t, ad9361_txmon_enable, (struct ad9361_rf_phy *p, int32_t m))
UNEXPECTED(uint32_t, ad9361_validate_rf_bw, (struct ad9361_rf_phy *p, uint32_t b))
int32_t ad9361_get_rx_lo_freq(struct ad9361_rf_phy *p, uint64_t *f)
{ (void)p; *f = 1000000000ull; return 0; }
int32_t ad9361_get_tx_lo_freq(struct ad9361_rf_phy *p, uint64_t *f)
{ (void)p; *f = 1000000000ull; return 0; }
int32_t ad9361_get_rx_sampling_freq(struct ad9361_rf_phy *p, uint32_t *f)
{ (void)p; *f = 1000000u; return 0; }
int32_t ad9361_get_tx_sampling_freq(struct ad9361_rf_phy *p, uint32_t *f)
{ (void)p; *f = 1000000u; return 0; }
