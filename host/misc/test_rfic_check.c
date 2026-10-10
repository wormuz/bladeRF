#include <stdbool.h>
#include <stdio.h>

#define RFIC_ERR(_thing, _status) ((void)0)
#include "rfic_check.h"

static bool check_bool_result(bool result)
{
    CHECK_TRUE(result);
    return true;
}

static bool check_status_result(int status)
{
    CHECK_BOOL(status);
    return true;
}

int main(void)
{
    if (check_bool_result(true) != true ||
        check_bool_result(false) != false ||
        check_status_result(0) != true ||
        check_status_result(-1) != false) {
        fputs("NIOS RFIC result checks confused bool and status returns\n",
              stderr);
        return 1;
    }

    return 0;
}
