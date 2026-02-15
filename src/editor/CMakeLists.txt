
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
)


target_link_libraries(bridgecommand-ed
    bc-irrlicht
)

if(CURL_FOUND)
    target_include_directories(bridgecommand-ed PRIVATE ${CURL_INCLUDE_DIRS})
    target_link_libraries(bridgecommand-ed ${CURL_LIBRARIES})
    target_compile_definitions(bridgecommand-ed PRIVATE WITH_CURL)
endif()
