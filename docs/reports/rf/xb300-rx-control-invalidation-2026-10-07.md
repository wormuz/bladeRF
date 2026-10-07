# XB-300 RX-affecting controls now revoke the certified epoch

The public XB-300 TRX selector and LNA control changed the external RX
antenna route or receive gain without revoking an already certified RX epoch.
The adjacent PA and auxiliary PA controls are TX-only and must not interrupt
RX.

`bladerf_xb300_set_trx()` now invalidates RX with the existing `RF_PORT`
reason before changing the shared TRX antenna route. The LNA branch of
`bladerf_xb300_set_amplifier_enable()` uses `GAIN`; PA and PA_AUX retain their
TX-only behavior. Both RX-affecting setters release the reservation through
the normal completion path, including downstream errors. A rejected fence
prevents the GPIO write.

The native mock-backend regression verifies successful TRX/LNA invalidation,
reason and channel provenance, PA/PA_AUX non-invalidation, and fail-closed LNA
GPIO behavior. Production libbladeRF and the same native RX setter test pass.
This closes the second expansion-board seam found after XB-200; it does not
qualify the external board electrically. Live hardware remains unavailable
because xA4 open times out while reading the FPGA version. No timeout or fixed
discard establishes IQ validity.
