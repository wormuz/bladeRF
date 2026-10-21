/* Fault injection for the shared retained bandwidth-filter child
 * (ad9361_rf_bandwidth_filter_transaction_start/poll).
 *
 * A clean run is recorded first (every SPI operation, in order). Then one SPI
 * operation at a time is made to fail, once as a read/write error that leaves
 * the register untouched, and the child must: surface exactly that error,
 * retire (terminal, inactive, owner released, no calibration wait left),
 * perform no SPI after the failing operation in the TIA/secondary/ADC stages
 * (BBF stages may run their documented cleanup writes), return the same
 * status on every further poll without touching the bus, and refuse a
 * restart only through the shared busy/unknown guards. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../../thirdparty/analogdevicesinc/no-OS/drivers/rf-transceiver/ad9361/ad9361.c"
#include "../../../../thirdparty/analogdevicesinc/no-OS/drivers/rf-transceiver/ad9361/ad9361_util.c"
#include "../../../../thirdparty/analogdevicesinc/no-OS/drivers/rf-transceiver/ad9361/ad9361_ext_band_ctrl.c"
#include "../../../../thirdparty/analogdevicesinc/no-OS/drivers/rf-transceiver/ad9361/ad9361_conv.c"
#include "../../../../thirdparty/analogdevicesinc/no-OS/drivers/axi_core/axi_adc_core/axi_adc_core.c"
#include "../../../../thirdparty/analogdevicesinc/no-OS/drivers/axi_core/axi_dac_core/axi_dac_core.c"

#define MAX_OPS 512
#define BBPLL_HZ 960000000u
#define REFCLK_HZ 40000000u

struct spi_op {
	bool write;
	uint16_t reg;
};

static uint8_t fake_regs[1024];
static struct spi_op op_log[MAX_OPS];
static unsigned op_count;
static int fail_at = -1;      /* index of the SPI operation to fail */
static bool calibration_hangs; /* REG_CALIBRATION_CTRL busy bit never clears */
static uint64_t fake_time_us;

int32_t no_os_axi_io_read(uint32_t base, uint32_t offset, uint32_t *data)
{
	(void)base; (void)offset; *data = 0; return 0;
}
int32_t no_os_axi_io_write(uint32_t base, uint32_t offset, uint32_t data)
{
	(void)base; (void)offset; (void)data; return 0;
}
void *no_os_malloc(size_t size) { return malloc(size); }
void *no_os_calloc(size_t n, size_t size) { return calloc(n, size); }
void no_os_free(void *ptr) { free(ptr); }
int32_t no_os_gpio_get(struct no_os_gpio_desc **d,
		       const struct no_os_gpio_init_param *p)
{ (void)d; (void)p; return -ENOSYS; }
int32_t no_os_gpio_get_optional(struct no_os_gpio_desc **d,
				const struct no_os_gpio_init_param *p)
{ (void)d; (void)p; return -ENOSYS; }
int32_t no_os_gpio_remove(struct no_os_gpio_desc *d) { (void)d; return 0; }
int32_t no_os_gpio_direction_output(struct no_os_gpio_desc *d, uint8_t v)
{ (void)d; (void)v; return 0; }
int32_t no_os_gpio_direction_input(struct no_os_gpio_desc *d)
{ (void)d; return 0; }
int32_t no_os_gpio_set_value(struct no_os_gpio_desc *d, uint8_t v)
{ (void)d; (void)v; return 0; }
int32_t no_os_spi_init(struct no_os_spi_desc **d,
		       const struct no_os_spi_init_param *p)
{ (void)d; (void)p; return -ENOSYS; }
int32_t no_os_spi_remove(struct no_os_spi_desc *d) { (void)d; return 0; }
uint64_t no_os_do_div(uint64_t *n, uint64_t base)
{
	uint64_t r = *n % base;
	*n /= base;
	return r;
}
struct no_os_time no_os_get_time(void)
{
	struct no_os_time now;
	now.s = (unsigned int)(fake_time_us / 1000000ULL);
	now.us = (unsigned int)(fake_time_us % 1000000ULL);
	return now;
}
void no_os_udelay(uint32_t us) { fake_time_us += us; }
void no_os_mdelay(uint32_t ms) { fake_time_us += (uint64_t)ms * 1000ULL; }

int32_t no_os_spi_write_and_read(struct no_os_spi_desc *desc, uint8_t *data,
				 uint16_t bytes_number)
{
	uint16_t command, reg, i;
	bool write;

	(void)desc;
	if (bytes_number < 3)
		return -EINVAL;
	command = (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
	reg = command & 0x3ff;
	write = command & AD_WRITE;
	if (op_count < MAX_OPS) {
		op_log[op_count].write = write;
		op_log[op_count].reg = reg;
	}
	if ((int)op_count++ == fail_at)
		return -EIO;
	if (write) {
		fake_regs[reg] = data[2];
		if (reg == REG_CALIBRATION_CTRL && !calibration_hangs)
			fake_regs[reg] = 0;
		return 0;
	}
	for (i = 0; i < bytes_number - 2; i++)
		data[2 + i] = fake_regs[reg - i];
	return 0;
}

static struct ad9361_rf_phy phy;
static struct ad9361_phy_platform_data pdata;
static struct no_os_spi_desc spi;
static struct no_os_clk refclk = { .rate = REFCLK_HZ };
static struct no_os_clk bbpll_clock = { .rate = BBPLL_HZ };
static struct refclk_scale bbpll_scale, adc_scale;

static void env_init(void)
{
	memset(&phy, 0, sizeof(phy));
	memset(&pdata, 0, sizeof(pdata));
	memset(fake_regs, 0, sizeof(fake_regs));
	op_count = 0;
	fail_at = -1;
	calibration_hangs = false;
	fake_time_us = 1000;
	phy.spi = &spi;
	phy.pdata = &pdata;
	phy.clks[BB_REFCLK] = &refclk;
	phy.clks[BBPLL_CLK] = &bbpll_clock;
	bbpll_scale = (struct refclk_scale){
		.spi = &spi, .phy = &phy, .source = BBPLL_CLK,
		.parent_source = BB_REFCLK,
	};
	adc_scale = (struct refclk_scale){
		.spi = &spi, .phy = &phy, .source = ADC_CLK,
		.parent_source = BBPLL_CLK,
	};
	phy.ref_clk_scale[BBPLL_CLK] = &bbpll_scale;
	phy.ref_clk_scale[ADC_CLK] = &adc_scale;
	phy.clks[BBPLL_CLK]->rate = BBPLL_HZ;
	/* 40 MHz x 24 = 960 MHz, no fractional word. */
	fake_regs[REG_INTEGER_BB_FREQ_WORD] = 24;
	fake_regs[REG_BBPLL] = 1;
	fake_regs[REG_RX_BBF_C3_MSB] = 1;
	fake_regs[REG_RX_BBF_R2346] = 1;
}

#define CHECK(cond, ...) do { if (!(cond)) { \
	fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
	fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); \
	return EXIT_FAILURE; } } while (0)

/* Drive start + polls to a terminal result. Returns the terminal status and
 * records, in *tia_op, the op index at which the TIA stage began. */
static int32_t run(struct ad9361_rf_bandwidth_filter_transaction *t,
		   int *tia_op, bool *start_failed)
{
	int32_t ret;
	unsigned polls = 0;

	*tia_op = -1;
	*start_failed = false;
	ret = ad9361_rf_bandwidth_filter_transaction_start(&phy, 10000000,
							   10000000, t);
	if (ret < 0) {
		*start_failed = true;
		return ret;
	}
	for (;;) {
		if (*tia_op < 0 &&
		    t->stage >= AD9361_RF_BANDWIDTH_FILTER_TIA_READ)
			*tia_op = (int)op_count;
		ret = ad9361_rf_bandwidth_filter_transaction_poll(&phy, t);
		if (ret != 1)
			return ret;
		fake_time_us += 2000;
		if (++polls > 4000)
			return -ETIMEDOUT;
	}
}

static int check_retired(struct ad9361_rf_bandwidth_filter_transaction *t,
			 int32_t status, const char *what)
{
	unsigned before = op_count;
	int i;

	CHECK(t->terminal && !t->active, "%s: not terminal/inactive", what);
	CHECK(t->terminal_status == status, "%s: terminal_status %d != %d",
	      what, (int)t->terminal_status, (int)status);
	CHECK(phy.rf_bandwidth_filter_transaction == NULL,
	      "%s: owner pointer not released", what);
	CHECK(phy.calibration_wait == NULL && !t->rx_bbf_job.wait.active &&
	      !t->tx_bbf_job.wait.active, "%s: calibration wait leaked", what);
	for (i = 0; i < 3; i++) {
		int32_t again = ad9361_rf_bandwidth_filter_transaction_poll(
			&phy, t);
		CHECK(again == status, "%s: repoll %d returned %d, want %d",
		      what, i, (int)again, (int)status);
	}
	CHECK(op_count == before, "%s: repoll after terminal touched the bus",
	      what);
	return 0;
}

int main(void)
{
	struct ad9361_rf_bandwidth_filter_transaction t;
	unsigned total_ops, k;
	int tia_op;
	bool start_failed;
	int32_t ret;

	/* Clean run: success is terminal, stable, and records the op sequence. */
	env_init();
	memset(&t, 0, sizeof(t));
	ret = run(&t, &tia_op, &start_failed);
	CHECK(ret == 0 && !start_failed, "clean run failed: %d", (int)ret);
	CHECK(tia_op > 0, "TIA stage never reached");
	total_ops = op_count;
	CHECK(total_ops < MAX_OPS, "op log overflow (%u)", total_ops);
	if (check_retired(&t, 0, "clean"))
		return EXIT_FAILURE;
	CHECK(!phy.calibration_state_unknown, "clean run marked state unknown");
	{
		unsigned reads = 0, writes = 0;
		for (k = 0; k < total_ops; k++)
			op_log[k].write ? writes++ : reads++;
		/* TIA 3 reads + 5 writes, TX 2nd 3, ADC 3 reads + 40 writes. */
		CHECK(writes >= 5 + 3 + 40 && reads >= 3 + 3 + 1,
		      "implausible op mix r=%u w=%u", reads, writes);
	}

	/* Every single SPI operation failing in turn. */
	{
		struct spi_op saved[MAX_OPS];
		unsigned n = total_ops;
		int tia_start = tia_op;

		memcpy(saved, op_log, sizeof(saved));
		for (k = 0; k < n; k++) {
			char what[64];

			env_init();
			memset(&t, 0, sizeof(t));
			fail_at = (int)k;
			ret = run(&t, &tia_op, &start_failed);
			snprintf(what, sizeof(what), "fault@op%u(%s 0x%03x)", k,
				 saved[k].write ? "wr" : "rd", saved[k].reg);
			CHECK(ret == -EIO,
			      "%s: status %d, want -EIO (first root cause)",
			      what, (int)ret);
			if (start_failed) {
				CHECK(phy.rf_bandwidth_filter_transaction ==
				      NULL, "%s: owner left after failed start",
				      what);
				CHECK(!t.active, "%s: active after failed start",
				      what);
				continue;
			}
			if (check_retired(&t, -EIO, what))
				return EXIT_FAILURE;
			if ((int)k >= tia_start) {
				/* No bus traffic after the failing op. */
				CHECK(op_count == k + 1,
				      "%s: %u SPI ops issued after the failure",
				      what, op_count - (k + 1));
				/* A failed write leaves hardware half-applied. */
				if (saved[k].write)
					CHECK(phy.calibration_state_unknown,
					      "%s: failed write did not mark state unknown",
					      what);
			}
		}
	}

	/* A calibration that has not completed is not timed out by software
	 * (the wait contract retains ownership until hardware completion or an
	 * observation failure). While pending the child must stay the owner,
	 * issue at most one SPI read per poll, and not mark the state unknown;
	 * once the hardware completes the transaction finishes normally. */
	env_init();
	memset(&t, 0, sizeof(t));
	calibration_hangs = true;
	ret = ad9361_rf_bandwidth_filter_transaction_start(&phy, 10000000,
							   10000000, &t);
	CHECK(ret == 0, "start under hung calibration: %d", (int)ret);
	for (k = 0; k < 200; k++) {
		unsigned before = op_count;

		ret = ad9361_rf_bandwidth_filter_transaction_poll(&phy, &t);
		CHECK(ret == 1, "pending calibration retired early: %d at poll %u",
		      (int)ret, k);
		CHECK(op_count <= before + (k == 0 ? 16 : 1),
		      "poll %u issued %u SPI ops", k, op_count - before);
		CHECK(phy.rf_bandwidth_filter_transaction == &t && t.active,
		      "owner dropped while calibration pending");
		CHECK(!phy.calibration_state_unknown,
		      "pending calibration marked state unknown");
		fake_time_us += 2000;
	}
	calibration_hangs = false;
	fake_regs[REG_CALIBRATION_CTRL] = 0;
	for (k = 0; k < 4000; k++) {
		ret = ad9361_rf_bandwidth_filter_transaction_poll(&phy, &t);
		if (ret != 1)
			break;
		fake_time_us += 2000;
	}
	CHECK(ret == 0, "did not finish after hardware completion: %d",
	      (int)ret);
	if (check_retired(&t, 0, "after-hang"))
		return EXIT_FAILURE;

	/* Start guards: busy owner, unknown state, non-idle sequencer, bad args. */
	env_init();
	memset(&t, 0, sizeof(t));
	phy.calibration_state_unknown = true;
	ret = ad9361_rf_bandwidth_filter_transaction_start(
		&phy, 10000000, 10000000, &t);
	CHECK(ret == -EIO && op_count == 0 && !t.active,
	      "start with unknown state: %d ops=%u", (int)ret, op_count);

	env_init();
	memset(&t, 0, sizeof(t));
	phy.calibration_wait_quarantined = true;
	ret = ad9361_rf_bandwidth_filter_transaction_start(
		&phy, 10000000, 10000000, &t);
	CHECK(ret == -EBUSY && op_count == 0 && !t.active,
	      "start while quarantined: %d ops=%u", (int)ret, op_count);

	env_init();
	memset(&t, 0, sizeof(t));
	fake_regs[REG_STATE] = CALIBRATION_SEQUENCE_STATE(2);
	ret = ad9361_rf_bandwidth_filter_transaction_start(
		&phy, 10000000, 10000000, &t);
	CHECK(ret == -EBUSY && !t.active &&
	      phy.rf_bandwidth_filter_transaction == NULL,
	      "start with busy calibration sequencer: %d", (int)ret);

	env_init();
	memset(&t, 0, sizeof(t));
	ret = ad9361_rf_bandwidth_filter_transaction_start(&phy, 10000000,
							   10000000, &t);
	CHECK(ret == 0 && phy.rf_bandwidth_filter_transaction == &t,
	      "plain start failed: %d", (int)ret);
	{
		struct ad9361_rf_bandwidth_filter_transaction t2;

		memset(&t2, 0, sizeof(t2));
		ret = ad9361_rf_bandwidth_filter_transaction_start(
			&phy, 10000000, 10000000, &t2);
		CHECK(ret == -EBUSY && phy.rf_bandwidth_filter_transaction == &t,
		      "second start did not get -EBUSY / stole owner: %d",
		      (int)ret);
		CHECK(ad9361_rf_bandwidth_filter_transaction_start(
			      &phy, 1, 1, &t) == -EBUSY,
		      "restart of an active transaction not rejected");
		/* A poll through a foreign owner marker fails closed. */
		ret = ad9361_rf_bandwidth_filter_transaction_poll(&phy, &t2);
		CHECK(ret < 0 && phy.calibration_state_unknown,
		      "poll of non-owner not fail-closed: %d", (int)ret);
	}
	CHECK(ad9361_rf_bandwidth_filter_transaction_start(NULL, 1, 1, &t) ==
	      -EINVAL, "NULL phy accepted");

	printf("ad9361 shared bandwidth-filter fault injection (%u ops): PASS\n",
	       total_ops);
	return EXIT_SUCCESS;
}
