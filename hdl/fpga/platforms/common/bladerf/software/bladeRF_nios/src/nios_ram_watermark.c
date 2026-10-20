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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <stdint.h>
#include <stdbool.h>

#include "nios_pkt_8x32.h"
#include "nios_ram_watermark.h"

#ifdef BLADERF_NIOS_PC_SIMULATION

/* The PC simulation has no linker-placed heap/stack gap to measure, and its
 * packet table does not route NIOS_PKT_8x32_TARGET_NIOS_RAM here. */
void nios_ram_watermark_paint(void)
{
}

bool nios_ram_watermark_read(uint8_t field, uint32_t *bytes)
{
    *bytes = 0;
    return false;
}

#else

#include <unistd.h>

/* linker.x: the heap starts after .bss, the stack starts at the RAM end and
 * grows down into the same gap. HAL sbrk() keeps the heap end word aligned. */
extern uint32_t __alt_heap_start[];
extern uint32_t __alt_stack_pointer[];

void nios_ram_watermark_paint(void)
{
    uint32_t *p = sbrk(0);
    uint32_t *sp;

    /* Interrupts stay enabled: an ISR never runs while this loop does, and
     * a frame it pushed below sp is dead once it returns. */
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    while (p < sp) {
        *p++ = NIOS_RAM_WATERMARK_PATTERN;
    }
}

bool nios_ram_watermark_read(uint8_t field, uint32_t *bytes)
{
    return nios_ram_watermark_measure(field, __alt_heap_start, sbrk(0),
                                      __alt_stack_pointer, bytes);
}

#endif
