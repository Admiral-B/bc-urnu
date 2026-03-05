#!/usr/bin/env python3
"""Generate normal.png and roughness.png for a Bridge Command world directory.

Reads height.f32 (raw float32 grid) or height.png (uint8 greyscale) and
terrain.ini, produces PBR maps using the same algorithms as
TerrainTextureBlender.cpp.

Usage: python generate_pbr_maps.py <world_dir>
  e.g. python generate_pbr_maps.py bin/World/PortsmouthHarbour
"""

import sys, os, struct, math, re
import numpy as np
from pathlib import Path

def read_ini_value(ini_path, key, default=None):
    """Read a value from a BC .ini file."""
    pattern = re.compile(rf'^\s*{re.escape(key)}\s*=\s*(.+)', re.IGNORECASE)
    with open(ini_path, 'r') as f:
        for line in f:
            m = pattern.match(line.strip())
            if m:
                return m.group(1).strip()
    return default

def load_height_f32(path, rows, cols):
    """Load raw float32 heightmap."""
    data = np.fromfile(path, dtype=np.float32)
    if rows <= 0 or cols <= 0:
        dim = int(math.sqrt(len(data)))
        assert dim * dim == len(data), f"Cannot infer dims from {len(data)} floats"
        rows = cols = dim
    return data.reshape(rows, cols)

def load_height_png(path, max_height, sea_max_depth):
    """Load uint8 greyscale height.png. Pixel 128 = sea level.
    Values > 128 scale to max_height, values < 128 scale to -sea_max_depth."""
    from PIL import Image
    img = Image.open(path).convert('L')
    raw = np.array(img, dtype=np.float32)
    height = np.where(
        raw >= 128,
        (raw - 128.0) / 127.0 * max_height,
        -(128.0 - raw) / 128.0 * sea_max_depth
    )
    return height

def generate_normal_map(height, world_width, world_depth):
    """Sobel 3x3 normal map, matching TerrainTextureBlender::generateNormalMap."""
    res = height.shape[0]
    scale_x = world_width / (res - 1)
    scale_z = world_depth / (res - 1)

    # Pad for boundary handling
    h = np.pad(height, 1, mode='edge')

    # Sobel kernels
    dx = (h[:-2, 2:] + 2.0 * h[1:-1, 2:] + h[2:, 2:]) - \
         (h[:-2, :-2] + 2.0 * h[1:-1, :-2] + h[2:, :-2])
    dy = (h[2:, :-2] + 2.0 * h[2:, 1:-1] + h[2:, 2:]) - \
         (h[:-2, :-2] + 2.0 * h[:-2, 1:-1] + h[:-2, 2:])

    nx = -dx / scale_x
    ny = np.full_like(nx, 8.0)
    nz = -dy / scale_z

    length = np.sqrt(nx*nx + ny*ny + nz*nz)
    length = np.maximum(length, 1e-8)
    nx /= length; ny /= length; nz /= length

    out = np.zeros((res, res, 3), dtype=np.uint8)
    out[:,:,0] = np.clip((nx * 127.5 + 127.5).astype(int), 0, 255)
    out[:,:,1] = np.clip((ny * 127.5 + 127.5).astype(int), 0, 255)
    out[:,:,2] = np.clip((nz * 127.5 + 127.5).astype(int), 0, 255)
    return out

def generate_roughness_map(height):
    """Roughness map with water distance BFS for wet sand.
    Without land-use data, defaults to 0.85 (grass) for all land."""
    res = height.shape[0]
    h_flat = height.ravel()

    # BFS water distance
    from collections import deque
    water_dist = np.full(res * res, -1, dtype=np.int32)
    q = deque()
    water_mask = h_flat <= 0.0
    water_dist[water_mask] = 0
    for i in np.where(water_mask)[0]:
        q.append(int(i))

    while q:
        idx = q.popleft()
        cr, cc = divmod(idx, res)
        for dr, dc in [(-1,0),(1,0),(0,-1),(0,1)]:
            nr, nc = cr+dr, cc+dc
            if 0 <= nr < res and 0 <= nc < res:
                ni = nr * res + nc
                if water_dist[ni] < 0:
                    water_dist[ni] = water_dist[idx] + 1
                    q.append(ni)

    rough = np.where(h_flat <= 0.0, 0.05, 0.85).astype(np.float32)

    # Wet sand: reduce roughness within ~15m of water
    land_mask = h_flat > 0.0
    w_dist_m = np.where(water_dist >= 0, water_dist.astype(np.float32) * 10.0, 999.0)
    wet_zone = land_mask & (w_dist_m < 15.0)
    wetness = (1.0 - w_dist_m / 15.0)
    wetness = wetness * wetness
    rough[wet_zone] *= (1.0 - wetness[wet_zone] * 0.5)

    rv = np.clip((rough * 255).astype(int), 0, 255).astype(np.uint8)
    out = np.stack([rv, rv, rv], axis=-1).reshape(res, res, 3)
    return out

def save_png(path, data):
    """Save RGB uint8 array as PNG using PIL or stb fallback."""
    try:
        from PIL import Image
        Image.fromarray(data, 'RGB').save(path)
    except ImportError:
        # Fallback: write raw PPM then convert
        h, w = data.shape[:2]
        ppm_path = str(path) + '.ppm'
        with open(ppm_path, 'wb') as f:
            f.write(f'P6\n{w} {h}\n255\n'.encode())
            f.write(data.tobytes())
        print(f"  PIL not available, wrote {ppm_path} (convert to PNG manually)")
        return
    print(f"  Wrote {path} ({os.path.getsize(path)} bytes)")

def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    world_dir = Path(sys.argv[1])
    terrain_ini = world_dir / 'terrain.ini'
    if not terrain_ini.exists():
        print(f"Error: {terrain_ini} not found")
        sys.exit(1)

    rows = int(read_ini_value(terrain_ini, 'TerrainHeightMapRows(1)', '0'))
    cols = int(read_ini_value(terrain_ini, 'TerrainHeightMapColumns(1)', '0'))
    lon = float(read_ini_value(terrain_ini, 'TerrainLong(1)', '0'))
    lat = float(read_ini_value(terrain_ini, 'TerrainLat(1)', '0'))
    lon_ext = float(read_ini_value(terrain_ini, 'TerrainLongExtent(1)', '0'))
    lat_ext = float(read_ini_value(terrain_ini, 'TerrainLatExtent(1)', '0'))

    mid_lat = lat + lat_ext / 2.0
    world_width = lon_ext * 111320.0 * math.cos(mid_lat * math.pi / 180.0)
    world_depth = lat_ext * 110540.0

    print(f"World: {world_dir.name}")
    print(f"  Grid: {rows}x{cols}, world: {world_width:.0f}m x {world_depth:.0f}m")

    # Find heightmap (prefer float32, fall back to PNG)
    height_f32 = world_dir / 'height.f32'
    height_png = world_dir / 'height.png'
    if height_f32.exists():
        print(f"  Loading {height_f32}...")
        height = load_height_f32(str(height_f32), rows, cols)
    elif height_png.exists():
        max_h = float(read_ini_value(terrain_ini, 'TerrainMaxHeight(1)', '100'))
        sea_d = float(read_ini_value(terrain_ini, 'SeaMaxDepth(1)', '100'))
        print(f"  Loading {height_png} (maxH={max_h}, seaD={sea_d})...")
        height = load_height_png(str(height_png), max_h, sea_d)
        if rows <= 0 or cols <= 0:
            rows = cols = height.shape[0]
    else:
        print(f"Error: no heightmap found ({height_f32} or {height_png})")
        sys.exit(1)
    print(f"  Height range: {height.min():.1f} to {height.max():.1f}m")

    print("  Generating normal map (Sobel 3x3)...")
    normal = generate_normal_map(height, world_width, world_depth)
    save_png(str(world_dir / 'normal.png'), normal)

    print("  Generating roughness map (water distance BFS)...")
    rough = generate_roughness_map(height)
    save_png(str(world_dir / 'roughness.png'), rough)

    print("Done.")

if __name__ == '__main__':
    main()
