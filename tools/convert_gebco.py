#!/usr/bin/env python3
"""Convert GEBCO GeoTIFF bathymetry data to compact binary depth grids.

Produces .gebco.bin files that the editor can load without GDAL.

Binary format:
  [float64] min_lon, max_lon, min_lat, max_lat
  [int32]   width, height
  [int16]   data[width*height]  (row-major, top-to-bottom, metres)

Usage:
  # Convert a full GEBCO tile (downsamples to manageable size):
  python convert_gebco.py input.tif output.gebco.bin [--max-size 4096]

  # Extract a regional subset:
  python convert_gebco.py input.tif output.gebco.bin --bounds -5 50 2 52

Requires: pip install rasterio (or GDAL Python bindings)
"""

import struct
import sys
import argparse
import numpy as np

def main():
    parser = argparse.ArgumentParser(description="Convert GEBCO GeoTIFF to binary depth grid")
    parser.add_argument("input", help="Input GeoTIFF file")
    parser.add_argument("output", help="Output .gebco.bin file")
    parser.add_argument("--max-size", type=int, default=4096,
                        help="Maximum grid dimension (default: 4096)")
    parser.add_argument("--bounds", nargs=4, type=float, metavar=("MIN_LON", "MIN_LAT", "MAX_LON", "MAX_LAT"),
                        help="Extract a regional subset")
    args = parser.parse_args()

    try:
        import rasterio
        from rasterio.windows import from_bounds
    except ImportError:
        try:
            from osgeo import gdal
            convert_with_gdal(args)
            return
        except ImportError:
            print("ERROR: Neither rasterio nor GDAL Python bindings found.", file=sys.stderr)
            print("Install one: pip install rasterio  OR  pip install GDAL", file=sys.stderr)
            sys.exit(1)

    with rasterio.open(args.input) as src:
        if args.bounds:
            min_lon, min_lat, max_lon, max_lat = args.bounds
            window = from_bounds(min_lon, min_lat, max_lon, max_lat, src.transform)
            data = src.read(1, window=window)
            transform = src.window_transform(window)
        else:
            data = src.read(1)
            transform = src.transform
            min_lon = transform.c
            max_lat = transform.f
            max_lon = min_lon + transform.a * data.shape[1]
            min_lat = max_lat + transform.e * data.shape[0]

        # Downsample if too large
        if data.shape[0] > args.max_size or data.shape[1] > args.max_size:
            factor = max(data.shape[0], data.shape[1]) // args.max_size + 1
            data = data[::factor, ::factor]
            print(f"Downsampled by factor {factor} to {data.shape[1]}x{data.shape[0]}")

        height, width = data.shape
        data_int16 = data.astype(np.int16)

        write_binary(args.output, min_lon, max_lon, min_lat, max_lat, width, height, data_int16)


def convert_with_gdal(args):
    from osgeo import gdal

    ds = gdal.Open(args.input)
    if not ds:
        print(f"ERROR: Cannot open {args.input}", file=sys.stderr)
        sys.exit(1)

    gt = ds.GetGeoTransform()
    band = ds.GetRasterBand(1)
    width = ds.RasterXSize
    height = ds.RasterYSize

    min_lon = gt[0]
    max_lat = gt[3]
    max_lon = min_lon + gt[1] * width
    min_lat = max_lat + gt[5] * height

    if args.bounds:
        bmin_lon, bmin_lat, bmax_lon, bmax_lat = args.bounds
        col_off = int((bmin_lon - min_lon) / gt[1])
        row_off = int((max_lat - bmax_lat) / (-gt[5]))
        ncols = int((bmax_lon - bmin_lon) / gt[1])
        nrows = int((bmax_lat - bmin_lat) / (-gt[5]))
        data = band.ReadAsArray(col_off, row_off, ncols, nrows)
        min_lon, min_lat, max_lon, max_lat = bmin_lon, bmin_lat, bmax_lon, bmax_lat
    else:
        data = band.ReadAsArray()

    # Downsample if needed
    if data.shape[0] > args.max_size or data.shape[1] > args.max_size:
        factor = max(data.shape[0], data.shape[1]) // args.max_size + 1
        data = data[::factor, ::factor]
        print(f"Downsampled by factor {factor} to {data.shape[1]}x{data.shape[0]}")

    height, width = data.shape
    data_int16 = data.astype(np.int16)

    ds = None
    write_binary(args.output, min_lon, max_lon, min_lat, max_lat, width, height, data_int16)


def write_binary(path, min_lon, max_lon, min_lat, max_lat, width, height, data_int16):
    with open(path, "wb") as f:
        f.write(struct.pack("<dddd", min_lon, max_lon, min_lat, max_lat))
        f.write(struct.pack("<ii", width, height))
        f.write(data_int16.tobytes())

    import os
    size_mb = os.path.getsize(path) / (1024 * 1024)
    print(f"Wrote {path}: {width}x{height} grid, {min_lat:.2f}-{max_lat:.2f}N, {min_lon:.2f}-{max_lon:.2f}E, {size_mb:.1f} MB")


if __name__ == "__main__":
    main()
