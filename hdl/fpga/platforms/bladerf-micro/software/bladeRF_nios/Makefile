#------------------------------------------------------------------------------
#              VARIABLES TO BE DEFINED BY PLATFORM MAKEFILE
#------------------------------------------------------------------------------

# paths are relative to hdl/fpga/platforms/bladerf-micro/software/bladeRF_nios

FPGA_COMMON_DIR     := ../../../../../../fpga_common
LIBBLADERF_DIR      := ../../../../../../host/libraries/libbladeRF
BLADERF_COMMON_DIR  := ../../../common/bladerf/software/bladeRF_nios
HOST_COMMON_DIR     := ../../../../../../host/common
FIRMWARE_COMMON_DIR := ../../../../../../firmware_common

QUARTUS_WORKDIR     := ../../../../../quartus/${WORKDIR}
NIOS_BUILD_OUTDIR   := $(QUARTUS_WORKDIR)/bladeRF_nios
MEM_INIT_DIR        := $(NIOS_BUILD_OUTDIR)/mem_init
BSP_BUILD_OUTDIR    := $(QUARTUS_WORKDIR)/bladeRF_nios_bsp
QUARTUS_OUTDIR      := $(QUARTUS_WORKDIR)/output_files

ALT_INCLUDE_DIRS    += ./src

# The no-OS ad9361 driver added ffunction-sections/fdata-sections at the
# compile step (hdl/fpga/ip/analogdevicesinc/no_OS/CMakeLists.txt); this is
# the matching linker side, dropping unreferenced no-OS objects/functions
# (e.g. iio_ad9361.c-only functions like ad9361_parse_fir, which pulls in
# sscanf that SMALL_C_LIB does not provide) from the final bare-metal image.
# Plain LDFLAGS, not ALT_LDFLAGS or APP_LDFLAGS_USER: the common Makefile
# resets both of those with := after this file is included, so a
# platform-level += to either is discarded before link time. LDFLAGS is
# never reset downstream and flows into APP_LDFLAGS as-is.
LDFLAGS             += -Wl,--gc-sections

C_SRCS              += ./src/bladeRF_nios.c
C_SRCS              += $(FPGA_COMMON_DIR)/src/ad936x_helpers.c
C_SRCS              += $(FPGA_COMMON_DIR)/src/bladerf2_common.c
C_SRCS              += $(BLADERF_COMMON_DIR)/src/devices_rfic.c
C_SRCS              += $(BLADERF_COMMON_DIR)/src/devices_rfic_cmds.c
C_SRCS              += $(BLADERF_COMMON_DIR)/src/devices_rfic_queue.c
C_SRCS              += $(BLADERF_COMMON_DIR)/src/pkt_retune2.c
C_SRCS              += $(HOST_COMMON_DIR)/src/range.c

CFLAGS              += -DBOARD_BLADERF_MICRO

# libad936x build configuration and lib inclusion
# These variables need to be set before including the common Makefile.
LIBAD936X_BUILDDIR  := $(QUARTUS_WORKDIR)/libad936x
LIBAD936X_DIR       := ../../../../fpga/ip/analogdevicesinc/no_OS

INC_DIRS            += $(LIBAD936X_BUILDDIR) \
                       ../$(LIBAD936X_DIR)/include \
                       ../../../../../../host/common/include \
                       ../../../../../../host/build/common/thirdparty/ad936x/ \
                       ../../../../../../thirdparty/analogdevicesinc/no-OS_local/platform_bladerf2/

LIB_DIRS            += $(LIBAD936X_BUILDDIR)
LDDEPS              += $(LIBAD936X_BUILDDIR)/libad936x.a
LIBS                += ad936x

#------------------------------------------------------------------------------
#              INCLUDE THE COMMON MAKEFILE
#------------------------------------------------------------------------------

include $(BLADERF_COMMON_DIR)/Makefile

#------------------------------------------------------------------------------
#              AD936X LIBRARY
#------------------------------------------------------------------------------

# Two-stage build:
# - cmake:  shuffle files, apply patches, generate Makefile
# - make:   do the actual compile, generate libad936x.a

# if any source file changes, re-run cmake so that patching, etc, occurs
#
# Explicit files that actually get compiled, plus the submodule gitlink, not
# a glob over the driver directory: a bare glob gives "No rule to make
# target" the moment upstream moves the driver to a new layout (happened
# when it moved from ad9361/sw/ to drivers/rf-transceiver/ad9361/, see
# CMakeLists.txt above).
NO_OS_DIR           := ../../../../../../thirdparty/analogdevicesinc/no-OS
LIBAD936X_MAKE_DEP  :=  ../../../../ip/analogdevicesinc/no_OS/CMakeLists.txt \
                        $(NO_OS_DIR)/.git \
                        $(NO_OS_DIR)/drivers/rf-transceiver/ad9361/ad9361.c \
                        $(NO_OS_DIR)/drivers/rf-transceiver/ad9361/ad9361_api.c \
                        ../../../../../../thirdparty/analogdevicesinc/no-OS_local/platform_bladerf2_headless/*

LIBAD936X_BUILD_DEP :=  $(LIBAD936X_BUILDDIR)/Makefile

# override clean_libs to ensure libad936x gets cleaned
clean_libs: $(LIB_CLEAN_TARGETS) clean_libad936x

clean_libad936x:
	@$(ECHO) Info: Cleaning libad936x
	@$(MAKE) --no-print-directory -C $(LIBAD936X_BUILDDIR) clean

$(LIBAD936X_BUILDDIR)/Makefile: $(LIBAD936X_MAKE_DEP)
	@$(ECHO) Info: Generating libad936x Makefile
	@mkdir -p $(LIBAD936X_BUILDDIR)
	@cd $(LIBAD936X_BUILDDIR) && \
		cmake -G"Unix Makefiles" -DCMAKE_TOOLCHAIN_FILE=$(LIBAD936X_DIR)/cmake/nios-toolchain.cmake \
			-DBLADERF_NIOS_ABLATE_AXI_ADC=$(BLADERF_NIOS_ABLATE_AXI_ADC) $(LIBAD936X_DIR)

$(LIBAD936X_BUILDDIR)/libad936x.a: $(LIBAD936X_BUILD_DEP)
	@$(ECHO) Info: Building libad936x
	@$(MAKE) --no-print-directory -C $(LIBAD936X_BUILDDIR) ad936x

# Ensure libad936x is built before the application objects that depend on its headers
$(APP_OBJS): $(LIBAD936X_BUILDDIR)/libad936x.a
