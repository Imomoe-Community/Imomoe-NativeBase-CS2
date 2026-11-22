# SPDX-License-Identifier: AGPL-3.0-or-later
cmake_minimum_required(VERSION 3.24)
project(funrounds_ghost_visibility_minimal LANGUAGES CXX)

foreach(required IN ITEMS FUNROUNDS_PLUGIFY_INCLUDE_DIR FUNROUNDS_S2SDK_GENERATED_INCLUDE_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} must point to the paired generated headers")
    endif()
endforeach()

add_library(ghost_visibility_minimal SHARED
    driver.cpp
    "${FUNROUNDS_S2SDK_GENERATED_INCLUDE_DIR}/s2sdk/s2sdk.cpp"
    "${FUNROUNDS_PLUGIFY_INCLUDE_DIR}/polyhook/polyhook.cpp")
set_target_properties(ghost_visibility_minimal PROPERTIES
    CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
    OUTPUT_NAME "ghost-visibility-minimal"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
include(GenerateExportHeader)
generate_export_header(ghost_visibility_minimal EXPORT_MACRO_NAME PLUGIN_API
    EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/exports/plugin_export.h")
target_include_directories(ghost_visibility_minimal PRIVATE
    "${FUNROUNDS_PLUGIFY_INCLUDE_DIR}"
    "${FUNROUNDS_S2SDK_GENERATED_INCLUDE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/exports")
target_compile_definitions(ghost_visibility_minimal PRIVATE
    PLUGIFY_IS_DEBUG=$<CONFIG:Debug> PLUGIFY_IS_RELEASE=$<CONFIG:Release>
    PLUGIFY_ARCH_BITS=$<IF:$<EQUAL:${CMAKE_SIZEOF_VOID_P},8>,64,32>
    PLUGIFY_MODULE="ghost_visibility_minimal")
if(MSVC)
    target_compile_options(ghost_visibility_minimal PRIVATE /W4 /WX /Zc:preprocessor)
    set_target_properties(ghost_visibility_minimal PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    target_compile_options(ghost_visibility_minimal PRIVATE -Wall -Wextra -Wshadow -Wconversion -Werror)
endif()
configure_file(driver.pplugin "${CMAKE_BINARY_DIR}/driver.pplugin" COPYONLY)
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/gamedata")
configure_file(gamedata/ghost-visibility-minimal.jsonc "${CMAKE_BINARY_DIR}/gamedata/ghost-visibility-minimal.jsonc" COPYONLY)
