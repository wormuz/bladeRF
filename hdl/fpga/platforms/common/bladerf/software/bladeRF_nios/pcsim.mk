#------------------------------------------------------------------------------
#              VARIABLES TO BE DEFINED BY PLATFORM MAKEFILE
#------------------------------------------------------------------------------

#  - FPGA_COMMON_DIR    : Path to fpga_common
#  - BLADERF_COMMON_DIR : Path to common bladeRF_nios dir
#  - NIOS_BUILD_OUTDIR  : Path to place the Nios build products

#------------------------------------------------------------------------------
#              MAKEFILE TARGETS
#------------------------------------------------------------------------------

# The platform directory supplies fpga_version.h.
INCLUDES := -I $(FPGA_COMMON_DIR)/include -I $(BLADERF_COMMON_DIR)/src -I src

CFLAGS := -Wall -Wextra -Wno-unused-parameter \
          -O0 -ggdb3 -DBLADERF_NIOS_PC_SIMULATION \
          $(INCLUDES) $(PCSIM_PLATFORM_CFLAGS)

# The platform directory supplies main() (src/bladeRF_nios.c).
SRC := $(wildcard src/*.c) $(wildcard $(BLADERF_COMMON_DIR)/src/*.c) \
       $(FPGA_COMMON_DIR)/src/band_select.c

all: $(NIOS_BUILD_OUTDIR)/bladeRF_nios.sim

$(NIOS_BUILD_OUTDIR)/bladeRF_nios.sim: $(SRC)
	mkdir -p $(NIOS_BUILD_OUTDIR)
	$(CC) $(CFLAGS) $^ -o $@

clean:
	rm -f $(NIOS_BUILD_OUTDIR)/bladeRF_nios.sim

.PHONY: clean
