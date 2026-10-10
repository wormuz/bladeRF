# RX2 B/C DC-offset packed-register handling

The AD9361 register definitions show that RX2 I's two low offset bits share
bits `[7:6]` of `REG_RX2_INPUT_BC_OFFSETS`, while RX2 Q's six high bits occupy
`[5:0]` of that same byte. RX2 Q's four low bits occupy `[7:4]` of
`REG_INPUT_BC_OFFSETS_1`, whose `[3:0]` bits hold RX1 I's high offset bits.

The bladeRF2 correction path previously decoded RX2 I from the wrong two bits,
decoded RX2 Q without masking off RX2 I, wrote RX2 I into RX2 Q's bits, wrote
RX2 Q over RX2 I's bits, and replaced the shared low nibble when writing RX2 Q.
This can corrupt both channel DC corrections and leave unwanted baseband DC
content.

The RX2 I/Q read and write packing now follows the AD9361 field masks and
preserves the adjacent packed fields. The bladeRF2 correction regression
applies distinct RX I/Q corrections on RX1 and RX2 in B/C mode and verifies
they remain independent, including an RX2 I value with nonzero low bits. It
also checks that writing RX2 Q leaves RX1 I in the shared B/C offset byte
unchanged, then restores both channels' prior values and RF ports.

Source evidence: the field definitions in the vendored ADI no-OS
`ad9361.h` (`RX2_INPUT_BC_I_DC_OFFSET`, `RX2_INPUT_BC_Q_DC_OFFSET_MSB`,
`RX2_INPUT_BC_Q_DC_OFFSET_LSB`, and `RX1_INPUT_BC_I_DC_OFFSET_MSB`). No build,
test, synthesis, or hardware operation was run because the complete RX+TX
source audit remains open.
