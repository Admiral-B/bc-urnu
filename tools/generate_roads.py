#!/usr/bin/env python3
"""Generate roads.obj for a Bridge Command world directory.

Queries Overpass API for highway=* ways within the world bounds, then
extrudes flat dark ribbon geometry at terrain height.

Usage: python generate_roads.py <world_dir>
"""

import sys, os, math, re, json, time, urllib.request
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
        return np.where(raw >= 128, (raw - 128.0) / 127.0 * max_h,
                        -(128.0 - raw) / 128.0 * sea_d)
    return None

def query_overpass(south, west, north, east):
    """Query Overpass API for highways in bounding box."""
    bbox = f"{south},{west},{north},{east}"
    query = f"""[out:json][timeout:60];
(
  way["highway"~"^(motorway|trunk|primary|secondary|tertiary|residential|service|unclassified)$"]({bbox});
);
out body;
>;
out skel qt;"""

    servers = [
        "https://overpass-api.de/api/interpreter",
        "https://overpass.kumi.systems/api/interpreter",
    ]

    for server in servers:
        try:
            print(f"  Querying {server}...")
            data = urllib.request.urlopen(
                urllib.request.Request(server, data=f"data={query}".encode(),
                                       headers={"Content-Type": "application/x-www-form-urlencoded"}),
                timeout=90
            ).read()
            return json.loads(data)
        except Exception as e:
            print(f"  Failed: {e}")
            time.sleep(2)

    return None

def get_road_width(highway_type):
    """Return road half-width in meters based on type."""
    widths = {
        'motorway': 6.0, 'trunk': 5.0, 'primary': 4.5,
        'secondary': 3.5, 'tertiary': 3.0, 'residential': 2.5,
        'service': 2.0, 'unclassified': 2.5
    }
    return widths.get(highway_type, 2.5)

def get_road_color(highway_type):
    """Return road RGB color (0-1)."""
    if highway_type in ('motorway', 'trunk'):
        return (0.25, 0.25, 0.28)
    elif highway_type in ('primary', 'secondary'):
        return (0.30, 0.30, 0.32)
    else:
        return (0.35, 0.35, 0.37)

def sample_height(height, rows, cols, u, v):
    """Sample height at normalized coords (u, v) with bilinear interpolation."""
    x = u * (cols - 1)
    z = v * (rows - 1)
    ix = int(x)
    iz = int(z)
    fx = x - ix
    fz = z - iz
    ix = max(0, min(cols - 2, ix))
    iz = max(0, min(rows - 2, iz))
    h00 = height[iz, ix]
    h10 = height[iz, ix + 1]
    h01 = height[iz + 1, ix]
    h11 = height[iz + 1, ix + 1]
    return h00 * (1-fx) * (1-fz) + h10 * fx * (1-fz) + h01 * (1-fx) * fz + h11 * fx * fz

def generate_road_mesh(ways, nodes, height, lon, lat, lon_ext, lat_ext,
                        world_width, world_depth):
    """Generate road ribbon mesh from OSM ways."""
    rows, cols = height.shape
    vertices = []
    normals = []
    indices = []
    colors = []

    for way in ways:
        if 'tags' not in way or 'highway' not in way['tags']:
            continue
        highway_type = way['tags']['highway']
        half_width = get_road_width(highway_type)
        color = get_road_color(highway_type)

        # Get node coordinates
        node_ids = way.get('nodes', [])
        points = []
        for nid in node_ids:
            if nid in nodes:
                nlat, nlon = nodes[nid]
                # Convert to world coords
                u = (nlon - lon) / lon_ext
                v = (nlat - lat) / lat_ext
                if 0 <= u <= 1 and 0 <= v <= 1:
                    wx = u * world_width
                    wz = v * world_depth
                    h = sample_height(height, rows, cols, u, v)
                    if h > -1.0:  # skip underwater
                        points.append((wx, h + 0.15, wz))  # 15cm above terrain

        if len(points) < 2:
            continue

        # Generate ribbon mesh along the polyline
        for i in range(len(points) - 1):
            p0 = points[i]
            p1 = points[i + 1]

            # Direction vector
            dx = p1[0] - p0[0]
            dz = p1[2] - p0[2]
            length = math.sqrt(dx * dx + dz * dz)
            if length < 0.1:
                continue

            # Perpendicular (right) vector
            rx = -dz / length * half_width
            rz = dx / length * half_width

            base_idx = len(vertices)

            # 4 vertices per segment: left0, right0, right1, left1
            # Pre-negate Z so importer's Z-flip restores correct LHS positions
            vertices.append((p0[0] - rx, p0[1], -(p0[2] - rz)))
            vertices.append((p0[0] + rx, p0[1], -(p0[2] + rz)))
            vertices.append((p1[0] + rx, p1[1], -(p1[2] + rz)))
            vertices.append((p1[0] - rx, p1[1], -(p1[2] - rz)))

            normals.append((0, 1, 0))
            normals.append((0, 1, 0))
            normals.append((0, 1, 0))
            normals.append((0, 1, 0))

            for _ in range(4):
                colors.append(color)

            # Two triangles (CW for left-handed)
            indices.append((base_idx + 0, base_idx + 1, base_idx + 2))
            indices.append((base_idx + 0, base_idx + 2, base_idx + 3))

    return vertices, normals, indices, colors

def write_obj(path, vertices, normals, indices, colors):
    """Write road mesh as OBJ with MTL."""
    mtl_path = str(path).replace('.obj', '.mtl')
    mtl_name = os.path.basename(mtl_path)

    # Write MTL
    with open(mtl_path, 'w') as f:
        f.write("newmtl road_surface\n")
        f.write("Kd 0.30 0.30 0.32\n")
        f.write("Ka 0.05 0.05 0.05\n")
        f.write("Ks 0.02 0.02 0.02\n")
        f.write("d 1.0\n")
        f.write("Pr 0.85\n")
        f.write("Pm 0.0\n")

    # Write OBJ
    with open(str(path), 'w') as f:
        f.write(f"mtllib {mtl_name}\n\n")
        for v in vertices:
            f.write(f"v {v[0]:.3f} {v[1]:.3f} {v[2]:.3f}\n")
        f.write("\n")
        for n in normals:
            f.write(f"vn {n[0]:.3f} {n[1]:.3f} {n[2]:.3f}\n")
        f.write("\nusemtl road_surface\n")
        for tri in indices:
            # Swap b<->c winding to compensate for Z negation
            a, b, c = tri[0]+1, tri[2]+1, tri[1]+1
            f.write(f"f {a}//{a} {b}//{b} {c}//{c}\n")

    print(f"  Wrote {path} ({os.path.getsize(str(path))} bytes, {len(indices)} tris)")

def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    world_dir = Path(sys.argv[1])
    terrain_ini = world_dir / 'terrain.ini'
    if not terrain_ini.exists():
        print(f"Error: {terrain_ini} not found")
        sys.exit(1)

    lon = float(read_ini_value(str(terrain_ini), 'TerrainLong(1)', '0'))
    lat = float(read_ini_value(str(terrain_ini), 'TerrainLat(1)', '0'))
    lon_ext = float(read_ini_value(str(terrain_ini), 'TerrainLongExtent(1)', '0'))
    lat_ext = float(read_ini_value(str(terrain_ini), 'TerrainLatExtent(1)', '0'))
    mid_lat = lat + lat_ext / 2.0
    world_width = lon_ext * 111320.0 * math.cos(mid_lat * math.pi / 180.0)
    world_depth = lat_ext * 110540.0

    print(f"World: {world_dir.name} ({world_width:.0f}m x {world_depth:.0f}m)")
    print(f"  Bounds: ({lat:.4f},{lon:.4f}) to ({lat+lat_ext:.4f},{lon+lon_ext:.4f})")

    height = load_heightmap(world_dir, str(terrain_ini))
    if height is None:
        print("Error: no heightmap found")
        sys.exit(1)

    # Query Overpass for roads
    result = query_overpass(lat, lon, lat + lat_ext, lon + lon_ext)
    if not result:
        print("Error: Overpass query failed")
        sys.exit(1)

    # Parse nodes and ways
    nodes = {}
    ways = []
    for elem in result.get('elements', []):
        if elem['type'] == 'node':
            nodes[elem['id']] = (elem['lat'], elem['lon'])
        elif elem['type'] == 'way':
            ways.append(elem)

    print(f"  Found {len(ways)} road ways, {len(nodes)} nodes")

    if not ways:
        print("  No roads found in this area")
        return

    # Generate mesh
    vertices, normals, indices, colors = generate_road_mesh(
        ways, nodes, height, lon, lat, lon_ext, lat_ext, world_width, world_depth)

    if not vertices:
        print("  No road geometry generated")
        return

    print(f"  Generated {len(vertices)} vertices, {len(indices)} triangles")

    # Write OBJ
    obj_path = world_dir / 'roads.obj'
    write_obj(obj_path, vertices, normals, indices, colors)
    print("Done.")

if __name__ == '__main__':
    main()
