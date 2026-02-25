# Support standalone and subdirectory builds
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.14)
    project(bc-tests CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(BC_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")
    find_package(GDAL)
else()
    set(BC_SRC_DIR "${CMAKE_SOURCE_DIR}")
endif()

include(FetchContent)
FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.5.2
)
FetchContent_MakeAvailable(Catch2)

# Build a static library of testable BC source files.
# These only need Irrlicht headers (for type aliases like irr::f32),
# not the full Irrlicht runtime library.
set(BC_TESTABLE_SOURCES
    ${BC_SRC_DIR}/Angles.cpp
    ${BC_SRC_DIR}/Utilities.cpp
    ${BC_SRC_DIR}/IniFile.cpp
    ${BC_SRC_DIR}/ScenarioDataStructure.cpp
    ${BC_SRC_DIR}/MMGPhysicsModel.cpp
    ${BC_SRC_DIR}/LegacyPhysicsModel.cpp
    ${BC_SRC_DIR}/BarrierFloodFill.cpp
    ${BC_SRC_DIR}/BuildingGenerator.cpp
    ${BC_SRC_DIR}/multiplayerHub/ShipPositions.cpp
    ${BC_SRC_DIR}/editor/OSMWaterReader.cpp
    ${BC_SRC_DIR}/editor/OSMBuildingReader.cpp
    ${BC_SRC_DIR}/editor/OSMLandPolygons.cpp
    ${BC_SRC_DIR}/editor/ElevationTile.cpp
    ${BC_SRC_DIR}/editor/TerrainTextureBlender.cpp
    ${BC_SRC_DIR}/gui/IniFileRW.cpp
    test_stubs.cpp
)

set(BC_TEST_SOURCES
    test_angles.cpp
    test_utilities.cpp
    test_mmg_physics.cpp
    test_legacy_physics.cpp
    test_tile_math.cpp
    test_global_data.cpp
    test_multiplayer.cpp
    test_barrier_flood_fill.cpp
    test_building_generator.cpp
    test_osm_water_reader.cpp
    test_ini_file_rw.cpp
    test_ocean_math.cpp
)

# Conditionally add chart/world generation if GDAL is available
if(GDAL_FOUND)
    list(APPEND BC_TESTABLE_SOURCES
        ${BC_SRC_DIR}/ChartReader.cpp
        ${BC_SRC_DIR}/HeightmapGenerator.cpp
        ${BC_SRC_DIR}/WorldGenerator.cpp
    )
    list(APPEND BC_TEST_SOURCES
        test_chart_reader.cpp
        test_heightmap_generator.cpp
        test_world_generator.cpp
    )
endif()

add_library(bc-testable STATIC ${BC_TESTABLE_SOURCES})
target_include_directories(bc-testable PUBLIC
    ${BC_SRC_DIR}
    ${BC_SRC_DIR}/libs/Irrlicht/irrlicht-svn/include
    ${BC_SRC_DIR}/libs/earcut
    ${BC_SRC_DIR}/libs
)
target_compile_definitions(bc-testable PRIVATE _CRT_SECURE_NO_WARNINGS)
if (APPLE)
    target_link_libraries(bc-testable PRIVATE "-framework CoreServices")
endif()
if(WIN32)
    target_link_libraries(bc-testable PRIVATE winhttp)
endif()
if(GDAL_FOUND)
    target_compile_definitions(bc-testable PUBLIC WITH_GDAL)
    target_include_directories(bc-testable PUBLIC ${GDAL_INCLUDE_DIRS})
    target_link_libraries(bc-testable PUBLIC ${GDAL_LIBRARIES})
endif()

add_executable(bc-tests ${BC_TEST_SOURCES})
target_include_directories(bc-tests PRIVATE
    ${BC_SRC_DIR}
    ${BC_SRC_DIR}/libs/Irrlicht/irrlicht-svn/include
)
target_link_libraries(bc-tests PRIVATE
    Catch2::Catch2WithMain
    bc-testable
)

include(CTest)
include(Catch)
catch_discover_tests(bc-tests)
