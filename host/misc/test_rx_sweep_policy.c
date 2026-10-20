#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "board/bladerf2/rx_sweep_policy.h"

/* Certification rule of RX sweep records: which sample timestamps are
 * certified at which record's LO. */

static int failures;

static void expect_label(const struct bladerf_rx_sweep_record *records,
                         unsigned int count, uint64_t timestamp,
                         bool certified, unsigned int expected_index,
                         const char *what)
{
    unsigned int index = 0xffffffffu;
    const bool got = bladerf2_rx_sweep_label(records, count, timestamp,
                                             &index);

    if (got != certified || (certified && index != expected_index)) {
        fprintf(stderr, "%s: t=%llu got %s index %u, expected %s index %u\n",
                what, (unsigned long long)timestamp,
                got ? "certified" : "uncertified", index,
                certified ? "certified" : "uncertified", expected_index);
        failures++;
    }
}

/* A success record whose LO readback is `readback` Hz (VALID), or, with 0,
 * the requested frequency read back exactly. */
static struct bladerf_rx_sweep_record success(uint32_t sequence,
                                              uint64_t frequency,
                                              uint64_t scheduled,
                                              uint64_t executed,
                                              uint8_t execution_flags,
                                              uint64_t readback,
                                              uint64_t settle)
{
    struct bladerf_rx_sweep_record record;

    memset(&record, 0, sizeof(record));
    record.sequence = sequence;
    record.frequency = frequency;
    record.scheduled_timestamp = scheduled;
    bladerf2_rx_sweep_record_success(&record,
                                     BLADERF2_RX_SWEEP_READBACK_VALID,
                                     readback != 0 ? readback : frequency,
                                     executed, execution_flags, settle);
    return record;
}

static struct bladerf_rx_sweep_record other(uint32_t sequence, uint8_t status,
                                            uint64_t scheduled)
{
    struct bladerf_rx_sweep_record record;

    memset(&record, 0, sizeof(record));
    record.sequence = sequence;
    record.status = status;
    record.scheduled_timestamp = scheduled;
    return record;
}

#define OK_FLAGS (NIOS_PKT_RETUNE2_EXEC_TS_VALID | \
                  NIOS_PKT_RETUNE2_EXEC_PLL_LOCKED)

static void check_record_classification(void)
{
    struct bladerf_rx_sweep_record r;

    r = success(1, 100000000, 1000, 1200, OK_FLAGS, 0, 50);
    if (r.flags != (BLADERF_RX_SWEEP_F_EXECUTED_VALID |
                    BLADERF_RX_SWEEP_F_PLL_LOCKED |
                    BLADERF_RX_SWEEP_F_READBACK_VALID |
                    BLADERF_RX_SWEEP_F_CERTIFIED) ||
        r.certified_start != 1250 || r.executed_timestamp != 1200) {
        fputs("locked success with valid timestamp not certified\n", stderr);
        failures++;
    }
    r = success(1, 100000000, 1000, 1200,
                NIOS_PKT_RETUNE2_EXEC_TS_VALID, 0, 50);
    if ((r.flags & BLADERF_RX_SWEEP_F_CERTIFIED) != 0) {
        fputs("unlocked PLL certified\n", stderr);
        failures++;
    }
    r = success(1, 100000000, 1000, 1200,
                NIOS_PKT_RETUNE2_EXEC_PLL_LOCKED, 0, 50);
    if ((r.flags & BLADERF_RX_SWEEP_F_CERTIFIED) != 0 ||
        r.executed_timestamp != 0) {
        fputs("success without executed timestamp certified\n", stderr);
        failures++;
    }
    r = success(1, 100000000, 1000, 1200, OK_FLAGS, 100001001, 50);
    if ((r.flags & BLADERF_RX_SWEEP_F_CERTIFIED) != 0 ||
        (r.flags & BLADERF_RX_SWEEP_F_READBACK_MISMATCH) == 0) {
        fputs("readback 1001 Hz off certified\n", stderr);
        failures++;
    }
    r = success(1, 100000000, 1000, 1200, OK_FLAGS, 100001000, 50);
    if ((r.flags & BLADERF_RX_SWEEP_F_CERTIFIED) == 0) {
        fputs("readback within 1 kHz not certified\n", stderr);
        failures++;
    }
    r = success(1, 100000000, 1000, UINT64_MAX - 10, OK_FLAGS, 0, 50);
    if ((r.flags & BLADERF_RX_SWEEP_F_CERTIFIED) != 0) {
        fputs("settle overflow certified\n", stderr);
        failures++;
    }
}

/* 925 MHz RX profile: integer 96, fractional 2970960, VCO divider 2 at a
 * 76.8 MHz RFPLL reference: (76.8e6 * 2970960 / 8388593 + 76.8e6 * 96)
 * >> 3 = 924999999 Hz in integer arithmetic. AD9361 slot 5. */
static const uint8_t profile_925[16] = {
    0x60, 0x00, 0x50, 0x55, 0x2d, 0, 0, 0, 0, 0, 0, 0, 0x02, 0, 0, 0
};

static void check_synth_readback(void)
{
    struct bladerf_rx_sweep_record r;
    enum bladerf2_rx_sweep_readback readback;
    uint8_t expected[NIOS_PKT_RETUNE2_SYNTH_LEN];
    uint8_t synth[NIOS_PKT_RETUNE2_SYNTH_LEN];
    uint64_t hz = 1;

    bladerf2_rx_sweep_synth_expected(profile_925, 5, expected);
    if (expected[5] != 0x02 || expected[6] != 0xa1) {
        fputs("expected synthesizer bytes wrong\n", stderr);
        failures++;
    }

    /* NIOS read the uploaded words; the setup register also carries bits
     * outside the compared mask. */
    memcpy(synth, expected, sizeof(synth));
    synth[NIOS_PKT_RETUNE2_SYNTH_SETUP] |= 0x0c;
    readback = bladerf2_rx_sweep_readback(synth, expected, 76800000, 0, &hz);
    if (readback != BLADERF2_RX_SWEEP_READBACK_VALID || hz != 924999999) {
        fprintf(stderr, "matching words: readback %d hz %llu\n", readback,
                (unsigned long long)hz);
        failures++;
    }
    memset(&r, 0, sizeof(r));
    r.frequency = 925000000;
    bladerf2_rx_sweep_record_success(&r, readback, hz, 1200, OK_FLAGS, 50);
    if ((r.flags & BLADERF_RX_SWEEP_F_CERTIFIED) == 0 ||
        r.readback_frequency != 924999999) {
        fputs("matching readback not certified\n", stderr);
        failures++;
    }

    /* Any differing word: a fractional byte, the divider, another slot. */
    memcpy(synth, expected, sizeof(synth));
    synth[2] ^= 0x01;
    readback = bladerf2_rx_sweep_readback(synth, expected, 76800000, 0, &hz);
    if (readback != BLADERF2_RX_SWEEP_READBACK_MISMATCH || hz == 0) {
        fputs("fractional word mismatch not detected\n", stderr);
        failures++;
    }
    memset(&r, 0, sizeof(r));
    r.frequency = 925000000;
    bladerf2_rx_sweep_record_success(&r, readback, hz, 1200, OK_FLAGS, 50);
    if ((r.flags & BLADERF_RX_SWEEP_F_CERTIFIED) != 0 ||
        (r.flags & BLADERF_RX_SWEEP_F_READBACK_MISMATCH) == 0) {
        fputs("mismatching words certified\n", stderr);
        failures++;
    }
    memcpy(synth, expected, sizeof(synth));
    synth[NIOS_PKT_RETUNE2_SYNTH_VCO_DIV] = 0x03;
    if (bladerf2_rx_sweep_readback(synth, expected, 76800000, 0, &hz) !=
        BLADERF2_RX_SWEEP_READBACK_MISMATCH) {
        fputs("VCO divider mismatch not detected\n", stderr);
        failures++;
    }
    memcpy(synth, expected, sizeof(synth));
    synth[NIOS_PKT_RETUNE2_SYNTH_SETUP] = 0x81; /* slot 4 selected */
    if (bladerf2_rx_sweep_readback(synth, expected, 76800000, 0, &hz) !=
        BLADERF2_RX_SWEEP_READBACK_MISMATCH) {
        fputs("selected slot mismatch not detected\n", stderr);
        failures++;
    }

    /* Nothing to certify by: old FPGA (no word 3), failed NIOS read, words
     * the host does not know, unknown reference rate. */
    memcpy(synth, expected, sizeof(synth));
    if (bladerf2_rx_sweep_readback(NULL, expected, 76800000, 0, &hz) !=
            BLADERF2_RX_SWEEP_READBACK_NONE ||
        bladerf2_rx_sweep_readback(synth, NULL, 76800000, 0, &hz) !=
            BLADERF2_RX_SWEEP_READBACK_NONE ||
        bladerf2_rx_sweep_readback(synth, expected, 0, 0, &hz) !=
            BLADERF2_RX_SWEEP_READBACK_NONE) {
        fputs("missing readback treated as valid\n", stderr);
        failures++;
    }
    synth[NIOS_PKT_RETUNE2_SYNTH_SETUP] = 0;
    if (bladerf2_rx_sweep_readback(synth, expected, 76800000, 0, &hz) !=
        BLADERF2_RX_SWEEP_READBACK_NONE) {
        fputs("failed NIOS read treated as valid\n", stderr);
        failures++;
    }
    memset(&r, 0, sizeof(r));
    r.frequency = 925000000;
    bladerf2_rx_sweep_record_success(&r, BLADERF2_RX_SWEEP_READBACK_NONE, 0,
                                     1200, OK_FLAGS, 50);
    if ((r.flags & (BLADERF_RX_SWEEP_F_CERTIFIED |
                    BLADERF_RX_SWEEP_F_READBACK_VALID)) != 0) {
        fputs("record without readback certified\n", stderr);
        failures++;
    }

    /* FPGA tuning mode: the NIOS AD9361 driver's Hz readback. */
    if (bladerf2_rx_sweep_readback(NULL, NULL, 0, 925000000, &hz) !=
            BLADERF2_RX_SWEEP_READBACK_VALID ||
        hz != 925000000) {
        fputs("NIOS Hz readback not used\n", stderr);
        failures++;
    }
}

static void check_ranges(void)
{
    /* 1 ok, 2 failed, 3 ok, 4 unlocked, 5 ok, 6 lost, end at 10000. */
    struct bladerf_rx_sweep_record r[8];

    r[0] = success(1, 100, 1000, 1100, OK_FLAGS, 0, 20);
    r[1] = other(2, BLADERF_RX_SWEEP_FAILED, 2000);
    r[2] = success(3, 300, 3000, 3300, OK_FLAGS, 0, 20);
    r[3] = success(4, 400, 4000, 4100,
                   NIOS_PKT_RETUNE2_EXEC_TS_VALID, 0, 20);
    r[4] = success(5, 500, 5000, 5050, OK_FLAGS, 0, 20);
    r[5] = other(6, BLADERF_RX_SWEEP_LOST, 6000);
    r[6] = success(7, 700, 7000, 7010, OK_FLAGS, 0, 20);
    r[7] = other(8, BLADERF_RX_SWEEP_END, 10000);

    expect_label(r, 8, 999, false, 0, "before first retune");
    expect_label(r, 8, 1000, false, 0, "at schedule, before execution");
    expect_label(r, 8, 1119, false, 0, "inside settle");
    expect_label(r, 8, 1120, true, 0, "first certified sample");
    expect_label(r, 8, 1999, true, 0, "last sample before next schedule");
    /* Next request scheduled at 2000 may change the LO from then on, even
     * though it failed: its failure stage may follow a partial recall. */
    expect_label(r, 8, 2000, false, 0, "failed request");
    expect_label(r, 8, 2999, false, 0, "after failed request");
    expect_label(r, 8, 3320, true, 2, "after failure, next success");
    expect_label(r, 8, 4200, false, 0, "unlocked PLL");
    expect_label(r, 8, 5070, true, 4, "success before lost record");
    expect_label(r, 8, 5999, true, 4, "last sample before lost request");
    expect_label(r, 8, 6000, false, 0, "lost request");
    expect_label(r, 8, 7030, true, 6, "success before end");
    expect_label(r, 8, 9999, true, 6, "last sample before end");
    expect_label(r, 8, 10000, false, 0, "end marker");
    expect_label(r, 8, 20000, false, 0, "after end");

    /* Without its successor a record's range has no known end. */
    expect_label(r, 1, 1500, false, 0, "single record, end unknown");
    expect_label(r, 7, 9000, false, 0, "last record, end unknown");

    /* A dropped record (sequence gap) certifies nothing across it. */
    {
        struct bladerf_rx_sweep_record gap[3] = { r[0], r[2], r[4] };
        expect_label(gap, 3, 1500, false, 0, "gap after record 1");
        expect_label(gap, 3, 3500, false, 0, "gap after record 3");
    }

    /* A zero end timestamp (unreadable) closes the last range. */
    {
        struct bladerf_rx_sweep_record tail[2] = {
            r[6], other(8, BLADERF_RX_SWEEP_END, 0)
        };
        expect_label(tail, 2, 7030, false, 0, "unreadable end timestamp");
    }

    expect_label(NULL, 0, 1500, false, 0, "no records");
}

int main(void)
{
    check_record_classification();
    check_synth_readback();
    check_ranges();
    if (failures != 0) {
        fprintf(stderr, "%d RX sweep policy failure(s)\n", failures);
        return 1;
    }
    puts("RX sweep certification rule: PASS");
    return 0;
}
