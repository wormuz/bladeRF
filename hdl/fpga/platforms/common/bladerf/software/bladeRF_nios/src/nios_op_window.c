#include "nios_op_window.h"

#ifdef BOARD_BLADERF_MICRO
#include <sys/alt_timestamp.h>

uint64_t nios_op_window_now(void)
{
    return (uint64_t)alt_timestamp();
}
#endif

struct nios_op_window nios_op_window;
