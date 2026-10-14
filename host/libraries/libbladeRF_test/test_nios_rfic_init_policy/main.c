#include <assert.h>
#include <errno.h>
#include <stdbool.h>

#include "rfic_init_policy.h"

enum {
    RFIC_INIT_STAGE_PDATA_NULL = 11,
    RFIC_INIT_STAGE_CLEANUP_FAILED = 15,
};

int main(void)
{
    assert(rfic_init_restore_standby_state(true, true));
    assert(!rfic_init_restore_standby_state(true, false));
    assert(!rfic_init_restore_standby_state(false, true));
    assert(!rfic_init_restore_standby_state(false, false));

    assert(rfic_init_adi_resume(true, false, 0, true));
    assert(!rfic_init_adi_resume(true, true, 0, true));
    assert(!rfic_init_adi_resume(true, false, -ETIMEDOUT, true));
    assert(!rfic_init_adi_resume(true, false, 0, false));
    assert(!rfic_init_adi_resume(false, false, 0, true));

    assert(rfic_init_cleanup_stage(0,
                                  RFIC_INIT_STAGE_PDATA_NULL,
                                  RFIC_INIT_STAGE_CLEANUP_FAILED) ==
           RFIC_INIT_STAGE_PDATA_NULL);
    assert(rfic_init_cleanup_stage(-1,
                                  RFIC_INIT_STAGE_PDATA_NULL,
                                  RFIC_INIT_STAGE_CLEANUP_FAILED) ==
           RFIC_INIT_STAGE_CLEANUP_FAILED);
    assert(rfic_init_failure_status(-ETIMEDOUT) == -ETIMEDOUT);
    assert(rfic_init_failure_status(-EINVAL) == -EINVAL);
    assert(rfic_init_failure_status(0) == -EIO);
    assert(rfic_init_failure_status(1) == -EIO);
    assert(rfic_init_digital_tune_policy_valid(2, true));
    assert(!rfic_init_digital_tune_policy_valid(0, true));
    assert(!rfic_init_digital_tune_policy_valid(1, true));
    assert(!rfic_init_digital_tune_policy_valid(2, false));
    assert(rfic_disconnect_verify_mask(0x000fffff, 1u) == 0x000ffffe);
    assert(rfic_disconnect_verify_mask(0x000fffff, 0x00000008) ==
           0x000ffff7);
    return 0;
}
