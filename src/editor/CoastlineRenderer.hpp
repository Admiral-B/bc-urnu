#pragma once

#include "CoastlineData.hpp"
#include <string>

namespace irr {
    namespace video { class IVideoDriver; }
}

class MapWidget;

// Renders coastline polygons as line overlays on the map widget.
// Automatically selects 50m data at low zoom, 10m at high zoom.
class CoastlineRenderer {
public:
    // Load both resolution levels. Paths should point to .bin files.
    bool load(const std::string& path50m, const std::string& path10m);

    // Render coastlines visible in the current viewport.
    void render(irr::video::IVideoDriver* driver, const MapWidget& mapWidget);

    bool isLoaded() const { return data50m.isLoaded() || data10m.isLoaded(); }

private:
    CoastlineData data50m; // 1:50m scale, for zoom 2-8
    CoastlineData data10m; // 1:10m scale, for zoom 8+

    void renderPolygons(irr::video::IVideoDriver* driver,
                        const MapWidget& mapWidget,
                        const CoastlineData& data,
                        int skipFactor);
};
