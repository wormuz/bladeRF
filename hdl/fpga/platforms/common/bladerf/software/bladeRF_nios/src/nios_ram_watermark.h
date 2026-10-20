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
#ifndef NIOS_RAM_WATERMARK_H_
#define NIOS_RAM_WATERMARK_H_

#include <stdint.h>
#include <stdbool.h>

#include "nios_pkt_8x32.h"

/* Stack/heap high-water mark; see NIOS_PKT_8x32_TARGET_NIOS_RAM. */

#define NIOS_RAM_WATERMARK_PATTERN 0xa5c35a3cu

/* Fill the gap between the heap end and the current stack pointer. Call once,
 * first thing in main(). */
void nios_ram_watermark_paint(void);

/* One NIOS_PKT_8x32_NIOS_RAM_* byte count; false for an unknown field. */
bool nios_ram_watermark_read(uint8_t field, uint32_t *bytes);

/* The read over an explicit layout, base <= heap <= top: the heap occupies
 * [base, heap), the stack grows down from top, and pattern words left
 * intact run upward from heap. */
static inline bool nios_ram_watermark_measure(uint8_t field,
                                              const uint32_t *base,
                                              const uint32_t *heap,
                                              const uint32_t *top,
                                              uint32_t *bytes)
{
    const uint32_t *p = heap;
    uintptr_t hi = (uintptr_t)top;
    uintptr_t lo = (uintptr_t)base;

    while (p < top && *p == NIOS_RAM_WATERMARK_PATTERN) {
        ++p;
    }

    if (field == NIOS_PKT_8x32_NIOS_RAM_UNTOUCHED) {
        hi = (uintptr_t)p;
        lo = (uintptr_t)heap;
    } else if (field == NIOS_PKT_8x32_NIOS_RAM_HEAP) {
        hi = (uintptr_t)heap;
    } else if (field == NIOS_PKT_8x32_NIOS_RAM_STACK) {
        lo = (uintptr_t)p;
    } else if (field != NIOS_PKT_8x32_NIOS_RAM_REGION) {
        return false;
    }

    *bytes = (uint32_t)(hi - lo);
    return true;
}

#endif
