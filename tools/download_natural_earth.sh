#!/bin/bash
# Downloads Natural Earth coastline data and converts to compact binary format.
# Output: bin/Data/Coastlines/coastlines_50m.bin and coastlines_10m.bin
#
# Requirements: Python 3 with pyshp (pip install pyshp)
# Optional: wget or curl for downloading

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
WORK_DIR="$SCRIPT_DIR/.ne_work"
OUT_DIR="$REPO_ROOT/bin/Data/Coastlines"

mkdir -p "$WORK_DIR" "$OUT_DIR"

echo "=== Downloading Natural Earth 1:50m coastline data ==="
if [ ! -f "$WORK_DIR/ne_50m_land.zip" ]; then
    curl -L -o "$WORK_DIR/ne_50m_land.zip" \
        "https://naciscdn.org/naturalearth/50m/physical/ne_50m_land.zip" \
        || wget -O "$WORK_DIR/ne_50m_land.zip" \
        "https://naciscdn.org/naturalearth/50m/physical/ne_50m_land.zip"
fi

echo "=== Downloading Natural Earth 1:10m coastline data ==="
if [ ! -f "$WORK_DIR/ne_10m_land.zip" ]; then
    curl -L -o "$WORK_DIR/ne_10m_land.zip" \
        "https://naciscdn.org/naturalearth/10m/physical/ne_10m_land.zip" \
        || wget -O "$WORK_DIR/ne_10m_land.zip" \
        "https://naciscdn.org/naturalearth/10m/physical/ne_10m_land.zip"
fi

echo "=== Extracting ==="
mkdir -p "$WORK_DIR/ne_50m_land" "$WORK_DIR/ne_10m_land"
unzip -o "$WORK_DIR/ne_50m_land.zip" -d "$WORK_DIR/ne_50m_land/"
unzip -o "$WORK_DIR/ne_10m_land.zip" -d "$WORK_DIR/ne_10m_land/"

echo "=== Converting to binary format ==="
python "$SCRIPT_DIR/convert_coastlines.py" \
    "$WORK_DIR/ne_50m_land/ne_50m_land.shp" \
    "$OUT_DIR/coastlines_50m.bin"

python "$SCRIPT_DIR/convert_coastlines.py" \
    "$WORK_DIR/ne_10m_land/ne_10m_land.shp" \
    "$OUT_DIR/coastlines_10m.bin"

echo "=== Done ==="
echo "Output files:"
ls -lh "$OUT_DIR"/coastlines_*.bin
echo ""
echo "You can now delete the work directory: rm -rf $WORK_DIR"
