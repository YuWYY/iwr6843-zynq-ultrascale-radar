# Copyright (C) 2023 - 2024 Advanced Micro Devices, Inc.  All rights reserved.
# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.16)

include(${CMAKE_SOURCE_DIR}/Empty_applicationExample.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/UserConfig.cmake)
set(APP_NAME radar_runtime)
option(RADAR_AUTOBOOT_ENABLE "Start the reviewed fixed-installation radar/DP demo after native admission" ON)
project(${APP_NAME})

find_package(common REQUIRED)
enable_language(C ASM CXX)
collect(PROJECT_LIB_DEPS xilstandalone;xiltimer)
collect(PROJECT_LIB_DEPS xil)
collect(PROJECT_LIB_DEPS gcc)
collect(PROJECT_LIB_DEPS c)

# Use CMAKE_LIBRARY_PATH in link_directories
link_directories(${CMAKE_LIBRARY_PATH})

# Explicit list only: never compile caches/tests/old flat copies implicitly.
set(_sources ${USER_COMPILE_SOURCES})
foreach(source IN LISTS _sources)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
        message(FATAL_ERROR "Missing production source: ${source}")
    endif()
endforeach()
file(GLOB _legacy_flat_c "${CMAKE_CURRENT_SOURCE_DIR}/*.c")
if(_legacy_flat_c)
    message(FATAL_ERROR "Legacy flat C files remain in src: ${_legacy_flat_c}. Replace the complete src tree, not overlay.")
endif()
foreach (source ${_sources})
    get_filename_component(ext ${source} EXT)
    list(APPEND src_ext ${ext})
endforeach()

if (NOT DEFINED PROJECT_TYPE)
find_project_type ("${src_ext}" PROJECT_TYPE)
endif()

if("${PROJECT_TYPE}" STREQUAL "c++")
collect(PROJECT_LIB_DEPS stdc++)
set(CMAKE_C_COMPILER ${CMAKE_CXX_COMPILER})
endif()
collector_list (_deps PROJECT_LIB_DEPS)
list (APPEND _deps ${USER_LINK_LIBRARIES})

if("${PROJECT_TYPE}" STREQUAL "c++")
string (REPLACE ";" ",-l" _deps "${_deps}")
endif()
if (CMAKE_EXPORT_COMPILE_COMMANDS AND
    (NOT YOCTO))
    set(CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES ${CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES})
    set(CMAKE_C_STANDARD_INCLUDE_DIRECTORIES ${CMAKE_C_IMPLICIT_INCLUDE_DIRECTORIES})
endif()
# The reviewed explicit SW4 linker script is the authority. Do not regenerate
# lscript.ld from the empty template or overwrite the DMA/CPU-result regions.
if(NOT EXISTS "${USER_LINKER_SCRIPT}")
    message(FATAL_ERROR "Missing SW4 linker script: ${USER_LINKER_SCRIPT}")
endif()
string(APPEND CMAKE_C_FLAGS ${USER_COMPILE_OPTIONS})
string(APPEND CMAKE_CXX_FLAGS ${USER_COMPILE_OPTIONS})
string(APPEND CMAKE_C_LINK_FLAGS ${USER_LINK_OPTIONS})
string(APPEND CMAKE_CXX_LINK_FLAGS ${USER_LINK_OPTIONS})
if(NOT "${_sources}" STREQUAL "")
add_dependency_on_bsp(_sources)
add_executable(${APP_NAME}.elf ${_sources})
set_target_properties(${APP_NAME}.elf PROPERTIES LINK_DEPENDS ${USER_LINKER_SCRIPT})
target_link_libraries(${APP_NAME}.elf -Wl,-T -Wl,\"${USER_LINKER_SCRIPT}\" -L\"${CMAKE_SOURCE_DIR}/\" -L\"${CMAKE_LIBRARY_PATH}/\" -L\"${USER_LINK_DIRECTORIES}/\" -Wl,--start-group,-l${_deps} -Wl,--end-group)
target_compile_definitions(${APP_NAME}.elf PUBLIC ${USER_COMPILE_DEFINITIONS})
target_compile_definitions(${APP_NAME}.elf PRIVATE RADAR_AUTOBOOT_ENABLE=$<BOOL:${RADAR_AUTOBOOT_ENABLE}>)
target_include_directories(${APP_NAME}.elf PUBLIC ${USER_INCLUDE_DIRECTORIES})
print_elf_size(CMAKE_SIZE ${APP_NAME})
endif()
