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

#ifndef __BARRIERFLOODFILL_HPP_INCLUDED__
#define __BARRIERFLOODFILL_HPP_INCLUDED__

#include <vector>
#include <utility>

// Standalone barrier flood-fill algorithm for heightmap generation.
// No GDAL dependency -- usable by both the S-57 chart path (HeightmapGenerator)
// and the no-chart fallback path (EditorApp).
//
// Algorithm:
//   1. Rasterize barrier polylines (dams, breakwaters) as boundary pixels (Bresenham)
//   2. Dilate boundary by 1 pixel to close diagonal gaps
//   3. Flood "open sea" from grid edges through water pixels, blocked by barriers
//   4. Water pixels NOT reached by flood = enclosed by barriers = reclassified as land
namespace BarrierFloodFill {

    struct Bounds {
        double minLon, maxLon;
        double minLat, maxLat;
    };

    // Apply barrier flood-fill on a flat row-major height grid.
    // grid: flat row-major array of size resolution*resolution (positive = land, <=0 = water)
    // barriers: polylines as (lat, lon) pairs
    // reclaimHeight: height assigned to enclosed water pixels (default 1.0m)
    // barrierHeight: height assigned to barrier boundary pixels (default 2.0m)
    // Returns number of reclaimed pixels.
    int apply(float* grid, int resolution,
              const Bounds& bounds,
              const std::vector<std::vector<std::pair<double,double>>>& barriers,
              float reclaimHeight = 1.0f,
              float barrierHeight = 2.0f);

} // namespace BarrierFloodFill

#endif // __BARRIERFLOODFILL_HPP_INCLUDED__
