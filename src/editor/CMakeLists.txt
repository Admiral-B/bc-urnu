
add_executable(bridgecommand-ed
    main.cpp
    ../IniFile.cpp
    ../Lang.cpp
    ../Utilities.cpp
    ../ScenarioDataStructure.cpp
    ControllerModel.cpp
    EventReceiver.cpp
    ImportExportGUI.cpp
    GUI.cpp
    StartupEventReceiver.cpp
    TileDownloader.cpp
    TileTextureManager.cpp
    MapWidget.cpp
    MapTileSources.cpp
    OpenSeaMapSource.cpp
    CoastlineData.cpp
    CoastlineRenderer.cpp
    GEBCOReader.cpp
    ENCCatalog.cpp
    EditorApp.cpp
    LocationSearch.cpp
    AreaSelector.cpp
    ShipPlacer.cpp
    ScenarioFileIO.cpp
    ChartOverlay.cpp
    MeasureTool.cpp
    SatelliteTexture.cpp
    OSMBuildingReader.cpp
    ../ChartReader.cpp
    ../HeightmapGenerator.cpp
    ../WorldGenerator.cpp
    ../BuildingGenerator.cpp
    ../miniz/miniz.c
    ../miniz/miniz_tdef.c
    ../miniz/miniz_tinfl.c
    ../miniz/miniz_zip.c
    ../graphics/wicked/imgui/imgui.cpp
    ../graphics/wicked/imgui/imgui_draw.cpp
    ../graphics/wicked/imgui/imgui_tables.cpp
    ../graphics/wicked/imgui/imgui_widgets.cpp
    ../graphics/wicked/imgui/backends/imgui_impl_opengl3.cpp
)

# Platform-specific ImGui backend
if(WIN32)
    target_sources(bridgecommand-ed PRIVATE
        ../graphics/wicked/imgui/backends/imgui_impl_win32.cpp
    )
endif()

target_include_directories(bridgecommand-ed PRIVATE
    "${CMAKE_SOURCE_DIR}/libs/earcut"
)

target_link_libraries(bridgecommand-ed
    bc-irrlicht
)

if(CURL_FOUND)
    target_include_directories(bridgecommand-ed PRIVATE ${CURL_INCLUDE_DIRS})
    target_link_libraries(bridgecommand-ed ${CURL_LIBRARIES})
    target_compile_definitions(bridgecommand-ed PRIVATE WITH_CURL)
endif()

if(GDAL_FOUND)
    target_include_directories(bridgecommand-ed PRIVATE ${GDAL_INCLUDE_DIRS})
    target_link_libraries(bridgecommand-ed ${GDAL_LIBRARIES})
    target_compile_definitions(bridgecommand-ed PRIVATE WITH_GDAL)
endif()
