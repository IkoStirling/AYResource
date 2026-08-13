set(MODULE_NAME AYResource_Tests)

set(AYRESOURCE_INTERNAL_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/../include/assetsImpl
    ${CMAKE_CURRENT_SOURCE_DIR}/../include/Converter
    ${CMAKE_CURRENT_SOURCE_DIR}/../include/Loader
)

add_executable(${MODULE_NAME}
    main.cpp
    AYTest_PublicApiSurface.cpp
    AYTest_ResourceCore.cpp
    AYTest_ResourceBootstrap.cpp
    AYTest_ResourcePhase2.cpp
    AYTest_ResourcePipelineP1.cpp
    AYTest_ResourcePipelineP2.cpp
    AYTest_ResourcePipelineP3.cpp
    AYTest_ResourcePipelineP4.cpp
    AYTest_ResourcePipelineP5.cpp
    AYTest_ResourcePipelineP6.cpp
    AYTest_MaterialFile.cpp
    AYTest_AssetPath.cpp
    AYTest_MeshLoader.cpp
    AYTest_MeshBinaryChunk.cpp
    AYTest_FBXConverter.cpp
    AYTest_FBXParserAnimation.cpp
    AYTest_AnimationConverter.cpp
    AYTest_TextureConverter.cpp
    AYTest_MaterialConverter.cpp
    AYTest_VirtualAssetPath_P0.cpp

    AYTest_SkeletonLoader.cpp
    AYTest_SkeletonMaskLoader.cpp
    AYTest_AnimationLoader.cpp
    AYTest_AudioLoader.cpp
    AYTest_AudioDecoder.cpp
    AYTest_VideoLoader.cpp
    AYTest_FontLoader.cpp
    AYTest_TilemapLoader.cpp
    # CM-2 (2026-08-11): .aytilemap.json -> .aytilemap v2 Converter.
    AYTest_TilemapConverter.cpp
)

target_link_libraries(${MODULE_NAME} PRIVATE
    AYResource
    AYTest
)

target_include_directories(${MODULE_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../interface
    ${CMAKE_CURRENT_SOURCE_DIR}/../interface/assetsDefs
    ${AYRESOURCE_INTERNAL_INCLUDE_DIRS}
)

# Absolute AYRuntime root for PublicApiSurface private-include scan (P6).
file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../.." _ay_runtime_root)
target_compile_definitions(${MODULE_NAME} PRIVATE
    AY_RUNTIME_ROOT="${_ay_runtime_root}"
)

ay_configure_test_tmpdir(${MODULE_NAME})
add_test(NAME ${MODULE_NAME} COMMAND ${MODULE_NAME})
