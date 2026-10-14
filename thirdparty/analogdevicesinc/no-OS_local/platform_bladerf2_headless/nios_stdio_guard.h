/* Copyright (c) 2026 Nuand LLC */

#ifndef BLADERF_NIOS_STDIO_GUARD_H_
#define BLADERF_NIOS_STDIO_GUARD_H_

/* The NIOS RFIC worker is single-threaded and its reduced JTAG-UART write
 * path can wait forever for FIFO space. Include stdio declarations first,
 * then make any later diagnostic output in the NIOS AD936x library a build
 * error. Host builds do not force-include this header. */
#include <stdio.h>

#pragma GCC poison printf fprintf vprintf vfprintf puts putchar putc fputs \
	fputc fwrite perror alt_printf

#endif /* BLADERF_NIOS_STDIO_GUARD_H_ */
