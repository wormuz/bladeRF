/* This file defines the FPGA version number.
 * In the future, this could/should moved to a QSYS port */
#ifndef BLADERF_FPGA_VERSION_H_
#define BLADERF_FPGA_VERSION_H_

#include <stdint.h>

/* 0x7778 and PATCH=1 mark an out-of-tree build, so a flashed image can be
 * told apart from Nuand's stock 0.16.0. The host keys compatibility off
 * MAJOR/MINOR only: find_fpga_compat() (helpers/version.c:95) returns the
 * newest table entry for anything above it, so 0.16.1 logs "newer than
 * entries in the compatibility table" and proceeds. VERSION_ID is never
 * compared against an expected value anywhere in the host or Nios tree. */
#define FPGA_VERSION_ID         0x7778
#define FPGA_VERSION_MAJOR      0
#define FPGA_VERSION_MINOR      16
#define FPGA_VERSION_PATCH      1
#define FPGA_VERSION ((uint32_t)( FPGA_VERSION_MAJOR        | \
                                 (FPGA_VERSION_MINOR << 8)  | \
                                 (FPGA_VERSION_PATCH << 16) ) )
#endif
