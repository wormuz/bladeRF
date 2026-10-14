/* This file is part of the bladeRF project:
 *   http://www.github.com/nuand/bladeRF
 *
 * Copyright (c) 2026 Nuand LLC
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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 */

#ifndef BLADERF_NIOS_RFIC_SCHEDULER_POLICY_H_
#define BLADERF_NIOS_RFIC_SCHEDULER_POLICY_H_

#include <stdbool.h>

/* Queue occupancy alone is not device ownership. Let other RFIC-capable
 * schedulers run between terminal queue entries, but never during an active
 * callback or a resumable hardware calibration. */
static inline bool rfic_scheduler_device_owned(bool queue_head_running,
                                               bool calibration_pending,
                                               bool external_transition_pending)
{
    return queue_head_running || calibration_pending ||
           external_transition_pending;
}

static inline bool rfic_scheduler_calibration_pending(bool rfdc_pending,
		bool tx_quad_pending, bool sample_rate_pending,
		bool bandwidth_pending, bool init_pending)
{
	return rfdc_pending || tx_quad_pending || sample_rate_pending ||
	       bandwidth_pending || init_pending;
}

static inline bool rfic_scheduler_reference_tune_allowed(
		bool sample_ready, bool rfic_device_owned)
{
	return sample_ready && !rfic_device_owned;
}

/* Reference-clock writes can invalidate an in-flight RFPLL acquisition even
 * when they arrive through a direct peripheral packet rather than the tamer. */
static inline bool rfic_scheduler_reference_write_allowed(
        bool rfic_device_owned)
{
    return !rfic_device_owned;
}

#endif
