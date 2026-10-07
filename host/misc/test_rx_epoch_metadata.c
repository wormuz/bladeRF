/* Focused contract test for RX sample-META epoch tags. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>

#include "host_config.h"
#include "streaming/metadata.h"

static void set_tag(uint8_t header[METADATA_HEADER_SIZE], uint8_t epoch_id)
{
    uint32_t tag = HOST_TO_LE32(METADATA_RX_EPOCH_TAG_VALID | epoch_id);
    memcpy(header + METADATA_RESV_OFFSET, &tag, sizeof(tag));
}

struct admission_race {
    pthread_mutex_t epoch_lock;
    pthread_mutex_t gate_lock;
    pthread_cond_t gate_changed;
    bool parsed;
    bool allow_commit;
    bool contract_enabled;
    bool certified;
    uint8_t epoch_id;
    uint64_t first_valid_timestamp;
    bool have_expected_timestamp;
    uint8_t timestamp_epoch_id;
    uint64_t expected_timestamp;
    bool admitted;
    uint8_t buffer[METADATA_HEADER_SIZE + 8];
};

static void *parse_then_commit(void *arg)
{
    struct admission_race *race = arg;
    const uint8_t snapshot_epoch = race->epoch_id;
    const uint64_t snapshot_boundary = race->first_valid_timestamp;
    uint64_t next_timestamp = 0;

    /* Match production ordering: parse the transfer outside epoch_lock. */
    enum metadata_rx_buffer_epoch_result parsed =
        metadata_rx_buffer_epoch_contiguous(
            race->buffer, sizeof(race->buffer), sizeof(race->buffer),
            snapshot_epoch, snapshot_boundary, false, 0, 4,
            &next_timestamp);
    assert(parsed == METADATA_RX_BUFFER_CONTIGUOUS);

    pthread_mutex_lock(&race->gate_lock);
    race->parsed = true;
    pthread_cond_broadcast(&race->gate_changed);
    while (!race->allow_commit) {
        pthread_cond_wait(&race->gate_changed, &race->gate_lock);
    }
    pthread_mutex_unlock(&race->gate_lock);

    /* Same production helper, under the same lock that protects RF state. */
    pthread_mutex_lock(&race->epoch_lock);
    race->admitted = metadata_rx_epoch_commit_timestamp(
        true, true, snapshot_epoch, snapshot_boundary,
        race->contract_enabled, race->certified, race->epoch_id,
        race->first_valid_timestamp, next_timestamp,
        &race->have_expected_timestamp, &race->timestamp_epoch_id,
        &race->expected_timestamp);
    pthread_mutex_unlock(&race->epoch_lock);
    return NULL;
}

static void test_async_admission_race(void)
{
    struct admission_race race = {
        .contract_enabled = true,
        .certified = true,
        .epoch_id = 7,
        .first_valid_timestamp = 1000,
        .have_expected_timestamp = false,
        .timestamp_epoch_id = 3,
        .expected_timestamp = 44,
    };
    pthread_t parser;

    metadata_set(race.buffer, 1000, 0);
    set_tag(race.buffer, 7);
    assert(pthread_mutex_init(&race.epoch_lock, NULL) == 0);
    assert(pthread_mutex_init(&race.gate_lock, NULL) == 0);
    assert(pthread_cond_init(&race.gate_changed, NULL) == 0);
    assert(pthread_create(&parser, NULL, parse_then_commit, &race) == 0);

    /* Deterministically revoke the certificate after successful META parse
     * and before the async admission commit point. */
    pthread_mutex_lock(&race.gate_lock);
    while (!race.parsed) {
        pthread_cond_wait(&race.gate_changed, &race.gate_lock);
    }
    pthread_mutex_lock(&race.epoch_lock);
    race.certified = false;
    pthread_mutex_unlock(&race.epoch_lock);
    race.allow_commit = true;
    pthread_cond_broadcast(&race.gate_changed);
    pthread_mutex_unlock(&race.gate_lock);

    assert(pthread_join(parser, NULL) == 0);
    assert(!race.admitted);
    assert(!race.have_expected_timestamp);
    assert(race.timestamp_epoch_id == 3);
    assert(race.expected_timestamp == 44);
    pthread_cond_destroy(&race.gate_changed);
    pthread_mutex_destroy(&race.gate_lock);
    pthread_mutex_destroy(&race.epoch_lock);
}

int main(void)
{
    uint8_t header[METADATA_HEADER_SIZE] = {0};
    uint8_t epoch_id = 0xff;
    bool have_expected = false;
    uint8_t timestamp_epoch = 3;
    uint64_t expected_timestamp = 44;

    /* The first matching META header seeds timestamp continuity. A
     * zero-initialized cursor must not make a packet exactly at the certified
     * first-valid boundary look like an invalid prefix. */
    assert(metadata_rx_epoch_disposition(
        true, true, true, 1000, 0, 1000, false, false) ==
        METADATA_RX_EPOCH_ACCEPT);
    assert(metadata_rx_epoch_disposition(
        true, true, true, 999, 0, 1000, false, false) ==
        METADATA_RX_EPOCH_SKIP_TIMESTAMP_PREFIX);
    assert(metadata_rx_epoch_disposition(
        true, true, true, 1000, 999, 1000, true, false) ==
        METADATA_RX_EPOCH_SKIP_TIMESTAMP_PREFIX);

    /* Async admission rechecks the same boundary after parsing. A setter can
     * revoke it while the transfer is being inspected, so matching bytes
     * alone are insufficient if the live certificate changed meanwhile.
     * Exercise the same commit helper used while holding the production lock. */
    assert(metadata_rx_epoch_commit_timestamp(
        true, true, 7, 1000, true, true, 7, 1000, 2048,
        &have_expected, &timestamp_epoch, &expected_timestamp));
    assert(have_expected && timestamp_epoch == 7 &&
           expected_timestamp == 2048);

    have_expected = false;
    timestamp_epoch = 3;
    expected_timestamp = 44;
    assert(!metadata_rx_epoch_commit_timestamp(
        true, true, 7, 1000, true, false, 7, 1000, 2048,
        &have_expected, &timestamp_epoch, &expected_timestamp));
    assert(!have_expected && timestamp_epoch == 3 && expected_timestamp == 44);
    assert(!metadata_rx_epoch_commit_timestamp(
        true, true, 7, 1000, true, true, 8, 1000, 2048,
        &have_expected, &timestamp_epoch, &expected_timestamp));
    assert(!metadata_rx_epoch_commit_timestamp(
        true, true, 7, 1000, true, true, 7, 1001, 2048,
        &have_expected, &timestamp_epoch, &expected_timestamp));
    assert(!have_expected && timestamp_epoch == 3 && expected_timestamp == 44);
    test_async_admission_race();

    /* Epoch zero is a valid tagged identity, distinct from legacy headers. */
    set_tag(header, 0);
    assert(metadata_get_rx_epoch_id(header, &epoch_id));
    assert(epoch_id == 0);
    assert(metadata_rx_epoch_matches(header, 0));
    assert(!metadata_rx_epoch_matches(header, 1));

    /* Matching and stale packet identities are decided per whole message. */
    set_tag(header, 0x2a);
    assert(metadata_get_rx_epoch_id(header, &epoch_id));
    assert(epoch_id == 0x2a);
    assert(metadata_rx_epoch_matches(header, 0x2a));
    assert(!metadata_rx_epoch_matches(header, 0x29));

    set_tag(header, 0xff);
    assert(metadata_rx_epoch_matches(header, 0xff));

    /* Legacy sample-META and arbitrary PACKET_META prefixes fail closed if
     * an epoch filter is enabled; parser caller owns the format distinction. */
    memset(header, 0, sizeof(header));
    assert(!metadata_get_rx_epoch_id(header, &epoch_id));
    assert(!metadata_rx_epoch_matches(header, 0));
    header[METADATA_RESV_OFFSET] = 0x21;
    header[METADATA_RESV_OFFSET + 1] = 0x43;
    header[METADATA_RESV_OFFSET + 2] = 0x34;
    header[METADATA_RESV_OFFSET + 3] = 0x12;
    assert(!metadata_rx_epoch_matches(header, 0x21));

    /* These are the exact header dispositions consumed by sync RX. */
    assert(metadata_rx_epoch_disposition(true, true, false,
               1200, 1200, 1000, true, false) ==
           METADATA_RX_EPOCH_DROP_MESSAGE);
    assert(metadata_rx_epoch_disposition(true, true, false,
               1200, 1200, 1000, true, true) ==
           METADATA_RX_EPOCH_RETURN_VALID_PREFIX);
    assert(metadata_rx_epoch_disposition(true, true, true,
               900, 900, 1000, true, false) ==
           METADATA_RX_EPOCH_SKIP_TIMESTAMP_PREFIX);
    assert(metadata_rx_epoch_disposition(true, true, true,
               1000, 1000, 1000, true, false) ==
           METADATA_RX_EPOCH_ACCEPT);
    assert(metadata_rx_epoch_disposition(true, true, true,
               1200, 1199, 1000, true, false) ==
           METADATA_RX_EPOCH_DISCONTINUITY);
    /* With a valid prefix already copied, a mixed-epoch next message ends
     * this read with an overrun indication; caller retries and drops it. */
    assert(metadata_rx_epoch_disposition(true, true, false,
               1200, 1200, 1000, true, true) ==
           METADATA_RX_EPOCH_RETURN_VALID_PREFIX);
    /* Legacy/unfiltered stream behavior does not drop on absent epoch tags. */
    assert(metadata_rx_epoch_disposition(true, false, false,
               1000, 1000, 1000, true, false) ==
           METADATA_RX_EPOCH_ACCEPT);

    return 0;
}
