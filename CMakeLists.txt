cmake_minimum_required(VERSION 3.12)
project(AYResource
    VERSION 2.0.0
    DESCRIPTION "AYResource — asset registry / GUID / IAnimation VERSION 4 + .aytilemap loader"
)

set(MODULE_NAME AYResource)

file(GLOB_RECURSE SRC_FILES
    src/*.cpp
)

find_package(assimp REQUIRED)
find_package(Stb REQUIRED)
find_package(unofficial-libsquish CONFIG REQUIRED)
find_package(basisu CONFIG REQUIRED)

# minimp3 (header-only) for AudioDecoder MP3 path (AYAudio design §2.3).
find_path(MINIMP3_INCLUDE_DIRS "minimp3/minimp3.h")
if(NOT MINIMP3_INCLUDE_DIRS)
    if(DEFINED ENV{VCPKG_ROOT})
        set(_ay_vcpkg_inc "$ENV{VCPKG_ROOT}/installed/x64-windows/include")
    elseif(EXISTS "${CMAKE_SOURCE_DIR}/vcpkg/installed/x64-windows/include")
        set(_ay_vcpkg_inc "${CMAKE_SOURCE_DIR}/vcpkg/installed/x64-windows/include")
    endif()
    if(_ay_vcpkg_inc AND EXISTS "${_ay_vcpkg_inc}/minimp3/minimp3.h")
        set(MINIMP3_INCLUDE_DIRS "${_ay_vcpkg_inc}")
    else()
        message(FATAL_ERROR
            "AYResource: minimp3/minimp3.h not found. "
            "Install via `vcpkg install minimp3:x64-windows`.")
    endif()
endif()

# Dev loose WAV/MP3/OGG loaders (design §4.2.1). Default ON for Debug /
# RelWithDebInfo; OFF for Release / MinSizeRel. Override: -DAY_AUDIO_LOOSE_FORMATS=ON|OFF
if(NOT DEFINED AY_AUDIO_LOOSE_FORMATS)
    set(AY_AUDIO_LOOSE_FORMATS "CONFIG" CACHE STRING
        "ON|OFF|CONFIG — allow Dev load of .wav/.mp3/.ogg without cook")
endif()

add_library(${MODULE_NAME} STATIC ${SRC_FILES})

target_include_directories(${MODULE_NAME}
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/interface>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/interface/assetsDefs>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include/assetsImpl
        ${CMAKE_CURRENT_SOURCE_DIR}/include/Converter
        ${CMAKE_CURRENT_SOURCE_DIR}/include/Loader
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${Stb_INCLUDE_DIR}
        ${MINIMP3_INCLUDE_DIRS}
)

if(AY_AUDIO_LOOSE_FORMATS STREQUAL "ON")
    target_compile_definitions(${MODULE_NAME} PUBLIC AY_AUDIO_LOOSE_FORMATS=1)
elseif(AY_AUDIO_LOOSE_FORMATS STREQUAL "OFF")
    # Explicitly disabled in all configs.
elseif(AY_AUDIO_LOOSE_FORMATS STREQUAL "CONFIG")
    target_compile_definitions(${MODULE_NAME} PUBLIC
        $<$<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>>:AY_AUDIO_LOOSE_FORMATS=1>
    )
else()
    message(FATAL_ERROR "AY_AUDIO_LOOSE_FORMATS must be ON, OFF, or CONFIG (got '${AY_AUDIO_LOOSE_FORMATS}')")
endif()

target_link_libraries(${MODULE_NAME}
    PUBLIC
        AYMath
        AYSerializer
        AYIO
        AYStorage
        AYTask
        AYEventSystem
    PRIVATE
        AYLog
        assimp::assimp
        unofficial::libsquish::squish
        basisu::basisu_encoder
)

enable_testing()
add_subdirectory(unittest)

message(STATUS "AYResource v2.0.0 (IAnimation V4 + IFBX/IMesh + .aytilemap loader + per-tile collision flags) — see ${CMAKE_CURRENT_SOURCE_DIR}/design.md")
