#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "rfic_progress_policy.h"

static void assert_roundtrip(struct bladerf_rfic_progress expected)
{
    struct bladerf_rfic_progress actual = bladerf_rfic_progress_unpack(
        bladerf_rfic_progress_pack(&expected));

    assert(actual.sequence == expected.sequence);
    assert(actual.transaction_id == expected.transaction_id);
    assert(actual.command == expected.command);
    assert(actual.phase_or_result == expected.phase_or_result);
}

int main(void)
{
    struct bladerf_rfic_progress zero = { 0, 0, 0, 0 };
    struct bladerf_rfic_progress max = {
        UINT32_MAX, UINT16_MAX, UINT8_MAX, UINT8_MAX
    };
    struct bladerf_rfic_progress tagged_running = {
        0x89abcdefu, 0x1234u, 0x0du, 2u
    };
    struct bladerf_rfic_progress retired_timeout = {
        0x89abcdf0u, 0x1234u, 0x06u, BLADERF_RFIC_RESULT_TIMEOUT
    };

    assert_roundtrip(zero);
    assert_roundtrip(max);
    assert_roundtrip(tagged_running);
    assert_roundtrip(retired_timeout);

    {
        const struct bladerf_rfic_result_detail expected = {
            0x89abcdefu, 0x1234u, BLADERF_RFIC_RESULT_IO,
            BLADERF_RFIC_RESULT_TIMEOUT
        };
        const struct bladerf_rfic_result_detail actual =
            bladerf_rfic_result_detail_unpack(
                bladerf_rfic_result_detail_pack(&expected));

        assert(actual.sequence == expected.sequence);
        assert(actual.transaction_id == expected.transaction_id);
        assert(actual.primary_result == expected.primary_result);
        assert(actual.cleanup_result == expected.cleanup_result);
        assert(bladerf_rfic_result_detail_matches(
            &actual, expected.transaction_id, expected.sequence,
            expected.primary_result));
        assert(!bladerf_rfic_result_detail_matches(
            &actual, (uint16_t)(expected.transaction_id + 1),
            expected.sequence, expected.primary_result));
        assert(!bladerf_rfic_result_detail_matches(
            &actual, expected.transaction_id, expected.sequence + 1,
            expected.primary_result));
        assert(!bladerf_rfic_result_detail_matches(
            &actual, expected.transaction_id, expected.sequence,
            BLADERF_RFIC_RESULT_TIMEOUT));
        {
            struct bladerf_rfic_result_detail empty = expected;
            empty.cleanup_result = BLADERF_RFIC_RESULT_NO_RESULT;
            assert(!bladerf_rfic_result_detail_matches(
                &empty, expected.transaction_id, expected.sequence,
                expected.primary_result));
        }
    }

    {
        const uint8_t rfdc_phase = bladerf_rfic_progress_phase_encode(
            BLADERF_RFIC_PROGRESS_PHASE_RFDC, 2);
        const uint8_t init_phase = bladerf_rfic_progress_phase_encode(
            BLADERF_RFIC_PROGRESS_PHASE_INIT, 15);
        const uint8_t rfpll_phase = bladerf_rfic_progress_phase_encode(
            BLADERF_RFIC_PROGRESS_PHASE_RFPLL, 3);
        const uint8_t fir_phase = bladerf_rfic_progress_phase_encode(
            BLADERF_RFIC_PROGRESS_PHASE_FIR, 1);

        assert(bladerf_rfic_progress_phase_is_tagged(rfdc_phase));
        assert(bladerf_rfic_progress_phase_class(rfdc_phase) ==
               BLADERF_RFIC_PROGRESS_PHASE_RFDC);
        assert(bladerf_rfic_progress_phase_step(rfdc_phase) == 2);
        assert(bladerf_rfic_progress_phase_class(init_phase) ==
               BLADERF_RFIC_PROGRESS_PHASE_INIT);
        assert(bladerf_rfic_progress_phase_step(init_phase) == 15);
        assert(bladerf_rfic_progress_phase_is_tagged(rfpll_phase));
        assert(bladerf_rfic_progress_phase_class(rfpll_phase) ==
               BLADERF_RFIC_PROGRESS_PHASE_RFPLL);
        assert(bladerf_rfic_progress_phase_step(rfpll_phase) == 3);
        assert(bladerf_rfic_progress_phase_is_tagged(fir_phase));
        assert(bladerf_rfic_progress_phase_class(fir_phase) ==
               BLADERF_RFIC_PROGRESS_PHASE_FIR);
        assert(bladerf_rfic_progress_phase_step(fir_phase) == 1);
        assert(strcmp(bladerf_rfic_progress_phase_name(fir_phase),
                      "fir-bandwidth-txq") == 0);
        assert(strcmp(bladerf_rfic_progress_phase_name(rfdc_phase),
                      "rfdc-calibration") == 0);
        assert(strcmp(bladerf_rfic_progress_phase_name(init_phase),
                      bladerf_rfic_progress_init_step_name(init_phase)) == 0);
        assert(!bladerf_rfic_progress_phase_is_tagged(
            BLADERF_RFIC_RESULT_TIMEOUT));
        assert(!bladerf_rfic_progress_phase_is_tagged(
            BLADERF_RFIC_RESULT_NO_RESULT));
        assert(bladerf_rfic_progress_phase_encode(0, 0) == 0);
        assert(bladerf_rfic_progress_phase_encode(8, 0) == 0);
        assert(bladerf_rfic_progress_phase_encode(1, 16) == 0);
        assert(bladerf_rfic_progress_phase_encode(
                   BLADERF_RFIC_PROGRESS_PHASE_FIR,
                   BLADERF_RFIC_PROGRESS_PHASE_STEP_MASK) == 0);

        /* INIT's nested constructor/tail uses stable step codes in the same
         * four-bit field; every named stage must remain representable and
         * distinct without changing the packet layout. */
        {
            static const uint8_t init_steps[] = {
                BLADERF_RFIC_PROGRESS_INIT_RESET,
                BLADERF_RFIC_PROGRESS_INIT_IDENTIFY_LOCK,
                BLADERF_RFIC_PROGRESS_INIT_CLOCKS,
                BLADERF_RFIC_PROGRESS_INIT_PRE_CLOCK,
                BLADERF_RFIC_PROGRESS_INIT_CLOCK_CHAIN,
                BLADERF_RFIC_PROGRESS_INIT_POST_CLOCK,
                BLADERF_RFIC_PROGRESS_INIT_SYNTH_CP,
                BLADERF_RFIC_PROGRESS_INIT_POST_CP,
                BLADERF_RFIC_PROGRESS_INIT_GAIN_TABLE,
                BLADERF_RFIC_PROGRESS_INIT_GAIN_CONTROL,
                BLADERF_RFIC_PROGRESS_INIT_CALIBRATION,
                BLADERF_RFIC_PROGRESS_INIT_TAIL_RESET_LO,
                BLADERF_RFIC_PROGRESS_INIT_TAIL_FILTER,
                BLADERF_RFIC_PROGRESS_INIT_TAIL_SAMPLE_RATE,
                BLADERF_RFIC_PROGRESS_INIT_TAIL_FINAL_LO_OUTPUT,
            };
            size_t i, j;

            for (i = 0; i < sizeof(init_steps) / sizeof(init_steps[0]); i++) {
                const uint8_t encoded = bladerf_rfic_progress_phase_encode(
                    BLADERF_RFIC_PROGRESS_PHASE_INIT, init_steps[i]);

                assert(bladerf_rfic_progress_phase_is_tagged(encoded));
                assert(bladerf_rfic_progress_phase_class(encoded) ==
                       BLADERF_RFIC_PROGRESS_PHASE_INIT);
                assert(bladerf_rfic_progress_phase_step(encoded) ==
                       init_steps[i]);
                assert(bladerf_rfic_progress_init_step_name(encoded)[0] !=
                       '\0');
                for (j = i + 1; j < sizeof(init_steps) / sizeof(init_steps[0]);
                     j++)
                    assert(init_steps[i] != init_steps[j]);
            }
            assert(bladerf_rfic_progress_init_step_name(0) != NULL);
        }
    }

    assert(bladerf_rfic_result_ack_is_duplicate(0x1234, 17, 0x1234, 17));
    assert(!bladerf_rfic_result_ack_is_duplicate(0x1234, 17, 0x1235, 17));
    assert(!bladerf_rfic_result_ack_is_duplicate(0x1234, 17, 0x1234, 18));
    assert(!bladerf_rfic_result_ack_is_duplicate(0x1234, 17, 0, 17));

    assert(bladerf_rfic_result_from_status(0) ==
           BLADERF_RFIC_RESULT_SUCCESS);
    assert(bladerf_rfic_result_from_status(-ETIMEDOUT) ==
           BLADERF_RFIC_RESULT_TIMEOUT);
    assert(bladerf_rfic_result_from_status(-EINVAL) ==
           BLADERF_RFIC_RESULT_INVALID_PARAMETER);
    assert(bladerf_rfic_result_from_status(-ERANGE) ==
           BLADERF_RFIC_RESULT_INVALID_PARAMETER);
    assert(bladerf_rfic_result_from_status(-EIO) ==
           BLADERF_RFIC_RESULT_IO);
    assert(bladerf_rfic_result_from_status(-ENOTSUP) ==
           BLADERF_RFIC_RESULT_UNSUPPORTED);
    assert(bladerf_rfic_result_latch_error(BLADERF_RFIC_RESULT_NO_RESULT,
                                           -ETIMEDOUT) ==
           BLADERF_RFIC_RESULT_TIMEOUT);
    assert(bladerf_rfic_result_latch_error(BLADERF_RFIC_RESULT_TIMEOUT,
                                           -EIO) ==
           BLADERF_RFIC_RESULT_TIMEOUT);
    assert(bladerf_rfic_result_latch_error(BLADERF_RFIC_RESULT_SUCCESS,
                                           -EIO) ==
           BLADERF_RFIC_RESULT_IO);
    assert(bladerf_rfic_result_from_status(-12345) ==
           BLADERF_RFIC_RESULT_INTERNAL);
    assert(bladerf_rfic_result_from_status(INT32_MIN) ==
           BLADERF_RFIC_RESULT_INTERNAL);
    assert(bladerf_rfic_result_finalize(BLADERF_RFIC_RESULT_NO_RESULT,
                                        true) ==
           BLADERF_RFIC_RESULT_SUCCESS);
    assert(bladerf_rfic_result_finalize(BLADERF_RFIC_RESULT_NO_RESULT,
                                        false) ==
           BLADERF_RFIC_RESULT_INTERNAL);
    assert(bladerf_rfic_result_finalize(BLADERF_RFIC_RESULT_IO, true) ==
           BLADERF_RFIC_RESULT_IO);
    assert(bladerf_rfic_result_finalize(BLADERF_RFIC_RESULT_IO, false) ==
           BLADERF_RFIC_RESULT_IO);
    assert(bladerf_rfic_result_finalize(
               bladerf_rfic_result_latch_error(
                   BLADERF_RFIC_RESULT_NO_RESULT, -ETIMEDOUT),
               true) == BLADERF_RFIC_RESULT_TIMEOUT);
    assert(bladerf_rfic_result_finalize(BLADERF_RFIC_RESULT_SUCCESS,
                                        false) ==
           BLADERF_RFIC_RESULT_INTERNAL);
    assert(!bladerf_rfic_cleanup_failed(BLADERF_RFIC_RESULT_SUCCESS));
    assert(bladerf_rfic_cleanup_failed(BLADERF_RFIC_RESULT_IO));
    assert(bladerf_rfic_result_finalize_with_cleanup(
               BLADERF_RFIC_RESULT_SUCCESS, true,
               BLADERF_RFIC_RESULT_IO) ==
           BLADERF_RFIC_RESULT_INTERNAL);
    assert(bladerf_rfic_result_finalize_with_cleanup(
               BLADERF_RFIC_RESULT_IO, false,
               BLADERF_RFIC_RESULT_TIMEOUT) == BLADERF_RFIC_RESULT_IO);
    assert(bladerf_rfic_result_ack_matches(true, 0, 0x1234, 17,
                                           0x1234, 17));
    assert(!bladerf_rfic_result_ack_matches(false, 0, 0x1234, 17,
                                            0x1234, 17));
    assert(!bladerf_rfic_result_ack_matches(true, 1, 0x1234, 17,
                                            0x1234, 17));
    assert(!bladerf_rfic_result_ack_matches(true, 0, 0x1234, 17,
                                            0x1235, 17));
    assert(!bladerf_rfic_result_ack_matches(true, 0, 0x1234, 17,
                                            0x1234, 18));
    assert(!bladerf_rfic_result_ack_matches(true, 0, 0x1234, 17, 0, 17));
    assert(bladerf_rfic_tagged_write_allowed(0, false, 0));
    assert(bladerf_rfic_tagged_write_allowed(0, true, 0));
    assert(!bladerf_rfic_tagged_write_allowed(1, false, 0));
    assert(!bladerf_rfic_tagged_write_allowed(0, true, 0x1234));
    return 0;
}
