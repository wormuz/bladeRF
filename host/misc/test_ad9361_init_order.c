/* Cold INIT of the real no-OS AD9361 driver, run end to end through its
 * retained transactions (ad9361_init_transaction_* owning
 * ad9361_setup_transaction_*), with only the host platform replaced
 * (ad9361_flat_platform.h).
 *
 * Contract under test:
 *  - INIT stages run in order RESETB, product id, lock config, clocks, setup,
 *    platform post-setup; the lock-config loop is the only repetition;
 *  - the setup parent advances through its stages in enum order (the gain
 *    table row loop is the only repetition), every stage once per pass, and
 *    keeps ownership of the PHY between caller polls;
 *  - the register prefix is written by the stage that owns it;
 *  - PLATFORM_POST_SETUP is entered only after the setup parent retired
 *    successfully;
 *  - a hardware write failing in ANY setup stage ends INIT with that error,
 *    never reaches PLATFORM_POST_SETUP, leaves no child job or wait active,
 *    publishes no PHY, leaks nothing and keeps the terminal result sticky. */
#include "ad9361_flat_platform.h"

#define MAX_POLLS 20000
#define MAX_VISITS 256

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; \
    printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

struct run {
    int32_t status;
    unsigned polls;
    int init_visits[MAX_VISITS], n_init;
    int setup_visits[MAX_VISITS], n_setup;
    bool reached_post_setup;
    bool owner_dropped_between_polls;
    bool setup_wrote_in_stage[128];
    bool post_setup_before_setup_terminal;
    struct ad9361_init_transaction job;
    struct ad9361_rf_phy *phy;
};

static void note(int *list, int *n, int v)
{
    if (*n == 0 || list[*n - 1] != v)
        if (*n < MAX_VISITS)
            list[(*n)++] = v;
}

static void do_run(struct run *r, int inject_stage)
{
    int32_t st;
    unsigned wc_before;

    memset(r, 0, sizeof(*r));
    platform_reset();
    inject_setup_stage = inject_stage;
    st = ad9361_init_transaction_start(&r->job, &r->phy,
                                       &bladerf2_rfic_init_params);
    CHECK(st == 0, "start: %d", st);
    for (r->polls = 0; r->polls < MAX_POLLS && st >= 0; r->polls++) {
        cur_init_stage = r->job.stage;
        cur_setup_stage = r->job.setup_job.stage;
        note(r->init_visits, &r->n_init, cur_init_stage);
        if (cur_init_stage == AD9361_INIT_TXN_SETUP_POLL)
            note(r->setup_visits, &r->n_setup, cur_setup_stage);
        if (cur_init_stage == AD9361_INIT_TXN_PLATFORM_POST_SETUP) {
            r->reached_post_setup = true;
            if (!r->job.setup_job.terminal ||
                r->job.setup_job.terminal_status < 0)
                r->post_setup_before_setup_terminal = true;
        }
        wc_before = write_count;
        st = ad9361_init_transaction_poll(&r->job);
        if (write_count != wc_before && cur_init_stage == AD9361_INIT_TXN_SETUP_POLL &&
            cur_setup_stage < 128)
            r->setup_wrote_in_stage[cur_setup_stage] = true;
        if (st == 1 && r->job.stage == AD9361_INIT_TXN_SETUP_POLL &&
            r->job.candidate != NULL &&
            r->job.candidate->setup_transaction != &r->job.setup_job &&
            !r->job.setup_job.terminal)
            r->owner_dropped_between_polls = true;
        if (st == 1)
            now_us += ad9361_init_transaction_poll_delay_us(&r->job);
    }
    r->status = st;
}

static const int init_order[] = {
    AD9361_INIT_TXN_RESET_ASSERT, AD9361_INIT_TXN_RESET_LOW_WAIT,
    AD9361_INIT_TXN_RESET_RELEASE, AD9361_INIT_TXN_RESET_HIGH_WAIT,
    AD9361_INIT_TXN_PRODUCT_ID,
    AD9361_INIT_TXN_LOCK_READ, AD9361_INIT_TXN_LOCK_WRITE, AD9361_INIT_TXN_LOCK_VERIFY,
    AD9361_INIT_TXN_LOCK_READ, AD9361_INIT_TXN_LOCK_WRITE, AD9361_INIT_TXN_LOCK_VERIFY,
    AD9361_INIT_TXN_CLOCKS, AD9361_INIT_TXN_SETUP_START,
    AD9361_INIT_TXN_SETUP_POLL, AD9361_INIT_TXN_PLATFORM_POST_SETUP,
};

/* First write of each prefix register belongs to the stage that owns it. */
static const struct { uint16_t reg; int stage; } prefix_owner[] = {
    { REG_CTRL, AD9361_SETUP_TRANSACTION_PRE_CTRL },
    { REG_BANDGAP_CONFIG0, AD9361_SETUP_TRANSACTION_PRE_BANDGAP0 },
    { REG_BANDGAP_CONFIG1, AD9361_SETUP_TRANSACTION_PRE_BANDGAP1 },
    { REG_CLOCK_ENABLE, AD9361_SETUP_TRANSACTION_PRE_CLOCK_ENABLE },
    { REG_FRACT_BB_FREQ_WORD_2, AD9361_SETUP_TRANSACTION_PRE_BBPLL_SEED_2 },
    { REG_FRACT_BB_FREQ_WORD_3, AD9361_SETUP_TRANSACTION_PRE_BBPLL_SEED_3 },
};

static void test_clean_run(struct run *r)
{
    unsigned i, j;
    int last = -1;

    do_run(r, -1);
    CHECK(r->status == 0, "clean INIT status %d after %u polls", r->status,
          r->polls);
    CHECK(r->job.terminal && r->job.terminal_status == 0, "INIT terminal");
    CHECK(r->phy != NULL, "PHY published");

    CHECK(r->n_init == (int)(sizeof(init_order) / sizeof(init_order[0])),
          "INIT stage count %d", r->n_init);
    for (i = 0; i < sizeof(init_order) / sizeof(init_order[0]) &&
                (int)i < r->n_init; i++)
        CHECK(r->init_visits[i] == init_order[i], "INIT visit %u is %d, want %d",
              i, r->init_visits[i], init_order[i]);

    /* setup stages: first visits strictly increase; only the GM row loop
     * (GM_ADDRESS .. GM_ROW_WAIT) may revisit an earlier stage */
    for (i = 0; i < (unsigned)r->n_setup; i++) {
        int s = r->setup_visits[i];

        if (s <= last) {
            CHECK(s >= AD9361_SETUP_TRANSACTION_GM_ADDRESS &&
                      s <= AD9361_SETUP_TRANSACTION_GM_ROW_WAIT &&
                      last <= AD9361_SETUP_TRANSACTION_GM_ROW_WAIT,
                  "setup stage %d visited after %d", s, last);
        }
        if (s > last)
            last = s;
    }
    for (i = 0; i < (unsigned)r->n_setup; i++) {
        bool first = true;

        for (j = 0; j < i; j++)
            first = first && r->setup_visits[j] != r->setup_visits[i];
        if (first)
            continue;
        CHECK(r->setup_visits[i] >= AD9361_SETUP_TRANSACTION_GM_ADDRESS &&
                  r->setup_visits[i] <= AD9361_SETUP_TRANSACTION_GM_ROW_WAIT,
              "stage %d repeated outside the GM row loop", r->setup_visits[i]);
    }

    /* anchors every cold setup passes through, in this order */
    {
        static const int anchors[] = {
            AD9361_SETUP_TRANSACTION_PRE_AUXDAC,
            AD9361_SETUP_TRANSACTION_PRE_CLOCK_ENABLE,
            AD9361_SETUP_TRANSACTION_CLOCK_CHAIN_START,
            AD9361_SETUP_TRANSACTION_CLOCK_CHAIN_WAIT,
            AD9361_SETUP_TRANSACTION_POST_CLOCK_CHAIN_SETUP,
            AD9361_SETUP_TRANSACTION_RX_CP_START,
            AD9361_SETUP_TRANSACTION_RX_CP_WAIT,
            AD9361_SETUP_TRANSACTION_TX_CP_START,
            AD9361_SETUP_TRANSACTION_TX_CP_WAIT,
            AD9361_SETUP_TRANSACTION_POST_CP_BEGIN,
            AD9361_SETUP_TRANSACTION_POST_CP_RX_MUX,
            AD9361_SETUP_TRANSACTION_POST_CP_TX_MUX,
            AD9361_SETUP_TRANSACTION_GM_START,
            AD9361_SETUP_TRANSACTION_GM_STOP,
            AD9361_SETUP_TRANSACTION_GC_SETUP,
            AD9361_SETUP_TRANSACTION_GC_UPDATE,
            AD9361_SETUP_TRANSACTION_CALIBRATION_START,
            AD9361_SETUP_TRANSACTION_CALIBRATION,
            AD9361_SETUP_TRANSACTION_POST_CALIBRATION_WAIT,
        };
        unsigned a, at = 0;

        for (a = 0; a < sizeof(anchors) / sizeof(anchors[0]); a++) {
            while (at < (unsigned)r->n_setup && r->setup_visits[at] != anchors[a])
                at++;
            CHECK(at < (unsigned)r->n_setup, "anchor stage %d missing or out of order",
                  anchors[a]);
            at++;
        }
    }
    CHECK(r->n_setup > 0 &&
              r->setup_visits[r->n_setup - 1] ==
                  AD9361_SETUP_TRANSACTION_POST_CALIBRATION_WAIT,
          "POST_CALIBRATION_WAIT is the last setup stage");
    CHECK(!r->owner_dropped_between_polls,
          "setup parent lost PHY ownership between caller polls");
    CHECK(!r->post_setup_before_setup_terminal,
          "PLATFORM_POST_SETUP entered before the setup parent retired");
    CHECK(r->job.setup_job.terminal && r->job.setup_job.terminal_status == 0,
          "setup parent retired successfully");
    CHECK(r->phy->setup_transaction == NULL, "no setup owner left after INIT");

    for (i = 0; i < sizeof(prefix_owner) / sizeof(prefix_owner[0]); i++) {
        unsigned w;
        int owner = -2;

        for (w = 0; w < write_count && w < MAX_WRITES; w++)
            if (write_log[w].reg == prefix_owner[i].reg) {
                owner = write_log[w].setup_stage;
                break;
            }
        CHECK(owner == prefix_owner[i].stage,
              "reg 0x%03x first written in setup stage %d, want %d",
              prefix_owner[i].reg, owner, prefix_owner[i].stage);
    }
}

static void test_fault_in_each_stage(const struct run *clean)
{
    unsigned s, tried = 0, w;
    struct run *r = malloc(sizeof(*r));

    for (s = 0; s < 128; s++) {
        if (!clean->setup_wrote_in_stage[s] ||
            s == AD9361_SETUP_TRANSACTION_GM_ABORT_STOP)
            continue;
        live_allocs = 0;
        do_run(r, (int)s);
        tried++;
        CHECK(injected, "stage %u: fault was not injected", s);
        CHECK(r->status < 0 && r->status != 1, "stage %u: INIT status %d", s,
              r->status);
        CHECK(r->job.terminal && r->job.terminal_status == r->status,
              "stage %u: terminal status", s);
        CHECK(!r->reached_post_setup, "stage %u: reached PLATFORM_POST_SETUP", s);
        CHECK(r->phy == NULL, "stage %u: PHY published after failure", s);
        CHECK(!r->job.active && !r->job.setup_job.active &&
                  !r->job.setup_job.calibration_job.active &&
                  !r->job.setup_job.clock_chain_job.active &&
                  !r->job.setup_job.post_calibration_wait.active,
              "stage %u: a job or wait is still active", s);
        CHECK(live_allocs == 0, "stage %u: %d allocations leaked", s, live_allocs);
        CHECK(!r->owner_dropped_between_polls, "stage %u: owner dropped", s);

        /* the terminal result is sticky and replays nothing */
        w = write_count;
        CHECK(ad9361_init_transaction_poll(&r->job) == r->status,
              "stage %u: terminal result not sticky", s);
        CHECK(write_count == w, "stage %u: poll after terminal wrote", s);
    }
    CHECK(tried >= 20, "only %u setup stages performed a register write", tried);
    free(r);
}

int main(void)
{
    struct run *clean = malloc(sizeof(*clean));

    setvbuf(stdout, NULL, _IOLBF, 0);

    test_clean_run(clean);
    test_fault_in_each_stage(clean);
    free(clean);
    if (failures != 0) {
        printf("AD9361 INIT order: %d FAILURES\n", failures);
        return 1;
    }
    puts("AD9361 INIT order: PASS");
    return 0;
}
