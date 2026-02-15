#!/bin/bash
# Downloads GEBCO 2024 global bathymetry data (GeoTIFF tiles).
# Output: bin/Data/GEBCO/*.tif
#
# GEBCO 2024 Sub-Ice Topo provides elevation/bathymetry at ~450m resolution.
# Total download: ~4 GB compressed, ~7.5 GB uncompressed.
#
# Requirements: wget or curl, unzip
# Optional: GDAL (gdal_translate) for verification

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT_DIR="$REPO_ROOT/bin/Data/GEBCO"

mkdir -p "$OUT_DIR"

echo "=== GEBCO 2024 Sub-Ice Topo Download ==="
echo "This will download approximately 4 GB of data."
echo "Output directory: $OUT_DIR"
echo ""

# GEBCO provides the grid split into 4 quarter-globe tiles.
# Visit https://download.gebco.net/ for the interactive download interface.
#
# Note: GEBCO may require you to accept terms before downloading.
# If direct download fails, visit the website manually and place .tif files
# in the output directory.

BASE_URL="https://www.bodc.ac.uk/data/open_download/gebco/gebco_2024_sub_ice_topo/geotiff"

echo "Attempting direct download..."
echo "(If this fails, visit https://download.gebco.net/ and download manually)"
echo ""

# Try downloading the global file (single file option)
if curl -L -o "$OUT_DIR/gebco_2024_sub_ice.zip" "$BASE_URL/" 2>/dev/null; then
    echo "Download complete. Extracting..."
    cd "$OUT_DIR"
    unzip -o gebco_2024_sub_ice.zip
    rm -f gebco_2024_sub_ice.zip
else
    echo ""
    echo "Direct download failed. Please download GEBCO data manually:"
    echo "  1. Visit https://download.gebco.net/"
    echo "  2. Select '2024 Sub-Ice Topo' grid"
    echo "  3. Choose GeoTIFF format"
    echo "  4. Download and extract .tif files to: $OUT_DIR"
    echo ""
    echo "Alternatively, download just the region you need:"
    echo "  - Use the GEBCO web interface to select a bounding box"
    echo "  - Download the subset as GeoTIFF"
    echo "  - Place the .tif file in: $OUT_DIR"
    exit 1
fi

echo ""
echo "=== Done ==="
echo "GEBCO tiles:"
ls -lh "$OUT_DIR"/*.tif 2>/dev/null || echo "No .tif files found"
echo ""
echo "To verify, run:"
echo "  gdalinfo $OUT_DIR/*.tif | head -20"
