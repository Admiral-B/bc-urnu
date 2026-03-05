#!/usr/bin/env python3
"""Generate trees.ini and tree_billboard.png for a Bridge Command world.

Scatters billboard tree positions on land areas using heightmap data.
Trees are placed on land > 2m elevation, away from water edges, with
density modulated by noise to create natural clustering.

Output: trees.ini (tree positions), tree_billboard.png (alpha-tested sprite)

Usage: python generate_trees.py <world_dir> [--density 0.003] [--seed 42]
"""

import sys, os, math, re, argparse
import numpy as np
from pathlib import Path

def read_ini_value(ini_path, key, default=None):
    pattern = re.compile(rf'^\s*{re.escape(key)}\s*=\s*(.+)', re.IGNORECASE)
    with open(ini_path, 'r') as f:
        for line in f:
            m = pattern.match(line.strip())
            if m:
                return m.group(1).strip().strip('"')
    return default

def load_heightmap(world_dir, terrain_ini):
    """Load heightmap in either F32 or PNG format."""
    rows = int(read_ini_value(terrain_ini, 'TerrainHeightMapRows(1)', '0'))
    cols = int(read_ini_value(terrain_ini, 'TerrainHeightMapColumns(1)', '0'))

    f32_path = world_dir / 'height.f32'
    png_path = world_dir / 'height.png'

    if f32_path.exists():
        data = np.fromfile(str(f32_path), dtype=np.float32)
        if rows <= 0 or cols <= 0:
            dim = int(math.sqrt(len(data)))
            rows = cols = dim
        return data.reshape(rows, cols)
    elif png_path.exists():
        from PIL import Image
        img = Image.open(str(png_path)).convert('L')
        raw = np.array(img, dtype=np.float32)
        max_h = float(read_ini_value(terrain_ini, 'TerrainMaxHeight(1)', '100'))
        sea_d = float(read_ini_value(terrain_ini, 'SeaMaxDepth(1)', '100'))
        height = np.where(
            raw >= 128,
            (raw - 128.0) / 127.0 * max_h,
            -(128.0 - raw) / 128.0 * sea_d
        )
        return height
    else:
        return None

def hash2d(x, y):
    h = np.uint32(np.int64(x) * 374761393 + np.int64(y) * 668265263)
    h = (h ^ (h >> 13)) * np.uint32(1274126177)
    return h ^ (h >> 16)

def generate_tree_billboard(output_path, size=128):
    """Generate a simple alpha-tested tree billboard sprite."""
    from PIL import Image
    img = np.zeros((size, size, 4), dtype=np.uint8)  # RGBA

    cx, cy_trunk_base = size // 2, size - 1
    trunk_w, trunk_h = size // 10, size // 3

    rng = np.random.RandomState(42)

    for y in range(size):
        for x in range(size):
            # Trunk
            if (cy_trunk_base - trunk_h < y <= cy_trunk_base and
                abs(x - cx) < trunk_w):
                noise = rng.randint(-8, 8)
                img[y, x] = [90 + noise, 65 + noise, 40 + noise, 255]
            else:
                # Canopy: elliptical shape in top 2/3
                canopy_cy = size * 0.35
                canopy_rx = size * 0.38
                canopy_ry = size * 0.33
                dx = (x - cx) / canopy_rx
                dy = (y - canopy_cy) / canopy_ry
                dist = dx * dx + dy * dy
                if dist < 1.0:
                    # Green canopy with variation
                    noise = rng.randint(-15, 15)
                    depth = 1.0 - dist
                    shadow = 0.7 + 0.3 * depth
                    g_base = int(100 * shadow + noise)
                    r_base = int(45 * shadow + noise // 2)
                    b_base = int(30 * shadow + noise // 3)
                    img[y, x] = [
                        max(0, min(255, r_base)),
                        max(0, min(255, g_base)),
                        max(0, min(255, b_base)),
                        255 if dist < 0.85 else int(255 * (1.0 - dist) / 0.15)
                    ]

    Image.fromarray(img, 'RGBA').save(str(output_path))
    print(f"  Wrote {output_path} ({os.path.getsize(output_path)} bytes)")

def scatter_trees(height, world_width, world_depth, density=0.003, seed=42):
    """Scatter tree positions on land using Poisson-like jittered grid.
    density: trees per square meter (0.003 = 1 tree per 333 sq meters)."""
    rows, cols = height.shape
    rng = np.random.RandomState(seed)

    # Grid spacing based on density
    spacing = 1.0 / math.sqrt(density)  # meters between grid points
    grid_nx = int(world_width / spacing)
    grid_nz = int(world_depth / spacing)

    # Compute water distance for each cell (simplified: use height > 2m threshold)
    land_mask = height > 2.0

    # Erode land mask to keep trees away from water edges
    from scipy import ndimage
    erode_pixels = max(1, int(30.0 / (world_width / cols)))  # ~30m buffer
    try:
        eroded = ndimage.binary_erosion(land_mask, iterations=erode_pixels)
    except Exception:
        eroded = land_mask  # fallback if scipy not available

    trees = []
    for gz in range(grid_nz):
        for gx in range(grid_nx):
            # Jittered position
            wx = (gx + rng.uniform(0.2, 0.8)) * spacing
            wz = (gz + rng.uniform(0.2, 0.8)) * spacing

            # Map to heightmap coords
            hc = int(wx / world_width * (cols - 1))
            hr = int(wz / world_depth * (rows - 1))
            if hc < 0 or hc >= cols or hr < 0 or hr >= rows:
                continue

            if not eroded[hr, hc]:
                continue

            # Density modulation: use hash for deterministic noise
            noise_val = hash2d(gx, gz) % 1000 / 1000.0
            # Cluster trees: only place if noise above threshold
            # This creates natural gaps and clusters
            threshold = 0.55 + 0.15 * math.sin(gx * 0.1) * math.cos(gz * 0.1)
            if noise_val < threshold:
                continue

            h = float(height[hr, hc])
            # Skip very high elevations (mountain tops)
            if h > 200:
                continue

            # Random tree properties
            tree_scale = 0.7 + rng.uniform(0, 0.6)  # 0.7 to 1.3
            tree_rot = rng.uniform(0, 360)

            trees.append((wx, wz, h, tree_scale, tree_rot))

    return trees

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('world_dir', help='Path to world directory')
    parser.add_argument('--density', type=float, default=0.003,
                        help='Trees per sq meter (default: 0.003)')
    parser.add_argument('--seed', type=int, default=42)
    args = parser.parse_args()

    world_dir = Path(args.world_dir)
    terrain_ini = world_dir / 'terrain.ini'
    if not terrain_ini.exists():
        print(f"Error: {terrain_ini} not found")
        sys.exit(1)

    lon = float(read_ini_value(terrain_ini, 'TerrainLong(1)', '0'))
    lat = float(read_ini_value(terrain_ini, 'TerrainLat(1)', '0'))
    lon_ext = float(read_ini_value(terrain_ini, 'TerrainLongExtent(1)', '0'))
    lat_ext = float(read_ini_value(terrain_ini, 'TerrainLatExtent(1)', '0'))
    mid_lat = lat + lat_ext / 2.0
    world_width = lon_ext * 111320.0 * math.cos(mid_lat * math.pi / 180.0)
    world_depth = lat_ext * 110540.0

    print(f"World: {world_dir.name} ({world_width:.0f}m x {world_depth:.0f}m)")

    height = load_heightmap(world_dir, str(terrain_ini))
    if height is None:
        print("Error: no heightmap found")
        sys.exit(1)

    print(f"  Height range: {height.min():.1f} to {height.max():.1f}m")

    # Generate tree billboard texture
    billboard_path = world_dir / 'tree_billboard.png'
    if not billboard_path.exists():
        print("  Generating tree billboard texture...")
        generate_tree_billboard(billboard_path)

    # Scatter trees
    print(f"  Scattering trees (density={args.density})...")
    trees = scatter_trees(height, world_width, world_depth,
                          density=args.density, seed=args.seed)
    print(f"  Placed {len(trees)} trees")

    # Convert world XZ positions to lon/lat for trees.ini
    trees_ini = world_dir / 'trees.ini'
    with open(str(trees_ini), 'w') as f:
        f.write(f"Number={len(trees)}\n")
        f.write(f"TextureFile=tree_billboard.png\n\n")
        for i, (wx, wz, h, scale, rot) in enumerate(trees):
            idx = i + 1
            # Convert world coords back to lon/lat
            tree_lon = lon + (wx / world_width) * lon_ext
            tree_lat = lat + (wz / world_depth) * lat_ext
            f.write(f"Long({idx})={tree_lon:.7f}\n")
            f.write(f"Lat({idx})={tree_lat:.7f}\n")
            f.write(f"Height({idx})={h:.1f}\n")
            f.write(f"Scale({idx})={scale:.2f}\n")
            f.write(f"Rotation({idx})={rot:.1f}\n")

    print(f"  Wrote {trees_ini} ({os.path.getsize(str(trees_ini))} bytes)")
    print("Done.")

if __name__ == '__main__':
    main()
