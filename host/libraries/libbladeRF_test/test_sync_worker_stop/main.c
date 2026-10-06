#include <assert.h>

#include "host_config.h"
#include "streaming/async.h"
#include "streaming/sync_worker.h"

int main(void)
{
    struct bladerf_stream stream = {0};
    struct sync_worker worker = {0};

    stream.state = STREAM_RUNNING;
    stream.layout = BLADERF_RX_X1;
    worker.stream = &stream;

    assert(MUTEX_INIT(&stream.lock) == 0);
    assert(MUTEX_INIT(&worker.request_lock) == 0);
    assert(COND_INIT(&worker.requests_pending) == 0);

    sync_worker_submit_request(&worker, SYNC_WORKER_STOP);

    assert(worker.requests & SYNC_WORKER_STOP);
    assert(stream.state == STREAM_SHUTTING_DOWN);

    assert(MUTEX_DESTROY(&worker.request_lock) == 0);
    assert(MUTEX_DESTROY(&stream.lock) == 0);
    return 0;
}
