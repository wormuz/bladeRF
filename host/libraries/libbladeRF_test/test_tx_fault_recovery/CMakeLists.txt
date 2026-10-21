cmake_minimum_required(VERSION 3.10...3.27)
project(libbladeRF_test_tx_fault_recovery C)

set(SRC_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../libbladeRF/src)

add_executable(libbladeRF_test_tx_fault_recovery
    main.c
    ${SRC_ROOT}/board/bladerf2/tx_fault_recovery.c
)
target_include_directories(libbladeRF_test_tx_fault_recovery PRIVATE
    ${SRC_ROOT}/board/bladerf2
    ${CMAKE_CURRENT_LIST_DIR}/../../libbladeRF/include
)
target_compile_options(libbladeRF_test_tx_fault_recovery PRIVATE -UNDEBUG)
