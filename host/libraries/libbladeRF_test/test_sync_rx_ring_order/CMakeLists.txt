cmake_minimum_required(VERSION 3.10...3.27)
project(libbladeRF_test_sync_rx_ring_order C)

if(NOT UNIX OR APPLE)
    message(STATUS "sync RX ring order test requires POSIX section GC")
    return()
endif()

find_package(Threads REQUIRED)

set(SRC_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../libbladeRF/src)
set(REPO_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../../..)

# main.c includes streaming/sync_worker.c to drive its static rx_callback().
add_executable(libbladeRF_test_sync_rx_ring_order
    main.c
    ${SRC_ROOT}/streaming/async.c
    ${SRC_ROOT}/streaming/sync.c
    ${SRC_ROOT}/helpers/wallclock.c
    ${SRC_ROOT}/helpers/timeout.c
    ${SRC_ROOT}/board/bladerf2/rx_event_history.c
    ${SRC_ROOT}/board/bladerf2/rx_transition_lifecycle.c
    ${SRC_ROOT}/board/bladerf2/rfic_fault.c
    ${SRC_ROOT}/board/bladerf2/tx_status_event.c
    ${BLADERF_HOST_COMMON_SOURCE_DIR}/log.c
    ${BLADERF_HOST_COMMON_SOURCE_DIR}/conversions.c
)

target_include_directories(libbladeRF_test_sync_rx_ring_order PRIVATE
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

# The sync-state dump is compiled in, as in the developer library build, and
# the test runs under AddressSanitizer to catch ring-sized buffer overruns.
target_compile_definitions(libbladeRF_test_sync_rx_ring_order PRIVATE
    LOGGING_ENABLED
    LOG_INCLUDE_FILE_INFO
    ENABLE_LIBBLADERF_SYNC_LOG_VERBOSE
)
target_compile_options(libbladeRF_test_sync_rx_ring_order PRIVATE
    -ffunction-sections -fdata-sections -UNDEBUG
    -fsanitize=address -fno-omit-frame-pointer
)
target_link_options(libbladeRF_test_sync_rx_ring_order PRIVATE
    -Wl,--gc-sections -fsanitize=address
)
target_link_libraries(libbladeRF_test_sync_rx_ring_order
    libbladerf_shared Threads::Threads
)
