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
 * 0.25.0 adds RFIC INIT timing telemetry over the custom NIOS packet target.
 * The host gates each format through version-derived capabilities. */
#define FPGA_VERSION_ID         0x7778
#define FPGA_VERSION_MAJOR      0
#define FPGA_VERSION_MINOR      25
#define FPGA_VERSION_PATCH      0
#define FPGA_VERSION ((uint32_t)( FPGA_VERSION_MAJOR        | \
                                 (FPGA_VERSION_MINOR << 8)  | \
                                 (FPGA_VERSION_PATCH << 16) ) )
#endif
