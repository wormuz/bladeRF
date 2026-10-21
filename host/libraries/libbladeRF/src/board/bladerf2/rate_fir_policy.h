/*
 * This file is part of the bladeRF project.
 * Copyright (C) 2026 Nuand LLC
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#ifndef BLADERF2_RATE_FIR_POLICY_H_
#define BLADERF2_RATE_FIR_POLICY_H_

#include <stdbool.h>
#include <stdint.h>

/* Native rate at which both FIR pairs validate: 30 * 4 = 120 MHz is under
 * MAX_TX_HB1 and the AD9361 clock chain keeps ADC/2 / RX_SAMPL >= 8, so the
 * 128-tap DEC4 filter passes "ADC/2 / RX_SAMPL * 16 > TAPS". */
#define BLADERF2_RATE_FIR_INTERMEDIATE 30000000u

struct bladerf2_rate_fir_plan {
    bool old_low;  /* AD9361 is in the 4x range now */
    bool new_low;  /* AD9361 will be in the 4x range */
    bool enter_4x; /* step to INTERMEDIATE, then RX DEC4 + TX INT4 */
    bool leave_4x; /* step to INTERMEDIATE, then default FIRs */
};

/* Both decisions are taken on the AD9361 rate, which is the user rate halved
 * under OVERSAMPLE. rfic_current comes from the controller getter, not the
 * board getter (that one doubles it): a chip left at 1.92 MHz with DEC4/INT4
 * reads back as 3.84 MHz under OVERSAMPLE, which skipped the restore of the
 * default filters. [lo_4x, hi_4x] is bladerf2_sample_rate_range_4x. */
static inline struct bladerf2_rate_fir_plan bladerf2_rate_fir_classify(
    uint32_t rfic_current, uint32_t requested, bool oversample,
    uint32_t lo_4x, uint32_t hi_4x)
{
    struct bladerf2_rate_fir_plan plan = { false, false, false, false };
    const uint32_t rfic_new = oversample ? requested / 2 : requested;

    plan.old_low = rfic_current >= lo_4x && rfic_current <= hi_4x;
    plan.new_low = rfic_new >= lo_4x && rfic_new <= hi_4x;
    return plan;
}

/* Entering 4x always passes through INTERMEDIATE. Enabling DEC4 at the
 * current rate fails the AD9361 TAPS check from 2.08-12 MHz (3.84 MHz:
 * max 96 < 128 taps) and from 61.44 MHz (test_bladerf2_rate_fir_transition);
 * a "current > 40 MHz" condition covers only the last. */
static inline void bladerf2_rate_fir_actions(
    struct bladerf2_rate_fir_plan *plan, bool fir_is_4x, bool fir_is_default)
{
    plan->enter_4x = plan->new_low && !fir_is_4x;
    plan->leave_4x = plan->old_low && !plan->new_low && !fir_is_default;
}

#endif
