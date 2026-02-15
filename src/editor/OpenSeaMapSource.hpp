#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <functional>

// Seamark data structs (standalone, no GDAL dependency)
struct OsmBuoy {
    double longitude = 0;
    double latitude = 0;
    int shape = 0;           // 1=conical,2=can,3=sphere,4=pillar,5=spar
    int categoryLateral = 0; // 1=port,2=starboard,3=pref_stbd,4=pref_port
    int categoryCardinal = 0;// 1=N,2=E,3=S,4=W
    int categorySpecial = 0;
    std::string name;
    std::string layerName;   // Synthetic: BOYLAT, BOYCAR, BOYISD, BOYSAW, BOYSPP
    bool grounded = false;   // true for beacons
};

struct OsmLight {
    double longitude = 0;
    double latitude = 0;
    int characteristic = 2;  // LITCHR: 1=F,2=Fl,3=LFl,4=Q,5=VQ,7=Iso,8=Oc
    double period = 4.0;
    std::string group;       // e.g. "(2)"
    double sectorStart = 0;
    double sectorEnd = 360;
    int colour = 1;          // IHO: 1=W,3=R,4=G,5=Bu,6=Y,11=Or
    double range = 5.0;      // NM
    double height = 5.0;     // metres
    int buoyIndex = -1;      // -1 if standalone
};

struct OsmLandmark {
    double longitude = 0;
    double latitude = 0;
    int category = 0;        // S-57 CATLMK code
    double height = 0;
    std::string name;
};

class OpenSeaMapSource {
public:
    using ProgressCallback = std::function<void(const std::string&)>;

    // Query Overpass API for seamark data in bounding box (blocking).
    bool query(double minLat, double maxLat, double minLon, double maxLon,
               ProgressCallback progress = nullptr);

    const std::vector<OsmBuoy>& getBuoys() const { return buoys; }
    const std::vector<OsmLight>& getLights() const { return lights; }
    const std::vector<OsmLandmark>& getLandmarks() const { return landmarks; }

    // Generate Bridge Command INI file contents
    std::string generateBuoyIni() const;
    std::string generateLightIni() const;
    std::string generateLandObjectIni() const;

    bool hasData() const { return queryDone; }
    const std::string& getError() const { return errorMsg; }

private:
    std::vector<OsmBuoy> buoys;
    std::vector<OsmLight> lights;
    std::vector<OsmLandmark> landmarks;
    bool queryDone = false;
    std::string errorMsg;

    static std::vector<uint8_t> httpPost(const std::string& url,
                                          const std::string& body,
                                          const std::string& userAgent);
    bool parseResponse(const std::string& jsonStr);

    // Mapping helpers
    static std::string mapBuoyType(const OsmBuoy& buoy);
    static std::string mapLandmarkType(int category);
    static std::string lightSequence(int litchr, double period, const std::string& group);
    static void colourToRGB(int code, int& r, int& g, int& b);
    static int parseCharacteristic(const std::string& s);
    static int parseColour(const std::string& s);
    static int findClosestBuoy(double lon, double lat, const std::vector<OsmBuoy>& buoys);
};
