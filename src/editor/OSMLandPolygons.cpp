/*   Bridge Command 5.0 Ship Simulator
     Copyright (C) 2024 James Packer

     This program is free software; you can redistribute it and/or modify
     it under the terms of the GNU General Public License version 2 as
     published by the Free Software Foundation

     This program is distributed in the hope that it will be useful,
     but WITHOUT ANY WARRANTY; without even the implied warranty of
     MERCHANTABILITY Or FITNESS For A PARTICULAR PURPOSE.  See the
     GNU General Public License For more details.

     You should have received a copy of the GNU General Public License along
     with this program; if not, write to the Free Software Foundation, Inc.,
     51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA. */

#include "OSMLandPolygons.hpp"

#ifdef WITH_GDAL

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "gdal.h"
#include "gdal_alg.h"
#include "ogr_api.h"

#include <vector>
#include <iostream>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#endif

static void ensureGdalInit() {
    static bool done = false;
    if (!done) {
#ifdef _WIN32
        char exePath[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        std::string exeDir(exePath);
        auto pos = exeDir.find_last_of("\\/");
        if (pos != std::string::npos) exeDir = exeDir.substr(0, pos);
        if (!getenv("GDAL_DATA"))
            _putenv_s("GDAL_DATA", (exeDir + "\\gdal-data").c_str());
        if (!getenv("PROJ_DATA"))
            _putenv_s("PROJ_DATA", (exeDir + "\\proj-data").c_str());
#endif
        GDALAllRegister();
        done = true;
    }
}

OSMLandPolygons::OSMLandPolygons() = default;

OSMLandPolygons::~OSMLandPolygons() {
    close();
}

bool OSMLandPolygons::load(const std::string& shapefilePath) {
    close();
    ensureGdalInit();

    GDALDatasetH ds = GDALOpenEx(
        shapefilePath.c_str(),
        GDAL_OF_VECTOR | GDAL_OF_READONLY,
        nullptr, nullptr, nullptr);

    if (!ds) {
        std::cerr << "OSMLandPolygons: failed to open " << shapefilePath << std::endl;
        return false;
    }

    // Verify it has at least one layer with polygon geometry
    OGRLayerH layer = GDALDatasetGetLayer(ds, 0);
    if (!layer) {
        std::cerr << "OSMLandPolygons: no layers in " << shapefilePath << std::endl;
        GDALClose(ds);
        return false;
    }

    dataset = ds;
    loaded = true;
    return true;
}

int OSMLandPolygons::rasterize(float* grid, int resolution,
                                double minLon, double maxLon,
                                double minLat, double maxLat,
                                float landHeight) {
    if (!dataset || !grid || resolution <= 0) return 0;
    if (maxLon <= minLon || maxLat <= minLat) return 0;

    GDALDatasetH ds = static_cast<GDALDatasetH>(dataset);
    OGRLayerH layer = GDALDatasetGetLayer(ds, 0);
    if (!layer) return 0;

    // Spatial filter: only process polygons overlapping our bbox
    OGR_L_SetSpatialFilterRect(layer, minLon, minLat, maxLon, maxLat);

    // Create in-memory byte raster as a mask
    GDALDriverH memDrv = GDALGetDriverByName("MEM");
    if (!memDrv) {
        OGR_L_SetSpatialFilter(layer, nullptr);
        return 0;
    }

    GDALDatasetH memDS = GDALCreate(memDrv, "", resolution, resolution,
                                     1, GDT_Byte, nullptr);
    if (!memDS) {
        OGR_L_SetSpatialFilter(layer, nullptr);
        return 0;
    }

    // Geotransform: pixel (0,0) = top-left corner = (minLon, maxLat)
    double gt[6] = {
        minLon,
        (maxLon - minLon) / resolution,
        0.0,
        maxLat,
        0.0,
        -(maxLat - minLat) / resolution
    };
    GDALSetGeoTransform(memDS, gt);

    // Rasterize: burn value 1 for all land polygons in the filtered layer
    double burnVal = 1.0;
    int bandNum = 1;

    CPLErr err = GDALRasterizeLayers(
        memDS, 1, &bandNum,
        1, &layer,
        nullptr, nullptr,   // no coordinate transform (shapefile is already WGS84)
        &burnVal,
        nullptr,             // options
        nullptr, nullptr     // progress
    );

    int count = 0;
    if (err == CE_None) {
        GDALRasterBandH rasterBand = GDALGetRasterBand(memDS, 1);
        std::vector<uint8_t> mask(resolution * resolution);
        GDALRasterIO(rasterBand, GF_Read, 0, 0, resolution, resolution,
                     mask.data(), resolution, resolution, GDT_Byte, 0, 0);

        for (int i = 0; i < resolution * resolution; i++) {
            if (mask[i]) {
                grid[i] = landHeight;
                count++;
            }
        }
    } else {
        std::cerr << "OSMLandPolygons: GDALRasterizeLayers failed" << std::endl;
    }

    GDALClose(memDS);
    OGR_L_SetSpatialFilter(layer, nullptr);
    return count;
}

void OSMLandPolygons::close() {
    if (dataset) {
        GDALClose(static_cast<GDALDatasetH>(dataset));
        dataset = nullptr;
    }
    loaded = false;
}

#else // !WITH_GDAL

// Stub implementation when GDAL is not available
OSMLandPolygons::OSMLandPolygons() = default;
OSMLandPolygons::~OSMLandPolygons() { close(); }
bool OSMLandPolygons::load(const std::string&) { return false; }
int OSMLandPolygons::rasterize(float*, int, double, double, double, double, float) { return 0; }
void OSMLandPolygons::close() { loaded = false; }

#endif
