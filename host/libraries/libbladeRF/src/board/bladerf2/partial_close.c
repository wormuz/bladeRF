#include <stdlib.h>

#include "board/board.h"

void bladerf2_close_partial_open(struct bladerf *dev)
{
    if (dev != NULL) {
        free(dev->flash_arch);
        dev->flash_arch = NULL;
    }
}
