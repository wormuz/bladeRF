cmake_minimum_required(VERSION 3.10...3.27)
project(libbladeRF_test_bladerf2_close_empty C)

set(SRC_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../libbladeRF/src)

add_executable(libbladeRF_test_bladerf2_close_empty
    main.c
    ${SRC_ROOT}/board/bladerf2/partial_close.c
)
target_include_directories(libbladeRF_test_bladerf2_close_empty PRIVATE
    ${SRC_ROOT}
    ${SRC_ROOT}/include
    ${BLADERF_HOST_COMMON_INCLUDE_DIRS}
    ${CMAKE_BINARY_DIR}/libraries/libbladeRF/src
)
target_link_libraries(libbladeRF_test_bladerf2_close_empty libbladerf_shared)
