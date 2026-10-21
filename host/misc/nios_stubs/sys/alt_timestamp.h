/* Host stand-in for the Nios II HAL timestamp timer. */
#ifndef NIOS_STUB_ALT_TIMESTAMP_H_
#define NIOS_STUB_ALT_TIMESTAMP_H_

unsigned long alt_timestamp(void);

static inline unsigned long alt_timestamp_freq(void)
{
    return 1000000ul;
}

#endif
