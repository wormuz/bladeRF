cmake_minimum_required(VERSION 3.10...3.27)
project(libbladeRF_test_sync_epoch_traversal C)

if(NOT UNIX OR APPLE)
    message(STATUS "sync epoch parser traversal test requires POSIX section GC")
    return()
endif()

find_package(Threads REQUIRED)

set(SRC_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../libbladeRF/src)
set(REPO_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../../..)

add_executable(libbladeRF_test_sync_epoch_traversal
    main.c
    ${SRC_ROOT}/streaming/sync.c
    ${BLADERF_HOST_COMMON_SOURCE_DIR}/log.c
)

target_include_directories(libbladeRF_test_sync_epoch_traversal PRIVATE
    ${SRC_ROOT}
    ${CMAKE_CURRENT_LIST_DIR}/../../libbladeRF/include
    ${BLADERF_HOST_COMMON_INCLUDE_DIRS}
    ${BLADERF_FW_COMMON_INCLUDE_DIR}
    ${BLADERF_FPGA_COMMON_INCLUDE_DIR}
    ${CMAKE_BINARY_DIR}/libraries/libbladeRF/src
    ${CMAKE_BINARY_DIR}/common/thirdparty/ad936x
    ${LIBUSB_INCLUDE_DIRS}
    ${REPO_ROOT}/thirdparty/lzma
    ${REPO_ROOT}/thirdparty/analogdevicesinc/no-OS_local/platform_bladerf2
)

target_compile_definitions(libbladeRF_test_sync_epoch_traversal PRIVATE
    LOGGING_ENABLED
    LOG_INCLUDE_FILE_INFO
)
target_compile_options(libbladeRF_test_sync_epoch_traversal PRIVATE
    -ffunction-sections -fdata-sections -UNDEBUG
)
target_link_options(libbladeRF_test_sync_epoch_traversal PRIVATE
    -Wl,--gc-sections
)
target_link_libraries(libbladeRF_test_sync_epoch_traversal
    libbladerf_shared Threads::Threads
)
