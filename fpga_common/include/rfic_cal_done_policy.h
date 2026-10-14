/* Copyright (c) 2026 Nuand LLC */

#ifndef FPGA_COMMON_RFIC_CAL_DONE_POLICY_H_
#define FPGA_COMMON_RFIC_CAL_DONE_POLICY_H_

#include <stdbool.h>
#include <stdint.h>

/* AD9361 REG_CALIBRATION_CTRL bits used by the xA4 no-OS driver. */
#define RFIC_AD9361_RX_BB_TUNE_CAL   (1u << 7)
#define RFIC_AD9361_TX_BB_TUNE_CAL   (1u << 6)
#define RFIC_AD9361_RX_QUAD_CAL      (1u << 5)
#define RFIC_AD9361_TX_QUAD_CAL      (1u << 4)
#define RFIC_AD9361_RX_GAIN_STEP_CAL (1u << 3)
#define RFIC_AD9361_TXMON_CAL        (1u << 2)
#define RFIC_AD9361_RFDC_CAL         (1u << 1)
#define RFIC_AD9361_BBDC_CAL         (1u << 0)

/* UG-570 Table 44, Control Output Pointer 0. The dedicated BB filter done
 * signals are D4/D3; Cal Done on D7 covers RFDC, BBDC, RX/TX quad and gain
 * step calibration. TX monitor calibration has only a busy signal elsewhere,
 * so it must keep using the calibration-control SPI status bit. */
static inline bool rfic_cal_done_output_bit(uint32_t calibration_mask,
					    uint8_t *output_bit)
{
	if (output_bit == 0) {
		return false;
	}

	switch (calibration_mask) {
	case RFIC_AD9361_RX_BB_TUNE_CAL:
		*output_bit = 4;
		return true;
	case RFIC_AD9361_TX_BB_TUNE_CAL:
		*output_bit = 3;
		return true;
	case RFIC_AD9361_RX_QUAD_CAL:
	case RFIC_AD9361_TX_QUAD_CAL:
	case RFIC_AD9361_RX_GAIN_STEP_CAL:
	case RFIC_AD9361_RFDC_CAL:
	case RFIC_AD9361_BBDC_CAL:
		*output_bit = 7;
		return true;
	default:
		return false;
	}
}

#endif
