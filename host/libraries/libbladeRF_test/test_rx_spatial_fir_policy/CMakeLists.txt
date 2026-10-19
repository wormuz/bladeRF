cmake_minimum_required(VERSION 3.10...3.27)
project(libbladeRF_test_rx_spatial_fir_policy C)

set(REPO_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../../..)

add_executable(libbladeRF_test_rx_spatial_fir_policy main.c)
target_include_directories(libbladeRF_test_rx_spatial_fir_policy PRIVATE
    ${REPO_ROOT}/host/libraries/libbladeRF/include
    ${REPO_ROOT}/host/libraries/libbladeRF/src/board/bladerf2
    ${REPO_ROOT}/fpga_common/include)
target_compile_options(libbladeRF_test_rx_spatial_fir_policy PRIVATE -UNDEBUG)
