# SPDX-License-Identifier: AGPL-3.0-or-later
cmake_minimum_required(VERSION 3.24)
project(imomoe_native_base_loader_fixture LANGUAGES CXX)

foreach(required_variable IN ITEMS
        FUNROUNDS_PLUGIFY_INCLUDE_DIR
        FUNROUNDS_S2SDK_SOURCE_DIR
        FUNROUNDS_S2SDK_GENERATED_INCLUDE_DIR
        FUNROUNDS_NATIVE_GENERATED_INCLUDE_DIR)
    if(NOT DEFINED ${required_variable})
        message(FATAL_ERROR "${required_variable} must be set for the standalone loader fixture")
    endif()
endforeach()

set(SOURCESDK_GAME_TARGET "cs2" CACHE STRING "SourceSDK header target" FORCE)
if(DEFINED FUNROUNDS_PROTOBUF_LIBRARY)
    set(SOURCESDK_COMPILE_PROTOBUF OFF CACHE BOOL "Reuse the paired protobuf library" FORCE)
else()
    set(SOURCESDK_COMPILE_PROTOBUF ON CACHE BOOL "Build paired protobuf for native message reflection" FORCE)
endif()
set(protobuf_WITH_ZLIB OFF CACHE BOOL "Match the native SDK protobuf build without MinGW zlib" FORCE)
set(SOURCESDK_ENABLE_TESTS OFF CACHE BOOL "Do not build SourceSDK tests for the loader fixture" FORCE)
set(SOURCESDK_CONFIGURE_EXPORT_MAP OFF CACHE BOOL "Keep Plugify entrypoints visible in fixture plugins" FORCE)
# The product native target includes CBot, whose CSGOUserCmd header imports
# both cs_usercmd.pb.h and usercmd.pb.h. Populate them before creating the
# parent SourceSDK target; the native subdirectory cannot add protos afterwards.
if(NOT (SOURCESDK_COMPILE_PROTOBUF AND DEFINED SOURCESDK_USE_MINIMAL_PROTOS AND
        NOT SOURCESDK_USE_MINIMAL_PROTOS AND NOT SOURCESDK_PROTOS))
    if(NOT SOURCESDK_PROTOS)
        set(SOURCESDK_PROTOS
            netmessages
            network_connection
            networkbasetypes
            networksystem_protomessages
            gameevents
            usermessages
            source2_steam_stats
            valveextensions
            usercmd
            cs_usercmd
            CACHE STRING "SourceSDK protos required by the native loader fixture" FORCE
        )
    else()
        if(NOT "usercmd" IN_LIST SOURCESDK_PROTOS)
            list(APPEND SOURCESDK_PROTOS usercmd)
        endif()
        if(NOT "cs_usercmd" IN_LIST SOURCESDK_PROTOS)
            list(APPEND SOURCESDK_PROTOS cs_usercmd)
        endif()
        set(SOURCESDK_PROTOS "${SOURCESDK_PROTOS}" CACHE STRING "SourceSDK protos required by the native loader fixture" FORCE)
    endif()
endif()
add_subdirectory("${FUNROUNDS_S2SDK_SOURCE_DIR}/external/sourcesdk" "${CMAKE_BINARY_DIR}/sourcesdk")

set(FUNROUNDS_NATIVE_LIFECYCLE_PROBE ON CACHE BOOL "Exercise native self-detour cleanup in the host fixture" FORCE)
set(FUNROUNDS_NATIVE_BUILD_TESTS OFF CACHE BOOL "Build native boundary tests separately" FORCE)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../.."
                 "${CMAKE_BINARY_DIR}/imomoe-native-base")

set(FIXTURE_EXTENSIONS_DIR "${CMAKE_BINARY_DIR}/extensions")
foreach(extension_name IN ITEMS s2sdk imomoe_native_base imomoe_native_base_test_driver)
    file(MAKE_DIRECTORY "${FIXTURE_EXTENSIONS_DIR}/${extension_name}/bin")
endforeach()
file(MAKE_DIRECTORY "${FIXTURE_EXTENSIONS_DIR}/imomoe_native_base/gamedata")

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/s2sdk-provider.pplugin"
               "${FIXTURE_EXTENSIONS_DIR}/s2sdk/s2sdk.pplugin" COPYONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/test-driver.pplugin"
               "${FIXTURE_EXTENSIONS_DIR}/imomoe_native_base_test_driver/test-driver.pplugin" COPYONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/../../imomoe_native_base.pplugin"
               "${FIXTURE_EXTENSIONS_DIR}/imomoe_native_base/imomoe_native_base.pplugin" COPYONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/../../gamedata/imomoe-native-base.jsonc"
               "${FIXTURE_EXTENSIONS_DIR}/imomoe_native_base/gamedata/imomoe-native-base.jsonc" COPYONLY)
if(WIN32)
    configure_file("${FUNROUNDS_S2SDK_SOURCE_DIR}/external/sourcesdk/lib/win64/tier0.dll"
                   "${FIXTURE_EXTENSIONS_DIR}/imomoe_native_base/bin/tier0.dll" COPYONLY)
endif()
add_custom_target(stage_imomoe_native_base
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:imomoe-native-base>"
        "${FIXTURE_EXTENSIONS_DIR}/imomoe_native_base/bin/$<TARGET_FILE_NAME:imomoe-native-base>")
add_dependencies(stage_imomoe_native_base imomoe-native-base)

function(configure_fixture_plugin target output_name extension_name)
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        PREFIX "${CMAKE_SHARED_LIBRARY_PREFIX}"
        OUTPUT_NAME "${output_name}"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    target_include_directories(${target} PRIVATE
        "${FUNROUNDS_PLUGIFY_INCLUDE_DIR}"
        "${CMAKE_CURRENT_BINARY_DIR}/${target}-exports")
    include(GenerateExportHeader)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${target}-exports")
    generate_export_header(${target}
        EXPORT_MACRO_NAME PLUGIN_API
        EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/${target}-exports/plugin_export.h")
    target_compile_definitions(${target} PRIVATE
        PLUGIFY_IS_DEBUG=$<CONFIG:Debug>
        PLUGIFY_IS_RELEASE=$<CONFIG:Release>
        PLUGIFY_ARCH_BITS=$<IF:$<EQUAL:${CMAKE_SIZEOF_VOID_P},8>,64,32>
        PLUGIFY_MODULE="${extension_name}")
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /Zc:preprocessor)
        set_target_properties(${target} PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wshadow -Wconversion -Werror)
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:${target}>"
            "${FIXTURE_EXTENSIONS_DIR}/${extension_name}/bin/$<TARGET_FILE_NAME:${target}>")
endfunction()

add_library(s2sdk_test_provider SHARED
    "${CMAKE_CURRENT_SOURCE_DIR}/s2sdk_provider.cpp"
    "${FUNROUNDS_PLUGIFY_INCLUDE_DIR}/polyhook/polyhook.cpp")
target_link_libraries(s2sdk_test_provider PRIVATE sourcesdk)
target_include_directories(s2sdk_test_provider SYSTEM PRIVATE
    "${FUNROUNDS_S2SDK_GENERATED_INCLUDE_DIR}"
    $<TARGET_PROPERTY:sourcesdk,INTERFACE_INCLUDE_DIRECTORIES>)
target_compile_definitions(s2sdk_test_provider PRIVATE
    $<TARGET_PROPERTY:sourcesdk,INTERFACE_COMPILE_DEFINITIONS>)
configure_fixture_plugin(s2sdk_test_provider "s2sdk" "s2sdk")

add_library(imomoe_native_base_test_driver SHARED
    "${CMAKE_CURRENT_SOURCE_DIR}/test_driver.cpp"
    "${FUNROUNDS_PLUGIFY_INCLUDE_DIR}/polyhook/polyhook.cpp"
    "${FUNROUNDS_S2SDK_GENERATED_INCLUDE_DIR}/s2sdk/s2sdk.cpp"
    "${FUNROUNDS_NATIVE_GENERATED_INCLUDE_DIR}/imomoe_native_base/imomoe_native_base.cpp")
target_include_directories(imomoe_native_base_test_driver PRIVATE
    "${FUNROUNDS_S2SDK_GENERATED_INCLUDE_DIR}"
    "${FUNROUNDS_NATIVE_GENERATED_INCLUDE_DIR}")
target_include_directories(imomoe_native_base_test_driver SYSTEM PRIVATE
    $<TARGET_PROPERTY:sourcesdk,INTERFACE_INCLUDE_DIRECTORIES>)
target_compile_definitions(imomoe_native_base_test_driver PRIVATE
    $<TARGET_PROPERTY:sourcesdk,INTERFACE_COMPILE_DEFINITIONS>)
configure_fixture_plugin(imomoe_native_base_test_driver "imomoe-native-base-test-driver"
                         "imomoe_native_base_test_driver")
