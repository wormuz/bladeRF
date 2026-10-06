cmake_minimum_required(VERSION 3.10...3.27)
project(libbladeRF_test_gain_set_error C)

add_executable(libbladeRF_test_gain_set_error main.c)
target_include_directories(libbladeRF_test_gain_set_error PRIVATE
    ${libbladeRF_SOURCE_DIR}/include
    ${libbladeRF_SOURCE_DIR}/src
    ${BLADERF_HOST_COMMON_INCLUDE_DIRS}
)
target_link_libraries(libbladeRF_test_gain_set_error PRIVATE libbladerf_shared)
