# TX async startup RFIC-certificate race

## Finding

The bladeRF2 TX START path returns after the FPGA transport epoch is acknowledged. AD9361 TX RFPLL, ENSM, BBPLL, and calibration-idle evidence arrives separately from the periodic RFIC monitor. Async TX backends ask `tx_async_buffer_valid()` before submitting a callback-produced buffer; before this fix that check treated the expected interval between transport ACK and the first RFIC observation as a permanent stream fault. An application starting async TX immediately after `bladerf_enable_module(TX)` could therefore have the stream shut down before the monitor's first (up to 100 ms later) status observation.

## Change

Added an explicit policy predicate that identifies the single waitable state: TX link certified, RFIC state not yet observed, and every other admission condition healthy. TX buffer admission now waits on the existing RX/TX epoch condition variable for a positive RFIC event, with a one-second watchdog. The monitor appends the RFIC certificate to durable event history before releasing the lock that makes the positive admission state observable, then wakes stream workers; revocation also wakes them. Fault, transport, abort, pending retune, invalid frequency model, and timeout never admit IQ.

Added source-level policy cases for the waitable state and each disqualifying fault condition. They were not executed under the active requirement to finish the RX+TX source audit before any build or test.

## Disposition

Source-level fix only. `git diff --check` is the only validation planned before the audit gate opens. No build, test, simulation, synthesis, firmware load, or hardware operation was run. After source closure, verify immediate post-enable async TX with an event trace and timeout/fault injection, then qualify on the exact image.
