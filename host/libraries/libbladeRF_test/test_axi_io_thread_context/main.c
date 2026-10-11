#define _XOPEN_SOURCE 700

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>

#include "board/board.h"

void bladerf2_axi_io_set_dev(struct bladerf *dev);
int32_t no_os_axi_io_read(uint32_t base, uint32_t offset, uint32_t *data);
int32_t no_os_axi_io_write(uint32_t base, uint32_t offset, uint32_t data);

struct test_device {
    struct bladerf dev;
    uint32_t identity;
    uint32_t register_value;
    int axi_status;
};

static pthread_barrier_t both_bound;

static int fake_axi_read(struct bladerf *dev, uint32_t addr, uint32_t *data)
{
    struct test_device *test_dev = dev->backend_data;

    (void)addr;
    *data = test_dev->register_value;
    return test_dev->axi_status;
}

static int fake_axi_write(struct bladerf *dev, uint32_t addr, uint32_t data)
{
    struct test_device *test_dev = dev->backend_data;

    (void)addr;
    test_dev->register_value = data;
    return test_dev->axi_status;
}

static const struct backend_fns backend = {
    .adi_axi_read = fake_axi_read,
    .adi_axi_write = fake_axi_write,
};

static void *exercise_device_context(void *arg)
{
    struct test_device *test_dev = arg;
    uint32_t value = 0;
    int barrier_status;

    bladerf2_axi_io_set_dev(&test_dev->dev);
    barrier_status = pthread_barrier_wait(&both_bound);
    assert(barrier_status == 0 ||
           barrier_status == PTHREAD_BARRIER_SERIAL_THREAD);
    assert(no_os_axi_io_read(0x1000, 4, &value) == 0);
    assert(value == test_dev->identity);
    assert(no_os_axi_io_write(0x1000, 8, test_dev->identity + 1) == 0);
    assert(test_dev->register_value == test_dev->identity + 1);
    test_dev->axi_status = BLADERF_ERR_TIMEOUT;
    value = UINT32_MAX;
    assert(no_os_axi_io_read(0x1000, 4, &value) == -ETIMEDOUT);
    assert(value == 0);
    assert(no_os_axi_io_write(0x1000, 8, test_dev->identity + 2) ==
           -ETIMEDOUT);
    bladerf2_axi_io_set_dev(NULL);
    assert(no_os_axi_io_read(0, 0, &value) < 0);
    return NULL;
}

int main(void)
{
    struct test_device a = {
        .identity = 0xA11CE001,
        .register_value = 0xA11CE001,
    };
    struct test_device b = {
        .identity = 0xB0B00002,
        .register_value = 0xB0B00002,
    };
    pthread_t ta, tb;

    a.dev.backend = &backend;
    a.dev.backend_data = &a;
    b.dev.backend = &backend;
    b.dev.backend_data = &b;

    assert(pthread_barrier_init(&both_bound, NULL, 2) == 0);
    assert(pthread_create(&ta, NULL, exercise_device_context, &a) == 0);
    assert(pthread_create(&tb, NULL, exercise_device_context, &b) == 0);
    assert(pthread_join(ta, NULL) == 0);
    assert(pthread_join(tb, NULL) == 0);
    assert(pthread_barrier_destroy(&both_bound) == 0);
    return 0;
}
