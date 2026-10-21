#include "nios_op_window.h"

#ifdef BOARD_BLADERF_MICRO
#include <sys/alt_timestamp.h>

uint32_t nios_op_window_now(void)
{
    return (uint32_t)alt_timestamp();
}
#else
/* The PC simulation builds the micro main loop without the board timer. */
uint32_t nios_op_window_now(void)
{
    return 0;
}
#endif

struct nios_op_window nios_op_window;
