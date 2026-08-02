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
)

target_link_libraries(${MODULE_NAME}
    PUBLIC
        AYMath
        AYSerializer
        AYIO
        AYStorage
    PRIVATE
        AYLog
        assimp::assimp
        unofficial::libsquish::squish
        basisu::basisu_encoder
)

enable_testing()
add_subdirectory(unittest)

message(STATUS "AYResource v2.0.0 (IAnimation V4 + IFBX/IMesh + .aytilemap loader + per-tile collision flags) — see ${CMAKE_CURRENT_SOURCE_DIR}/design.md")
