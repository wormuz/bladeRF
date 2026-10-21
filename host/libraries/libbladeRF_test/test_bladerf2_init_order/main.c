/* Order of the bladeRF 2.0 low-level initialization (_bladerf2_initialize),
 * executed for real with the backend, RFIC controllers and board services
 * substituted by recording fakes.
 *
 * Contract under test: the NIOS work queue is reconciled (RETUNE2 queue clear
 * for RX then TX) before any other NIOS/RFIC work, the RFIC is initialized
 * before the VCTCXO trim and PLL are programmed, the RX fault monitor starts
 * last, and STATE_INITIALIZED is published only after every stage succeeded.
 * A failure of any stage aborts the sequence at that stage: nothing later is
 * attempted and the board never reports STATE_INITIALIZED. */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../libbladeRF/src/board/bladerf2/bladerf2.c"

#define MAX_EVENTS 64

static const char *events[MAX_EVENTS];
static unsigned event_count;
static const char *fail_event;
static int fail_status;
static unsigned retune2_clears;
static int state_at_fault_monitor = -1;
static struct bladerf dev;
static struct bladerf2_board_data *bd;

static int ev(const char *name)
{
    if (event_count < MAX_EVENTS)
        events[event_count++] = name;
    if (fail_event != NULL && strcmp(fail_event, name) == 0)
        return fail_status;
    return 0;
}

/* ---- backend ---------------------------------------------------------- */
static int be_get_fpga_version(struct bladerf *d, struct bladerf_version *v)
{
    (void)d;
    memset(v, 0, sizeof(*v));
    v->major = 0; v->minor = 33; v->patch = 0;
    v->describe = "0.33.0";
    return ev("fpga_version");
}
static int be_set_fpga_protocol(struct bladerf *d, backend_fpga_protocol p)
{
    (void)d; (void)p;
    return ev("protocol");
}
static int be_retune2(struct bladerf *d, bladerf_channel ch, uint64_t ts,
                      uint16_t np, uint8_t rp, uint8_t port, uint8_t spdt,
                      uint16_t token)
{
    (void)d; (void)np; (void)rp; (void)port; (void)spdt; (void)token;
    if (ts != NIOS_PKT_RETUNE2_CLEAR_QUEUE)
        return ev("retune2_other");
    ++retune2_clears;
    return ev(BLADERF_CHANNEL_IS_TX(ch) ? "clear_tx" : "clear_rx");
}
static int be_config_gpio_read(struct bladerf *d, uint32_t *v)
{
    (void)d;
    *v = 1u << CFG_GPIO_PLL_EN;   /* PLL enabled, onboard clock */
    return 0;
}
static int be_config_gpio_write(struct bladerf *d, uint32_t v)
{
    (void)d; (void)v;
    return 0;
}
static int be_rffe_control_read(struct bladerf *d, uint32_t *v)
{
    (void)d;
    *v = 1u << RFFE_CONTROL_ADF_MUXOUT;   /* locked */
    return 0;
}
static int be_adf400x_write(struct bladerf *d, uint8_t a, uint32_t v)
{
    (void)d; (void)v;
    return a == 0 ? ev("pll_write") : 0;
}
static int be_trim_write(struct bladerf *d, uint16_t v)
{
    (void)d; (void)v;
    return ev("trim_dac");
}

static struct backend_fns backend = {
    .get_fpga_version = be_get_fpga_version,
    .set_fpga_protocol = be_set_fpga_protocol,
    .retune2 = be_retune2,
    .config_gpio_read = be_config_gpio_read,
    .config_gpio_write = be_config_gpio_write,
    .rffe_control_read = be_rffe_control_read,
    .adf400x_write = be_adf400x_write,
    .ad56x1_vctcxo_trim_dac_write = be_trim_write,
};

/* ---- board services used by the sequence -------------------------------- */
int ina219_init(struct bladerf *d, float r) { (void)d; (void)r; return ev("ina219"); }
const float ina219_r_shunt = 0.001f;
AD9361_InitParam bladerf2_rfic_init_params;
int spi_flash_read_vctcxo_trim(struct bladerf *d, uint16_t *t)
{
    (void)d;
    *t = 0x1000;
    return ev("flash_trim");
}
int bladerf2_rx_fault_monitor_start(struct bladerf *d)
{
    (void)d;
    state_at_fault_monitor = bd->state;
    return ev("fault_monitor");
}
int bladerf2_rx_data_invalidate(struct bladerf *d, bladerf_channel ch,
                                uint32_t reason)
{
    (void)d; (void)ch; (void)reason;
    return 0;
}
void bladerf2_rx_reconfigure_complete(struct bladerf *d, bladerf_channel ch)
{
    (void)d; (void)ch;
}
void bladerf2_tx_stream_event(struct bladerf *d,
                              bladerf_rf_event_type event_type,
                              uint64_t loss_count, uint32_t link_status,
                              int32_t error_code)
{
    (void)d; (void)event_type; (void)loss_count; (void)link_status;
    (void)error_code;
}
bladerf_tuning_mode default_tuning_mode(struct bladerf *d)
{
    (void)d;
    return BLADERF_TUNING_MODE_HOST;
}

/* ---- RFIC controllers --------------------------------------------------- */
static bool host_initialized;
static bool ctl_is_present(struct bladerf *d) { (void)d; return true; }
static int fpga_get_init_state(struct bladerf *d, bladerf_rfic_init_state *s)
{
    (void)d;
    *s = BLADERF_RFIC_INIT_STATE_OFF;
    return ev("rfic_other_state");
}
static int fpga_deinit(struct bladerf *d) { (void)d; return ev("rfic_other_release"); }
static int host_get_init_state(struct bladerf *d, bladerf_rfic_init_state *s)
{
    (void)d;
    *s = host_initialized ? BLADERF_RFIC_INIT_STATE_ON
                          : BLADERF_RFIC_INIT_STATE_OFF;
    return 0;
}
static int host_init(struct bladerf *d)
{
    (void)d;
    host_initialized = true;
    return ev("rfic_init");
}
static int host_deinit(struct bladerf *d) { (void)d; return 0; }

struct controller_fns const rfic_host_control = {
    .is_present = ctl_is_present,
    .get_init_state = host_get_init_state,
    .initialize = host_init,
    .deinitialize = host_deinit,
};
struct controller_fns const rfic_fpga_control = {
    .is_present = ctl_is_present,
    .get_init_state = fpga_get_init_state,
    .deinitialize = fpga_deinit,
};

#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "FAIL line %d: ", __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); return 1; } } while (0)

static void setup(const char *fail, int status)
{
    memset(&dev, 0, sizeof(dev));
    bd = calloc(1, sizeof(*bd));
    event_count = 0;
    retune2_clears = 0;
    host_initialized = false;
    state_at_fault_monitor = -1;
    fail_event = fail;
    fail_status = status;
    dev.backend = &backend;
    dev.board = &bladerf2_board_fns;
    dev.board_data = bd;
    MUTEX_INIT(&dev.lock);
    MUTEX_INIT(&bd->rx_async_epoch_lock);
    bd->tuning_mode = (bladerf_tuning_mode)-1;
    bd->state = STATE_FPGA_LOADED;
    bd->fw_version.major = 2; bd->fw_version.minor = 6; bd->fw_version.patch = 1;
}

static void teardown(void)
{
    MUTEX_DESTROY(&bd->rx_async_epoch_lock);
    MUTEX_DESTROY(&dev.lock);
    free(bd);
}

static int index_of(const char *name)
{
    unsigned i;

    for (i = 0; i < event_count; i++)
        if (strcmp(events[i], name) == 0)
            return (int)i;
    return -1;
}

int main(void)
{
    static const char *const order[] = {
        "fpga_version", "protocol", "clear_rx", "clear_tx", "ina219",
        "rfic_other_release", "rfic_init", "flash_trim", "trim_dac",
        "pll_write", "fault_monitor",
    };
    const unsigned n = sizeof(order) / sizeof(order[0]);
    unsigned i, k;
    int status, prev;

    /* Full success: key events in exactly this order, each once. */
    setup(NULL, 0);
    status = _bladerf2_initialize(&dev);
    CHECK(status == 0, "initialize failed: %d", status);
    prev = -1;
    for (i = 0; i < n; i++) {
        int at = index_of(order[i]);

        CHECK(at >= 0, "stage %s never ran", order[i]);
        CHECK(at > prev, "stage %s ran out of order (index %d, previous %d)",
              order[i], at, prev);
        prev = at;
    }
    CHECK(retune2_clears == 2, "expected RX and TX queue clears, got %u",
          retune2_clears);
    CHECK(index_of("retune2_other") < 0, "unexpected RETUNE2 request");
    CHECK(state_at_fault_monitor == STATE_FPGA_LOADED,
          "STATE_INITIALIZED published before the RX fault monitor started");
    CHECK(bd->state == STATE_INITIALIZED, "state not published on success");
    teardown();

    /* A failure at any stage ends the sequence there. */
    for (k = 0; k < n; k++) {
        int expected_status = -100 - (int)k;

        setup(order[k], expected_status);
        status = _bladerf2_initialize(&dev);
        if (strcmp(order[k], "flash_trim") == 0) {
            /* A missing stored trim falls back to the default DAC value by
             * design; initialization continues. */
            CHECK(status == 0, "flash trim failure aborted initialization");
            teardown();
            continue;
        }
        CHECK(status == expected_status, "stage %s failing: status %d, want %d",
              order[k], status, expected_status);
        CHECK(strcmp(events[event_count - 1], order[k]) == 0,
              "stage %s failing: %s ran after it", order[k],
              events[event_count - 1]);
        CHECK(bd->state != STATE_INITIALIZED,
              "stage %s failing but board reports initialized", order[k]);
        teardown();
    }

    /* The queue clear precedes every other NIOS access that mutates state. */
    setup("clear_rx", BLADERF_ERR_TIMEOUT);
    status = _bladerf2_initialize(&dev);
    CHECK(status == BLADERF_ERR_TIMEOUT && index_of("clear_tx") < 0 &&
          index_of("ina219") < 0 && index_of("rfic_init") < 0,
          "RX queue-clear failure was followed by NIOS/RFIC work");
    teardown();

    printf("bladeRF2 initialization order: PASS\n");
    return 0;
}
