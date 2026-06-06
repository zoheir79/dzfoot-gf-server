"""
Modular GLB Exporter — generates reusable body/hair/beard templates + separate textures.

Usage: python export_modular.py [--data-dir PATH] [--output-dir PATH]

Output structure:
  output/modular/
    bodies/
      body_thin.glb      (skeleton + body mesh + 17 animations)
      body_average.glb
      body_muscular.glb
      body_heavy.glb
    parts/
      hair_short.glb     (single mesh, no skeleton)
      hair_long.glb
      hair_mohawk.glb
      hair_curly.glb
      hair_ponytail.glb
      hair_bald.glb      (empty mesh)
      beard_none.glb     (empty mesh)
      beard_stubble.glb
      beard_short.glb
      beard_full.glb
    textures/
      skin_0.png .. skin_6.png   (7 skin tones)
      hair_0.png .. hair_7.png   (8 hair colors)
    modular_catalog.json

Client Android composes a player at runtime by:
  1. Loading the right body_X.glb (based on body_type)
  2. Attaching hair_*.glb and beard_*.glb to the "head" node
  3. Swapping skin_*.png and hair_*.png textures at draw time
"""
import os
import sys
import io
import json
import struct
import argparse
import numpy as np
from PIL import Image
from typing import Tuple, List

sys.path.insert(0, os.path.dirname(__file__))

from parse_ase import parse_all_models
from parse_anim import parse_all_animations, select_best_animations
from parse_object import build_skeleton_from_object, BODY_PART_ORDER, BODY_PART_INDEX
from write_glb import GLBBuilder, pack_glb
from customize import (
    generate_hair_mesh, generate_beard_mesh,
    generate_skin_texture, SKIN_TONES, HAIR_COLORS
)
from export_all import (
    find_data_dir, load_shared_data, build_body_mesh_data,
    _submesh_material_category, load_bmp_as_png_bytes,
    load_image_as_png_bytes
)


# ─── Body Type Scaling ───────────────────────────────────────────
BODY_TYPE_SCALES = {
    'thin':      (0.92, 1.0, 0.92),
    'average':   (1.00, 1.0, 1.00),
    'muscular':  (1.06, 1.0, 1.04),
    'heavy':     (1.08, 1.0, 1.10),
}

# ─── Hair / Beard Style Names ────────────────────────────────────
HAIR_STYLES = ['short', 'long', 'mohawk', 'curly', 'ponytail', 'bald']
BEARD_STYLES = ['none', 'stubble', 'short', 'full']


def _map_hair_color_to_idx():
    """Map hair color name to index 0..7."""
    names = list(HAIR_COLORS.keys())
    return {name: i for i, name in enumerate(names)}


def _map_skin_tone_to_idx():
    """Map skin tone name to index 0..6."""
    names = list(SKIN_TONES.keys())
    return {name: i for i, name in enumerate(names)}


def build_body_template_glb(body_type: str, skeleton, bone_meshes,
                            selected_anims, data_dir: str) -> bytes:
    """Build a body GLB with skeleton, meshes, animations. No hair/beard."""
    builder = GLBBuilder()

    # ── Load base textures (kit, shoe — shared across all bodies) ──
    kit_png = load_bmp_as_png_bytes(data_dir, 'kit_template.bmp') if data_dir else None
    shoe_png = load_bmp_as_png_bytes(data_dir, 'shoe.bmp') if data_dir else None

    kit_tex_idx = builder.add_image(kit_png) if kit_png else -1
    shoe_tex_idx = builder.add_image(shoe_png) if shoe_png else -1

    # ── Materials (no skin texture embedded — will be swapped at runtime) ──
    skin_mat = builder.add_material('skin', [0.82, 0.65, 0.50, 1.0])
    head_skin_mat = builder.add_material('head_skin', [1.0, 1.0, 1.0, 1.0])
    kit_upper_mat = builder.add_material('kit_upper', [0.95, 0.95, 0.95, 1.0], texture_idx=kit_tex_idx)
    kit_lower_mat = builder.add_material('kit_lower', [0.85, 0.85, 0.85, 1.0], texture_idx=kit_tex_idx)
    shoe_mat = builder.add_material('shoe', [0.85, 0.85, 0.85, 1.0], texture_idx=shoe_tex_idx)

    mat_map = {
        'skin': skin_mat,
        'head_skin': head_skin_mat,
        'kit_upper': kit_upper_mat,
        'kit_lower': kit_lower_mat,
        'shoe': shoe_mat,
    }

    # ── Skeleton nodes ──
    bone_nodes = builder.add_skeleton_nodes(skeleton)

    # ── Apply body type scale ──
    sx, sy, sz = BODY_TYPE_SCALES[body_type]
    for bp_name in ['body', 'middle']:
        for i, b in enumerate(skeleton):
            if b.name == bp_name:
                builder.gltf['nodes'][bone_nodes[i]]['scale'] = [sx, sy, sz]
                break

    # ── Assign meshes to bones ──
    for bone_name, submeshes in bone_meshes.items():
        if bone_name not in BODY_PART_INDEX:
            continue
        bone_idx = BODY_PART_INDEX[bone_name]
        node_idx = bone_nodes[bone_idx]

        primitives = []
        for sub in submeshes:
            mat_idx = mat_map.get(sub['category'], skin_mat)
            prim = builder.add_primitive_geometry(
                sub['verts'], sub['faces'],
                sub.get('normals'), sub.get('uvs'),
                material_idx=mat_idx
            )
            primitives.append(prim)

        if primitives:
            mesh_idx = builder.add_mesh(f'mesh_{bone_name}', primitives)
            builder.gltf['nodes'][node_idx]['mesh'] = mesh_idx

    # ── Skin + IBM ──
    ibm_acc = builder.add_inverse_bind_matrices(skeleton)
    skin_idx = builder.add_skin(bone_nodes, ibm_acc)
    # Attach skin to root node (player)
    for i, b in enumerate(skeleton):
        if b.parent_index < 0:
            builder.gltf['nodes'][bone_nodes[i]]['skin'] = skin_idx
            break

    # ── Animations ──
    skeleton_names = [b.name for b in skeleton]
    for anim_id in range(17):
        clip = selected_anims.get(anim_id)
        if clip:
            builder.add_animation(f"anim_{anim_id:02d}", clip,
                                skeleton_names, bone_nodes)

    return builder.finalize()


def build_part_glb(part_name: str, verts: List, faces: List,
                   material_name: str = 'hair',
                   base_color: List = None) -> bytes:
    """Build a standalone mini-GLB for a hair/beard part.
    Single mesh, no skeleton, no animation. Positioned in head local space."""
    builder = GLBBuilder()

    if not verts or not faces:
        # Empty mesh (e.g. bald, none)
        # Still create a valid GLB with an empty node
        builder.add_node(part_name)
        builder.gltf['scenes'][0]['nodes'] = [0]
        return builder.finalize()

    color = base_color or [1.0, 1.0, 1.0, 1.0]
    mat_idx = builder.add_material(material_name, color)

    prim = builder.add_primitive_geometry(verts, faces,
                                           material_idx=mat_idx)
    mesh_idx = builder.add_mesh(part_name, [prim])

    # Single node at origin — the client will parent this to the "head" bone
    node_idx = builder.add_node(part_name, mesh_idx=mesh_idx)
    builder.gltf['scenes'][0]['nodes'] = [node_idx]

    return builder.finalize()


def generate_skin_texture_png(skin_tone: str, resolution: int = 256) -> bytes:
    """Generate a skin texture as PNG bytes for a given tone."""
    arr = generate_skin_texture(skin_tone, 'brown', 'none', 'black', resolution)
    img = Image.fromarray(arr)
    buf = io.BytesIO()
    img.save(buf, format='PNG')
    return buf.getvalue()


def export_modular(data_dir: str, output_dir: str):
    """Main export pipeline for modular avatar system."""
    print("=" * 60)
    print("  MODULAR GLB EXPORTER")
    print("=" * 60)

    out = os.path.join(output_dir, 'modular')
    os.makedirs(out, exist_ok=True)
    os.makedirs(os.path.join(out, 'bodies'), exist_ok=True)
    os.makedirs(os.path.join(out, 'parts'), exist_ok=True)
    os.makedirs(os.path.join(out, 'textures'), exist_ok=True)

    # ── 1. Load shared data ──
    print("\n[1/5] Loading GameplayFootball assets...")
    models, skeleton, selected_anims = load_shared_data(data_dir)
    body_data = build_body_mesh_data(models, skeleton)

    # ── 2. Export body templates (4 body types) ──
    print("\n[2/5] Exporting body templates...")
    for body_type in ['thin', 'average', 'muscular', 'heavy']:
        glb = build_body_template_glb(body_type, skeleton, body_data,
                                       selected_anims, data_dir)
        path = os.path.join(out, 'bodies', f'body_{body_type}.glb')
        with open(path, 'wb') as f:
            f.write(glb)
        print(f"  body_{body_type}.glb  ({len(glb)/1024:.1f} KB)")

    # ── 3. Export hair parts (6 styles) ──
    print("\n[3/5] Exporting hair parts...")
    for style in HAIR_STYLES:
        verts, faces, _ = generate_hair_mesh(style)
        glb = build_part_glb(f'hair_{style}', verts, faces, 'hair')
        path = os.path.join(out, 'parts', f'hair_{style}.glb')
        with open(path, 'wb') as f:
            f.write(glb)
        print(f"  hair_{style}.glb  ({len(glb)/1024:.1f} KB)")

    # ── 4. Export beard parts (4 styles) ──
    print("\n[4/5] Exporting beard parts...")
    for style in BEARD_STYLES:
        verts, faces, _ = generate_beard_mesh(style)
        # Beard uses hair material (same texture family)
        glb = build_part_glb(f'beard_{style}', verts, faces, 'beard')
        path = os.path.join(out, 'parts', f'beard_{style}.glb')
        with open(path, 'wb') as f:
            f.write(glb)
        print(f"  beard_{style}.glb  ({len(glb)/1024:.1f} KB)")

    # ── 5. Export shared textures from GF assets (kit, shoe) ──
    print("\n[5/5] Exporting shared textures from GF assets...")
    shared_textures = {
        'kit_template.bmp': 'kit_template.png',
        'shoe.bmp': 'shoe.png',
    }
    tex_src_dir = os.path.join(data_dir, 'media', 'objects', 'players', 'textures') if data_dir else None
    for src_name, dst_name in shared_textures.items():
        src_path = os.path.join(tex_src_dir, src_name) if tex_src_dir else None
        dst_path = os.path.join(out, 'textures', dst_name)
        if src_path and os.path.exists(src_path):
            img = Image.open(src_path)
            img.save(dst_path, 'PNG')
            print(f"  {dst_name}  ({img.size[0]}x{img.size[1]} from {src_name})")
        else:
            # Fallback: generate procedural placeholder
            arr = np.full((256, 256, 4), 255, dtype=np.uint8)
            arr[:, :, 0] = 200  # light grey
            arr[:, :, 1] = 200
            arr[:, :, 2] = 200
            img = Image.fromarray(arr)
            img.save(dst_path)
            print(f"  {dst_name}  (procedural fallback, {src_name} not found)")

    # ── 6. Export skin textures (7 tones) ──
    print("\n[6/5] Exporting skin textures...")
    skin_names = list(SKIN_TONES.keys())
    for i, tone in enumerate(skin_names):
        arr = generate_skin_texture(tone, 'brown', 'none', 'black', resolution=256)
        img = Image.fromarray(arr)
        path = os.path.join(out, 'textures', f'skin_{i}.png')
        img.save(path)
        print(f"  skin_{i}.png  ({tone})")

    # ── 6. Export hair color textures (8 colors) ──
    print("\n[6/5] Exporting hair color textures...")
    hair_names = list(HAIR_COLORS.keys())
    for i, color_name in enumerate(hair_names):
        rgba = HAIR_COLORS[color_name]
        # Create a solid-color 64x64 texture
        arr = np.full((64, 64, 4), 255, dtype=np.uint8)
        for c in range(3):
            arr[:, :, c] = int(rgba[c] * 255)
        arr[:, :, 3] = 255
        img = Image.fromarray(arr)
        path = os.path.join(out, 'textures', f'hair_{i}.png')
        img.save(path)
        print(f"  hair_{i}.png  ({color_name})")

    # ── 7. Generate catalog ──
    catalog = {
        "version": "2.0",
        "body_types": {k: f"bodies/body_{k}.glb" for k in BODY_TYPE_SCALES},
        "hair_styles": {s: f"parts/hair_{s}.glb" for s in HAIR_STYLES},
        "beard_styles": {s: f"parts/beard_{s}.glb" for s in BEARD_STYLES},
        "skin_textures": {i: f"textures/skin_{i}.png" for i in range(len(skin_names))},
        "hair_textures": {i: f"textures/hair_{i}.png" for i in range(len(hair_names))},
        "shared_textures": {
            "kit_template": "textures/kit_template.png",
            "shoe": "textures/shoe.png",
        },
        "skin_names": skin_names,
        "hair_names": hair_names,
    }
    cat_path = os.path.join(out, 'modular_catalog.json')
    with open(cat_path, 'w') as f:
        json.dump(catalog, f, indent=2)
    print(f"\n  Catalog: {cat_path}")

    # ── Summary ──
    total_kb = 0
    for root, _, files in os.walk(out):
        for fname in files:
            fpath = os.path.join(root, fname)
            total_kb += os.path.getsize(fpath) / 1024.0

    print("\n" + "=" * 60)
    print(f"  MODULAR EXPORT COMPLETE — {total_kb:.0f} KB total")
    print("=" * 60)
    print(f"\n  Assets:")
    print(f"    4 body templates  (skeleton + mesh + 17 animations)")
    print(f"    6 hair parts      (standalone meshes)")
    print(f"    4 beard parts     (standalone meshes)")
    print(f"    7 skin textures   (PNG, 256x256)")
    print(f"    8 hair textures   (PNG, 64x64)")
    print(f"\n  Android loads: body_X.glb + hair_Y.glb + beard_Z.glb")
    print(f"  Android swaps:  skin_I.png + hair_J.png at runtime")


def main():
    parser = argparse.ArgumentParser(description='Modular GLB Template Exporter')
    parser.add_argument('--data-dir', type=str, help='Path to GameplayFootball/data')
    parser.add_argument('--output-dir', type=str, default='./output')
    args = parser.parse_args()

    data_dir = args.data_dir or find_data_dir()
    if not data_dir:
        print("ERROR: Could not find GameplayFootball data directory. Use --data-dir PATH")
        sys.exit(1)

    export_modular(data_dir, args.output_dir)


if __name__ == '__main__':
    main()
