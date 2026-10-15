/* Exercise the production TX condition-variable wait with two simultaneous
 * observers. Both must finish on readiness and both must finish on a fault;
 * there is no elapsed-time fallback to conceal a missed notification. The
 * cond_wait wrapper synchronizes the test with actual production wait entry.
 */
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include "bladeRF.h"
#include "board/board.h"
#include "board/bladerf2/common.h"

static struct bladerf2_board_data *board;
static pthread_mutex_t sync_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sync_cond = PTHREAD_COND_INITIALIZER;
static unsigned waiting;
static int results[2];
static struct bladerf dev;

int __real_pthread_cond_wait(pthread_cond_t *, pthread_mutex_t *);
int __wrap_pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *lock)
{
    if (cond == &board->rx_async_epoch_cond) {
        pthread_mutex_lock(&sync_lock);
        ++waiting;
        pthread_cond_signal(&sync_cond);
        pthread_mutex_unlock(&sync_lock);
    }
    return __real_pthread_cond_wait(cond, lock);
}
static void *waiter(void *arg)
{
    results[(size_t)arg] = bladerf2_tx_wait_for_validity(&dev);
    return NULL;
}
static void run_case(bool fault)
{
    pthread_t threads[2];
    waiting = 0;
    board->tx_async_link_certified = true;
    board->tx_async_rfic_state_certified = false;
    board->tx_stream_faulted_async = false;
    board->rfic_reference_clock_known = true;
    board->rfic_frequency_model_known = true;
    for (size_t i = 0; i < 2; ++i)
        assert(pthread_create(&threads[i], NULL, waiter, (void *)i) == 0);
    pthread_mutex_lock(&sync_lock);
    while (waiting < 2)
        __real_pthread_cond_wait(&sync_cond, &sync_lock);
    pthread_mutex_unlock(&sync_lock);
    /* Both waiters entered the production wait while holding this mutex.
     * Taking it now confirms the second has atomically entered cond_wait. */
    pthread_mutex_lock(&board->rx_async_epoch_lock);
    board->tx_async_rfic_state_certified = !fault;
    pthread_mutex_unlock(&board->rx_async_epoch_lock);
    bladerf2_tx_stream_set_faulted(board, fault);
    for (size_t i = 0; i < 2; ++i) {
        assert(pthread_join(threads[i], NULL) == 0);
        assert(results[i] == (fault ? BLADERF_ERR_WOULD_BLOCK : 0));
    }
}
int main(void)
{
    board = calloc(1, sizeof(*board));
    assert(board != NULL);
    dev.board_data = board;
    assert(pthread_mutex_init(&board->rx_async_epoch_lock, NULL) == 0);
    assert(pthread_cond_init(&board->rx_async_epoch_cond, NULL) == 0);
    board->rx_async_epoch_cond_initialized = true;
    run_case(false);
    run_case(true);
    pthread_cond_destroy(&board->rx_async_epoch_cond);
    pthread_mutex_destroy(&board->rx_async_epoch_lock);
    free(board);
    puts("TX event wait: both waiters observe readiness and fault: PASS");
    return 0;
}
