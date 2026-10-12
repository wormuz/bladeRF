#include <assert.h>
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

    assert(rfic_init_cleanup_stage(0,
                                  RFIC_INIT_STAGE_PDATA_NULL,
                                  RFIC_INIT_STAGE_CLEANUP_FAILED) ==
           RFIC_INIT_STAGE_PDATA_NULL);
    assert(rfic_init_cleanup_stage(-1,
                                  RFIC_INIT_STAGE_PDATA_NULL,
                                  RFIC_INIT_STAGE_CLEANUP_FAILED) ==
           RFIC_INIT_STAGE_CLEANUP_FAILED);
    return 0;
}
