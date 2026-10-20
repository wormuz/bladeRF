#------------------------------------------------------------------------------
#              VARIABLES TO BE DEFINED BY PLATFORM MAKEFILE
#------------------------------------------------------------------------------

FPGA_COMMON_DIR    := ../../../../../../fpga_common
BLADERF_COMMON_DIR := ../../../common/bladerf/software/bladeRF_nios

QUARTUS_WORKDIR    := ../../../../../quartus/work/bladerf-micro
NIOS_BUILD_OUTDIR  := $(QUARTUS_WORKDIR)/bladeRF_nios

#------------------------------------------------------------------------------
#              INCLUDE THE COMMON MAKEFILE
#------------------------------------------------------------------------------

# This platform registers the 16x64 packet handler.
PCSIM_PLATFORM_CFLAGS := -DBLADERF_NIOS_PC_SIMULATION_PKT_16x64

include $(BLADERF_COMMON_DIR)/pcsim.mk
