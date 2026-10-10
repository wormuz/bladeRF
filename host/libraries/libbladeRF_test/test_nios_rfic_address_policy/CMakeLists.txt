cmake_minimum_required(VERSION 3.10...3.27)
project(libbladeRF_test_nios_rfic_address_policy C)

set(REPO_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../../..)
set(NIOS_SRC
    ${REPO_ROOT}/hdl/fpga/platforms/common/bladerf/software/bladeRF_nios/src)

add_executable(libbladeRF_test_nios_rfic_address_policy main.c)
target_include_directories(libbladeRF_test_nios_rfic_address_policy PRIVATE
    ${NIOS_SRC}
    ${REPO_ROOT}/host/libraries/libbladeRF/include
    ${REPO_ROOT}/fpga_common/include)
target_compile_options(libbladeRF_test_nios_rfic_address_policy PRIVATE -UNDEBUG)
