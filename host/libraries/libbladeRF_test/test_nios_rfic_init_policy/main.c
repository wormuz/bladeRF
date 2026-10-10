#include <assert.h>
#include <stdbool.h>

#include "rfic_init_policy.h"

int main(void)
{
    assert(rfic_init_restore_standby_state(true, true));
    assert(!rfic_init_restore_standby_state(true, false));
    assert(!rfic_init_restore_standby_state(false, true));
    assert(!rfic_init_restore_standby_state(false, false));
    return 0;
}
