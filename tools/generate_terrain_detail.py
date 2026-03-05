#!/usr/bin/env python3
"""Generate terrain_detail.png for a Bridge Command world directory.

Produces a small (256x256) tileable grayscale detail texture that adds
micro-surface variation to the satellite terrain texture via the
OCCLUSIONMAP slot in WickedTerrainNode.

Values range 0.7-1.0 (180-255 uint8) to add subtle darkening without
washing out the base color.

Usage: python generate_terrain_detail.py <world_dir>
"""

import sys, os
import numpy as np
from pathlib import Path

def make_tileable_noise(size, octaves=4, seed=42):
    """Generate tileable Perlin-like noise via spectral synthesis."""
    rng = np.random.RandomState(seed)
    result = np.zeros((size, size), dtype=np.float32)
    for octave in range(octaves):
        freq = 2 ** octave
        amp = 0.5 ** octave
        # Random phase and direction for each octave
        phase = rng.uniform(0, 2 * np.pi, (freq, freq)).astype(np.float32)
        # Create base pattern at this frequency
        base = rng.uniform(-1, 1, (freq, freq)).astype(np.float32)
        # Tile it to full size using bicubic-like interpolation
        y = np.linspace(0, freq, size, endpoint=False)
        x = np.linspace(0, freq, size, endpoint=False)
        yy, xx = np.meshgrid(y, x, indexing='ij')
        iy = yy.astype(np.int32) % freq
        ix = xx.astype(np.int32) % freq
        fy = yy - np.floor(yy)
        fx = xx - np.floor(xx)
        # Smoothstep
        fy = fy * fy * (3 - 2 * fy)
        fx = fx * fx * (3 - 2 * fx)
        # Bilinear interpolation with wrapping
        iy1 = (iy + 1) % freq
        ix1 = (ix + 1) % freq
        v00 = base[iy, ix]
        v10 = base[iy, ix1]
        v01 = base[iy1, ix]
        v11 = base[iy1, ix1]
        interp = v00 * (1-fx) * (1-fy) + v10 * fx * (1-fy) + v01 * (1-fx) * fy + v11 * fx * fy
        result += interp * amp
    # Normalize to 0-1
    result = (result - result.min()) / (result.max() - result.min() + 1e-8)
    return result

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

def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    world_dir = Path(sys.argv[1])
    size = 256

    print(f"Generating terrain detail texture ({size}x{size})...")

    # Combine multiple noise patterns for natural look
    noise1 = make_tileable_noise(size, octaves=4, seed=42)  # broad variation
    noise2 = make_tileable_noise(size, octaves=6, seed=137)  # fine grass detail

    # Combine: broad + fine detail
    combined = noise1 * 0.6 + noise2 * 0.4

    # Map to 0.70-1.0 range (subtle occlusion, never too dark)
    detail = 0.70 + combined * 0.30
    detail_u8 = np.clip((detail * 255).astype(np.int32), 0, 255).astype(np.uint8)

    # RGB grayscale
    out = np.stack([detail_u8, detail_u8, detail_u8], axis=-1)
    save_png(str(world_dir / 'terrain_detail.png'), out)
    print("Done.")

if __name__ == '__main__':
    main()
