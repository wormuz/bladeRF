#include <libbladeRF.h>

#include "board/board.h"

int main(void)
{
    struct bladerf dev = {0};

    /* Board open can fail before board_data is allocated. Generic open
     * cleanup still invokes the board close callback in that case. */
    bladerf2_board_fns.close(&dev);
    return 0;
}
