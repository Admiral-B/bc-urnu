"""Inspect a GLB/GLTF file and generate a starter boat.ini for Bridge Command.

Usage:
    python tools/glb_inspect.py path/to/ship.glb [options]

Options:
    --type ownship|othership   Ship type (default: ownship)
    --length-meters N          Real-world ship length in meters (for scale calculation)
    --output DIR               Write boat.ini to DIR/ instead of stdout
"""

import argparse
import math
import os
import struct
import sys

from pygltflib import GLTF2

# glTF component types -> struct format chars
COMPONENT_TYPE_MAP = {
    5120: 'b',   # BYTE
    5121: 'B',   # UNSIGNED_BYTE
    5122: 'h',   # SHORT
    5123: 'H',   # UNSIGNED_SHORT
    5125: 'I',   # UNSIGNED_INT
    5126: 'f',   # FLOAT
}

COMPONENT_SIZE = {
    5120: 1, 5121: 1, 5122: 2, 5123: 2, 5125: 4, 5126: 4,
}

TYPE_COUNT = {
    'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4,
    'MAT2': 4, 'MAT3': 9, 'MAT4': 16,
}


def decode_accessor(gltf, accessor_index):
    """Decode a glTF accessor into a list of tuples."""
    accessor = gltf.accessors[accessor_index]
    buffer_view = gltf.bufferViews[accessor.bufferView]
    blob = gltf.binary_blob()

    comp_fmt = COMPONENT_TYPE_MAP[accessor.componentType]
    comp_size = COMPONENT_SIZE[accessor.componentType]
    n_components = TYPE_COUNT[accessor.type]
    stride = buffer_view.byteStride or (comp_size * n_components)
    offset = (buffer_view.byteOffset or 0) + (accessor.byteOffset or 0)

    fmt = f'<{n_components}{comp_fmt}'
    fmt_size = struct.calcsize(fmt)
    results = []
    for i in range(accessor.count):
        start = offset + i * stride
        values = struct.unpack_from(fmt, blob, start)
        results.append(values)
    return results


def inspect_glb(filepath):
    """Parse GLB and return model info dict."""
    gltf = GLTF2.load(filepath)
    info = {
        'filepath': filepath,
        'filename': os.path.basename(filepath),
        'meshes': len(gltf.meshes) if gltf.meshes else 0,
        'materials': len(gltf.materials) if gltf.materials else 0,
        'nodes': len(gltf.nodes) if gltf.nodes else 0,
        'images': len(gltf.images) if gltf.images else 0,
        'animations': len(gltf.animations) if gltf.animations else 0,
    }

    # Compute global AABB and vertex count
    min_x = min_y = min_z = float('inf')
    max_x = max_y = max_z = float('-inf')
    total_verts = 0

    if gltf.meshes:
        for mesh in gltf.meshes:
            for prim in mesh.primitives:
                pos_idx = prim.attributes.POSITION
                if pos_idx is None:
                    continue
                positions = decode_accessor(gltf, pos_idx)
                total_verts += len(positions)
                for x, y, z in positions:
                    min_x = min(min_x, x)
                    min_y = min(min_y, y)
                    min_z = min(min_z, z)
                    max_x = max(max_x, x)
                    max_y = max(max_y, y)
                    max_z = max(max_z, z)

    info['total_vertices'] = total_verts
    info['min'] = (min_x, min_y, min_z)
    info['max'] = (max_x, max_y, max_z)

    if total_verts > 0:
        info['dims'] = (max_x - min_x, max_y - min_y, max_z - min_z)
    else:
        info['dims'] = (0, 0, 0)

    # PBR texture inventory
    pbr_info = []
    if gltf.materials:
        for mat in gltf.materials:
            mat_info = {'name': mat.name or '(unnamed)'}
            pbr = mat.pbrMetallicRoughness
            if pbr:
                mat_info['baseColor'] = pbr.baseColorTexture is not None
                mat_info['metallicRoughness'] = pbr.metallicRoughnessTexture is not None
                mat_info['baseColorFactor'] = list(pbr.baseColorFactor) if pbr.baseColorFactor else [1, 1, 1, 1]
                mat_info['roughnessFactor'] = pbr.roughnessFactor
                mat_info['metallicFactor'] = pbr.metallicFactor
            else:
                mat_info['baseColor'] = False
                mat_info['metallicRoughness'] = False
            mat_info['normal'] = mat.normalTexture is not None
            mat_info['occlusion'] = mat.occlusionTexture is not None
            mat_info['emissive'] = mat.emissiveTexture is not None
            mat_info['doubleSided'] = mat.doubleSided
            mat_info['alphaMode'] = mat.alphaMode
            pbr_info.append(mat_info)
    info['materials_detail'] = pbr_info

    return info


def print_report(info):
    """Print human-readable model report."""
    print(f"=== GLB Model Report: {info['filename']} ===\n")
    print(f"  Meshes: {info['meshes']}")
    print(f"  Materials: {info['materials']}")
    print(f"  Nodes: {info['nodes']}")
    print(f"  Images/Textures: {info['images']}")
    print(f"  Animations: {info['animations']}")
    print(f"  Total vertices: {info['total_vertices']:,}")

    if info['total_vertices'] > 0:
        mn = info['min']
        mx = info['max']
        dims = info['dims']
        print(f"\n  Bounding box:")
        print(f"    Min: ({mn[0]:.3f}, {mn[1]:.3f}, {mn[2]:.3f})")
        print(f"    Max: ({mx[0]:.3f}, {mx[1]:.3f}, {mx[2]:.3f})")
        print(f"    Dimensions (X x Y x Z): {dims[0]:.3f} x {dims[1]:.3f} x {dims[2]:.3f}")

        # Identify which axis is likely the ship length (longest horizontal)
        longest_h = max(dims[0], dims[2])
        beam = min(dims[0], dims[2])
        print(f"    Likely length: {longest_h:.2f}, beam: {beam:.2f}, height: {dims[1]:.2f}")

    print(f"\n  PBR Material Inventory:")
    has_any_pbr = False
    for mat in info['materials_detail']:
        maps = []
        if mat['baseColor']:
            maps.append('baseColor')
        if mat.get('metallicRoughness'):
            maps.append('metallicRoughness')
        if mat.get('normal'):
            maps.append('normal')
        if mat.get('occlusion'):
            maps.append('occlusion')
        if mat.get('emissive'):
            maps.append('emissive')
        if maps:
            has_any_pbr = True
        status = ', '.join(maps) if maps else 'no textures'
        alpha = f" [{mat['alphaMode']}]" if mat['alphaMode'] != 'OPAQUE' else ''
        ds = ' [double-sided]' if mat['doubleSided'] else ''
        print(f"    {mat['name']}: {status}{alpha}{ds}")

    if not has_any_pbr:
        print("\n  WARNING: No PBR textures found. Model may render with flat colors.")
    print()


def estimate_physics(length_m, beam_m, height_m):
    """Estimate ship physics from hull dimensions."""
    # Very rough heuristics based on typical cargo/passenger vessel data
    # Displacement ~ L * B * D * Cb * rho_water (Cb ~ 0.65 for general cargo)
    draft_m = height_m * 0.4  # assume draft is ~40% of total height
    cb = 0.65
    displacement_kg = length_m * beam_m * draft_m * cb * 1025.0
    mass = max(displacement_kg, 100000)

    # Propulsion force ~ mass * 0.05 (rough rule of thumb for ~15kn service speed)
    max_prop = mass * 0.05

    # Moment of inertia ~ mass * (L/4)^2 (simplified)
    inertia = mass * (length_m / 4.0) ** 2

    # Speed coefficients (empirical fit from existing boats)
    # DynamicsSpeedA ~ drag coefficient * wetted area
    speed_a = mass * 0.009
    speed_b = mass * 0.015

    # Rudder effectiveness scales with hull area
    rudder_a = mass * 0.04
    rudder_b = 4.0

    # Propeller spacing ~ 40% of beam
    prop_space = beam_m * 0.4

    return {
        'mass': int(mass),
        'max_prop': int(max_prop),
        'inertia': int(inertia),
        'speed_a': round(speed_a, 2),
        'speed_b': round(speed_b, 2),
        'rudder_a': int(rudder_a),
        'rudder_b': rudder_b,
        'prop_space': round(prop_space, 1),
        'draft': round(draft_m, 1),
    }


def generate_boat_ini(info, ship_type, length_meters=None):
    """Generate boat.ini content from model info."""
    dims = info['dims']
    mn = info['min']
    mx = info['max']

    # Determine which axis is ship length
    # In glTF (right-handed Y-up): typically Z is forward or X is forward
    # After WE Z-flip: +Z becomes forward (bow)
    # Pick the longest horizontal axis as length
    if dims[2] >= dims[0]:
        # Z is length in GLB space -> Z is length in WE space (just flipped sign)
        length_model = dims[2]
        beam_model = dims[0]
        # In WE space (post Z-flip): bow = -minZ_glb, stern = -maxZ_glb
        bow_z_we = -mn[2]
        stern_z_we = -mx[2]
    else:
        # X is length -- model is rotated 90 degrees, will need AngleCorrection
        length_model = dims[0]
        beam_model = dims[2]
        bow_z_we = -mn[2]
        stern_z_we = -mx[2]

    height_model = dims[1]

    # Scale factor
    if length_meters and length_model > 0:
        scale = length_meters / length_model
    else:
        scale = 1.0
        length_meters = length_model

    beam_meters = beam_model * scale
    height_meters = height_model * scale

    # YCorrection: offset so waterline is at Y=0
    # Assume waterline is at ~40% of total height from bottom
    waterline_y = mn[1] + height_model * 0.4
    y_correction = -waterline_y

    # Camera positions (in model coordinates, pre-scale)
    # Bridge: ~85% height, ~75% forward
    bridge_y = mn[1] + height_model * 0.85
    bridge_z_glb = mn[2] + dims[2] * 0.75  # 75% forward in GLB Z
    bridge_z_we = -bridge_z_glb  # flip for WE

    # Wing view: offset X by ~40% of beam
    wing_x = beam_model * 0.4
    wing_y = bridge_y - height_model * 0.02
    wing_z_we = bridge_z_we - dims[2] * 0.02

    # Overhead view
    overhead_y = mx[1] + height_model * 0.8
    overhead_z_we = -(mn[2] + dims[2] * 0.5)  # midship

    # Physics
    physics = estimate_physics(length_meters, beam_meters, height_meters)

    # Nav light positions (in model coordinates)
    # Masthead: top of mast, forward
    mast_y = mx[1] * 0.95
    mast_fwd_z_we = -(mn[2] + dims[2] * 0.7)
    # Sidelights: ~60% height, forward quarter, offset to sides
    side_y = mn[1] + height_model * 0.6
    side_z_we = -(mn[2] + dims[2] * 0.65)
    side_x = beam_model * 0.45
    # Stern light: low, aft
    stern_y = mn[1] + height_model * 0.5
    stern_z_we = -mx[2]  # very aft (max Z in GLB = stern)
    # Second masthead: slightly lower, aft
    mast2_y = mast_y * 0.9
    mast2_z_we = -(mn[2] + dims[2] * 0.3)

    lines = []
    lines.append(f'# Generated by glb_inspect.py -- review and adjust all values')
    lines.append(f'FileName="{info["filename"]}"')
    lines.append(f'ScaleFactor={scale:.6f}')
    lines.append(f'YCorrection={y_correction:.2f}')
    lines.append(f'AngleCorrection=0')
    lines.append(f'Depth={max(5, int(physics["draft"] + 2))}')
    lines.append(f'')

    if ship_type == 'ownship':
        lines.append(f'HasGPS=1')
        lines.append(f'HasDepthSounder=1')
        lines.append(f'MaxDepth=100')
        lines.append(f'HasRateOfTurnIndicator=1')
        lines.append(f'MakeTransparent=1')
        lines.append(f'')
        lines.append(f'Views=3')
        lines.append(f'ViewX(1)=0.0')
        lines.append(f'ViewY(1)={bridge_y:.1f}')
        lines.append(f'ViewZ(1)={bridge_z_we:.1f}')
        lines.append(f'ViewX(2)={wing_x:.1f}')
        lines.append(f'ViewY(2)={wing_y:.1f}')
        lines.append(f'ViewZ(2)={wing_z_we:.1f}')
        lines.append(f'ViewX(3)=0.0')
        lines.append(f'ViewY(3)={overhead_y:.1f}')
        lines.append(f'ViewZ(3)={overhead_z_we:.1f}')
        lines.append(f'ViewHigh(3)=1')
        lines.append(f'')

    lines.append(f'Max_propulsion_force={physics["max_prop"]}')
    lines.append(f'AsternEfficiency=0.667')
    lines.append(f'Mass={physics["mass"]}')
    lines.append(f'MaxRevs=100')
    lines.append(f'DynamicsSpeedA={physics["speed_a"]:.2f}')
    lines.append(f'DynamicsSpeedB={physics["speed_b"]:.2f}')
    lines.append(f'RudderA={physics["rudder_a"]}')
    lines.append(f'RudderB={physics["rudder_b"]}')
    lines.append(f'Inertia={physics["inertia"]}')
    lines.append(f'PropSpace={physics["prop_space"]}')
    lines.append(f'')

    if ship_type == 'othership':
        lines.append(f'NumberOfLights=5')
        # Masthead
        lines.append(f'LightX(1)=0.0')
        lines.append(f'LightY(1)={mast_y:.1f}')
        lines.append(f'LightZ(1)={mast_fwd_z_we:.1f}')
        lines.append(f'LightRange(1)=6')
        lines.append(f'LightRed(1)=255')
        lines.append(f'LightGreen(1)=255')
        lines.append(f'LightBlue(1)=255')
        lines.append(f'LightStartAngle(1)=-112.5')
        lines.append(f'LightEndAngle(1)=112.5')
        # Port (red)
        lines.append(f'LightX(2)={-side_x:.1f}')
        lines.append(f'LightY(2)={side_y:.1f}')
        lines.append(f'LightZ(2)={side_z_we:.1f}')
        lines.append(f'LightRange(2)=3')
        lines.append(f'LightRed(2)=255')
        lines.append(f'LightGreen(2)=0')
        lines.append(f'LightBlue(2)=0')
        lines.append(f'LightStartAngle(2)=-1')
        lines.append(f'LightEndAngle(2)=112.5')
        # Starboard (green)
        lines.append(f'LightX(3)={side_x:.1f}')
        lines.append(f'LightY(3)={side_y:.1f}')
        lines.append(f'LightZ(3)={side_z_we:.1f}')
        lines.append(f'LightRange(3)=3')
        lines.append(f'LightRed(3)=0')
        lines.append(f'LightGreen(3)=255')
        lines.append(f'LightBlue(3)=0')
        lines.append(f'LightStartAngle(3)=-112.5')
        lines.append(f'LightEndAngle(3)=-1')
        # Stern (white)
        lines.append(f'LightX(4)=0.0')
        lines.append(f'LightY(4)={stern_y:.1f}')
        lines.append(f'LightZ(4)={stern_z_we:.1f}')
        lines.append(f'LightRange(4)=3')
        lines.append(f'LightRed(4)=255')
        lines.append(f'LightGreen(4)=255')
        lines.append(f'LightBlue(4)=255')
        lines.append(f'LightStartAngle(4)=112.5')
        lines.append(f'LightEndAngle(4)=-112.5')
        # Second masthead
        lines.append(f'LightX(5)=0.0')
        lines.append(f'LightY(5)={mast2_y:.1f}')
        lines.append(f'LightZ(5)={mast2_z_we:.1f}')
        lines.append(f'LightRange(5)=6')
        lines.append(f'LightRed(5)=255')
        lines.append(f'LightGreen(5)=255')
        lines.append(f'LightBlue(5)=255')
        lines.append(f'LightStartAngle(5)=-112.5')
        lines.append(f'LightEndAngle(5)=112.5')

    return '\n'.join(lines) + '\n'


def main():
    parser = argparse.ArgumentParser(
        description='Inspect GLB/GLTF and generate Bridge Command boat.ini')
    parser.add_argument('glb_file', help='Path to .glb or .gltf file')
    parser.add_argument('--type', choices=['ownship', 'othership'],
                        default='ownship', help='Ship type (default: ownship)')
    parser.add_argument('--length-meters', type=float, default=None,
                        help='Real-world ship length in meters')
    parser.add_argument('--output', type=str, default=None,
                        help='Output directory for boat.ini')
    args = parser.parse_args()

    if not os.path.isfile(args.glb_file):
        print(f"Error: File not found: {args.glb_file}", file=sys.stderr)
        sys.exit(1)

    info = inspect_glb(args.glb_file)
    print_report(info)

    if info['total_vertices'] == 0:
        print("Error: No geometry found in model.", file=sys.stderr)
        sys.exit(1)

    ini_content = generate_boat_ini(info, args.type, args.length_meters)

    if args.output:
        os.makedirs(args.output, exist_ok=True)
        out_path = os.path.join(args.output, 'boat.ini')
        with open(out_path, 'w') as f:
            f.write(ini_content)
        print(f"Wrote {out_path}")
    else:
        print("=== Generated boat.ini ===\n")
        print(ini_content)


if __name__ == '__main__':
    main()
