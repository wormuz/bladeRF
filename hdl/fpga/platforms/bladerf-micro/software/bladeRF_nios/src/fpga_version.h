/* This file defines the FPGA version number.
 * In the future, this could/should moved to a QSYS port */
#ifndef BLADERF_FPGA_VERSION_H_
#define BLADERF_FPGA_VERSION_H_

#include <stdint.h>

/* 0x7778 marks this out-of-tree image. FPGA 0.18.0 adds active RFIC queue
 * head fields to STATUS; 0.19.0 adds cooperative RFIC progress telemetry;
 * 0.20.0 adds retained-result acknowledgement; 0.21.0 adds explicit BUSY
 * responses for tagged RFIC mutations received during callback ownership;
 * 0.22.0 adds retained RETUNE2 failure identity and cause records;
 * 0.23.0 adds session-bound request tokens to deferred outcomes;
 * 0.24.0 adds correlated primary/cleanup result detail for RFIC commands;
 * 0.25.0 adds RFIC INIT timing telemetry over the custom NIOS packet target;
 * 0.26.0 reports true worker-idle service gaps separately from worker time.
 * 0.27.0 measures effective service-hook entry gaps and execution time;
 * 0.28.0 includes subsequent NIOS RFIC lifecycle, cleanup propagation,
 * ownership, and initialization fixes; 0.29.0 switches I2C deadline checks
 * to raw monotonic ticks to avoid repeated 64-bit time conversions;
 * 0.30.0 keeps read-only FPGA status and fail-closed TX gate commands
 * available while deferred RFIC work owns the peripheral bus.
 * The host gates each format through version-derived capabilities. */
#define FPGA_VERSION_ID         0x7778
#define FPGA_VERSION_MAJOR      0
#define FPGA_VERSION_MINOR      30
#define FPGA_VERSION_PATCH      0
#define FPGA_VERSION ((uint32_t)( FPGA_VERSION_MAJOR        | \
                                 (FPGA_VERSION_MINOR << 8)  | \
                                 (FPGA_VERSION_PATCH << 16) ) )
#endif
