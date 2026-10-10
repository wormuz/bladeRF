# RX B/C-band gain correction register alias

The bladeRF2 correction lookup selected the AD9361 phase-correction register
for RX gain correction in the B/C RF band on both RX channels. Consequently,
setting gain correction overwrote phase correction, and get-correction read
the same phase register as gain. The ordinary same-field set/get test could not
detect this alias.

The RX1 and RX2 B/C gain entries now point to their corresponding
`RX*_INPUT_BC_GAIN_CORR` registers. The bladeRF2 correction control regression
now selects `B_BALANCED`, saves the old gain/phase values, applies distinct
values, checks both values remain independent on RX1 and RX2, and restores the
original values and RF port.

Source inspection only: no build, test, synthesis, or hardware operation was
run because the complete RX+TX source audit is still open. Hardware execution
of the correction regression remains part of release qualification.
