// Quick test: verify GDAL can open an S-57 chart and extract features
// Compile: cl /I"C:\vcpkg\installed\x86-windows\include" /EHsc /MT test_gdal_chart.cpp
//          /link /LIBPATH:"C:\vcpkg\installed\x86-windows\lib" gdal.lib

#include <iostream>
#include <cstdlib>
#define NOMINMAX
#include <windows.h>

#include "gdal_priv.h"
#include "ogrsf_frmts.h"

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: test_gdal_chart <path-to-.000-file>" << std::endl;
        return 1;
    }

    // Set data paths
    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string exeDir(exePath);
    auto pos = exeDir.find_last_of("\\/");
    if (pos != std::string::npos) exeDir = exeDir.substr(0, pos);
    _putenv_s("GDAL_DATA", (exeDir + "\\gdal-data").c_str());
    _putenv_s("PROJ_DATA", (exeDir + "\\proj-data").c_str());

    GDALAllRegister();
    std::cout << "GDAL version: " << GDALVersionInfo("RELEASE_NAME") << std::endl;

    // Check S57 driver
    GDALDriver* s57drv = GetGDALDriverManager()->GetDriverByName("S57");
    if (s57drv) {
        std::cout << "S57 driver: available" << std::endl;
    } else {
        std::cerr << "S57 driver: NOT FOUND" << std::endl;
        return 1;
    }

    // Open chart
    const char* openOptions[] = { "SPLIT_MULTIPOINT=ON", "ADD_SOUNDG_DEPTH=ON", nullptr };
    GDALDataset* ds = static_cast<GDALDataset*>(
        GDALOpenEx(argv[1], GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, openOptions, nullptr));

    if (!ds) {
        std::cerr << "Failed to open chart: " << argv[1] << std::endl;
        return 1;
    }

    std::cout << "Chart opened successfully!" << std::endl;
    int layerCount = ds->GetLayerCount();
    std::cout << "Layers: " << layerCount << std::endl;

    int totalFeatures = 0;
    for (int i = 0; i < layerCount; i++) {
        OGRLayer* layer = ds->GetLayer(i);
        if (!layer) continue;
        int fc = (int)layer->GetFeatureCount();
        if (fc > 0) {
            std::cout << "  " << layer->GetName() << ": " << fc << " features" << std::endl;
            totalFeatures += fc;
        }
    }
    std::cout << "Total features: " << totalFeatures << std::endl;

    GDALClose(ds);
    return 0;
}
