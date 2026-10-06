/* Focused contract test for RX sample-META epoch tags. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "host_config.h"
#include "streaming/metadata.h"

static void set_tag(uint8_t header[METADATA_HEADER_SIZE], uint8_t epoch_id)
{
    uint32_t tag = HOST_TO_LE32(METADATA_RX_EPOCH_TAG_VALID | epoch_id);
    memcpy(header + METADATA_RESV_OFFSET, &tag, sizeof(tag));
}

int main(void)
{
    uint8_t header[METADATA_HEADER_SIZE] = {0};
    uint8_t epoch_id = 0xff;

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

    return 0;
}
