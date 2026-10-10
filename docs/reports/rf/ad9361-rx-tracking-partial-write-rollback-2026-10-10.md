# AD9361 RX tracking partial-write rollback

## Finding

The RX RFDC, baseband DC, and quadrature tracking setters update one cached enable bit and then program the shared tracking register set. That set spans five SPI writes. If a later write failed, the setter restored only the software bit; all earlier hardware writes remained active until a later API call happened to replay the set. The API returned an error, but the running receiver could retain a mixed DC/IQ correction configuration in the meantime.

## Change

Each setter now restores its previous cached enable state and replays the complete previous tracking configuration after a programming error. If replay also fails, the driver logs that separately while returning the original programming error. Extended the existing SPI fault-injection regression source to fail the final tracking-register write once and verify all five register values return to their prior image before a same-state retry.

## RF relevance and limits

This closes a software-created partial-state path that can affect DC offset and quadrature correction behavior. It does not prove that any reported shielding issue or observed spur was caused by this path, and a persistent SPI failure can still prevent rollback. Correlating a specific RF spur requires captures with the event and register trace.

## Disposition

Source-only change. `git diff --check` only; regression source was not built or run under the active RX+TX source-audit gate. No build, simulation, synthesis, firmware load, or hardware operation was performed.
