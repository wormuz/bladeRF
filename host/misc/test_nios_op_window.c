/* Host test of the NIOS operation window (nios_op_window.h): maxima, gaps,
 * open/close/clear semantics, register map, 32-bit wrap. */
#include "nios_op_window.h"

#include <stdio.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

struct nios_op_window nios_op_window;
uint32_t nios_op_window_now(void) { return 0; }

static uint64_t rd(const struct nios_op_window *w, uint8_t a)
{
    uint64_t v = 0;
    return nios_op_window_read(w, a, &v) ? v : ~0ull;
}

int main(void)
{
    struct nios_op_window w = {0};

    /* closed window records nothing */
    nios_op_window_pass_end(&w, 20);
    nios_op_window_worker(&w, 0, 99);
    CHECK(rd(&w, NIOS_PKT_8x64_OPWIN_PASSES) == 0);
    CHECK(rd(&w, NIOS_PKT_8x64_OPWIN_MAX_WORKER_TICKS) == 0);

    CHECK(nios_op_window_write(&w, NIOS_PKT_8x64_OPWIN_CONTROL, 1));
    CHECK(rd(&w, NIOS_PKT_8x64_OPWIN_CONTROL) == 1);
    nios_op_window_pass_begin(&w, 110);   /* first pass: no gap yet */
    nios_op_window_worker(&w, 110, 140);
    nios_op_window_pass_end(&w, 150);
    nios_op_window_pass_begin(&w, 160);   /* gap 10 */
    nios_op_window_worker(&w, 160, 170);
    nios_op_window_pass_end(&w, 175);
    nios_op_window_pass_begin(&w, 275);   /* gap 100 */
    nios_op_window_pass_end(&w, 280);
    CHECK(rd(&w, NIOS_PKT_8x64_OPWIN_MAX_SERVICE_GAP_TICKS) == 100);
    CHECK(rd(&w, NIOS_PKT_8x64_OPWIN_MAX_WORKER_TICKS) == 30);
    CHECK(rd(&w, NIOS_PKT_8x64_OPWIN_PASSES) == 3);

    CHECK(nios_op_window_hook_begin(&w, 400));
    nios_op_window_hook_end(&w, 400, 420);
    CHECK(nios_op_window_hook_begin(&w, 500));   /* hook gap 100 */
    nios_op_window_hook_end(&w, 500, 505);
    CHECK(rd(&w, NIOS_PKT_8x64_OPWIN_MAX_HOOK_GAP_TICKS) == 100);
    CHECK(rd(&w, NIOS_PKT_8x64_OPWIN_MAX_HOOK_TICKS) == 20);

    /* close stops recording, values stay readable */
    CHECK(nios_op_window_write(&w, NIOS_PKT_8x64_OPWIN_CONTROL, 0));
    CHECK(rd(&w, NIOS_PKT_8x64_OPWIN_CONTROL) == 0);
    nios_op_window_worker(&w, 0, 5000);
    nios_op_window_pass_end(&w, 600);
    CHECK(rd(&w, NIOS_PKT_8x64_OPWIN_MAX_WORKER_TICKS) == 30);
    CHECK(rd(&w, NIOS_PKT_8x64_OPWIN_PASSES) == 3);
    CHECK(!nios_op_window_hook_begin(&w, 900));

    /* reopen clears */
    CHECK(nios_op_window_write(&w, NIOS_PKT_8x64_OPWIN_CONTROL, 1));
    CHECK(rd(&w, NIOS_PKT_8x64_OPWIN_MAX_SERVICE_GAP_TICKS) == 0);
    CHECK(rd(&w, NIOS_PKT_8x64_OPWIN_PASSES) == 0);

    /* 32-bit counter wrap does not break a gap */
    nios_op_window_pass_end(&w, 0xfffffff0u);
    nios_op_window_pass_begin(&w, 0x10u);
    CHECK(rd(&w, NIOS_PKT_8x64_OPWIN_MAX_SERVICE_GAP_TICKS) == 0x20);

    /* bad control/address are rejected */
    CHECK(!nios_op_window_write(&w, NIOS_PKT_8x64_OPWIN_CONTROL, 2));
    CHECK(!nios_op_window_write(&w, NIOS_PKT_8x64_OPWIN_PASSES, 1));
    CHECK(rd(&w, 0x16) == ~0ull);
    CHECK(rd(&w, 0x0f) == ~0ull);

    puts("nios_op_window selfcheck PASS");
    return 0;
}
