# SPDX-License-Identifier: AGPL-3.0-or-later
cmake_minimum_required(VERSION 3.24)
project(funrounds_third_person_minimal LANGUAGES CXX)

foreach(required IN ITEMS FUNROUNDS_PLUGIFY_INCLUDE_DIR FUNROUNDS_S2SDK_GENERATED_INCLUDE_DIR FUNROUNDS_NATIVE_GENERATED_INCLUDE_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} must point to the paired generated headers")
    endif()
endforeach()

add_library(third_person_minimal SHARED
    driver.cpp
    abi_probe.cpp
    "${FUNROUNDS_PLUGIFY_INCLUDE_DIR}/polyhook/polyhook.cpp"
    "${FUNROUNDS_S2SDK_GENERATED_INCLUDE_DIR}/s2sdk/s2sdk.cpp"
    "${FUNROUNDS_NATIVE_GENERATED_INCLUDE_DIR}/imomoe_native_base/imomoe_native_base.cpp")
if(MSVC)
    enable_language(ASM_MASM)
    add_library(third_person_abi_asm OBJECT abi_probe_win.asm)
else()
    enable_language(ASM)
    add_library(third_person_abi_asm OBJECT abi_probe_linux.S)
    set_target_properties(third_person_abi_asm PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()
target_sources(third_person_minimal PRIVATE $<TARGET_OBJECTS:third_person_abi_asm>)
set_target_properties(third_person_minimal PROPERTIES
    CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
    OUTPUT_NAME "third-person-minimal"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
include(GenerateExportHeader)
generate_export_header(third_person_minimal EXPORT_MACRO_NAME PLUGIN_API
    EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/exports/plugin_export.h")
target_include_directories(third_person_minimal PRIVATE
    "${FUNROUNDS_PLUGIFY_INCLUDE_DIR}"
    "${FUNROUNDS_S2SDK_GENERATED_INCLUDE_DIR}"
    "${FUNROUNDS_NATIVE_GENERATED_INCLUDE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/exports")
target_compile_definitions(third_person_minimal PRIVATE
    PLUGIFY_IS_DEBUG=$<CONFIG:Debug> PLUGIFY_IS_RELEASE=$<CONFIG:Release>
    PLUGIFY_ARCH_BITS=$<IF:$<EQUAL:${CMAKE_SIZEOF_VOID_P},8>,64,32>
    PLUGIFY_MODULE="third_person_minimal")
if(MSVC)
    target_compile_options(third_person_minimal PRIVATE /W4 /WX /Zc:preprocessor)
    set_target_properties(third_person_minimal PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    target_compile_options(third_person_minimal PRIVATE -Wall -Wextra -Wshadow -Wconversion -Werror)
endif()
configure_file(driver.pplugin "${CMAKE_BINARY_DIR}/driver.pplugin" COPYONLY)
