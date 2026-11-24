# SPDX-License-Identifier: AGPL-3.0-or-later
cmake_minimum_required(VERSION 3.24)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(imomoe_native_base LANGUAGES CXX)
endif()

option(FUNROUNDS_NATIVE_BUILD_TESTS "Build native boundary tests" OFF)
option(FUNROUNDS_NATIVE_BOUNDARY_TESTS_ONLY "Build only the SDK-independent native boundary tests" OFF)

if(WIN32)
    if(NOT MSVC)
        message(FATAL_ERROR "The reviewed Windows player Ignite ABI requires the MSVC MASM thunk")
    endif()
    enable_language(ASM_MASM)
endif()

function(add_funrounds_native_boundary_test)
    foreach(test_name IN ITEMS funrounds_native_boundary_test airmove_test recoil_impulse_policy_test)
        add_executable(${test_name} "${CMAKE_CURRENT_SOURCE_DIR}/tests/${test_name}.cpp")
        target_include_directories(${test_name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
        target_compile_features(${test_name} PRIVATE cxx_std_23)
        if(MSVC)
            target_compile_options(${test_name} PRIVATE /W4 /WX)
        else()
            target_compile_options(${test_name} PRIVATE -Wall -Wextra -Wshadow -Wconversion -Werror)
        endif()
        add_test(NAME ${test_name} COMMAND ${test_name})
    endforeach()
    target_include_directories(airmove_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/airmove_sdk")
endfunction()

function(add_ghost_visibility_policy_test)
    add_executable(ghost_visibility_policy_test
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/ghost_visibility_policy_test.cpp"
    )
    target_include_directories(ghost_visibility_policy_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_features(ghost_visibility_policy_test PRIVATE cxx_std_23)
    if(MSVC)
        target_compile_options(ghost_visibility_policy_test PRIVATE /W4 /WX)
    else()
        target_compile_options(ghost_visibility_policy_test PRIVATE -Wall -Wextra -Wshadow -Wconversion -Werror)
    endif()
    add_test(NAME ghost_visibility_policy_test COMMAND ghost_visibility_policy_test)
endfunction()

function(add_ghost_radar_policy_test)
    add_executable(ghost_radar_policy_test
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/ghost_radar_policy_test.cpp"
    )
    target_include_directories(ghost_radar_policy_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_features(ghost_radar_policy_test PRIVATE cxx_std_23)
    if(MSVC)
        target_compile_options(ghost_radar_policy_test PRIVATE /W4 /WX)
    else()
        target_compile_options(ghost_radar_policy_test PRIVATE -Wall -Wextra -Wshadow -Wconversion -Werror)
    endif()
    add_test(NAME ghost_radar_policy_test COMMAND ghost_radar_policy_test)
endfunction()

function(add_bighead_policy_test)
    add_executable(bighead_policy_test
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/bighead_policy_test.cpp"
    )
    target_include_directories(bighead_policy_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_features(bighead_policy_test PRIVATE cxx_std_23)
    if(MSVC)
        target_compile_options(bighead_policy_test PRIVATE /W4 /WX)
    else()
        target_compile_options(bighead_policy_test PRIVATE -Wall -Wextra -Wshadow -Wconversion -Werror)
    endif()
    add_test(NAME bighead_policy_test COMMAND bighead_policy_test)
endfunction()

function(add_detour_slot_test)
    add_executable(detour_slot_test "${CMAKE_CURRENT_SOURCE_DIR}/tests/detour_slot_test.cpp")
    target_include_directories(detour_slot_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/tests/airmove_sdk")
    target_compile_features(detour_slot_test PRIVATE cxx_std_23)
    if(MSVC)
        target_compile_options(detour_slot_test PRIVATE /W4 /WX)
    else()
        target_compile_options(detour_slot_test PRIVATE -Wall -Wextra -Wshadow -Wconversion -Werror)
    endif()
    add_test(NAME detour_slot_test COMMAND detour_slot_test)
endfunction()

function(add_standard_grenade_policy_test)
    add_executable(standard_grenade_policy_test
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/standard_grenade_policy_test.cpp"
    )
    target_include_directories(standard_grenade_policy_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_features(standard_grenade_policy_test PRIVATE cxx_std_23)
    if(MSVC)
        target_compile_options(standard_grenade_policy_test PRIVATE /W4 /WX)
    else()
        target_compile_options(standard_grenade_policy_test PRIVATE -Wall -Wextra -Wshadow -Wconversion -Werror)
    endif()
    add_test(NAME standard_grenade_policy_test COMMAND standard_grenade_policy_test)
endfunction()

if(FUNROUNDS_NATIVE_BOUNDARY_TESTS_ONLY)
    enable_testing()
    add_funrounds_native_boundary_test()
    add_ghost_visibility_policy_test()
    add_ghost_radar_policy_test()
    add_bighead_policy_test()
    add_standard_grenade_policy_test()
    add_detour_slot_test()
    return()
endif()

if(NOT DEFINED FUNROUNDS_PLUGIFY_INCLUDE_DIR)
    message(FATAL_ERROR "FUNROUNDS_PLUGIFY_INCLUDE_DIR must point to the SDK's generated Plugify headers")
endif()
if(NOT DEFINED FUNROUNDS_S2SDK_SOURCE_DIR)
    message(FATAL_ERROR "FUNROUNDS_S2SDK_SOURCE_DIR must point to the pinned S2SDK source tree")
endif()
if(NOT DEFINED FUNROUNDS_S2SDK_GENERATED_INCLUDE_DIR)
    message(FATAL_ERROR "FUNROUNDS_S2SDK_GENERATED_INCLUDE_DIR must contain generated s2sdk C++ imports")
endif()
# An explicit path is an input contract: reject it rather than silently selecting
# a different protobuf build.
if(DEFINED FUNROUNDS_PROTOBUF_LIBRARY)
    if(NOT EXISTS "${FUNROUNDS_PROTOBUF_LIBRARY}" OR IS_DIRECTORY "${FUNROUNDS_PROTOBUF_LIBRARY}")
        message(FATAL_ERROR "FUNROUNDS_PROTOBUF_LIBRARY must name an existing library file paired with the selected SourceSDK: ${FUNROUNDS_PROTOBUF_LIBRARY}")
    endif()
    set(FUNROUNDS_PROTOBUF_TARGET "${FUNROUNDS_PROTOBUF_LIBRARY}")
elseif(TARGET libprotobuf)
    set(FUNROUNDS_PROTOBUF_TARGET libprotobuf)
endif()

# Reuse the exact SourceSDK header and compile-definition contract selected by S2SDK.
if(NOT TARGET sourcesdk)
    set(SOURCESDK_GAME_TARGET "cs2" CACHE STRING "SourceSDK header target" FORCE)
    if(DEFINED FUNROUNDS_PROTOBUF_LIBRARY OR TARGET libprotobuf)
        set(SOURCESDK_COMPILE_PROTOBUF OFF CACHE BOOL "Reuse the selected protobuf library" FORCE)
    else()
        set(SOURCESDK_COMPILE_PROTOBUF ON CACHE BOOL "Build protobuf for the standalone native plugin" FORCE)
        set(protobuf_WITH_ZLIB OFF CACHE BOOL "Match the SDK protobuf build without zlib" FORCE)
    endif()
    # CCSBot's public SourceSDK declaration includes CSGOUserCmd, whose generated
    # header imports usercmd.pb.h. Generate both headers when the paired
    # protobuf library is reused rather than rebuilt.
    # When SourceSDK is compiling its full proto set, it will generate these
    # imported headers itself. Otherwise, include its normal minimal set plus
    # the two CCSBot header dependencies.
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
                CACHE STRING "SourceSDK protos required by the native plugin" FORCE
            )
        else()
            if(NOT "usercmd" IN_LIST SOURCESDK_PROTOS)
                list(APPEND SOURCESDK_PROTOS usercmd)
            endif()
            if(NOT "cs_usercmd" IN_LIST SOURCESDK_PROTOS)
                list(APPEND SOURCESDK_PROTOS cs_usercmd)
            endif()
            set(SOURCESDK_PROTOS "${SOURCESDK_PROTOS}" CACHE STRING "SourceSDK protos required by the native plugin" FORCE)
        endif()
    endif()
    set(SOURCESDK_ENABLE_TESTS OFF CACHE BOOL "Do not build SourceSDK tests for the native plugin" FORCE)
    set(SOURCESDK_CONFIGURE_EXPORT_MAP OFF CACHE BOOL "Keep Plugify entrypoints visible in the native plugin" FORCE)
    add_subdirectory("${FUNROUNDS_S2SDK_SOURCE_DIR}/external/sourcesdk" "${CMAKE_BINARY_DIR}/sourcesdk")
endif()

# A parent may add SourceSDK without compiling protobuf; in that case it must
# provide the paired library explicitly rather than causing a duplicate SDK build.
if(NOT FUNROUNDS_PROTOBUF_TARGET AND TARGET libprotobuf)
    set(FUNROUNDS_PROTOBUF_TARGET libprotobuf)
endif()
if(NOT FUNROUNDS_PROTOBUF_TARGET)
    message(FATAL_ERROR "No paired libprotobuf is available. Set FUNROUNDS_PROTOBUF_LIBRARY or configure the parent SourceSDK with SOURCESDK_COMPILE_PROTOBUF=ON.")
endif()

get_target_property(FUNROUNDS_S2SDK_INCLUDE_DIRS sourcesdk INTERFACE_INCLUDE_DIRECTORIES)
get_target_property(FUNROUNDS_S2SDK_COMPILE_DEFINITIONS sourcesdk INTERFACE_COMPILE_DEFINITIONS)
set(FUNROUNDS_S2SDK_INCLUDE_DIRS
    ${FUNROUNDS_S2SDK_INCLUDE_DIRS}
    "${FUNROUNDS_S2SDK_SOURCE_DIR}/external/plugify/include"
    "${FUNROUNDS_S2SDK_GENERATED_INCLUDE_DIR}"
)

# RunCommand filters fields of the original engine user command.
set(usercmd_proto_sources)
if(NOT TARGET sourcesdk::protos)
    get_target_property(sourcesdk_binary_dir sourcesdk BINARY_DIR)
    foreach(proto IN ITEMS network_connection networkbasetypes valveextensions usercmd cs_usercmd)
        list(APPEND usercmd_proto_sources "${sourcesdk_binary_dir}/protos/${proto}.pb.cc")
    endforeach()
    set_source_files_properties(${usercmd_proto_sources} PROPERTIES COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/W0,-w>")
endif()

add_library(imomoe-native-base SHARED
    ${usercmd_proto_sources}
    src/plugin.cpp
    src/c4_access.cpp
    src/chrono_attack.cpp
    src/recoil_impulse.cpp
    src/target_attack.cpp
    src/round_score.cpp
    src/engine.cpp
    src/userid.cpp
    src/view_angles.cpp
    src/bighead.cpp
    src/burn.cpp
    src/fire_rain.cpp
    src/grenade_factory.cpp
    src/damage.cpp
    src/touch.cpp
    src/suicide.cpp
    src/collision.cpp
    src/ground_reverse.cpp
    src/airmove.cpp
    src/pose_move.cpp
    src/player_movement_policy.cpp
    src/pose_anim.cpp
    src/third_person.cpp
    src/ghost_visibility.cpp
    src/ghost_radar.cpp
    src/ghost_passive_noise.cpp
    src/ghost_radar_state.cpp
    src/interactions.cpp
    src/random_spawn.cpp
    src/reload_sync.cpp
    src/standard_grenade.cpp
    src/world_item.cpp
    src/movement_trace.cpp
    "${FUNROUNDS_PLUGIFY_INCLUDE_DIR}/polyhook/polyhook.cpp"
    "${FUNROUNDS_S2SDK_GENERATED_INCLUDE_DIR}/s2sdk/s2sdk.cpp"
)
if(WIN32)
    add_library(imomoe-native-base-ignite-win OBJECT src/burn_ignite_win.asm)
    target_sources(imomoe-native-base PRIVATE $<TARGET_OBJECTS:imomoe-native-base-ignite-win>)
endif()

option(FUNROUNDS_NATIVE_LIFECYCLE_PROBE "Run a self-detour lifecycle probe from plugin callbacks" OFF)
if(FUNROUNDS_NATIVE_LIFECYCLE_PROBE)
    target_sources(imomoe-native-base PRIVATE src/lifecycle_probe.cpp)
    target_compile_definitions(imomoe-native-base PRIVATE FUNROUNDS_NATIVE_LIFECYCLE_PROBE)
endif()

set_target_properties(imomoe-native-base PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    PREFIX "${CMAKE_SHARED_LIBRARY_PREFIX}"
    OUTPUT_NAME "imomoe-native-base"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

target_include_directories(imomoe-native-base PRIVATE
    "${FUNROUNDS_PLUGIFY_INCLUDE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_BINARY_DIR}/exports"
)
target_include_directories(imomoe-native-base SYSTEM PRIVATE ${FUNROUNDS_S2SDK_INCLUDE_DIRS})
target_link_libraries(imomoe-native-base PRIVATE sourcesdk "${FUNROUNDS_PROTOBUF_TARGET}" ${CMAKE_DL_LIBS})
target_compile_definitions(imomoe-native-base PRIVATE
    PLUGIFY_IS_DEBUG=$<CONFIG:Debug>
    PLUGIFY_IS_RELEASE=$<CONFIG:Release>
    PLUGIFY_ARCH_BITS=$<IF:$<EQUAL:${CMAKE_SIZEOF_VOID_P},8>,64,32>
    PLUGIFY_MODULE="imomoe_native_base"
    S2SDK_LIBRARY_PREFIX="${CMAKE_SHARED_LIBRARY_PREFIX}"
    S2SDK_LIBRARY_SUFFIX="${CMAKE_SHARED_LIBRARY_SUFFIX}"
    S2SDK_GAME_NAME="csgo"
)
target_compile_definitions(imomoe-native-base PRIVATE ${FUNROUNDS_S2SDK_COMPILE_DEFINITIONS})

file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/gamedata")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/gamedata/imomoe-native-base.jsonc"
    "${CMAKE_BINARY_DIR}/gamedata/imomoe-native-base.jsonc"
    COPYONLY
)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/imomoe_native_base.pplugin"
    "${CMAKE_BINARY_DIR}/imomoe_native_base.pplugin"
    COPYONLY
)

include(GenerateExportHeader)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/exports")
generate_export_header(imomoe-native-base
    EXPORT_MACRO_NAME PLUGIN_API
    EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/exports/plugin_export.h"
)

if(MSVC)
    target_compile_options(imomoe-native-base PRIVATE /W4 /WX /Zc:preprocessor)
    set_target_properties(imomoe-native-base PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    target_compile_options(imomoe-native-base PRIVATE -Wall -Wextra -Wshadow -Wconversion -Werror)
endif()

if(FUNROUNDS_NATIVE_BUILD_TESTS)
    enable_testing()
    add_funrounds_native_boundary_test()
    add_ghost_visibility_policy_test()
    add_ghost_radar_policy_test()
    add_bighead_policy_test()
    add_standard_grenade_policy_test()
    add_detour_slot_test()
    add_executable(interaction_owner_test
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/interaction_owner_test.cpp"
        "${FUNROUNDS_PLUGIFY_INCLUDE_DIR}/polyhook/polyhook.cpp"
        "${FUNROUNDS_S2SDK_GENERATED_INCLUDE_DIR}/s2sdk/s2sdk.cpp"
    )
    target_include_directories(interaction_owner_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_BINARY_DIR}/exports"
        "${FUNROUNDS_PLUGIFY_INCLUDE_DIR}"
    )
    target_include_directories(interaction_owner_test SYSTEM PRIVATE ${FUNROUNDS_S2SDK_INCLUDE_DIRS})
    target_link_libraries(interaction_owner_test PRIVATE sourcesdk "${FUNROUNDS_PROTOBUF_TARGET}")
    target_compile_definitions(interaction_owner_test PRIVATE IMOMOE_NATIVE_BASE_STATIC_DEFINE ${FUNROUNDS_S2SDK_COMPILE_DEFINITIONS})
    target_compile_features(interaction_owner_test PRIVATE cxx_std_23)
    if(MSVC)
        target_compile_options(interaction_owner_test PRIVATE /W4 /WX /Zc:preprocessor)
    else()
        target_compile_options(interaction_owner_test PRIVATE -Wall -Wextra -Wshadow -Wconversion -Werror)
    endif()
    add_test(NAME interaction_owner_test COMMAND interaction_owner_test)
endif()
