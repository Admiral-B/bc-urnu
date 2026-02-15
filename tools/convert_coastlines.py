#!/usr/bin/env python3
"""Convert a Natural Earth shapefile (.shp) to a compact binary coastline format.

Binary format:
  [uint32] polygon_count
  For each polygon:
    [float32] min_lon, max_lon, min_lat, max_lat  (bounding box for culling)
    [uint32]  vertex_count
    [float32] lon0, lat0, lon1, lat1, ...          (vertex pairs)

Usage:
  python convert_coastlines.py input.shp output.bin
"""

import struct
import sys

try:
    import shapefile
except ImportError:
    print("ERROR: pyshp not installed. Run: pip install pyshp", file=sys.stderr)
    sys.exit(1)


def convert(shp_path: str, out_path: str) -> None:
    sf = shapefile.Reader(shp_path)
    polygons = []

    for shape in sf.shapes():
        # Each shape can have multiple parts (rings)
        parts = list(shape.parts) + [len(shape.points)]
        for i in range(len(parts) - 1):
            start = parts[i]
            end = parts[i + 1]
            ring = shape.points[start:end]
            if len(ring) < 3:
                continue

            lons = [p[0] for p in ring]
            lats = [p[1] for p in ring]
            bbox = (min(lons), max(lons), min(lats), max(lats))
            polygons.append((bbox, ring))

    # Write binary
    with open(out_path, "wb") as f:
        f.write(struct.pack("<I", len(polygons)))
        for bbox, ring in polygons:
            f.write(struct.pack("<ffff", *bbox))  # min_lon, max_lon, min_lat, max_lat
            f.write(struct.pack("<I", len(ring)))
            for lon, lat in ring:
                f.write(struct.pack("<ff", lon, lat))

    total_verts = sum(len(ring) for _, ring in polygons)
    size_mb = 0
    import os
    size_mb = os.path.getsize(out_path) / (1024 * 1024)
    print(f"Wrote {out_path}: {len(polygons)} polygons, {total_verts} vertices, {size_mb:.1f} MB")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} input.shp output.bin", file=sys.stderr)
        sys.exit(1)
    convert(sys.argv[1], sys.argv[2])
