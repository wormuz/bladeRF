#ifndef NIOS_GPIO_POLICY_H_
#define NIOS_GPIO_POLICY_H_

#include <stdbool.h>
#include <stdint.h>

/* The control PIO loops Nios GPIO output fields into its input image. Only
 * these fields are outputs; power/core/clock-monitor status are inputs. */
#define NIOS_GPIO_OUTPUT_MASK UINT32_C(0xc03fff80)

static inline bool nios_gpio_outputs_match(uint32_t expected,
                                           uint32_t observed)
{
    return (observed & NIOS_GPIO_OUTPUT_MASK) ==
           (expected & NIOS_GPIO_OUTPUT_MASK);
}

#endif
