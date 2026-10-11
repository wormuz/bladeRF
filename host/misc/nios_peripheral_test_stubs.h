#include <stdint.h>
#include <stdbool.h>
#include "rffe_csr_policy.h"
uint32_t rffe_csr_read(void);
void rffe_csr_write(uint32_t value);
bool rffe_csr_read_checked(uint32_t *value);
bool rffe_csr_write_checked(uint32_t value, uint32_t verify_mask);
