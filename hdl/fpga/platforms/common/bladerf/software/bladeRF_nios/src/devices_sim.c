/* This file is part of the bladeRF project:
 *   http://www.github.com/nuand/bladeRF
 *
 * Copyright (c) 2015 Nuand LLC
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#ifdef BLADERF_NIOS_PC_SIMULATION

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include <string.h>

#include "rfic_channel_validation.h"

#include "nios_pkt_retune2.h"
#include "pkt_handler.h"
#include "devices.h"
#include "fpga_version.h"
#include "debug.h"

#include "sim_test_cases.h"

/* This global is used to flag the end of a simulation */
bool run_nios = true;

void command_uart_set_worker_busy(bool busy)
{
    (void)busy;
}

uint32_t command_uart_current_session_generation(void)
{
    return 0;
}

static size_t test_case_idx = 0;

const char *module2str(bladerf_module m)
{
    switch (m) {
        case BLADERF_MODULE_RX:
            return "RX";

        case BLADERF_MODULE_TX:
            return "TX";

        default:
            return "Invalid";
    }
}

void bladerf_nios_init(struct pkt_buf *pkt,
                       struct vctcxo_tamer_pkt_buf *vctcxo_tamer_pkt)
{
    (void)pkt;
    (void)vctcxo_tamer_pkt;
    DBG("%s()\n", __FUNCTION__);
}

bool lms6_read(uint8_t addr, uint8_t *data) {
    const uint8_t ret = 0x17;
    DBG("%s: addr=0x%02x, returning 0x%02x\n", __FUNCTION__, addr, ret);
    ASSERT(addr == 0x2f);
    if (data == NULL) {
        return false;
    }
    *data = ret;
    return true;
}

bool lms6_write(uint8_t addr, uint8_t data)
{
    DBG("%s: addr=0x%02x, data=0x%02x\n", __FUNCTION__, addr, data);
    ASSERT(addr == 0x07);
    ASSERT(data == 0x09);
    return true;
}

bool adi_spi_read_checked(uint16_t addr, uint64_t *value) {
    const uint64_t ret = 0x17;
    DBG("%s: addr=0x%04x, returning 0x%04" PRIx64 "\n", __FUNCTION__, addr, ret);
    ASSERT(addr == 0x2f2f);
    if (value == NULL) {
        return false;
    }
    *value = ret;
    return true;
}

uint64_t rx_epoch_status_snapshot_read(void)
{
    return UINT64_C(0x123456789abcdef0);
}

bool adi_spi_write(uint16_t addr, uint64_t data)
{
    DBG("%s: addr=0x%04x, data=0x%04" PRIx64 "\n", __FUNCTION__, addr, data);
    ASSERT(addr == 0x0707);
    ASSERT(data == 0x09);
    return true;
}

bool wishbone_master_read(uint32_t addr, uint32_t *data)
{
    (void)addr;
    if (data == NULL) {
        return false;
    }
    *data = 0;
    return true;
}

bool wishbone_master_write(uint32_t addr, uint32_t data)
{
    (void)addr;
    (void)data;
    return true;
}

bool adi_spi_update_bits(uint16_t addr, uint8_t mask, uint8_t value,
                         uint8_t *result)
{
    DBG("%s: addr=0x%04x mask=0x%02x value=0x%02x\n", __FUNCTION__,
        addr, mask, value);
    ASSERT(addr == 0x2f2f);
    ASSERT(mask == 0x0f);
    ASSERT(value == 0x05);
    ASSERT(result != NULL);
    *result = 0x15;
    return true;
}

bool adi_spi_write_register_batch(uint16_t count, uint64_t data,
                                  uint8_t *completed)
{
    static const uint16_t expected_regs[] = { 0x23b, 0x23c, 0x23d };
    static const uint8_t expected_values[] = { 0x11, 0x22, 0x33 };
    uint16_t i;

    if (completed == NULL || count != ARRAY_SIZE(expected_regs)) {
        return false;
    }

    *completed = 0;
    for (i = 0; i < count; i++) {
        const uint64_t entry = (data >> (18 * i)) & 0x3ffff;
        if ((entry & 0x3ff) != expected_regs[i] ||
            ((entry >> 10) & 0xff) != expected_values[i]) {
            return false;
        }
        (*completed)++;
    }

    return true;
}

bool si5338_read(uint8_t addr, uint8_t *data)
{
    const uint8_t ret = 0x88;
    DBG("%s: addr=0x%02x, returning 0x%02x\n", __FUNCTION__, addr, ret);
    ASSERT(addr == 0x3);
    *data = ret;
    return true;
}

bool si5338_write(uint8_t addr, uint8_t data)
{
    DBG("%s: addr=0x%02x, data=0x%02x\n", __FUNCTION__, addr, data);
    ASSERT(addr == 0x05);
    ASSERT(data == 0xab);
    return true;
}

bool vctcxo_trim_dac_write(uint8_t cmd, uint16_t val)
{
    DBG("%s: cmd=0x%02x, val=0x%04x\n", __FUNCTION__, cmd, val);
    ASSERT(cmd == 0x28   || cmd == 0x08);
    ASSERT(val == 0x0000 || val == 0x8012);
    return true;
}

bool vctcxo_trim_dac_read(uint8_t cmd, uint16_t *val)
{
    *val = 0x1234;

    DBG("%s: cmd=0x%02x, val=0x%04x\n", __FUNCTION__, cmd, *val);

    ASSERT(cmd == 0x98);
    return true;
}

bool adf4351_write(uint32_t val)
{
    DBG("%s: val=0x%08x\n", __FUNCTION__, val);
    ASSERT(0x580005);
    return true;
}

uint32_t control_reg_read(void)
{
    uint32_t ret = 0x8abcde57;
    DBG("%s: returning 0x%08x\n", __FUNCTION__, ret);
    return ret;
}

bool control_reg_write(uint32_t value)
{
    DBG("%s: value=0x%08x\n", __FUNCTION__, value);
    ASSERT(value == 0x80402057);
    return true;
}

/* The simulated board has no RFFE PIO; behave as the NIOS build does when
 * GPIO_RFFE_0_BASE is absent. */
bool rffe_csr_read_checked(uint32_t *value)
{
    (void)value;
    return false;
}

bool rffe_csr_write_checked(uint32_t value, uint32_t verify_mask)
{
    (void)value;
    (void)verify_mask;
    return false;
}

bool iqbal_get_gain(bladerf_module m, uint16_t *value)
{
    if (value == NULL) {
        return false;
    }

    switch (m) {
        case BLADERF_MODULE_RX:
            *value = 0x14d2;
            break;

        case BLADERF_MODULE_TX:
            *value = 0x0812;
            break;

        default:
            DBG("%s: Invalid module %u\n", __FUNCTION__, m);
            return false;
    }

    DBG("%s: module=%s, returning 0x%04x\n",
        __FUNCTION__, module2str(m), *value);

    return true;
}

bool iqbal_set_gain(bladerf_module m, uint16_t value)
{
    switch (m) {
        case BLADERF_MODULE_RX:
            if (value != 0x14d2) return false;
            break;

        case BLADERF_MODULE_TX:
            if (value != 0x0812) return false;
            break;

        default:
            DBG("%s: Invalid module %u\n", __FUNCTION__, m);
            return false;
    }

    DBG("%s: module=%s, value=0x%04x\n", __FUNCTION__, module2str(m), value);
    return true;
}

bool iqbal_get_phase(bladerf_module m, uint16_t *value)
{
    if (value == NULL) {
        return false;
    }

    switch (m) {
        case BLADERF_MODULE_RX:
            *value = 0x101c;
            break;

        case BLADERF_MODULE_TX:
            *value = 0x021f;
            break;

        default:
            DBG("%s: Invalid module %u\n", __FUNCTION__, m);
            return false;
    }

    DBG("%s: module=%s, returning 0x%04x\n",
        __FUNCTION__, module2str(m), *value);

    return true;
}

bool iqbal_set_phase(bladerf_module m, uint16_t value)
{
    switch (m) {
        case BLADERF_MODULE_RX:
            if (value != 0x101c) return false;
            break;

        case BLADERF_MODULE_TX:
            if (value != 0x021f) return false;
            break;

        default:
            DBG("%s: Invalid module %u\n", __FUNCTION__, m);
            return false;
    }

    DBG("%s: module=%s, value=0x%04x\n", __FUNCTION__, module2str(m), value);
    return true;
}

uint32_t expansion_port_read(void)
{
    uint32_t ret = 0xffffc7ff;
    DBG("%s: returning 0x%08x\n", __FUNCTION__, ret);
    return ret;
}

bool expansion_port_write(uint32_t value)
{
    DBG("%s: value=0x%08x\n", __FUNCTION__, value);
    return value == 0x3c000800;
}

uint32_t expansion_port_get_direction()
{
    uint32_t ret = 0x0a1b2c3d;
    DBG("%s: returning 0x%08x\n", __FUNCTION__, ret);
    return ret;
}

bool expansion_port_set_direction(uint32_t dir)
{
    DBG("%s: dir=0x%08x\n", __FUNCTION__, dir);
    return true;
}

bool time_tamer_read(bladerf_module m, uint64_t *time)
{
    if (time == NULL) {
        return false;
    }

    switch (m) {
        case BLADERF_MODULE_RX:
            *time = 0x123456780a1b2c3d;
            break;

        case BLADERF_MODULE_TX:
            *time = 0x87654321a1b2c3d4;
            break;

        default:
            DBG("%s: Invalid module %u\n", __FUNCTION__, m);
            return false;
    }

    DBG("%s: module=%s, returning 0x%016"PRIx64"\n",
         __FUNCTION__, module2str(m), *time);

    return true;
}

bool tamer_compare_fired(bladerf_module m)
{
    (void)m;
    return false;
}

bool tamer_clock_stalled(bladerf_module m)
{
    (void)m;
    return false;
}

void time_tamer_reset(bladerf_module m)
{
    switch (m) {
        case BLADERF_MODULE_RX:
        case BLADERF_MODULE_TX:
            break;

        default:
            DBG("%s: Invalid module %u\n", __FUNCTION__, m);
            ASSERT(0);
            break;
    }

    DBG("%s: module=%s\n", __FUNCTION__, module2str(m));
}

void time_tamer_clear_interrupt(bladerf_module m)
{
    switch (m) {
        case BLADERF_MODULE_RX:
        case BLADERF_MODULE_TX:
            break;

        default:
            DBG("%s: Invalid module %u\n", __FUNCTION__, m);
            ASSERT(0);
            break;
    }

    DBG("%s: module=%s\n", __FUNCTION__, module2str(m));
}

bool tamer_schedule_request(bladerf_module m, uint64_t time) {
    DBG("%s: module=%s, time=%"PRIu64"\n", __FUNCTION__, module2str(m), time);
    return m == BLADERF_MODULE_RX || m == BLADERF_MODULE_TX;
}

bool tamer_schedule_acknowledged(bladerf_module m)
{
    return m == BLADERF_MODULE_RX || m == BLADERF_MODULE_TX;
}

bool tamer_schedule_failed(bladerf_module m)
{
    return m != BLADERF_MODULE_RX && m != BLADERF_MODULE_TX;
}

bool tamer_cancel_failed(bladerf_module m)
{
    return m != BLADERF_MODULE_RX && m != BLADERF_MODULE_TX;
}

bool tamer_cancel_request(bladerf_module m, uint8_t *ack_before)
{
    DBG("%s: module=%s\n", __FUNCTION__, module2str(m));
    if (ack_before == NULL ||
        (m != BLADERF_MODULE_RX && m != BLADERF_MODULE_TX)) {
        return false;
    }
    *ack_before = 0;
    return true;
}

bool tamer_cancel_acknowledged(bladerf_module m, uint8_t ack_before)
{
    (void)ack_before;
    return m == BLADERF_MODULE_RX || m == BLADERF_MODULE_TX;
}

int lms_set_precalculated_frequency(struct bladerf *dev, bladerf_module mod,
                                    struct lms_freq *f)
{
    DBG("%s: module=%s, nint=0x%04x, nfrac=0x%08x, freqsel=0x%02x, flags=0x%02x\n",
        __FUNCTION__, module2str(mod), f->nint, f->nfrac, f->freqsel, f->flags);

    return 0;
}

int lms_select_band(struct bladerf *dev, bladerf_module module, bool low_band)
{
    DBG("%s: module=%s, low_band=%s\n", __FUNCTION__, module2str(module),
        low_band ? "true" : "false");

    return 0;
}

void command_uart_read_request(uint8_t *req) {

    printf("\nTest case %-2zd: %s\n", test_case_idx + 1,
            test_cases[test_case_idx].desc);
    printf("--------------------------------------------------------\n");

    if (test_case_idx < ARRAY_SIZE(test_cases)) {
        memcpy(req, &test_cases[test_case_idx].req, NIOS_PKT_LEN);
        test_case_idx++;

        if (test_case_idx == ARRAY_SIZE(test_cases)) {
            run_nios = false;
        }
    } else {
        memset(req, 0xff, NIOS_PKT_LEN);
        DBG("Read past test case array.\n");
        return;
    }
}

uint32_t command_uart_request_session_generation(void)
{
    return 0;
}

void command_uart_enable_isr(bool enable)
{
    (void)enable;
}

bool command_uart_write_response(uint8_t *resp, uint32_t session_generation) {
    (void)session_generation;
    print_bytes("Response data:", resp, NIOS_PKT_LEN);

    /* At this point, we'll already have incremented the test_case_idx past
     * this test. */
    ASSERT(test_case_idx > 0);

    if (memcmp(resp, test_cases[test_case_idx - 1].resp, NIOS_PKT_LEN)) {
        printf("Failed.\n");
        if (!getenv("CONTINUE_ON_FAIL")) {
                ASSERT(!"Aborting due to failed test case.\n");
        }
    } else {
        printf("Pass.\n\n");
    }
    return true;
}

bool command_uart_publish_response(uint8_t *resp,
                                   uint32_t session_generation)
{
    return command_uart_write_response(resp, session_generation);
}

enum command_uart_try_publish_result command_uart_try_publish_response(
    uint8_t *resp, uint32_t session_generation)
{
    (void)session_generation;
    return command_uart_write_response(resp, session_generation)
               ? COMMAND_UART_TRY_PUBLISH_OK
               : COMMAND_UART_TRY_PUBLISH_ERROR;
}

void command_uart_check_session_recovery(void)
{
    /* The simulator has no asynchronous UART/session reset state. */
}

bool rfic_command_write(uint16_t addr, uint64_t data)
{
    if (!_rfic_address_fields_valid(addr)) {
        return false;
    }

    DBG("%s: addr=%02x data=%08" PRIx64 "\n", __FUNCTION__, addr, data);
    return true;
}

bool rfic_command_read(uint16_t addr, uint64_t *data)
{
    if (data == NULL || !_rfic_address_fields_valid(addr)) {
        return false;
    }

    DBG("%s: addr=%02x *data=%08" PRIx64 "\n", __FUNCTION__, addr, *data);
    return true;
}


/* Peripherals the micro platform and shared packet handlers reference but
 * the simulated board does not have. Each mirrors the NIOS build's
 * behaviour where it is pure bookkeeping and otherwise reports success
 * without hardware side effects. */

uint16_t vctcxo_trim_dac_value = 0x7FFF;

static bladerf_vctcxo_tamer_mode sim_tamer_mode =
    BLADERF_VCTCXO_TAMER_DISABLED;
static uint8_t sim_tx_trigger_ctl;
static uint8_t sim_rx_trigger_ctl;

void vctcxo_tamer_enable_isr(bool enable)
{
    DBG("%s: enable=%d\n", __FUNCTION__, enable);
}

void vctcxo_tamer_reset_counters(bool reset)
{
    DBG("%s: reset=%d\n", __FUNCTION__, reset);
}

bool vctcxo_tamer_set_tune_mode(bladerf_vctcxo_tamer_mode mode)
{
    sim_tamer_mode = mode;
    return true;
}

bladerf_vctcxo_tamer_mode vctcxo_tamer_get_tune_mode()
{
    return sim_tamer_mode;
}

bool tx_trigger_ctl_write(uint8_t data)
{
    sim_tx_trigger_ctl = data;
    return true;
}

uint8_t tx_trigger_ctl_read(void)
{
    return sim_tx_trigger_ctl;
}

bool rx_trigger_ctl_write(uint8_t data)
{
    sim_rx_trigger_ctl = data;
    return true;
}

uint8_t rx_trigger_ctl_read(void)
{
    return sim_rx_trigger_ctl;
}

bool agc_dc_corr_write(uint16_t addr, uint16_t value)
{
    DBG("%s: addr=0x%04x value=0x%04x\n", __FUNCTION__, addr, value);
    return true;
}

bool adi_spi_gain_table_row(uint16_t row, uint8_t data1, uint8_t data2,
                            uint8_t data3, uint8_t config,
                            uint32_t delay_us)
{
    DBG("%s: row=%u data=%02x %02x %02x config=%02x delay=%u\n",
        __FUNCTION__, row, data1, data2, data3, config, delay_us);
    return delay_us == 2;
}

/* Fastlock profile cache and the simulated RFIC profile bank. The cache
 * bookkeeping follows devices.c; SPI programming completes immediately and
 * a recall observes lock on its first poll. */
fastlock_profile fastlocks_rx[NUM_BBP_FASTLOCK_PROFILES];
fastlock_profile fastlocks_tx[NUM_BBP_FASTLOCK_PROFILES];

static struct {
    bool active;
    bladerf_module module;
} sim_load_job, sim_recall_job;

static bool sim_rfic_owned(void)
{
    return sim_recall_job.active;
}

static fastlock_profile *sim_fastlock_cache(bladerf_module m)
{
    return BLADERF_CHANNEL_IS_TX(m) ? fastlocks_tx : fastlocks_rx;
}

bool adi_fastlock_load(bladerf_module m, fastlock_profile *p)
{
    if (p == NULL) {
        return false;
    }
    if (p->state == FASTLOCK_STATE_RFFE ||
        p->state == FASTLOCK_STATE_BBP_RFFE) {
        return true;
    }
    if (sim_rfic_owned()) {
        return false;
    }
    fastlock_cache_invalidate_slot(sim_fastlock_cache(m),
                                   NUM_BBP_FASTLOCK_PROFILES,
                                   p->profile_num);
    p->state = FASTLOCK_STATE_BBP_RFFE;
    return true;
}

int32_t adi_fastlock_load_start(bladerf_module m, fastlock_profile *p)
{
    if (p == NULL || (m != BLADERF_MODULE_RX && m != BLADERF_MODULE_TX) ||
        sim_load_job.active || sim_rfic_owned()) {
        return -1;
    }
    if (p->state == FASTLOCK_STATE_RFFE ||
        p->state == FASTLOCK_STATE_BBP_RFFE) {
        return 1;
    }
    fastlock_cache_invalidate_slot(sim_fastlock_cache(m),
                                   NUM_BBP_FASTLOCK_PROFILES,
                                   p->profile_num);
    sim_load_job.active = true;
    sim_load_job.module = m;
    return 0;
}

int32_t adi_fastlock_load_step(bladerf_module m)
{
    if (!sim_load_job.active || sim_load_job.module != m) {
        return -1;
    }
    sim_load_job.active = false;
    return 1;
}

bool adi_fastlock_load_cancel(bladerf_module m)
{
    if (!sim_load_job.active || sim_load_job.module != m) {
        return false;
    }
    sim_load_job.active = false;
    return true;
}

void adi_fastlock_profile_cache_commit(bladerf_module m,
                                       uint16_t nios_profile,
                                       fastlock_profile *p)
{
    fastlock_profile *profiles = sim_fastlock_cache(m);

    if (p == NULL || nios_profile >= NUM_BBP_FASTLOCK_PROFILES) {
        return;
    }
    fastlock_cache_invalidate_slot(profiles, NUM_BBP_FASTLOCK_PROFILES,
                                   p->profile_num);
    p->state = FASTLOCK_STATE_BBP_RFFE;
    profiles[nios_profile] = *p;
}

bool adi_fastlock_recall_start(bladerf_module m, fastlock_profile *p,
                               uint8_t *failure_stage)
{
    if (failure_stage != NULL) {
        *failure_stage = NIOS_PKT_RETUNE2_FAILURE_NONE;
    }
    if (sim_recall_job.active || p == NULL ||
        (m != BLADERF_MODULE_RX && m != BLADERF_MODULE_TX)) {
        if (failure_stage != NULL) {
            *failure_stage = NIOS_PKT_RETUNE2_FAILURE_INVALID_REQUEST;
        }
        return false;
    }
    sim_recall_job.active = true;
    sim_recall_job.module = m;
    return true;
}

int32_t adi_fastlock_recall_poll(bladerf_module m, uint8_t *failure_stage)
{
    if (failure_stage != NULL) {
        *failure_stage = NIOS_PKT_RETUNE2_FAILURE_NONE;
    }
    if (!sim_recall_job.active || sim_recall_job.module != m) {
        if (failure_stage != NULL) {
            *failure_stage = NIOS_PKT_RETUNE2_FAILURE_INTERNAL;
        }
        return -1;
    }
    sim_recall_job.active = false;
    return 0;
}

bool adi_fastlock_recall(bladerf_module m, fastlock_profile *p,
                         uint8_t *failure_stage)
{
    return adi_fastlock_recall_start(m, p, failure_stage) &&
           adi_fastlock_recall_poll(m, failure_stage) == 0;
}

/* The simulated RFPLL is always locked once a recall has finished. */
bool adi_rfpll_lock_read(bladerf_module m, bool *locked)
{
    if (locked == NULL) {
        return false;
    }
    *locked = !sim_recall_job.active || sim_recall_job.module != m;
    return true;
}

/* The simulation keeps no AD9361 profile bank: report a failed read. */
void adi_rfpll_synth_read(bladerf_module m, uint8_t rffe_profile,
                          uint8_t *synth)
{
    (void)m;
    (void)rffe_profile;
    memset(synth, 0, NIOS_PKT_RETUNE2_SYNTH_LEN);
}

bool adi_rfport_select(fastlock_profile *p)
{
    return p != NULL;
}

bool adi_rfspdt_select(bladerf_module m, fastlock_profile *p)
{
    return p != NULL && (m == BLADERF_MODULE_RX || m == BLADERF_MODULE_TX);
}
#endif
