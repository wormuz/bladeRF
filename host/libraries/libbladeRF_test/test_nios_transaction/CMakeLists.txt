cmake_minimum_required(VERSION 3.10...3.27)
project(libbladeRF_test_nios_transaction C)

if(NOT UNIX OR APPLE)
    message(STATUS "NIOS transaction transport test requires POSIX")
    return()
endif()

find_package(Threads REQUIRED)

set(SRC_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../libbladeRF/src)
set(REPO_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../../..)

add_executable(libbladeRF_test_nios_transaction
    main.c
    ${SRC_ROOT}/backend/usb/nios_transaction.c
)

target_include_directories(libbladeRF_test_nios_transaction PRIVATE
    ${SRC_ROOT}
    ${CMAKE_CURRENT_LIST_DIR}/../../libbladeRF/include
    ${BLADERF_HOST_COMMON_INCLUDE_DIRS}
    ${BLADERF_FW_COMMON_INCLUDE_DIR}
    ${BLADERF_FPGA_COMMON_INCLUDE_DIR}
    ${CMAKE_BINARY_DIR}/libraries/libbladeRF/src
    ${REPO_ROOT}/thirdparty/lzma
)

target_compile_options(libbladeRF_test_nios_transaction PRIVATE -UNDEBUG)
target_link_libraries(libbladeRF_test_nios_transaction Threads::Threads)
