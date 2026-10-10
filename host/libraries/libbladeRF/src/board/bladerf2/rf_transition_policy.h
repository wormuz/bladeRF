/* Event requirement normalization for the bladeRF 2 RX transition API. */
#ifndef BLADERF2_RF_TRANSITION_POLICY_H_
#define BLADERF2_RF_TRANSITION_POLICY_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <libbladeRF.h>

static inline bool bladerf2_rfic_frequency_model_valid(
    bool reference_clock_known, bool rfic_clock_tree_known)
{
    return reference_clock_known && rfic_clock_tree_known;
}

static inline bool bladerf2_clock_select_valid(bladerf_clock_select select)
{
    return select == CLOCK_SELECT_ONBOARD ||
           select == CLOCK_SELECT_EXTERNAL;
}

static inline bool bladerf2_channel_valid(bladerf_channel channel)
{
    return channel == BLADERF_CHANNEL_RX(0) ||
           channel == BLADERF_CHANNEL_RX(1) ||
           channel == BLADERF_CHANNEL_TX(0) ||
           channel == BLADERF_CHANNEL_TX(1);
}

static inline bool bladerf2_rx_channel_valid(bladerf_channel channel)
{
    return channel == BLADERF_CHANNEL_RX(0) ||
           channel == BLADERF_CHANNEL_RX(1);
}

struct bladerf2_rx_invalidation_policy {
    bool reserve_setter;
    bool abort_rx_epoch;
};

/* TX startup without an RX direction must not issue an unrelated RX ABORT,
 * but it still owns the setter reservation until the shared link START ends.
 * Otherwise an RX setter can race into the gap between the no-RX check and
 * TX module enable. */
static inline struct bladerf2_rx_invalidation_policy
bladerf2_rx_invalidation_policy(bladerf_channel channel,
                                bool rx_direction_active)
{
    struct bladerf2_rx_invalidation_policy policy = {
        .reserve_setter = true,
        .abort_rx_epoch = !BLADERF_CHANNEL_IS_TX(channel) ||
                          rx_direction_active,
    };
    return policy;
}

#define BLADERF2_RF_LINK_STATUS_VERSION_MASK 0xf0000000u
#define BLADERF2_RF_LINK_STATUS_VERSION_2    0x20000000u
#define BLADERF2_RF_LINK_STATUS_RX_PREVIOUS_FAULT (1u << 3)
#define BLADERF2_RF_LINK_STATUS_TX_PREVIOUS_FAULT (1u << 6)
#define BLADERF2_RF_LINK_STATUS_RX_LINK_ACTIVE   (1u << 16)
#define BLADERF2_RF_LINK_STATUS_TX_LINK_ACTIVE   (1u << 0)
#define BLADERF2_RF_LINK_STATUS_RX_EPOCH_CURRENT (1u << 8)
#define BLADERF2_RF_LINK_STATUS_TX_EPOCH_CURRENT (1u << 9)
#define BLADERF2_RF_LINK_STATUS_TX_EPOCH_VALID   (1u << 12)
#define BLADERF2_AD9361_VCO_LOCK_BIT 0x02u
#define BLADERF2_AD9361_BBPLL_LOCK_BIT 0x80u
#define BLADERF2_AD9361_CALIBRATION_BUSY_MASK 0xffu
#define BLADERF2_AD9361_ENSM_STATE_MASK 0x0fu
#define BLADERF2_AD9361_ENSM_STATE_TX 0x06u
#define BLADERF2_AD9361_ENSM_STATE_FDD 0x0au

/* A completion batch is usable only while every reported success still has
 * a retained request. Counter wrap is intentional unsigned sequence math. */
static inline bool bladerf2_retune_completion_batch_valid(
    uint32_t completed, uint32_t pending, uint32_t ring_depth)
{
    return completed <= ring_depth && completed <= pending;
}

static inline bool bladerf2_retune_completion_matches(
    uint64_t requested_timestamp, uint64_t requested_frequency_hz,
    uint16_t requested_profile, uint64_t completed_timestamp,
    uint64_t readback_frequency_hz, uint16_t completed_profile)
{
    return requested_timestamp == completed_timestamp &&
           requested_frequency_hz == readback_frequency_hz &&
           requested_profile == completed_profile;
}

/* The two 32-bit PIOs are read in separate NIOS transactions. The high word
 * must match around the low-word read before the host can join the snapshot. */
static inline bool bladerf2_loss_counter_snapshot(
    uint32_t high_before, uint32_t low, uint32_t high_after,
    uint64_t *count)
{
    if (high_before != high_after || count == NULL) {
        return false;
    }
    *count = ((uint64_t)high_after << 32) | low;
    return true;
}

enum bladerf2_tx_rfic_observation {
    BLADERF2_TX_RFIC_OBSERVATION_UNAVAILABLE = 0,
    BLADERF2_TX_RFIC_OBSERVATION_INVALID,
    BLADERF2_TX_RFIC_OBSERVATION_VALID,
};

static inline bool bladerf2_rx_fir_configuration_valid(
    bool host_phy_available, bool fir_config_valid, bool rx2tx2,
    uint8_t rx_coefficients_valid_mask, uint8_t tx_coefficients_valid_mask)
{
    const uint8_t required_mask = rx2tx2 ? 0x03 : 0x01;
    return host_phy_available && fir_config_valid &&
           (rx_coefficients_valid_mask & required_mask) == required_mask &&
           (tx_coefficients_valid_mask & required_mask) == required_mask;
}

/* A TX FPGA link ACK does not prove the analog RFIC path is operational. */
static inline enum bladerf2_tx_rfic_observation
bladerf2_tx_rfic_observation(bool status_read_succeeded,
                             bool fir_config_valid,
                             uint8_t tx_pll_status,
                             uint8_t ensm_status, uint8_t bbpll_status,
                             uint8_t calibration_status)
{
    if (!status_read_succeeded) {
        return BLADERF2_TX_RFIC_OBSERVATION_UNAVAILABLE;
    }
    if (!fir_config_valid) {
        return BLADERF2_TX_RFIC_OBSERVATION_INVALID;
    }
    const uint8_t ensm = ensm_status & BLADERF2_AD9361_ENSM_STATE_MASK;
    return (tx_pll_status & BLADERF2_AD9361_VCO_LOCK_BIT) != 0 &&
           (bbpll_status & BLADERF2_AD9361_BBPLL_LOCK_BIT) != 0 &&
           (calibration_status &
            BLADERF2_AD9361_CALIBRATION_BUSY_MASK) == 0 &&
           (ensm == BLADERF2_AD9361_ENSM_STATE_TX ||
            ensm == BLADERF2_AD9361_ENSM_STATE_FDD)
        ? BLADERF2_TX_RFIC_OBSERVATION_VALID
        : BLADERF2_TX_RFIC_OBSERVATION_INVALID;
}

/* The TX transport certificate is tied to the FPGA generation that the host
 * acknowledged. A readable status word with a stale/missing acknowledgement
 * is a lost certificate, even when the sticky fault bit has not asserted. */
static inline bool bladerf2_rf_link_tx_epoch_valid(uint32_t status,
                                                   uint8_t expected_epoch)
{
    return (status & BLADERF2_RF_LINK_STATUS_VERSION_MASK) ==
               BLADERF2_RF_LINK_STATUS_VERSION_2 &&
           (status & BLADERF2_RF_LINK_STATUS_TX_EPOCH_CURRENT) != 0 &&
           (status & BLADERF2_RF_LINK_STATUS_TX_EPOCH_VALID) != 0 &&
           (status & BLADERF2_RF_LINK_STATUS_TX_LINK_ACTIVE) != 0 &&
           (uint8_t)(status >> 20) == expected_epoch;
}

/* A confirmed START may clear faults that predate the transaction, but a
 * transport failure observed after its baseline must remain the newest state. */
static inline bool bladerf2_tx_epoch_generation_unchanged(
    uint64_t failure_generation, uint64_t transaction_baseline)
{
    return failure_generation == transaction_baseline;
}

/* Preserve the transition handle that scoped an RX epoch on invalid-data
 * notifications. The epoch is shared by both RX lanes, but its requested
 * control channel is useful provenance for RX1/RX2 consumers. */
static inline uint32_t bladerf2_rx_transition_channel_event_flags(
    bladerf_channel channel, bool channel_valid)
{
    if (!channel_valid ||
        (channel != BLADERF_CHANNEL_RX(0) &&
         channel != BLADERF_CHANNEL_RX(1))) {
        return 0;
    }

    return BLADERF_RF_EVENT_F_TRANSITION_CHANNEL_VALID |
           (channel == BLADERF_CHANNEL_RX(1)
                ? BLADERF_RF_EVENT_F_TRANSITION_RX2 : 0);
}

/* Reasons that invalidate the shared RX epoch must report provenance from
 * the epoch that was certified, rather than the lane used internally by a
 * setter. The RX LO is device-wide, and RX FIR programming uses a dual-lane
 * AD9361 configuration (rx=3). */
static inline bool bladerf2_rx_invalidation_reason_is_shared(uint32_t reason)
{
    const uint32_t shared_reasons = BLADERF_RF_INVALIDATE_FREQUENCY |
                                    BLADERF_RF_INVALIDATE_RFIC_REG |
                                    BLADERF_RF_INVALIDATE_RX_FIR |
                                    BLADERF_RF_INVALIDATE_TX_FIR |
                                    BLADERF_RF_INVALIDATE_CLOCK |
                                    BLADERF_RF_INVALIDATE_MODULE |
                                    BLADERF_RF_INVALIDATE_SAMPLE_RATE |
                                    BLADERF_RF_INVALIDATE_BANDWIDTH |
                                    BLADERF_RF_INVALIDATE_LOOPBACK |
                                    BLADERF_RF_INVALIDATE_RX_MUX |
                                    BLADERF_RF_INVALIDATE_STREAM_CONFIG |
                                    BLADERF_RF_INVALIDATE_TUNING_MODE |
                                    BLADERF_RF_INVALIDATE_DEVICE_RESET |
                                    BLADERF_RF_INVALIDATE_FPGA_RELOAD |
                                    BLADERF_RF_INVALIDATE_WISHBONE |
                                    BLADERF_RF_INVALIDATE_CONFIG_GPIO |
                                    BLADERF_RF_INVALIDATE_FEATURE |
                                    BLADERF_RF_INVALIDATE_BOOTLOADER;

    return (reason & shared_reasons) != 0;
}

/* The durable epoch certificate is also the snapshot used when the first
 * host META packet is admitted. Preserve channel identity in that snapshot,
 * not only in the separately published RX_EPOCH_VALID notification. */
static inline uint32_t bladerf2_rx_epoch_admission_event_flags(
    bladerf_channel channel, bool channel_valid)
{
    return BLADERF_RF_EVENT_F_FPGA_TIMESTAMP_VALID |
           bladerf2_rx_transition_channel_event_flags(channel, channel_valid);
}

static inline struct bladerf_rf_event
bladerf2_rx_invalidation_channel_event(
    const struct bladerf_rf_event *invalidation, uint32_t channel_flags)
{
    struct bladerf_rf_event event = {0};
    const uint32_t channel_mask =
        BLADERF_RF_EVENT_F_TRANSITION_CHANNEL_VALID |
        BLADERF_RF_EVENT_F_TRANSITION_RX2;

    if (invalidation == NULL) {
        return event;
    }

    event = *invalidation;
    event.event_type = BLADERF_RF_EVT_RX_INVALIDATION_CHANNEL;
    event.flags = channel_flags & channel_mask;
    event.rfic_status = 0;
    event.error_code = 0;
    return event;
}

/* A hardware observation is timely only if it completed strictly before the
 * caller's deadline. Checking only before a blocking SPI/NIOS read can accept
 * a late completion and turn an expired wait into success. */
static inline bool bladerf2_rf_transition_deadline_expired(
    uint64_t now_ns, uint64_t deadline_ns)
{
    return now_ns >= deadline_ns;
}

/* A set summary fault bit still invalidates IQ when its detailed-cause PIO
 * cannot be read. Preserve that read failure in the invalidation event; a
 * successful read with no cause bits is also an inconsistent status snapshot. */
static inline int bladerf2_rx_fault_causes_read_error(
    bool read_attempted, bool causes_valid, int read_status)
{
    if (!read_attempted || causes_valid) {
        return 0;
    }

    return read_status != 0 ? read_status : BLADERF_ERR_UNEXPECTED;
}

static inline bool bladerf2_rf_transition_normalize_requirements(
    uint32_t requested, bool require_rx_data_valid, uint32_t *effective)
{
    const uint32_t supported = BLADERF_RF_REQUIRE_PLL_LOCKED |
                              BLADERF_RF_REQUIRE_ENSM_RX |
                              BLADERF_RF_REQUIRE_DATAPATH_ARMED |
                              BLADERF_RF_REQUIRE_EPOCH_VALID |
                              BLADERF_RF_REQUIRE_BBPLL_LOCKED |
                              BLADERF_RF_REQUIRE_FIRST_HOST_DATA |
                              BLADERF_RF_REQUIRE_RX_X2_HOST_DATA |
                              BLADERF_RF_REQUIRE_RX_RFDC_CAL_DONE;

    if (effective == NULL || (requested & ~supported) != 0) {
        return false;
    }

    *effective = requested;
    if (require_rx_data_valid ||
        (requested & BLADERF_RF_REQUIRE_RX_RFDC_CAL_DONE) != 0 ||
        (requested & BLADERF_RF_REQUIRE_FIRST_HOST_DATA) != 0 ||
        (requested & BLADERF_RF_REQUIRE_RX_X2_HOST_DATA) != 0 ||
        (requested & BLADERF_RF_REQUIRE_DATAPATH_ARMED) != 0) {
        *effective |= BLADERF_RF_REQUIRE_EPOCH_VALID;
    }

    if (*effective == 0) {
        return false;
    }

    if (*effective & BLADERF_RF_REQUIRE_RX_X2_HOST_DATA) {
        *effective |= BLADERF_RF_REQUIRE_FIRST_HOST_DATA;
    }

    if ((*effective & BLADERF_RF_REQUIRE_EPOCH_VALID) != 0) {
        *effective |= BLADERF_RF_REQUIRE_PLL_LOCKED |
                      BLADERF_RF_REQUIRE_ENSM_RX |
                      BLADERF_RF_REQUIRE_BBPLL_LOCKED;
    }
    return true;
}

/* The event-driven transition wait reads RFIC status through the host-owned
 * AD9361 PHY object. FPGA tuning mode removes that object and assigns RFIC
 * ownership to NIOS, so the current transition API must reject that mode
 * before beginning rather than dereference a NULL PHY. */
static inline bool bladerf2_rf_transition_mode_supported(
    bladerf_tuning_mode mode)
{
    return mode == BLADERF_TUNING_MODE_HOST;
}

static inline bool bladerf2_rf_event_is_transition_terminal(
    bladerf_rf_event_type type)
{
    return type == BLADERF_RF_EVT_RX_EPOCH_VALID ||
           type == BLADERF_RF_EVT_CONTROL_PLANE_CONFIRMED ||
           type == BLADERF_RF_EVT_RX_DATAPATH_ARMED ||
           type == BLADERF_RF_EVT_RX_EPOCH_ABORT_FAILED ||
           type == BLADERF_RF_EVT_ERROR;
}

/* Runtime RX notifications share the history ring with transition events.
 * Host-data lifecycle events carry the epoch's transaction ID, but happen
 * after transition completion and must not replace the event returned by
 * rx_transition_wait(). If ring pressure has also evicted the terminal
 * transition event, fail closed instead of returning CONFIG_ACCEPTED as
 * successful completion. */
static inline bool bladerf2_rf_event_latest_transition_result_for_transaction(
    const struct bladerf_rf_event *events, uint32_t capacity, uint32_t head,
    uint32_t count, uint32_t transaction_id,
    struct bladerf_rf_event *result)
{
    if (events == NULL || result == NULL || capacity == 0 ||
        head >= capacity || count > capacity || transaction_id == 0) {
        return false;
    }

    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t slot = (head + capacity - 1u - i) % capacity;
        if (events[slot].transaction_id == transaction_id) {
            if (!bladerf2_rf_event_is_transition_terminal(
                    events[slot].event_type)) {
                continue;
            }
            *result = events[slot];
            return true;
        }
    }

    return false;
}

/* Public scheduling revokes the current RX certificate before reaching this
 * policy. A queued recall is safe only outside a transition transaction; its
 * data remains invalid until an explicit transition establishes a new epoch. */
static inline bool bladerf2_rx_scheduled_retune_blocked(
    bool is_rx, bool transition_pending, bool transaction_fastlock)
{
    return is_rx && !transaction_fastlock && transition_pending;
}

/* Clearing the RX fastlock queue can cancel the retune owned by an active
 * epoch transaction just as surely as adding an unreported retune can bypass
 * its RF-path contract. Only rx_transition_begin() may clear it internally. */
static inline bool bladerf2_rx_scheduled_retune_cancel_blocked(
    bool is_rx, bool transition_pending, bool epoch_contract_enabled,
    bool sync_epoch_filter_enabled, bool transition_cancel)
{
    return is_rx && !transition_cancel &&
           (transition_pending || epoch_contract_enabled ||
            sync_epoch_filter_enabled);
}

/* Epoch-required transitions cannot safely hand data to a running async RX
 * consumer that has no packet-level epoch identity. */
static inline bool bladerf2_rx_epoch_transition_blocked_by_async_format(
    bool require_epoch, unsigned int epochless_async_stream_count)
{
    return require_epoch && epochless_async_stream_count != 0;
}

/* Runtime status reads happen outside the async admission lock. Match their
 * result to both the short FPGA epoch tag and the host transaction that was
 * certified when the read began, so an old observation cannot revoke a later
 * epoch after the 8-bit FPGA epoch counter wraps. */
static inline bool bladerf2_rx_fault_observation_is_current(
    uint32_t observed_transaction_id, uint32_t current_transaction_id,
    uint8_t observed_epoch_id, uint8_t current_epoch_id)
{
    return observed_transaction_id != 0 &&
           observed_transaction_id == current_transaction_id &&
           observed_epoch_id == current_epoch_id;
}

/* Transaction ID zero is reserved by the public history APIs. Keep it
 * reserved when the 32-bit sequence wraps during long-running sweeps. */
static inline uint32_t bladerf2_rx_transition_next_transaction_id(
    uint32_t previous_id)
{
    return previous_id == UINT32_MAX ? 1u : previous_id + 1u;
}

/* A cursor beyond the current tail cannot be treated as an empty, complete
 * history: doing so would silently hide every event until the ring catches
 * up to that fabricated sequence. */
static inline bool bladerf2_rf_event_cursor_is_valid(uint64_t after_sequence,
                                                     uint64_t current_sequence)
{
    return after_sequence <= current_sequence;
}

/* Active directions ACK a shared START as one generation. Accept it only
 * when the generation advanced, every required direction consumed it and is
 * active, and none of their sticky fault bits is set. */
static inline bool bladerf2_rf_link_epoch_ack_valid(
    uint32_t entry_status, uint32_t observed_status,
    uint32_t version_mask, uint32_t version_value,
    uint32_t epoch_count_mask, uint32_t applied_bit,
    uint32_t active_bit, uint32_t fault_bit)
{
    return (entry_status & version_mask) == version_value &&
           (observed_status & version_mask) == version_value &&
           (entry_status & epoch_count_mask) !=
               (observed_status & epoch_count_mask) &&
           (observed_status & applied_bit) == applied_bit &&
           (observed_status & active_bit) == active_bit &&
           (observed_status & fault_bit) == 0;
}

/* If START was attempted, its response may have been lost after NIOS applied
 * the toggle. Once rollback leaves both RF directions down, issue one STOP
 * to retire either the accepted or ambiguous shared link epoch. */
static inline bool bladerf2_rf_link_stop_required(
    bool start_attempted, bool rx_direction_active,
    bool tx_direction_active)
{
    return start_attempted && !rx_direction_active && !tx_direction_active;
}

/* FPGA_OP is ignorable only when the preceding status read positively
 * identified gateware without protocol-v2 support (or the status target
 * itself is explicitly unsupported). A failed status read is not evidence
 * of legacy gateware and must not mask a STOP failure. */
static inline bool bladerf2_rf_link_stop_failure_actionable(
    bool legacy_unsupported_confirmed, int stop_status,
    int legacy_unsupported_status)
{
    return stop_status != 0 &&
           !(legacy_unsupported_confirmed &&
             stop_status == legacy_unsupported_status);
}

/* Previous-epoch sticky flags and the direction ACK cross independently into
 * sys_clock. Do not let an ACK sampled on the first read outrun the sticky
 * snapshot's CDC path; require the same snapshot for the same generation on
 * consecutive status observations before publishing the new link as valid. */
static inline bool bladerf2_rf_link_previous_fault_snapshot_stable(
    bool previous_observation_valid, uint8_t previous_epoch_id,
    uint32_t previous_fault_flags, uint8_t current_epoch_id,
    uint32_t current_fault_flags)
{
    return previous_observation_valid &&
           previous_epoch_id == current_epoch_id &&
           previous_fault_flags == current_fault_flags;
}

static inline uint8_t bladerf2_rf_link_epoch_id(uint32_t link_status,
                                                uint8_t fallback_epoch_id)
{
    return (link_status & BLADERF2_RF_LINK_STATUS_VERSION_MASK) ==
               BLADERF2_RF_LINK_STATUS_VERSION_2
        ? (uint8_t)(link_status >> 20) : fallback_epoch_id;
}

static inline bool bladerf2_rx_first_host_data_admissible(
    bool required, bool already_reported, uint64_t deadline_ns,
    uint64_t now_ns)
{
    return !required || already_reported ||
           (deadline_ns != 0 && now_ns != 0 && now_ns < deadline_ns);
}

static inline bool bladerf2_rx_first_host_data_before_deadline(
    uint64_t event_ns, uint64_t deadline_ns)
{
    return event_ns != 0 && deadline_ns != 0 && event_ns < deadline_ns;
}

static inline bool bladerf2_rx_layout_satisfies_transition(
    bool require_rx_x2, bladerf_channel_layout layout)
{
    return !require_rx_x2 || layout == BLADERF_RX_X2;
}

/* RX_X1 has no channel identity in its layout enum, so its host-data event
 * may certify only the single RFIC lane selected by the transition. RX_X2
 * requires both channel enables. `mask` uses bit 0 = RX1, bit 1 = RX2. */
static inline bool bladerf2_rx_layout_matches_channel_mask(
    bladerf_channel_layout layout, bladerf_channel transition_channel,
    bool mask_valid, uint8_t mask)
{
    if (!mask_valid) {
        return false;
    }
    if (layout == BLADERF_RX_X2) {
        return mask == 0x3;
    }
    if (layout != BLADERF_RX_X1 || BLADERF_CHANNEL_IS_TX(transition_channel) ||
        ((unsigned int)transition_channel >> 1) > 1) {
        return false;
    }
    return mask == (uint8_t)(1u <<
                             ((unsigned int)transition_channel >> 1));
}

enum bladerf2_rx_channel_mask_observation {
    BLADERF2_RX_CHANNEL_MASK_UNAVAILABLE = 0,
    BLADERF2_RX_CHANNEL_MASK_MATCH,
    BLADERF2_RX_CHANNEL_MASK_CHANGED,
};

/* A certified RX epoch depends on the channel set remaining observable and
 * unchanged. Keep this decision in the shared policy layer so the runtime
 * monitor and native tests exercise the same fail-closed classification. */
static inline enum bladerf2_rx_channel_mask_observation
bladerf2_rx_channel_mask_observation(bool status_read_succeeded,
                                     bool expected_mask_valid,
                                     uint8_t expected_mask,
                                     uint8_t observed_mask)
{
    if (!status_read_succeeded || !expected_mask_valid) {
        return BLADERF2_RX_CHANNEL_MASK_UNAVAILABLE;
    }
    return expected_mask == observed_mask
        ? BLADERF2_RX_CHANNEL_MASK_MATCH
        : BLADERF2_RX_CHANNEL_MASK_CHANGED;
}

static inline bool bladerf2_rx_x1_consumer_blocks_x2_transition(
    bool require_rx_x2, unsigned int active_async_rx_x1,
    bool sync_rx_x1_configured)
{
    return require_rx_x2 &&
           (active_async_rx_x1 != 0 || sync_rx_x1_configured);
}

/* TX samples stay fenced until the transport epoch and AD9361 RFIC state are
 * certified and both the stream and hardware gate are recoverable. Keep
 * direct abort-pending and transport-fault latches in the rule rather than
 * relying on callers to mirror them into the aggregate stream-fault bit. */
static inline bool bladerf2_tx_admission_allowed(
    bool link_certified, bool rfic_state_certified,
    bool stream_faulted, bool transport_faulted,
    bool gate_abort_pending,
    unsigned int pending_retunes, bool retune_completion_faulted,
    bool frequency_model_valid)
{
    return link_certified && rfic_state_certified &&
           !stream_faulted && !transport_faulted &&
           !gate_abort_pending &&
           pending_retunes == 0 && !retune_completion_faulted &&
           frequency_model_valid;
}

/* The only retryable startup state is a positively acknowledged TX transport
 * epoch whose RFIC observation has not arrived yet. Any other missing
 * admission condition is a fault/configuration state and must fail closed. */
static inline bool bladerf2_tx_rfic_certificate_pending(
    bool link_certified, bool rfic_state_certified,
    bool stream_faulted, bool transport_faulted,
    bool gate_abort_pending, unsigned int pending_retunes,
    bool retune_completion_faulted, bool frequency_model_valid)
{
    return link_certified && !rfic_state_certified &&
           !stream_faulted && !transport_faulted && !gate_abort_pending &&
           pending_retunes == 0 && !retune_completion_faulted &&
           frequency_model_valid;
}

#endif
