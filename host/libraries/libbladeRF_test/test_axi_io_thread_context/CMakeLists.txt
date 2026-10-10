cmake_minimum_required(VERSION 3.10...3.27)
project(libbladeRF_test_axi_io_thread_context C)

if(NOT UNIX OR APPLE)
    message(STATUS "AXI context regression requires POSIX barriers")
    return()
endif()

find_package(Threads REQUIRED)

set(SRC_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../libbladeRF/src)
set(REPO_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../../..)
set(AXI_IO_SOURCE
    ${REPO_ROOT}/thirdparty/analogdevicesinc/no-OS_local/platform_bladerf2/bladerf2_axi_io.c)

add_executable(libbladeRF_test_axi_io_thread_context
    main.c
    ${AXI_IO_SOURCE}
)

target_include_directories(libbladeRF_test_axi_io_thread_context PRIVATE
    ${SRC_ROOT}
    ${CMAKE_CURRENT_LIST_DIR}/../../libbladeRF/include
    ${BLADERF_HOST_COMMON_INCLUDE_DIRS}
    ${BLADERF_FW_COMMON_INCLUDE_DIR}
    ${BLADERF_FPGA_COMMON_INCLUDE_DIR}
    ${CMAKE_BINARY_DIR}/libraries/libbladeRF/src
    ${REPO_ROOT}/thirdparty/analogdevicesinc/no-OS_local/platform_bladerf2
)

target_compile_options(libbladeRF_test_axi_io_thread_context PRIVATE -UNDEBUG)
target_link_libraries(libbladeRF_test_axi_io_thread_context Threads::Threads)
