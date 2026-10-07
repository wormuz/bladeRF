# Public RX scheduled-retune cancellation obeys the epoch contract

The board path blocked ordinary RX scheduled-retune requests while an
event-driven epoch contract was active, but public
`bladerf_cancel_scheduled_retunes()` still cleared the NIOS queue. During a
transition this could cancel its accepted fastlock recall. LO readback alone
does not prove that the requested quick-tune RF port/profile was applied, so
the previous guard left a route to certify an epoch after the operation that
selected that route had been removed.

The public RX cancellation path now uses a dedicated policy guard and returns
`BLADERF_ERR_WOULD_BLOCK` while a transition is pending or RX epoch protection
is active. TX cancellation remains independent. The transaction-begin path
retains an explicit scoped exception to clear older queued RX recalls before
arming its own epoch; the flag is protected by the existing device lock and is
cleared immediately after the internal call.

The RX transition policy test covers active transaction, async epoch contract,
sync epoch filter, TX cancellation, ordinary unprotected RX, and the internal
begin-path exception. The production shared library builds and native sync
epoch traversal passes. Live NIOS queue race qualification remains open: the
xA4 currently enumerates on USB but open times out reading FPGA version. No
timeout or sample discard is used as a validity condition.
