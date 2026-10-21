cmake_minimum_required(VERSION 3.10...3.27)
project(libbladeRF_test_host_api_audit C)

if(NOT UNIX OR APPLE)
    message(STATUS "host API audit test requires POSIX signals")
    return()
endif()

# The audited entry points are reached through the board tables, whose members
# are static and hidden in the shared object, so the test compiles the library
# sources themselves with the library's own flags.
get_target_property(AUDIT_LIB_SOURCES libbladerf_shared SOURCES)
get_target_property(AUDIT_LIB_LIBS libbladerf_shared LINK_LIBRARIES)
get_directory_property(AUDIT_LIB_DEFS DIRECTORY ${libbladeRF_SOURCE_DIR}
                       COMPILE_DEFINITIONS)
get_directory_property(AUDIT_LIB_INCS DIRECTORY ${libbladeRF_SOURCE_DIR}
                       INCLUDE_DIRECTORIES)

set(AUDIT_SOURCES)
foreach(src ${AUDIT_LIB_SOURCES})
    if(NOT src MATCHES "\\.c$")
        continue()
    endif()
    if(IS_ABSOLUTE "${src}")
        list(APPEND AUDIT_SOURCES "${src}")
    else()
        list(APPEND AUDIT_SOURCES "${libbladeRF_SOURCE_DIR}/${src}")
    endif()
endforeach()

set_source_files_properties(
    ${libbladeRF_SOURCE_DIR}/../../../thirdparty/lzma/CpuArch.c
    ${libbladeRF_SOURCE_DIR}/../../../thirdparty/lzma/LzFind.c
    ${libbladeRF_SOURCE_DIR}/../../../thirdparty/lzma/LzFindOpt.c
    ${libbladeRF_SOURCE_DIR}/../../../thirdparty/lzma/LzmaEnc.c
    PROPERTIES COMPILE_DEFINITIONS Z7_ST
)

add_executable(libbladeRF_test_host_api_audit main.c ${AUDIT_SOURCES})
include(Shortfile)
define_file_basename_for_sources(libbladeRF_test_host_api_audit)

target_include_directories(libbladeRF_test_host_api_audit PRIVATE
    ${AUDIT_LIB_INCS}
    ${libbladeRF_SOURCE_DIR}/src
    ${libbladeRF_SOURCE_DIR}/include
    ${BLADERF_HOST_COMMON_INCLUDE_DIRS}
    ${libbladeRF_BINARY_DIR}/src
)
target_compile_definitions(libbladeRF_test_host_api_audit PRIVATE
    ${AUDIT_LIB_DEFS}
)
target_compile_options(libbladeRF_test_host_api_audit PRIVATE -UNDEBUG)
target_link_libraries(libbladeRF_test_host_api_audit ${AUDIT_LIB_LIBS})
