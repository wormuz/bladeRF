# Raw expansion GPIO mutations revoke RX validity

The public expansion GPIO write APIs passed arbitrary pin changes directly to
the backend. Such pins can control an external RF switch or other receive-path
hardware, but libbladeRF has no ownership or pin-purpose metadata with which to
prove a write is harmless. Direction changes can also expose a driven RF
control line, so these APIs had the same validity gap as the named XB-200 and
XB-300 setters.

Full and nonempty masked data writes, plus full and nonempty masked direction
writes, now fence the shared RX epoch with `RF_PORT` before entering the
backend and release the reservation through the existing completion callback.
A rejected fence prevents the pin mutation. A zero-mask write remains a no-op
and does not revoke the epoch. This policy is intentionally conservative:
callers using generic GPIO for indicators may also invalidate RX because the
library cannot infer signal purpose. Dedicated non-RF GPIO APIs can be
considered separately if that distinction is needed.

The native mock-backend regression covers each public write form, channel and
reason provenance, zero-mask behavior, and fail-closed fencing. Production
libbladeRF build and the targeted regression pass. No hardware GPIO route was
electrically qualified, and no arbitrary timeout or sample discard is used to
restore validity.
