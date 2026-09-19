cmake_minimum_required(VERSION 3.10)
project(ad936x C)

message(STATUS "libad936x: Configuring build...")

option(NIOS_HAS_HARDWARE_MULTIPLY_DIVIDE
       "Use hardware multiply/divide in Nios II/f"
       OFF
)

option(BUILD_AD936X_PRINTF
       "Enable console printf output for the AD936x library."
       OFF
)

option(BUILD_AD936X_PRINTF_DEBUG
       "Enable debugging output for the AD936x library."
       OFF
)

################################################################################
# Paths
################################################################################

# Repository root
set(LIBAD936X_BLADERF_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../..")
# Source directory for local files (patches, platform_bladerf2, etc)
set(LIBAD936X_LOCAL_DIR "${LIBAD936X_BLADERF_ROOT}/thirdparty/analogdevicesinc/no-OS_local")
# libbladeRF relative path
set(LIBAD936X_LIBBLADERF_DIR "${LIBAD936X_BLADERF_ROOT}/host/libraries/libbladeRF")
# fpga_common relative path
set(LIBAD936X_FPGACOMMON_DIR "${LIBAD936X_BLADERF_ROOT}/fpga_common")

# Patch files live here
set(LIBAD936X_PATCH_DIR "${LIBAD936X_LOCAL_DIR}/patches")
# Platform files
set(LIBAD936X_PLATFORM_DIR "${LIBAD936X_LOCAL_DIR}/platform_bladerf2_headless")

# Upstream source (clean)
#
# The driver moved out of ad9361/sw/ into drivers/rf-transceiver/ad9361/ and
# util.c was renamed ad9361_util.c. The shared headers and helpers it now pulls
# in live in include/ and util/. Mirrors host/common/thirdparty/ad936x/CMakeLists.txt.
set(LIBAD936X_UPSTREAM_ROOT "${LIBAD936X_LOCAL_DIR}/../no-OS")
set(LIBAD936X_UPSTREAM_DIR "${LIBAD936X_UPSTREAM_ROOT}/drivers/rf-transceiver/ad9361")
set(LIBAD936X_UPSTREAM_INCLUDE_DIR "${LIBAD936X_UPSTREAM_ROOT}/include")
set(LIBAD936X_UPSTREAM_UTIL_DIR "${LIBAD936X_UPSTREAM_ROOT}/util")
set(LIBAD936X_UPSTREAM_API_DIR "${LIBAD936X_UPSTREAM_ROOT}/drivers/api")
set(LIBAD936X_UPSTREAM_AXI_DIR "${LIBAD936X_UPSTREAM_ROOT}/drivers/axi_core")
# Upstream source (patched)
set(LIBAD936X_UPSTREAM_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}")

# Use CMake modules from the host tree
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${LIBAD936X_BLADERF_ROOT}/host/cmake/modules/")


# Ensure the toolchain file got loaded...
if(NOT CMAKE_SYSTEM_PROCESSOR STREQUAL "nios2")
    message(FATAL_ERROR "CMAKE_SYSTEM_PROCESSOR is ${CMAKE_SYSTEM_PROCESSOR}, expected nios2")
endif()


################################################################################
# Configure build
################################################################################

add_library(ad936x STATIC
    ${LIBAD936X_UPSTREAM_BUILD_DIR}/ad9361.c
    ${LIBAD936X_UPSTREAM_BUILD_DIR}/ad9361_api.c
    ${LIBAD936X_UPSTREAM_BUILD_DIR}/ad9361_conv.c
    ${LIBAD936X_UPSTREAM_BUILD_DIR}/ad9361_util.c
    ${LIBAD936X_UPSTREAM_BUILD_DIR}/ad9361_ext_band_ctrl.c
    # Shared helpers the driver now calls through the no-OS headers.
    ${LIBAD936X_UPSTREAM_BUILD_DIR}/no_os_alloc.c
    ${LIBAD936X_UPSTREAM_BUILD_DIR}/no_os_clk.c
    ${LIBAD936X_UPSTREAM_BUILD_DIR}/no_os_util.c
    ${LIBAD936X_UPSTREAM_BUILD_DIR}/no_os_mutex.c
    # Generic API layer: dispatches to the platform ops tables below.
    ${LIBAD936X_UPSTREAM_BUILD_DIR}/no_os_gpio.c
    ${LIBAD936X_UPSTREAM_BUILD_DIR}/no_os_spi.c
    ${LIBAD936X_PLATFORM_DIR}/platform.c
    ${LIBAD936X_PLATFORM_DIR}/bladerf2_headless_spi.c
    ${LIBAD936X_PLATFORM_DIR}/bladerf2_headless_gpio.c
    # FPGA AXI ad9361 core: axi_adc_init/ad9361_post_setup deassert its
    # reset - without them the RX sample path is dead after every cold
    # power-up. AXI_ADC_NOT_PRESENT is not defined for this board.
    ${LIBAD936X_PLATFORM_DIR}/bladerf2_headless_axi_io.c
    ${LIBAD936X_UPSTREAM_BUILD_DIR}/axi_adc_core.c
    ${LIBAD936X_UPSTREAM_BUILD_DIR}/axi_dac_core.c
    ${LIBAD936X_FPGACOMMON_DIR}/src/ad936x_params.c
)

target_compile_options(ad936x PRIVATE
    -DBLADERF_NIOS_BUILD -DNUAND_MODIFICATIONS -Wall -Os
)

# ABLATION, temporary: does the Nios wedge on the AXI ad9361 core?
# set_tuning_mode(FPGA) leaves the board answering nothing until the
# bitstream is reloaded, and the suspect is ad9361_init -> axi_adc_init ->
# IOWR_32DIRECT(0x10000) against a core clocked by if_l_clk. Defining this
# drops those accesses; if the wedge survives, the suspect is wrong.
# ⛔ The RX sample path is dead in this build - diagnostic only.
if(BLADERF_NIOS_ABLATE_AXI_ADC)
    message(WARNING "libad936x: AXI_ADC_NOT_PRESENT - diagnostic build, RX dead")
    target_compile_options(ad936x PRIVATE -DAXI_ADC_NOT_PRESENT)
endif()

if(NOT NIOS_HAS_HARDWARE_MULTIPLY_DIVIDE)
    message(STATUS "libad936x: building with software mul/mulx/div operations")
    target_compile_options(ad936x PRIVATE
        -mno-hw-div -mno-hw-mul -mno-hw-mulx
    )
endif(NOT NIOS_HAS_HARDWARE_MULTIPLY_DIVIDE)

if(NOT MSVC)
    target_compile_options(ad936x PRIVATE
        -ffunction-sections -fdata-sections
        -Wno-unused-but-set-variable -std=gnu11
    )
endif(NOT MSVC)

# Main Nios app build uses -fshort-enums, so we need to use it here too
target_compile_options(ad936x PRIVATE
    -fshort-enums
)

if(BUILD_AD936X_PRINTF OR BUILD_AD936X_PRINTF_DEBUG)
    message(STATUS "libad936x: Enabling console output")
    target_compile_options(ad936x PRIVATE -DHAVE_VERBOSE_MESSAGES)
endif(BUILD_AD936X_PRINTF OR BUILD_AD936X_PRINTF_DEBUG)

if(BUILD_AD936X_PRINTF_DEBUG)
    message(STATUS "libad936x: Enabling full debugging output")
    target_compile_options(ad936x PRIVATE -DHAVE_DEBUG_MESSAGES)
endif(BUILD_AD936X_PRINTF_DEBUG)

target_include_directories(ad936x PUBLIC
    ${CMAKE_SOURCE_DIR}/include

    ${LIBAD936X_PLATFORM_DIR}
    ${LIBAD936X_UPSTREAM_BUILD_DIR}
    ${LIBAD936X_LIBBLADERF_DIR}/src
    ${LIBAD936X_LIBBLADERF_DIR}/include

    ${LIBAD936X_BLADERF_ROOT}/host/common/include
    ${LIBAD936X_BLADERF_ROOT}/firmware_common

    ${LIBAD936X_BLADERF_ROOT}/hdl/fpga/platforms/common/bladerf/software/bladeRF_nios/src
    ${LIBAD936X_BLADERF_ROOT}/hdl/fpga/platforms/bladerf-micro/software/bladeRF_nios/src
    ${LIBAD936X_BLADERF_ROOT}/fpga_common/include
    ${CMAKE_BINARY_DIR}/../bladeRF_nios_bsp
    ${CMAKE_BINARY_DIR}/../bladeRF_nios_bsp/drivers/inc
    ${CMAKE_BINARY_DIR}/../bladeRF_nios_bsp/HAL/inc
)

################################################################################
# Dependencies
################################################################################

# If the submodule hasn't been initialized/updated, do so
if(NOT IS_DIRECTORY ${LIBAD936X_UPSTREAM_DIR})
    message(STATUS "libad936x: Attempting to initialize missing submodule")

    find_package(Git QUIET)

    if(Git_FOUND)
        execute_process(
            COMMAND ${Git_EXECUTABLE} submodule init
            WORKING_DIRECTORY ${LIBAD936X_BLADERF_ROOT}
        )
        execute_process(
            COMMAND ${Git_EXECUTABLE} submodule update
            WORKING_DIRECTORY ${LIBAD936X_BLADERF_ROOT}
        )
    endif()
endif()

# Then, re-check for submodule...
if(NOT IS_DIRECTORY ${LIBAD936X_UPSTREAM_DIR})
    message(FATAL_ERROR "Directory ${LIBAD936X_UPSTREAM_DIR} was not found. "
                        "It appears that this submodule has not been properly "
                        "initialized. Please run 'git submodule init' and "
                        "'git submodule update', and then re-run cmake.")
endif()


################################################################################
# Copy files and apply patches
################################################################################

# Copy needed files from the upstream repository
set(LIBAD936X_UPSTREAM_FILES
    ad9361.c
    ad9361.h
    ad9361_api.c
    ad9361_api.h
    ad9361_conv.c
    common.h
    ad9361_util.c
    ad9361_util.h
    ad9361_ext_band_ctrl.h
    ad9361_ext_band_ctrl.c
)

foreach(cpfile IN ITEMS ${LIBAD936X_UPSTREAM_FILES})
    configure_file(
        "${LIBAD936X_UPSTREAM_DIR}/${cpfile}"
        "${LIBAD936X_UPSTREAM_BUILD_DIR}/"
        COPYONLY
    )
endforeach(cpfile)

# Shared no-OS headers the driver includes, and the few helpers it links
# against. Copied alongside the driver so the build directory stays the single
# include root, as it was when the driver lived in one directory.
file(GLOB LIBAD936X_UPSTREAM_NOOS_HEADERS
     "${LIBAD936X_UPSTREAM_INCLUDE_DIR}/no_os_*.h")

foreach(cpfile IN LISTS LIBAD936X_UPSTREAM_NOOS_HEADERS)
    configure_file("${cpfile}" "${LIBAD936X_UPSTREAM_BUILD_DIR}/" COPYONLY)
endforeach(cpfile)

set(LIBAD936X_UPSTREAM_UTIL_FILES
    no_os_alloc.c
    no_os_clk.c
    no_os_util.c
    no_os_mutex.c
)

foreach(cpfile IN ITEMS no_os_gpio.c no_os_spi.c)
    configure_file(
        "${LIBAD936X_UPSTREAM_API_DIR}/${cpfile}"
        "${LIBAD936X_UPSTREAM_BUILD_DIR}/"
        COPYONLY
    )
endforeach(cpfile)

foreach(cpfile IN ITEMS ${LIBAD936X_UPSTREAM_UTIL_FILES})
    configure_file(
        "${LIBAD936X_UPSTREAM_UTIL_DIR}/${cpfile}"
        "${LIBAD936X_UPSTREAM_BUILD_DIR}/"
        COPYONLY
    )
endforeach(cpfile)

# Both halves of the FPGA-side ad9361 interface core are built: axi_adc_init
# brings the RX side out of reset, axi_dac_init does the same for TX and -
# critically - points the DAC at the DMA stream instead of its internal DDS.
configure_file(
    "${LIBAD936X_UPSTREAM_AXI_DIR}/axi_adc_core/axi_adc_core.h"
    "${LIBAD936X_UPSTREAM_BUILD_DIR}/" COPYONLY)
configure_file(
    "${LIBAD936X_UPSTREAM_AXI_DIR}/axi_adc_core/axi_adc_core.c"
    "${LIBAD936X_UPSTREAM_BUILD_DIR}/" COPYONLY)
configure_file(
    "${LIBAD936X_UPSTREAM_AXI_DIR}/axi_dac_core/axi_dac_core.h"
    "${LIBAD936X_UPSTREAM_BUILD_DIR}/" COPYONLY)
configure_file(
    "${LIBAD936X_UPSTREAM_AXI_DIR}/axi_dac_core/axi_dac_core.c"
    "${LIBAD936X_UPSTREAM_BUILD_DIR}/" COPYONLY)

# No patches are applied any more. See host/common/thirdparty/ad936x/
# CMakeLists.txt for the full explanation of why each of the six patches this
# tree used to carry against 0bba46e (2018) is now unnecessary.

message(STATUS "libad936x: Build configured")
