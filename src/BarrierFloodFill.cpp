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

#include "BarrierFloodFill.hpp"

#include <algorithm>
#include <cmath>

int BarrierFloodFill::apply(float* grid, int resolution,
                            const Bounds& bounds,
                            const std::vector<std::vector<std::pair<double,double>>>& barriers,
                            float reclaimHeight,
                            float barrierHeight) {

    if (!grid || resolution <= 0 || barriers.empty()) return 0;

    double lonRange = bounds.maxLon - bounds.minLon;
    double latRange = bounds.maxLat - bounds.minLat;
    if (lonRange <= 0 || latRange <= 0) return 0;

    int res = resolution;

    auto lonToPixel = [&](double lon) -> int {
        return std::max(0, std::min(res - 1,
            static_cast<int>((lon - bounds.minLon) / lonRange * (res - 1))));
    };
    auto latToPixel = [&](double lat) -> int {
        return std::max(0, std::min(res - 1,
            static_cast<int>((bounds.maxLat - lat) / latRange * (res - 1))));
    };

    // Step 1: Rasterize barrier polylines as boundary pixels (Bresenham)
    std::vector<bool> isBoundary(res * res, false);

    auto rasterizeLine = [&](int x0, int y0, int x1, int y1) {
        int ddx = std::abs(x1 - x0), ddy = std::abs(y1 - y0);
        int sx = (x0 < x1) ? 1 : -1;
        int sy = (y0 < y1) ? 1 : -1;
        int err = ddx - ddy;
        while (true) {
            if (x0 >= 0 && x0 < res && y0 >= 0 && y0 < res)
                isBoundary[y0 * res + x0] = true;
            if (x0 == x1 && y0 == y1) break;
            int e2 = 2 * err;
            if (e2 > -ddy) { err -= ddy; x0 += sx; }
            if (e2 < ddx) { err += ddx; y0 += sy; }
        }
    };

    for (const auto& barrier : barriers) {
        for (size_t i = 1; i < barrier.size(); i++) {
            rasterizeLine(
                lonToPixel(barrier[i-1].second), latToPixel(barrier[i-1].first),
                lonToPixel(barrier[i].second),   latToPixel(barrier[i].first));
        }
    }

    // Step 1.5: Snap barrier vertices to nearest land pixel.
    // For each barrier vertex that falls on water and has land within snapRadius,
    // extend the barrier to connect to it. This handles both:
    //   - Open polylines: endpoints don't quite reach the coastline
    //   - Closed polygons (e.g., Cardiff Bay Barrage): the polygon outline sits in
    //     water and BFS can flow around its ends. Snapping vertices near headlands
    //     connects the barrier to land on both sides.
    const int snapRadius = 40;
    for (const auto& barrier : barriers) {
        if (barrier.size() < 2) continue;

        // Deduplicate vertex pixel positions to avoid redundant snap searches
        std::vector<std::pair<int,int>> uniquePixels;
        for (const auto& pt : barrier) {
            int px = lonToPixel(pt.second), py = latToPixel(pt.first);
            if (px < 0 || px >= res || py < 0 || py >= res) continue;
            if (grid[py * res + px] > 0.0f) continue; // already on land

            bool dup = false;
            for (const auto& [ux, uy] : uniquePixels) {
                if (ux == px && uy == py) { dup = true; break; }
            }
            if (!dup) uniquePixels.emplace_back(px, py);
        }

        for (const auto& [epx, epy] : uniquePixels) {
            int bestDist2 = (snapRadius + 1) * (snapRadius + 1);
            int bestX = -1, bestY = -1;

            int minDy = std::max(-snapRadius, -epy);
            int maxDy = std::min(snapRadius, res - 1 - epy);
            int minDx = std::max(-snapRadius, -epx);
            int maxDx = std::min(snapRadius, res - 1 - epx);

            for (int dy = minDy; dy <= maxDy; dy++) {
                for (int dx = minDx; dx <= maxDx; dx++) {
                    int dist2 = dx * dx + dy * dy;
                    if (dist2 < bestDist2 && grid[(epy + dy) * res + (epx + dx)] > 0.0f) {
                        bestDist2 = dist2;
                        bestX = epx + dx;
                        bestY = epy + dy;
                    }
                }
            }

            if (bestX >= 0) {
                rasterizeLine(epx, epy, bestX, bestY);
            }
        }
    }

    // Step 2: Dilate boundary by 1 pixel to close diagonal gaps
    {
        const int d4x[] = {-1, 1, 0, 0};
        const int d4y[] = {0, 0, -1, 1};
        std::vector<bool> dilated = isBoundary;
        for (int py = 0; py < res; py++) {
            for (int px = 0; px < res; px++) {
                if (isBoundary[py * res + px]) {
                    for (int d = 0; d < 4; d++) {
                        int nx = px + d4x[d], ny = py + d4y[d];
                        if (nx >= 0 && nx < res && ny >= 0 && ny < res)
                            dilated[ny * res + nx] = true;
                    }
                }
            }
        }
        isBoundary = std::move(dilated);
    }

    // Step 3: Raise barrier boundary pixels that sit on water.
    // Maritime barriers (barrages, breakwaters, harbour walls) create protected
    // water areas, NOT reclaimed land.  Only the barrier structure itself becomes
    // land -- the enclosed water behind it remains navigable.
    //
    // Previous behaviour flood-filled from grid edges and converted all enclosed
    // water to land.  This was wrong for barrages like Cardiff Bay (impounded
    // freshwater lake) and breakwater-protected harbours.
    int reclaimedCount = 0;
    for (int i = 0; i < res * res; i++) {
        if (isBoundary[i] && grid[i] <= 0.0f) {
            grid[i] = barrierHeight;
            reclaimedCount++;
        }
    }

    return reclaimedCount;
}
