cmake_minimum_required(VERSION 3.10...3.27)
project(libbladeRF_test_sync_worker_stop C)

if(NOT UNIX OR APPLE)
    message(STATUS "sync worker stop test requires POSIX")
    return()
endif()

find_package(Threads REQUIRED)
set(SRC_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../libbladeRF/src)

add_executable(libbladeRF_test_sync_worker_stop
    main.c
    ${SRC_ROOT}/streaming/sync_worker.c
)
target_include_directories(libbladeRF_test_sync_worker_stop PRIVATE
    ${SRC_ROOT}
    ${CMAKE_CURRENT_LIST_DIR}/../../libbladeRF/include
    ${CMAKE_BINARY_DIR}/libraries/libbladeRF/src
    ${BLADERF_HOST_COMMON_INCLUDE_DIRS}
)
target_compile_options(libbladeRF_test_sync_worker_stop PRIVATE
    -ffunction-sections -fdata-sections -UNDEBUG
)
target_link_options(libbladeRF_test_sync_worker_stop PRIVATE
    -Wl,--gc-sections
)
target_link_libraries(libbladeRF_test_sync_worker_stop
    libbladerf_shared Threads::Threads
)
