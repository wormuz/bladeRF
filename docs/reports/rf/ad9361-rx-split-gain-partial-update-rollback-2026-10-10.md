# AD9361 RX split-gain partial-update rollback

## Finding

In split gain-table mode the RX manual-gain setter writes the LMT index, LPF gain, and optionally digital gain through separate registers. A later SPI failure returned an error after earlier stages had already changed, leaving the channel at a mixed gain configuration.

## Change

`ad9361_set_rx_gain()` now snapshots the selected gain register image before programming. The split/full table helpers mark when a write has been attempted; on failure the setter replays the original register values and logs rollback failures while preserving the primary error. The existing host-side AD9361 SPI fault-injection source now injects an ambiguous second-stage failure and verifies all split-gain registers are restored.

## RF relevance and limits

This closes a software partial-gain state path that can affect receive level and spur/image measurements. It does not show that shielding complaints were caused by gain programming. Persistent SPI failure can still defeat rollback, and the regression has not been run under the active build gate.

## Disposition

Source-only change; `git diff --check` only. No build, test, simulation, synthesis, firmware load, or hardware operation was run.
