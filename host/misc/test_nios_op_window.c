/* Host test of the NIOS operation window (nios_op_window.h): maxima, gaps,
 * open/close/clear semantics and register map. */
#include "nios_op_window.h"

#include <stdio.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

struct nios_op_window nios_op_window;
uint64_t nios_op_window_now(void) { return 0; }

static uint64_t rd(const struct nios_op_window *w, uint8_t a, uint64_t now)
{
    uint64_t v = 0xdead;
    if (!nios_op_window_read(w, a, now, &v)) return ~0ull;
    return v;
}

int main(void)
{
    struct nios_op_window w = {0};

    /* closed window records nothing */
    nios_op_window_pass_begin(&w, 10);
    nios_op_window_pass_end(&w, 20);
    nios_op_window_worker(&w, 0, 99);
    CHECK(rd(&w, NIOS_PKT_8x64_OPWIN_PASSES, 30) == 0);
    CHECK(rd(&w, NIOS_PKT_8x64_OPWIN_MAX_WORKER_TICKS, 30) == 0);

    CHECK(nios_op_window_write(&w, NIOS_PKT_8x64_OPWIN_CONTROL, 1, 100));
    CHECK(rd(&w, NIOS_PKT_8x64_OPWIN_CONTROL, 100) == 1);
    nios_op_window_pass_begin(&w, 110);   /* first pass: no gap yet */
    nios_op_window_worker(&w, 110, 140);
    nios_op_window_pass_end(&w, 150);
    nios_op_window_pass_begin(&w, 160);   /* gap 10 */
    nios_op_window_worker(&w, 160, 170);
    nios_op_window_pass_end(&w, 175);
    nios_op_window_pass_begin(&w, 275);   /* gap 100 */
    nios_op_window_pass_end(&w, 280);
    nios_op_window_pass_begin(&w, 290);   /* gap 10 */
    nios_op_window_pass_end(&w, 295);
    CHECK(rd(&w, NIOS_PKT_8x64_OPWIN_MAX_SERVICE_GAP_TICKS, 300) == 100);
    CHECK(rd(&w, NIOS_PKT_8x64_OPWIN_MAX_WORKER_TICKS, 300) == 30);
    CHECK(rd(&w, NIOS_PKT_8x64_OPWIN_PASSES, 300) == 4);
    CHECK(rd(&w, NIOS_PKT_8x64_OPWIN_ELAPSED_TICKS, 300) == 200);

    CHECK(nios_op_window_hook_begin(&w, 400));
    nios_op_window_hook_end(&w, 400, 420);
    CHECK(nios_op_window_hook_begin(&w, 500));   /* hook gap 100 */
    nios_op_window_hook_end(&w, 500, 505);
    CHECK(rd(&w, NIOS_PKT_8x64_OPWIN_MAX_HOOK_GAP_TICKS, 600) == 100);
    CHECK(rd(&w, NIOS_PKT_8x64_OPWIN_MAX_HOOK_TICKS, 600) == 20);

    /* close freezes elapsed and stops recording */
    CHECK(nios_op_window_write(&w, NIOS_PKT_8x64_OPWIN_CONTROL, 0, 700));
    CHECK(rd(&w, NIOS_PKT_8x64_OPWIN_CONTROL, 800) == 0);
    CHECK(rd(&w, NIOS_PKT_8x64_OPWIN_ELAPSED_TICKS, 800) == 600);
    nios_op_window_worker(&w, 0, 5000);
    CHECK(rd(&w, NIOS_PKT_8x64_OPWIN_MAX_WORKER_TICKS, 800) == 30);
    CHECK(!nios_op_window_hook_begin(&w, 900));

    /* reopen clears */
    CHECK(nios_op_window_write(&w, NIOS_PKT_8x64_OPWIN_CONTROL, 1, 1000));
    CHECK(rd(&w, NIOS_PKT_8x64_OPWIN_MAX_SERVICE_GAP_TICKS, 1000) == 0);
    CHECK(rd(&w, NIOS_PKT_8x64_OPWIN_PASSES, 1000) == 0);

    /* bad control/address are rejected */
    CHECK(!nios_op_window_write(&w, NIOS_PKT_8x64_OPWIN_CONTROL, 2, 1));
    CHECK(!nios_op_window_write(&w, NIOS_PKT_8x64_OPWIN_PASSES, 1, 1));
    CHECK(rd(&w, 0x17, 1) == ~0ull);

    puts("nios_op_window selfcheck PASS");
    return 0;
}
