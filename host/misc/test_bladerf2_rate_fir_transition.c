/* bladerf2_set_sample_rate() 4x FIR transitions, run on the real no-OS AD9361
 * driver with only the host platform replaced (ad9361_flat_platform.h).
 *
 * set_rate() below performs the RFIC steps of bladerf2_set_sample_rate() in
 * the same order, with the decisions taken by rate_fir_policy.h. Built with
 * -DRATE_FIR_MASTER_LOGIC it takes the decisions of c7405ad6 instead
 * (intermediate step only from > 40 MHz, 4x range tested on the board rate,
 * which OVERSAMPLE doubles) and the test fails.
 *
 * Contract under test:
 *  - DEFAULT: 3.84 <-> 1.92 MHz (and 1.92 MHz from 2.5-61.44 MHz) succeeds;
 *    entering DEC4 straight from 3.84 MHz fails the AD9361 TAPS check;
 *  - DEFAULT 1.92 MHz (DEC4/INT4), then OVERSAMPLE 7.68 MHz restores the
 *    default filters, so OVERSAMPLE 122.88 MHz succeeds afterwards. */
#include "ad9361_flat_platform.h"
#include "../libraries/libbladeRF/src/board/bladerf2/rate_fir_policy.h"

#define LO_4X 520834u
#define HI_4X 2083334u

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; \
    printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

struct model {
    struct ad9361_rf_phy *phy;
    bool rx_dec4, tx_int4; /* bladerf2 rxfir/txfir cache: DEC4/INT4 or default */
};

static void model_init(struct model *m)
{
    static struct ad9361_init_transaction job;
    int32_t st;

    memset(&job, 0, sizeof(job));
    memset(m, 0, sizeof(*m));
    platform_reset();
    st = ad9361_init_transaction_start(&job, &m->phy,
                                       &bladerf2_rfic_init_params);
    while (st >= 0 && !job.terminal) {
        st = ad9361_init_transaction_poll(&job);
        if (st == 1)
            now_us += ad9361_init_transaction_poll_delay_us(&job);
    }
    if (st < 0 || m->phy == NULL) {
        printf("FAIL: AD9361 init %d\n", st);
        exit(2);
    }
    /* _rfic_host_initialize: RX DEC1, TX bypass */
    if (ad9361_set_rx_fir_config(m->phy, bladerf2_rfic_rx_fir_config) < 0 ||
        ad9361_set_rx_fir_en_dis(m->phy, 1) < 0 ||
        ad9361_set_tx_fir_config(m->phy, bladerf2_rfic_tx_fir_config) < 0 ||
        ad9361_set_tx_fir_en_dis(m->phy, 0) < 0) {
        printf("FAIL: default FIRs\n");
        exit(2);
    }
}

static int32_t rfic_rate(struct model *m, uint32_t rate)
{
    return ad9361_set_tx_sampling_freq(m->phy, rate);
}

/* The RX FIR step defers TX QUAD calibration as bladerf2_set_sample_rate()
 * does for the transient RX/TX clock ratio. */
static int32_t set_rx_fir(struct model *m, bool dec4)
{
    bool manual = m->phy->manual_tx_quad_cal_en;
    int32_t st = ad9361_set_rx_fir_config(
        m->phy, dec4 ? bladerf2_rfic_rx_fir_config_dec4
                     : bladerf2_rfic_rx_fir_config);
    m->phy->manual_tx_quad_cal_en = true;
    if (st >= 0)
        st = ad9361_set_rx_fir_en_dis(m->phy, 1);
    m->phy->manual_tx_quad_cal_en = manual;
    if (st >= 0)
        m->rx_dec4 = dec4;
    return st;
}

static int32_t set_tx_fir(struct model *m, bool int4)
{
    int32_t st = ad9361_set_tx_fir_config(
        m->phy, int4 ? bladerf2_rfic_tx_fir_config_int4
                     : bladerf2_rfic_tx_fir_config);
    if (st >= 0)
        st = ad9361_set_tx_fir_en_dis(m->phy, int4 ? 1 : 0);
    if (st >= 0)
        m->tx_int4 = int4;
    return st;
}

static int32_t set_rate(struct model *m, uint32_t rate, bool oversample)
{
    uint32_t current = m->phy->clks[TX_SAMPL_CLK]->rate;
    struct bladerf2_rate_fir_plan plan;
    bool intermediate;
    int32_t st;

#ifdef RATE_FIR_MASTER_LOGIC
    /* board getter: doubled under OVERSAMPLE; requested rate not halved */
    if (oversample)
        current *= 2;
    plan = bladerf2_rate_fir_classify(current, rate, false, LO_4X, HI_4X);
    intermediate = current > 40000000u && rate < HI_4X;
#else
    plan = bladerf2_rate_fir_classify(current, rate, oversample, LO_4X, HI_4X);
    intermediate = true;
#endif
    bladerf2_rate_fir_actions(&plan, m->rx_dec4 && m->tx_int4,
                              !m->rx_dec4 && !m->tx_int4);
    if (plan.enter_4x) {
        if (intermediate &&
            (st = rfic_rate(m, BLADERF2_RATE_FIR_INTERMEDIATE)) < 0)
            return st;
        if ((st = set_rx_fir(m, true)) < 0 || (st = set_tx_fir(m, true)) < 0)
            return st;
    }
    if (oversample)
        rate /= 2;
    if (plan.leave_4x) {
        if ((st = rfic_rate(m, BLADERF2_RATE_FIR_INTERMEDIATE)) < 0 ||
            (st = set_rx_fir(m, false)) < 0 || (st = set_tx_fir(m, false)) < 0)
            return st;
    }
    return rfic_rate(m, rate);
}

static void test_direct_dec4_fails_taps(void)
{
    struct model m;
    model_init(&m);
    CHECK(rfic_rate(&m, 3840000) == 0, "3.84 MHz");
    CHECK(set_rx_fir(&m, true) == -EINVAL,
          "DEC4 straight from 3.84 MHz must fail the TAPS check");
}

static void test_default_sub_native(void)
{
    static const uint32_t starts[] = { 2500000, 3840000, 7680000, 12000000,
                                       15360000, 30720000, 61440000 };
    for (unsigned i = 0; i < sizeof(starts) / sizeof(starts[0]); ++i) {
        struct model m;
        model_init(&m);
        CHECK(set_rate(&m, starts[i], false) == 0, "start %u", starts[i]);
        CHECK(set_rate(&m, 1920000, false) == 0, "%u -> 1.92 MHz", starts[i]);
        CHECK(m.rx_dec4 && m.tx_int4, "%u -> 1.92 MHz: 4x FIRs", starts[i]);
    }
}

static void test_default_alternation(void)
{
    struct model m;
    model_init(&m);
    CHECK(set_rate(&m, 3840000, false) == 0, "3.84 MHz");
    for (int i = 0; i < 4; ++i) {
        CHECK(set_rate(&m, 1920000, false) == 0, "round %d: 1.92 MHz", i);
        CHECK(set_rate(&m, 3840000, false) == 0, "round %d: 3.84 MHz", i);
        CHECK(!m.rx_dec4 && !m.tx_int4, "round %d: default FIRs", i);
    }
}

static void test_oversample_leaves_4x(void)
{
    struct model m;
    model_init(&m);
    CHECK(set_rate(&m, 30720000, false) == 0, "30.72 MHz");
    CHECK(set_rate(&m, 1920000, false) == 0, "1.92 MHz");
    CHECK(set_rate(&m, 7680000, true) == 0, "OVERSAMPLE 7.68 MHz");
    CHECK(!m.rx_dec4 && !m.tx_int4,
          "OVERSAMPLE 7.68 MHz (AD9361 3.84) keeps DEC4/INT4");
    CHECK(set_rate(&m, 122880000, true) == 0, "OVERSAMPLE 122.88 MHz");
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    test_direct_dec4_fails_taps();
    test_default_sub_native();
    test_default_alternation();
    test_oversample_leaves_4x();
    if (failures != 0) {
        printf("bladerf2 rate FIR transition: %d FAILURES\n", failures);
        return 1;
    }
    puts("bladerf2 rate FIR transition: PASS");
    return 0;
}
