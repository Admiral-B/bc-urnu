#!/usr/bin/env python3
"""Generate building facade textures for a Bridge Command world directory.

Produces: building_wall.png, building_wall_normal.png, building_wall_roughness.png,
          building_roof.png, building_roof_normal.png, building_roof_roughness.png

Matches the procedural generation in EditorApp.cpp.

Usage: python generate_building_textures.py <world_dir>
  e.g. python generate_building_textures.py bin/World/PortsmouthHarbour
"""

import sys, os, math
import numpy as np
from pathlib import Path

def hash2d(x, y):
    """Integer hash matching EditorApp.cpp."""
    h = np.uint32(np.int64(x) * 374761393 + np.int64(y) * 668265263)
    h = (h ^ (h >> 13)) * np.uint32(1274126177)
    return h ^ (h >> 16)

def hash2d_arr(x, y):
    """Vectorized integer hash for numpy arrays."""
    x = np.asarray(x, dtype=np.int64)
    y = np.asarray(y, dtype=np.int64)
    h = (x * 374761393 + y * 668265263).astype(np.uint32)
    h = (h ^ (h >> 13)) * np.uint32(1274126177)
    return (h ^ (h >> 16)).astype(np.uint32)

def vnoise(x, y, seed):
    """Value noise with linear interpolation, matching EditorApp.cpp."""
    ix = np.floor(x).astype(np.int32)
    iy = np.floor(y).astype(np.int32)
    fx = x - ix
    fy = y - iy
    def h(a, b):
        return (hash2d_arr(a + seed, b) % 1000).astype(np.float32) / 1000.0
    v00 = h(ix, iy); v10 = h(ix+1, iy); v01 = h(ix, iy+1); v11 = h(ix+1, iy+1)
    a = v00 + (v10 - v00) * fx
    b = v01 + (v11 - v01) * fx
    return a + (b - a) * fy

def save_png(path, data):
    try:
        from PIL import Image
        Image.fromarray(data, 'RGB').save(path)
    except ImportError:
        h, w = data.shape[:2]
        ppm_path = str(path) + '.ppm'
        with open(ppm_path, 'wb') as f:
            f.write(f'P6\n{w} {h}\n255\n'.encode())
            f.write(data.tobytes())
        print(f"  PIL not available, wrote {ppm_path}")
        return
    print(f"  Wrote {path} ({os.path.getsize(path)} bytes)")

def generate_wall_textures(output_dir):
    """Generate wall atlas (1024x1024) with 4 types in 2x2 grid."""
    cellW, cellH = 512, 512
    atlasW, atlasH = cellW * 2, cellH * 2

    wall = np.zeros((atlasH, atlasW, 3), dtype=np.uint8)
    wall_height = np.zeros((atlasH, atlasW), dtype=np.float32)
    wall_rough = np.zeros((atlasH, atlasW, 3), dtype=np.uint8)

    # Window dimensions
    winW, winH = 150, 220
    winMarginX = (cellW - 2 * winW) // 3
    winY0 = (cellH - winH) // 2
    win1X = winMarginX
    win2X = winMarginX * 2 + winW

    for wallType in range(4):
        cx0 = (wallType % 2) * cellW
        cy0 = (wallType // 2) * cellH

        # Create coordinate grids for this cell
        cy_arr, cx_arr = np.mgrid[0:cellH, 0:cellW]
        gy = cy0 + cy_arr  # global y
        gx = cx0 + cx_arr  # global x

        # Window masks
        is_win1 = (cx_arr >= win1X) & (cx_arr < win1X + winW) & (cy_arr >= winY0) & (cy_arr < winY0 + winH)
        is_win2 = (cx_arr >= win2X) & (cx_arr < win2X + winW) & (cy_arr >= winY0) & (cy_arr < winY0 + winH)
        is_window = is_win1 | is_win2

        fw = 6
        def frame_check(wx):
            return ((cx_arr >= wx - fw) & (cx_arr < wx + winW + fw) &
                    (cy_arr >= winY0 - fw) & (cy_arr < winY0 + winH + fw) &
                    ~((cx_arr >= wx) & (cx_arr < wx + winW) & (cy_arr >= winY0) & (cy_arr < winY0 + winH)))
        is_frame = frame_check(win1X) | frame_check(win2X)

        r = np.zeros((cellH, cellW), dtype=np.float32)
        g = np.zeros((cellH, cellW), dtype=np.float32)
        b = np.zeros((cellH, cellW), dtype=np.float32)
        h_field = np.zeros((cellH, cellW), dtype=np.float32)
        rough = np.zeros((cellH, cellW), dtype=np.float32)

        noise_px = (hash2d_arr(cx_arr, cy_arr) % 8).astype(np.float32) - 4.0

        if wallType == 0:
            # Brick
            brickH, brickW, mortarW = 38, 17, 2
            row = cy_arr // brickH
            offX = np.where(row % 2 == 1, brickW // 2, 0)
            bx = (cx_arr + offX) % brickW
            by = cy_arr % brickH
            is_mortar = (bx < mortarW) | (by < mortarW)

            mv = 175 + (hash2d_arr(cx_arr // 3, cy_arr // 3) % 12).astype(np.float32)
            brickId = row * 100 + (cx_arr + offX) // brickW
            base = (hash2d_arr(brickId, np.zeros_like(brickId)) % 30).astype(np.float32)
            brick_r = 155 + base
            brick_g = 115 + base * 3 / 4
            brick_b = 85 + base / 2
            brick_r = np.clip(brick_r + noise_px, 0, 255)
            brick_g = np.clip(brick_g + noise_px, 0, 255)
            brick_b = np.clip(brick_b + noise_px, 0, 255)

            r = np.where(is_mortar, mv, brick_r)
            g = np.where(is_mortar, mv - 5, brick_g)
            b = np.where(is_mortar, mv - 10, brick_b)

            h_field = np.where(is_mortar, 0.0, 0.6)
            h_field += (hash2d_arr(cx_arr // 4, cy_arr // 4) % 100).astype(np.float32) / 1000.0
            rough[:] = np.where(is_mortar, 0.65, 0.75)

        elif wallType == 1:
            # Concrete
            stain = vnoise(cx_arr * 0.02, cy_arr * 0.03, 111) * 20.0 - 10.0
            formLine = (cy_arr % 60) < 2
            v = 165 + stain + np.where(formLine, -12.0, 0.0)
            noise_small = (hash2d_arr(cx_arr, cy_arr) % 6).astype(np.float32) - 3.0
            v = np.clip(v + noise_small, 0, 255)
            r = v; g = np.maximum(0, v - 2); b = np.maximum(0, v - 1)

            h_field = np.where(formLine, 0.0, 0.15)
            h_field += vnoise(cx_arr * 0.03, cy_arr * 0.03, 333) * 0.1
            stain_check = vnoise(cx_arr * 0.02, cy_arr * 0.03, 555)
            rough[:] = np.where(stain_check < 0.3, 0.72, 0.80)

        elif wallType == 2:
            # Stone masonry
            blockH_base = 30 + (hash2d_arr(np.zeros_like(cy_arr), cy_arr // 35) % 15).astype(np.int32)
            blockW_base = 50 + (hash2d_arr(cy_arr // 35, np.zeros_like(cy_arr)) % 30).astype(np.int32)
            row = cy_arr // blockH_base
            offX = np.where(row % 2 == 1, blockW_base // 3, 0)
            bx = (cx_arr + offX) % blockW_base
            by = cy_arr % blockH_base
            is_mortar = (bx < 3) | (by < 3)

            mv = 160 + (hash2d_arr(cx_arr // 3, cy_arr // 3) % 15).astype(np.float32)
            blockId = row * 50 + (cx_arr + offX) // blockW_base
            base = 135 + (hash2d_arr(blockId, np.full_like(blockId, 7)) % 40).astype(np.float32)
            noise_s = (hash2d_arr(cx_arr, cy_arr) % 10).astype(np.float32) - 5.0

            r = np.where(is_mortar, mv, np.clip(base + noise_s, 0, 255))
            g = np.where(is_mortar, mv - 3, np.clip(base + noise_s - 5, 0, 255))
            b = np.where(is_mortar, mv - 6, np.clip(base + noise_s - 8, 0, 255))

            h_field = np.where(is_mortar, 0.0, 0.5)
            h_field += (hash2d_arr(cx_arr // 5, cy_arr // 5) % 100).astype(np.float32) / 800.0
            rough[:] = 0.85

        else:
            # Stucco
            streak = vnoise(cx_arr * 0.005, cy_arr * 0.08, 222) * 15.0 - 7.0
            noise_s = (hash2d_arr(cx_arr, cy_arr) % 6).astype(np.float32) - 3.0
            v = np.clip(210 + streak + noise_s, 0, 255)
            r = np.minimum(255, v + 5)
            g = np.minimum(255, v + 2)
            b = v

            h_field = 0.1 + vnoise(cx_arr * 0.05, cy_arr * 0.05, 444) * 0.05
            rough[:] = 0.60

        # Weathering gradient
        weatherY = cy_arr.astype(np.float32) / cellH
        weatherDarken = weatherY * weatherY * 0.15
        stainNoise = vnoise(cx_arr * 0.01, cy_arr * 0.01, wallType * 100) * 0.08
        factor = np.clip(1.0 - weatherDarken - stainNoise, 0.5, 1.0)
        r = r * factor; g = g * factor; b = b * factor

        # Ground-floor dirt
        dirt_mask = weatherY > 0.85
        dirtAmount = np.clip((weatherY - 0.85) / 0.15, 0, 1)
        dirtFactor = 1.0 - dirtAmount * 0.18
        dirtNoise = vnoise(cx_arr * 0.04, cy_arr * 0.02, 9000 + wallType) * 0.06
        dirtFactor = dirtFactor - dirtNoise
        r = np.where(dirt_mask, np.clip(r * dirtFactor, 0, 255), r)
        g = np.where(dirt_mask, np.clip(g * dirtFactor, 0, 255), g)
        b = np.where(dirt_mask, np.clip(b * dirtFactor, 0, 255), b)

        # Color temperature
        if wallType == 0:
            r = np.minimum(255, r + 6); b = np.maximum(0, b - 3)
        elif wallType == 2:
            r = np.minimum(255, r + 5); g = np.minimum(255, g + 2); b = np.maximum(0, b - 5)

        # Window/frame override
        win_v = 35 + (hash2d_arr(cx_arr // 30, cy_arr // 30) % 15).astype(np.float32)
        frame_v = 185 + (hash2d_arr(cx_arr, cy_arr) % 10).astype(np.float32)

        r = np.where(is_window, win_v, np.where(is_frame, frame_v, r))
        g = np.where(is_window, win_v + 8, np.where(is_frame, frame_v, g))
        b = np.where(is_window, win_v + 20, np.where(is_frame, frame_v, b))

        # Roughness micro-variation
        rough += (hash2d_arr(cx_arr, cy_arr) % 100).astype(np.float32) / 2000.0 - 0.025

        # Write to atlas
        wall[cy0:cy0+cellH, cx0:cx0+cellW, 0] = np.clip(r, 0, 255).astype(np.uint8)
        wall[cy0:cy0+cellH, cx0:cx0+cellW, 1] = np.clip(g, 0, 255).astype(np.uint8)
        wall[cy0:cy0+cellH, cx0:cx0+cellW, 2] = np.clip(b, 0, 255).astype(np.uint8)
        wall_height[cy0:cy0+cellH, cx0:cx0+cellW] = h_field
        rv = np.clip((rough * 255).astype(np.int32), 0, 255).astype(np.uint8)
        wall_rough[cy0:cy0+cellH, cx0:cx0+cellW, 0] = rv
        wall_rough[cy0:cy0+cellH, cx0:cx0+cellW, 1] = rv
        wall_rough[cy0:cy0+cellH, cx0:cx0+cellW, 2] = rv

    # Normal map from height field
    wall_normal = np.zeros((atlasH, atlasW, 3), dtype=np.uint8)
    padded = np.pad(wall_height, 1, mode='edge')
    dx = padded[1:-1, 2:] - padded[1:-1, :-2]
    dy = padded[2:, 1:-1] - padded[:-2, 1:-1]
    nx = -dx * 4.0; ny = -dy * 4.0; nz = np.ones_like(nx)
    length = np.sqrt(nx*nx + ny*ny + nz*nz)
    nx /= length; ny /= length; nz /= length
    wall_normal[:,:,0] = np.clip((nx * 127.5 + 127.5).astype(np.int32), 0, 255)
    wall_normal[:,:,1] = np.clip((ny * 127.5 + 127.5).astype(np.int32), 0, 255)
    wall_normal[:,:,2] = np.clip((nz * 127.5 + 127.5).astype(np.int32), 0, 255)

    save_png(str(output_dir / 'building_wall.png'), wall)
    save_png(str(output_dir / 'building_wall_normal.png'), wall_normal)
    save_png(str(output_dir / 'building_wall_roughness.png'), wall_rough)

def generate_roof_textures(output_dir):
    """Generate roof atlas (1024x512) with 4 types in 2x2 grid."""
    cellW, cellH = 512, 256
    atlasW, atlasH = cellW * 2, cellH * 2

    roof = np.zeros((atlasH, atlasW, 3), dtype=np.uint8)
    roof_height = np.ones((atlasH, atlasW), dtype=np.float32)
    roof_rough = np.zeros((atlasH, atlasW, 3), dtype=np.uint8)

    roofBase = np.array([
        [75, 78, 82],     # Dark slate
        [155, 85, 55],    # Terracotta
        [75, 60, 48],     # Dark brown
        [175, 178, 180]   # Light grey/zinc
    ], dtype=np.float32)

    roofBaseRough = [0.85, 0.70, 0.80, 0.40]
    tileH, tileW = 20, 40

    for roofType in range(4):
        rx0 = (roofType % 2) * cellW
        ry0 = (roofType // 2) * cellH

        py, px = np.mgrid[0:cellH, 0:cellW]
        row = py // tileH
        offX = np.where(row % 2 == 1, tileW // 2, 0)
        tx = (px + offX) % tileW
        ty = py % tileH
        is_edge = (tx == 0) | (ty == 0)
        is_gap = (tx <= 1) | (ty <= 1)

        tileId = row * 50 + (px + offX) // tileW
        tileVar = (hash2d_arr(tileId, np.full_like(tileId, 42 + roofType)) % 25).astype(np.float32)
        noise = (hash2d_arr(px + 7777, py + 3333 + roofType * 1111) % 6).astype(np.float32) - 3.0

        rv = roofBase[roofType][0] + tileVar + noise
        gv = roofBase[roofType][1] + tileVar + noise
        bv = roofBase[roofType][2] + tileVar + noise
        rv = np.where(is_edge, rv - 15, rv)
        gv = np.where(is_edge, gv - 15, gv)
        bv = np.where(is_edge, bv - 15, bv)
        rv = np.where(is_gap, rv - 8, rv)
        gv = np.where(is_gap, gv - 8, gv)
        bv = np.where(is_gap, bv - 8, bv)

        # Weathering
        weatherY = py.astype(np.float32) / cellH
        weatherDarken = weatherY * weatherY * 0.12

        # Moss patches
        mossFactor = np.zeros_like(weatherY)
        moss_mask = weatherY > 0.6
        mossNoise = vnoise(px * 0.04, py * 0.04, 777 + roofType * 100)
        moss_active = moss_mask & (mossNoise > 0.6)
        mossFactor = np.where(moss_active,
                              np.minimum((mossNoise - 0.6) * 2.5 * (weatherY - 0.6) * 2.5, 0.4),
                              0.0)

        factor = 1.0 - weatherDarken
        rv = (rv * factor * (1.0 - mossFactor) + 45 * mossFactor)
        gv = (gv * factor * (1.0 - mossFactor) + 65 * mossFactor)
        bv = (bv * factor * (1.0 - mossFactor) + 30 * mossFactor)

        roof[ry0:ry0+cellH, rx0:rx0+cellW, 0] = np.clip(rv, 0, 255).astype(np.uint8)
        roof[ry0:ry0+cellH, rx0:rx0+cellW, 1] = np.clip(gv, 0, 255).astype(np.uint8)
        roof[ry0:ry0+cellH, rx0:rx0+cellW, 2] = np.clip(bv, 0, 255).astype(np.uint8)

        # Height field for normals
        h = np.ones((cellH, cellW), dtype=np.float32)
        h = np.where(tx <= 2, 0.0, np.where(tx <= 4, 0.5, h))
        h = np.where(ty <= 2, 0.0, np.where(ty <= 4, np.minimum(h, 0.5), h))
        roof_height[ry0:ry0+cellH, rx0:rx0+cellW] = h

        # Roughness
        rough = np.full((cellH, cellW), roofBaseRough[roofType], dtype=np.float32)
        rough += weatherY * 0.08
        rough = np.where((tx <= 2) | (ty <= 2), rough - 0.1, rough)
        rough += (hash2d_arr(px + 9999, py + 8888 + roofType * 2222) % 100).astype(np.float32) / 2000.0 - 0.025
        rv = np.clip((rough * 255).astype(np.int32), 0, 255).astype(np.uint8)
        roof_rough[ry0:ry0+cellH, rx0:rx0+cellW, 0] = rv
        roof_rough[ry0:ry0+cellH, rx0:rx0+cellW, 1] = rv
        roof_rough[ry0:ry0+cellH, rx0:rx0+cellW, 2] = rv

    # Roof normal map
    roof_normal = np.zeros((atlasH, atlasW, 3), dtype=np.uint8)
    padded = np.pad(roof_height, 1, mode='edge')
    dx = padded[1:-1, 2:] - padded[1:-1, :-2]
    dy = padded[2:, 1:-1] - padded[:-2, 1:-1]
    nx = -dx * 4.0; ny = -dy * 4.0; nz = np.ones_like(nx)
    length = np.sqrt(nx*nx + ny*ny + nz*nz)
    nx /= length; ny /= length; nz /= length
    roof_normal[:,:,0] = np.clip((nx * 127.5 + 127.5).astype(np.int32), 0, 255)
    roof_normal[:,:,1] = np.clip((ny * 127.5 + 127.5).astype(np.int32), 0, 255)
    roof_normal[:,:,2] = np.clip((nz * 127.5 + 127.5).astype(np.int32), 0, 255)

    save_png(str(output_dir / 'building_roof.png'), roof)
    save_png(str(output_dir / 'building_roof_normal.png'), roof_normal)
    save_png(str(output_dir / 'building_roof_roughness.png'), roof_rough)

def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    world_dir = Path(sys.argv[1])
    if not world_dir.exists():
        print(f"Error: {world_dir} not found")
        sys.exit(1)

    print(f"Generating building textures for {world_dir.name}...")
    print("  Wall textures (1024x1024)...")
    generate_wall_textures(world_dir)
    print("  Roof textures (1024x512)...")
    generate_roof_textures(world_dir)
    print("Done.")

if __name__ == '__main__':
    main()
