/* nios_ram_watermark_measure() over a painted array standing in for the
 * NIOS stack/heap gap. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "nios_ram_watermark.h"

#define WORDS 64u

static uint32_t ram[WORDS];

static int expect(uint8_t field, const uint32_t *heap, bool ok,
                  uint32_t bytes)
{
    uint32_t got = 0xffffffffu;
    bool got_ok = nios_ram_watermark_measure(field, &ram[0], heap,
                                             &ram[WORDS], &got);

    if (got_ok != ok || (ok && got != bytes)) {
        fprintf(stderr, "field %u: ok=%d bytes=%u, want ok=%d bytes=%u\n",
                field, got_ok, got, ok, bytes);
        return 1;
    }
    return 0;
}

static int expect_layout(const uint32_t *heap, uint32_t untouched,
                         uint32_t heap_bytes, uint32_t stack)
{
    return expect(NIOS_PKT_8x32_NIOS_RAM_UNTOUCHED, heap, true, untouched) |
           expect(NIOS_PKT_8x32_NIOS_RAM_HEAP, heap, true, heap_bytes) |
           expect(NIOS_PKT_8x32_NIOS_RAM_STACK, heap, true, stack) |
           expect(NIOS_PKT_8x32_NIOS_RAM_REGION, heap, true, WORDS * 4u);
}

int main(void)
{
    unsigned int i;
    int fail = 0;

    /* Heap [0, 8) words, painted [8, 50), stack [50, 64). */
    for (i = 0; i < WORDS; ++i) {
        ram[i] = i >= 8 && i < 50 ? NIOS_RAM_WATERMARK_PATTERN : i;
    }
    fail |= expect_layout(&ram[8], 42 * 4, 8 * 4, 14 * 4);

    /* A stack word that happens to hold the pattern above the first
     * overwritten word does not shrink the reported depth. */
    ram[55] = NIOS_RAM_WATERMARK_PATTERN;
    fail |= expect_layout(&ram[8], 42 * 4, 8 * 4, 14 * 4);

    /* The heap grew by 4 words after painting: the scan starts at the
     * current heap end, so those words count as heap, not as gap. */
    fail |= expect_layout(&ram[12], 38 * 4, 12 * 4, 14 * 4);

    /* Nothing left untouched: the stack reached the heap end. */
    ram[12] = 0;
    fail |= expect_layout(&ram[12], 0, 12 * 4, 52 * 4);

    /* The whole gap untouched up to the top: the scan stops at top. */
    for (i = 0; i < WORDS; ++i) {
        ram[i] = NIOS_RAM_WATERMARK_PATTERN;
    }
    fail |= expect_layout(&ram[0], WORDS * 4, 0, 0);

    fail |= expect(NIOS_PKT_8x32_NIOS_RAM_REGION + 1, &ram[0], false, 0);

    if (fail) {
        return 1;
    }
    puts("NIOS RAM watermark measure: PASS");
    return 0;
}
