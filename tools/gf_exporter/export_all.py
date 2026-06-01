"""
Main GF Exporter — converts GameplayFootball assets to GLB with customizations.
Usage: python export_all.py [--data-dir PATH] [--output-dir PATH]
"""
import os
import sys
import argparse
import json
import struct
import io
import re
import numpy as np
from typing import Optional, List, Tuple
from pathlib import Path

sys.path.insert(0, os.path.dirname(__file__))

from parse_ase import parse_all_models, parse_ase, ASEMesh
from parse_anim import parse_all_animations, select_best_animations, AnimClip
from parse_object import build_skeleton_from_object, BODY_PART_ORDER, BONE_PARENTS, BODY_PART_INDEX, axis_angle_to_quaternion
from write_glb import GLBBuilder
from customize import (
    generate_hair_mesh, generate_ears,
    generate_fingers, generate_face_features,
    generate_skin_texture, generate_beard_mesh,
    SKIN_TONES, HAIR_COLORS
)
# playermodifier_loader is imported lazily inside build_avatar_glb


def _submesh_material_category(submesh_name: str, bone_name: str) -> str:
    """Map ASE submesh name to material category for Android kit replacement."""
    n = submesh_name.lower()
    if 'shirt' in n:
        return 'kit_upper'
    if 'trunks' in n:
        return 'kit_lower'
    if 'sock' in n:
        return 'kit_lower'
    if 'shoe' in n or 'sole' in n or 'plane' in n:
        return 'shoe'
    # Single-mesh bones: body/middle are torso (kit upper)
    if bone_name in ('body', 'middle'):
        return 'kit_upper'
    if 'head' in n:
        return 'head_skin'
    return 'skin'
from avatar_combinations import (
    PRESET_AVATARS, AvatarConfig, avatar_to_dict, export_avatar_catalog,
    generate_all_combinations
)
from coords import vec3_zup_to_yup, quat_zup_to_yup, normalize_quat, axis_zup_to_yup


def find_data_dir():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    candidates = [
        os.path.join(script_dir, '..', '..', 'GameplayFootball', 'data'),
        os.path.join(script_dir, '..', 'GameplayFootball', 'data'),
        os.path.join(script_dir, '..', '..', '..', 'GameplayFootball', 'data'),
    ]
    for c in candidates:
        if os.path.isdir(c):
            return os.path.abspath(c)
    return None


def mirror_animation(clip: AnimClip) -> AnimClip:
    import copy
    mirrored = copy.deepcopy(clip)
    mirrored.name = clip.name + "_mirror"
    swap_pairs = [
        ('left_thigh', 'right_thigh'), ('left_knee', 'right_knee'),
        ('left_ankle', 'right_ankle'), ('left_shoulder', 'right_shoulder'),
        ('left_elbow', 'right_elbow'),
    ]
    for left, right in swap_pairs:
        if left in mirrored.tracks and right in mirrored.tracks:
            mirrored.tracks[left], mirrored.tracks[right] = mirrored.tracks[right], mirrored.tracks[left]
    for track in mirrored.tracks.values():
        for kf in track.keyframes:
            qx, qy, qz, qw = kf.rotation
            kf.rotation = (-qx, qy, qz, qw)
    return mirrored


# ─── Shared data loading ─────────────────────────────────────────

def load_shared_data(data_dir: str):
    """Load models, skeleton, and animations once, shared by all exports."""
    models_dir = os.path.join(data_dir, 'media', 'objects', 'players', 'models')
    object_path = os.path.join(data_dir, 'media', 'objects', 'players', 'player.object')
    anim_dir = os.path.join(data_dir, 'media', 'animations')

    print("  Parsing .ase model files...")
    models = parse_all_models(models_dir)

    print("  Building skeleton from player.object...")
    skeleton = build_skeleton_from_object(object_path)

    print("  Parsing animation files...")
    all_anims = parse_all_animations(anim_dir)
    selected_anims = select_best_animations(all_anims)

    mirror_map = {4: 5, 15: 14}  # Shoot R -> L, GK Dive R -> L
    for src_id, dst_id in mirror_map.items():
        if dst_id not in selected_anims and src_id in selected_anims:
            selected_anims[dst_id] = mirror_animation(selected_anims[src_id])
    fallback_map = {
        2: 3,
        13: 0,
        16: 8,
        14: 16,
        15: 14,
    }
    for dst_id, src_id in fallback_map.items():
        if dst_id not in selected_anims and src_id in selected_anims:
            selected_anims[dst_id] = selected_anims[src_id]
    for missing_id in range(17):
        if missing_id not in selected_anims and 0 in selected_anims:
            selected_anims[missing_id] = selected_anims[0]

    return models, skeleton, selected_anims


def resolve_asset_path(data_dir: str, asset_ref: str, base_dir: Optional[str] = None,
                       extension_candidates: Optional[List[str]] = None) -> Optional[str]:
    if not asset_ref:
        return None

    normalized = asset_ref.strip().strip('"').replace('\\', os.sep).replace('/', os.sep)
    candidates = []

    def add_candidate(path: Optional[str]):
        if not path:
            return
        norm = os.path.normpath(path)
        if norm not in candidates:
            candidates.append(norm)

    if os.path.isabs(normalized):
        add_candidate(normalized)

    if base_dir:
        add_candidate(os.path.join(base_dir, normalized))
        add_candidate(os.path.join(base_dir, os.path.basename(normalized)))

    add_candidate(os.path.join(data_dir, normalized))
    add_candidate(os.path.join(data_dir, 'media', normalized))

    expanded = []
    ext_candidates = [ext.lower() for ext in (extension_candidates or [])]
    for candidate in candidates:
        if candidate not in expanded:
            expanded.append(candidate)
        stem, ext = os.path.splitext(candidate)
        if ext_candidates:
            for candidate_ext in ext_candidates:
                with_ext = stem + candidate_ext
                if with_ext not in expanded:
                    expanded.append(with_ext)

    for candidate in expanded:
        if os.path.isfile(candidate):
            return os.path.abspath(candidate)

        folder = os.path.dirname(candidate)
        stem = os.path.splitext(os.path.basename(candidate))[0].lower()
        if not os.path.isdir(folder):
            continue

        for entry in os.listdir(folder):
            entry_path = os.path.join(folder, entry)
            if not os.path.isfile(entry_path):
                continue
            entry_stem, entry_ext = os.path.splitext(entry)
            if entry_stem.lower() != stem:
                continue
            if ext_candidates and entry_ext.lower() not in ext_candidates:
                continue
            return os.path.abspath(entry_path)

    return None


def load_image_as_png_bytes(data_dir: str, image_ref: str, base_dir: Optional[str] = None) -> Optional[bytes]:
    image_path = resolve_asset_path(
        data_dir,
        image_ref,
        base_dir=base_dir,
        extension_candidates=['.png', '.bmp', '.jpg', '.jpeg']
    )
    if not image_path:
        return None

    try:
        from PIL import Image
        img = Image.open(image_path)
        if img.mode != 'RGBA':
            img = img.convert('RGBA')
        buf = io.BytesIO()
        img.save(buf, format='PNG')
        return buf.getvalue()
    except Exception as e:
        print(f"  [ERROR] Failed to convert texture {image_ref}: {e}")
        return None


def load_bmp_as_png_bytes(data_dir: str, rel_path: str) -> Optional[bytes]:
    """Load a BMP texture and convert it to PNG bytes for glTF compatibility."""
    base_dir = os.path.join(data_dir, 'media', 'objects', 'players', 'textures')
    return load_image_as_png_bytes(data_dir, rel_path, base_dir=base_dir)


def _map_hair_style_to_gf(style: str) -> str:
    mapping = {
        'bald': 'bald',
        'short': 'short01',
        'curly': 'short02',
        'mohawk': 'medium01',
        'long': 'long01',
        'ponytail': 'long02',
    }
    return mapping.get(style, 'short01')


def _map_hair_color_to_gf(color: str) -> str:
    mapping = {
        'black': 'black',
        'dark_brown': 'brown',
        'brown': 'brown',
        'light_brown': 'darkblonde',
        'blonde': 'blonde',
        'red': 'red',
        'grey': 'darkblonde',
        'white': 'blonde',
    }
    return mapping.get(color, 'brown')


def load_gf_hairstyle_mesh(data_dir: str, hair_style: str):
    gf_style = _map_hair_style_to_gf(hair_style)
    if gf_style == 'bald':
        return [], [], None, None

    ase_path = os.path.join(data_dir, 'media', 'objects', 'players', 'hairstyles', f'{gf_style}.ase')
    if not os.path.isfile(ase_path):
        return [], [], None, None

    meshes = parse_ase(ase_path)
    verts = []
    faces = []
    normals = []
    uvs = []
    vert_offset = 0

    for mesh in meshes:
        local_verts = [vec3_zup_to_yup(v) for v in mesh.vertices]
        verts.extend(local_verts)
        faces.extend([(a + vert_offset, b + vert_offset, c + vert_offset) for a, b, c in mesh.faces])

        if mesh.normals and len(mesh.normals) == len(mesh.vertices):
            normals.extend([vec3_zup_to_yup(n) for n in mesh.normals])
        else:
            normals.extend([(0.0, 1.0, 0.0)] * len(mesh.vertices))

        if mesh.uvs and len(mesh.uvs) == len(mesh.vertices):
            uvs.extend(list(mesh.uvs))
        else:
            uvs.extend([(0.0, 0.0)] * len(mesh.vertices))

        vert_offset += len(mesh.vertices)

    return verts, faces, normals, uvs


def parse_generic_object_geometries(object_path: str) -> List[dict]:
    with open(object_path, 'r', encoding='utf-8', errors='replace') as f:
        content = f.read()

    if '<node>' in content:
        return []

    blocks = re.findall(r'<geometry>(.*?)</geometry>', content, flags=re.IGNORECASE | re.DOTALL)
    geometries = []

    def extract_tag(block: str, tag: str) -> str:
        match = re.search(rf'<{tag}>\s*([^<]+?)\s*</{tag}>', block, flags=re.IGNORECASE | re.DOTALL)
        return match.group(1).strip() if match else ''

    def parse_vec3(text: str) -> Tuple[float, float, float]:
        parts = [p.strip() for p in text.split(',')]
        if len(parts) < 3:
            return (0.0, 0.0, 0.0)
        try:
            return (float(parts[0]), float(parts[1]), float(parts[2]))
        except ValueError:
            return (0.0, 0.0, 0.0)

    def parse_axis_angle(text: str) -> Tuple[float, float, float, float]:
        parts = [p.strip() for p in text.split(',')]
        if len(parts) < 4:
            return (0.0, 0.0, 0.0, 0.0)
        try:
            return (float(parts[0]), float(parts[1]), float(parts[2]), float(parts[3]))
        except ValueError:
            return (0.0, 0.0, 0.0, 0.0)

    for index, block in enumerate(blocks):
        filename = extract_tag(block, 'filename')
        if not filename:
            continue
        name = extract_tag(block, 'name') or f'geometry_{index:02d}'
        position = parse_vec3(extract_tag(block, 'position'))
        rotation = parse_axis_angle(extract_tag(block, 'rotation'))
        geometries.append({
            'filename': filename,
            'name': name,
            'position': position,
            'rotation': rotation,
        })

    return geometries


def axis_angle_zup_to_yup_quat(rotation: Tuple[float, float, float, float]) -> Tuple[float, float, float, float]:
    angle, ax, ay, az = rotation
    ax, ay, az = axis_zup_to_yup(ax, ay, az)
    return axis_angle_to_quaternion(angle, ax, ay, az)


def attach_child_node(builder: GLBBuilder, parent_idx: int, child_idx: int):
    parent = builder.gltf['nodes'][parent_idx]
    if 'children' not in parent:
        parent['children'] = []
    parent['children'].append(child_idx)


def build_static_object_glb(data_dir: str, object_path: str) -> Optional[bytes]:
    object_dir = os.path.dirname(object_path)
    geometries = parse_generic_object_geometries(object_path)
    if not geometries:
        return None

    builder = GLBBuilder()
    root_name = Path(object_path).stem
    root_node_idx = builder.add_node(root_name)
    builder.gltf['scenes'][0]['nodes'].append(root_node_idx)

    mesh_cache = {}
    material_cache = {}

    def get_material(texture_ref: str) -> int:
        cache_key = texture_ref or '__default__'
        if cache_key in material_cache:
            return material_cache[cache_key]

        texture_png = load_image_as_png_bytes(data_dir, texture_ref, base_dir=object_dir) if texture_ref else None
        texture_idx = builder.add_image(texture_png) if texture_png else -1
        material_name = Path(texture_ref).stem if texture_ref else 'default'
        material_idx = builder.add_material(material_name, [1.0, 1.0, 1.0, 1.0], texture_idx=texture_idx)
        material_cache[cache_key] = material_idx
        return material_idx

    for geom in geometries:
        ase_path = resolve_asset_path(data_dir, geom['filename'], base_dir=object_dir, extension_candidates=['.ase'])
        if not ase_path:
            print(f"  [WARN] Missing geometry file for {object_path}: {geom['filename']}")
            continue

        if ase_path not in mesh_cache:
            mesh_cache[ase_path] = parse_all_models(os.path.dirname(ase_path)).get(Path(ase_path).stem, [])
        ase_meshes = mesh_cache[ase_path]

        geom_translation = vec3_zup_to_yup(geom['position'])
        geom_rotation = axis_angle_zup_to_yup_quat(geom['rotation'])
        geom_node_idx = builder.add_node(geom['name'], translation=geom_translation, rotation=geom_rotation)
        attach_child_node(builder, root_node_idx, geom_node_idx)

        for mesh_index, ase_mesh in enumerate(ase_meshes):
            vertices = [vec3_zup_to_yup(v) for v in ase_mesh.vertices]
            normals = [vec3_zup_to_yup(n) for n in ase_mesh.normals] if ase_mesh.normals else None
            uvs = ase_mesh.uvs if ase_mesh.uvs else None
            material_idx = get_material(ase_mesh.texture_path)
            prim = builder.add_primitive_geometry(
                vertices,
                ase_mesh.faces,
                normals,
                uvs,
                material_idx=material_idx
            )
            mesh_name = f"{geom['name']}_{mesh_index:02d}"
            mesh_idx = builder.add_mesh(mesh_name, [prim])
            mesh_node_idx = builder.add_node(mesh_name, mesh_idx=mesh_idx)
            attach_child_node(builder, geom_node_idx, mesh_node_idx)

    return builder.finalize()


def export_static_objects(data_dir: str, output_dir: str):
    print("\n" + "=" * 60)
    print("  STEP 3: Exporting reusable static objects")
    print("=" * 60)

    objects_root = os.path.join(data_dir, 'media', 'objects')
    static_dir = os.path.join(output_dir, 'static')
    os.makedirs(static_dir, exist_ok=True)

    exported = []
    skipped = []

    for root, _, files in os.walk(objects_root):
        for fname in sorted(files):
            if not fname.endswith('.object'):
                continue

            object_path = os.path.join(root, fname)
            rel_object = os.path.relpath(object_path, objects_root)
            geometries = parse_generic_object_geometries(object_path)
            if not geometries:
                skipped.append(rel_object)
                continue

            glb_data = build_static_object_glb(data_dir, object_path)
            if not glb_data:
                skipped.append(rel_object)
                continue

            rel_no_ext = os.path.splitext(rel_object)[0] + '.glb'
            output_path = os.path.join(static_dir, rel_no_ext)
            os.makedirs(os.path.dirname(output_path), exist_ok=True)
            with open(output_path, 'wb') as f:
                f.write(glb_data)

            exported.append({
                'source_object': rel_object.replace('\\', '/'),
                'output_glb': os.path.relpath(output_path, output_dir).replace('\\', '/'),
                'geometry_count': len(geometries),
                'size_kb': round(len(glb_data) / 1024.0, 1),
            })
            print(f"  [STATIC] {rel_object} -> {rel_no_ext} ({len(glb_data)/1024:.1f} KB)")

    catalog_path = os.path.join(static_dir, 'catalog.json')
    with open(catalog_path, 'w', encoding='utf-8') as f:
        json.dump({'exported': exported, 'skipped': skipped}, f, indent=2)

    print(f"\n  {len(exported)} static GLBs in: {static_dir}")
    if skipped:
        print(f"  {len(skipped)} object files skipped (non-geometry or skeletal)")



def compose_mat4(pos, rot):
    """Compose a 4x4 column-major matrix from a translation vector and a quaternion rotation."""
    qx, qy, qz, qw = rot
    tx, ty, tz = pos
    
    xx, xy, xz = qx*qx, qx*qy, qx*qz
    yy, yz, zz = qy*qy, qy*qz, qz*qz
    wx, wy, wz = qw*qx, qw*qy, qw*qz
    
    m0 = 1.0 - 2.0 * (yy + zz)
    m1 = 2.0 * (xy + wz)
    m2 = 2.0 * (xz - wy)
    
    m4 = 2.0 * (xy - wz)
    m5 = 1.0 - 2.0 * (xx + zz)
    m6 = 2.0 * (yz + wx)
    
    m8 = 2.0 * (xz + wy)
    m9 = 2.0 * (yz - wx)
    m10 = 1.0 - 2.0 * (xx + yy)
    
    return [
        m0, m1, m2, 0.0,
        m4, m5, m6, 0.0,
        m8, m9, m10, 0.0,
        tx, ty, tz, 1.0
    ]


def mat4_identity():
    return [
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 1.0,
    ]


def mat4_multiply(A, B):
    C = [0.0] * 16
    for col in range(4):
        for row in range(4):
            val = 0.0
            for k in range(4):
                val += A[k * 4 + row] * B[col * 4 + k]
            C[col * 4 + row] = val
    return C


def mat4_transform_point(M, v):
    x, y, z = v
    nx = M[0] * x + M[4] * y + M[8] * z + M[12]
    ny = M[1] * x + M[5] * y + M[9] * z + M[13]
    nz = M[2] * x + M[6] * y + M[10] * z + M[14]
    return (nx, ny, nz)


def mat4_transform_normal(M, n):
    x, y, z = n
    nx = M[0] * x + M[4] * y + M[8] * z
    ny = M[1] * x + M[5] * y + M[9] * z
    nz = M[2] * x + M[6] * y + M[10] * z
    length = (nx*nx + ny*ny + nz*nz) ** 0.5
    if length > 0.0001:
        return (nx/length, ny/length, nz/length)
    return (nx, ny, nz)


def build_body_mesh_data(models: dict, skeleton) -> dict:
    """Build local-space mesh data for each bone of the skeleton.

    GameplayFootball body-part ASE files are authored in the local space of their
    corresponding rigid node. We keep submeshes separate so each can receive the
    correct material (skin, kit_upper, kit_lower, shoe) for Android dynamic replacement.
    """

    bone_meshes = {}
    bone_mesh_map = {}
    for i, bone in enumerate(skeleton):
        if bone.mesh_file:
            mesh_key = os.path.basename(bone.mesh_file).replace('.ase', '')
            bone_mesh_map[bone.name] = (mesh_key, i)

    for bone_name, (mesh_key, bone_idx) in bone_mesh_map.items():
        if mesh_key not in models:
            continue

        submeshes = []
        for ase_mesh in models[mesh_key]:
            if bone_name == 'neck' and ase_mesh.name.lower() == 'hair':
                continue

            nv = len(ase_mesh.vertices)
            verts_local = [vec3_zup_to_yup(v) for v in ase_mesh.vertices]

            if ase_mesh.normals and len(ase_mesh.normals) == nv:
                normals_local = [vec3_zup_to_yup(n) for n in ase_mesh.normals]
            else:
                normals_local = [(0, 1, 0)] * nv

            if ase_mesh.uvs and len(ase_mesh.uvs) == nv:
                uvs_local = list(ase_mesh.uvs)
            else:
                uvs_local = [(0, 0)] * nv

            faces_local = list(ase_mesh.faces)
            mat_cat = _submesh_material_category(ase_mesh.name, bone_name)

            submeshes.append({
                'name': ase_mesh.name,
                'category': mat_cat,
                'verts': verts_local,
                'faces': faces_local,
                'normals': normals_local,
                'uvs': uvs_local,
            })

        bone_meshes[bone_name] = submeshes

    return bone_meshes


def png_bytes_from_texture(tex: np.ndarray) -> bytes:
    """Convert numpy RGBA texture to PNG bytes."""
    try:
        from PIL import Image
        buf = io.BytesIO()
        Image.fromarray(tex).save(buf, format='PNG')
        return buf.getvalue()
    except ImportError:
        return tex.tobytes()


# ─── GLB builders ────────────────────────────────────────────────

def build_base_glb(skeleton, bone_meshes, selected_anims) -> bytes:
    """Build the base player GLB (no customizations) using a clean rigid hierarchy."""
    builder = GLBBuilder()
    data_dir = find_data_dir()

    # Load textures from game assets (PNG/JPG as they exist in Beta 2)
    skin_png = load_bmp_as_png_bytes(data_dir, 'skin01.png') if data_dir else None
    kit_png  = load_bmp_as_png_bytes(data_dir, 'kit_template.png') if data_dir else None
    shoe_png = load_bmp_as_png_bytes(data_dir, 'shoe.jpg') if data_dir else None

    skin_tex_idx = builder.add_image(skin_png) if skin_png else -1
    kit_tex_idx  = builder.add_image(kit_png) if kit_png else -1
    shoe_tex_idx = builder.add_image(shoe_png) if shoe_png else -1

    skin_mat = builder.add_material('skin', [0.82, 0.65, 0.50, 1.0], texture_idx=skin_tex_idx)
    kit_upper_mat = builder.add_material('kit_upper', [0.90, 0.90, 0.90, 1.0], texture_idx=kit_tex_idx)
    kit_lower_mat = builder.add_material('kit_lower', [0.85, 0.85, 0.85, 1.0], texture_idx=kit_tex_idx)
    shoe_mat = builder.add_material('shoe', [0.20, 0.20, 0.20, 1.0], texture_idx=shoe_tex_idx)

    bone_nodes = builder.add_skeleton_nodes(skeleton)

    mat_map = {
        'skin': skin_mat,
        'kit_upper': kit_upper_mat,
        'kit_lower': kit_lower_mat,
        'shoe': shoe_mat,
    }

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
                sub['normals'], sub['uvs'],
                material_idx=mat_idx
            )
            primitives.append(prim)

        if primitives:
            mesh_idx = builder.add_mesh(f'mesh_{bone_name}', primitives)
            builder.gltf['nodes'][node_idx]['mesh'] = mesh_idx

    skeleton_names = [b.name for b in skeleton]
    for anim_id in range(17):
        clip = selected_anims.get(anim_id)
        if clip:
            builder.add_animation(f"anim_{anim_id:02d}", clip, skeleton_names, bone_nodes)

    return builder.finalize()


def build_avatar_glb(avatar: AvatarConfig, skeleton, bone_meshes, selected_anims) -> bytes:
    """Build a fully customized GLB for one avatar with modular materials for Android kit replacement."""
    builder = GLBBuilder()

    data_dir = find_data_dir()

    # Resolve correct skin texture based on skin tone preset
    skin_bmp_map = {
        'light': 'skin01.bmp',
        'medium': 'skin02.bmp',
        'olive': 'skin03.bmp',
        'dark': 'skin04.bmp',
        'black': 'skin04.bmp',
    }
    skin_bmp = skin_bmp_map.get(avatar.skin_tone, 'skin02.bmp')

    # Load BMP textures from game assets and convert to embedded PNGs
    skin_png = load_bmp_as_png_bytes(data_dir, skin_bmp) if data_dir else None
    kit_png = load_bmp_as_png_bytes(data_dir, 'kit_template.bmp') if data_dir else None
    shoe_png = load_bmp_as_png_bytes(data_dir, 'shoe.bmp') if data_dir else None
    hair_tex_name = _map_hair_color_to_gf(avatar.hair_color) + '.bmp'
    hair_png = load_image_as_png_bytes(
        data_dir,
        hair_tex_name,
        base_dir=os.path.join(data_dir, 'media', 'objects', 'players', 'textures', 'hair')
    ) if data_dir else None

    # Fallback colors
    skin_color = SKIN_TONES.get(avatar.skin_tone, SKIN_TONES['medium'])

    skin_tex_idx = builder.add_image(skin_png) if skin_png else -1
    kit_tex_idx = builder.add_image(kit_png) if kit_png else -1
    shoe_tex_idx = builder.add_image(shoe_png) if shoe_png else -1
    hair_tex_idx = builder.add_image(hair_png) if hair_png else -1

    # Head skin texture: full skin map with face features painted at the
    # verified UV anchors of head.ase. Replaces the old decal-plane approach.
    head_skin_arr = generate_skin_texture(
        avatar.skin_tone,
        avatar.eye_color,
        avatar.beard_style,
        avatar.beard_color,
        resolution=512,
    )
    head_skin_png = png_bytes_from_texture(head_skin_arr)
    head_skin_tex_idx = builder.add_image(head_skin_png) if head_skin_png else -1

    # Materials named for Android runtime replacement
    skin_mat = builder.add_material('skin', list(skin_color), texture_idx=skin_tex_idx)
    head_skin_mat = builder.add_material('head_skin', [1.0, 1.0, 1.0, 1.0], texture_idx=head_skin_tex_idx)
    kit_upper_mat = builder.add_material('kit_upper', [0.95, 0.95, 0.95, 1.0], texture_idx=kit_tex_idx)
    kit_lower_mat = builder.add_material('kit_lower', [0.85, 0.85, 0.85, 1.0], texture_idx=kit_tex_idx)
    shoe_mat = builder.add_material('shoe', [0.85, 0.85, 0.85, 1.0], texture_idx=shoe_tex_idx)
    hair_mat = builder.add_material('hair', [1.0, 1.0, 1.0, 1.0], texture_idx=hair_tex_idx)

    # Eye materials — iris color varies per avatar
    _EYE_COLOR_RGB = {
        'brown': [0.50, 0.25, 0.10, 1.0],
        'blue':  [0.10, 0.30, 0.55, 1.0],
        'green': [0.10, 0.45, 0.20, 1.0],
    }
    eye_white_mat = builder.add_material('eye_white', [0.96, 0.94, 0.90, 1.0])
    eye_black_mat = builder.add_material('eye_black', [0.04, 0.04, 0.04, 1.0])
    eye_brown_mat = builder.add_material(
        'eye_brown',
        _EYE_COLOR_RGB.get(avatar.eye_color, [0.50, 0.25, 0.10, 1.0])
    )
    eye_materials = {
        'eye_white': eye_white_mat,
        'eye_black': eye_black_mat,
        'eye_brown': eye_brown_mat,
    }

    mat_map = {
        'skin': skin_mat,
        'head_skin': head_skin_mat,
        'kit_upper': kit_upper_mat,
        'kit_lower': kit_lower_mat,
        'shoe': shoe_mat,
    }

    # Add skeleton nodes
    bone_nodes = builder.add_skeleton_nodes(skeleton)

    # Height factor on root bone
    hf = avatar.height_factor
    builder.gltf['nodes'][bone_nodes[0]]['scale'] = [hf, hf, hf]

    # Body type scaling on body + middle bones
    bt = {'thin': (0.92, 0.92), 'average': (1.0, 1.0),
          'muscular': (1.06, 1.04), 'heavy': (1.08, 1.10)}
    sx, sz = bt.get(avatar.body_type, (1.0, 1.0))
    for bp_name in ['body', 'middle']:
        for i, b in enumerate(skeleton):
            if b.name == bp_name:
                builder.gltf['nodes'][bone_nodes[i]]['scale'] = [sx, 1.0, sz]
                break

    # Replace GF head with playermodifier merged head+neck mesh (fixed config)
    try:
        from playermodifier_loader import get_head_mesh
        head_verts, head_norms, head_uvs, head_idx = get_head_mesh()
        head_faces = [(int(head_idx[i]), int(head_idx[i+1]), int(head_idx[i+2]))
                      for i in range(0, len(head_idx), 3)]
        modified_bone_meshes = {}
        for bone_name, submeshes in bone_meshes.items():
            if bone_name == 'neck':
                new_submeshes = []
                replaced = False
                for sub in submeshes:
                    if sub['name'].lower() == 'head' and not replaced:
                        new_submeshes.append({
                            'name': 'head',
                            'category': 'head_skin',
                            'verts': head_verts.tolist(),
                            'faces': head_faces,
                            'normals': head_norms.tolist(),
                            'uvs': head_uvs.tolist(),
                        })
                        replaced = True
                    else:
                        new_submeshes.append(sub)
                modified_bone_meshes[bone_name] = new_submeshes
            else:
                modified_bone_meshes[bone_name] = submeshes
        bone_meshes = modified_bone_meshes
    except Exception as e:
        print(f"  [WARN] Failed to load headproto head: {e}")

    # Assign per-submesh materials
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
                sub['normals'], sub['uvs'],
                material_idx=mat_idx
            )
            primitives.append(prim)

        if primitives:
            mesh_idx = builder.add_mesh(f'mesh_{bone_name}', primitives)
            builder.gltf['nodes'][node_idx]['mesh'] = mesh_idx

    # Attach rigid customization elements
    neck_idx = BODY_PART_ORDER.index('neck')
    neck_node_idx = bone_nodes[neck_idx]

    def add_rigid_custom_part(name: str, verts, faces, material_idx: int,
                               parent_node_idx: int, translation=(0, 0, 0),
                               normals=None, uvs=None):
        if not verts or not faces:
            return
        prim = builder.add_primitive_geometry(verts, faces, normals, uvs,
                                               material_idx=material_idx)
        mesh_idx = builder.add_mesh(name, [prim])
        child_node_idx = builder.add_node(
            name + '_node', mesh_idx=mesh_idx,
            translation=list(translation)
        )
        p_node = builder.gltf['nodes'][parent_node_idx]
        if 'children' not in p_node:
            p_node['children'] = []
        p_node['children'].append(child_node_idx)

    # Ears (flat against the side of the head)
    ear_v, ear_f, _ = generate_ears()
    if ear_v and ear_f:
        add_rigid_custom_part('ears', ear_v, ear_f, skin_mat, neck_node_idx)

    # Hair (on top of head — vertices already positioned)
    if avatar.hair_style != 'bald':
        hv, hf, hn, huvs = load_gf_hairstyle_mesh(data_dir, avatar.hair_style)
        if hv and hf:
            add_rigid_custom_part('hair', hv, hf, hair_mat, neck_node_idx,
                                  normals=hn, uvs=huvs)
        else:
            hv, hf, _ = generate_hair_mesh(avatar.hair_style)
            add_rigid_custom_part('hair', hv, hf, hair_mat, neck_node_idx)

    # Face features are now painted directly on the head's skin texture via
    # generate_skin_texture(); no more decal planes are added here.

    # Eyes (from playermodifier JSON, positioned on the new head face)
    try:
        eye_meshes = get_eye_meshes()
        for eye_verts, eye_norms, eye_uvs, eye_idx, eye_name in eye_meshes:
            if eye_verts is None or len(eye_verts) == 0:
                continue
            faces = [(int(eye_idx[i]), int(eye_idx[i+1]), int(eye_idx[i+2]))
                     for i in range(0, len(eye_idx), 3)]
            mat_idx = eye_materials.get(eye_name, eye_white_mat)
            add_rigid_custom_part(
                eye_name, eye_verts.tolist(), faces, mat_idx,
                neck_node_idx, normals=eye_norms.tolist(), uvs=eye_uvs.tolist()
            )
    except Exception as e:
        print(f"  [WARN] Failed to add playermodifier eyes: {e}")

    # Fingers
    for side, elbow_name in [('left', 'left_elbow'), ('right', 'right_elbow')]:
        fgv, fgf, _ = generate_fingers(side)
        elbow_idx = BODY_PART_ORDER.index(elbow_name)
        elbow_node_idx = bone_nodes[elbow_idx]
        add_rigid_custom_part(f'fingers_{side}', fgv, fgf, skin_mat, elbow_node_idx)

    # Beard
    if avatar.beard_style != 'none':
        bv, bf, _ = generate_beard_mesh(avatar.beard_style)
        if bv and bf:
            add_rigid_custom_part('beard', bv, bf, hair_mat, neck_node_idx)

    # Animations
    skeleton_names = [b.name for b in skeleton]
    for anim_id in range(17):
        clip = selected_anims.get(anim_id)
        if clip:
            builder.add_animation(f"anim_{anim_id:02d}", clip, skeleton_names, bone_nodes)

    return builder.finalize()


# ─── Export functions ────────────────────────────────────────────

def export_base_player(data_dir: str, output_dir: str):
    print("\n" + "=" * 60)
    print("  STEP 1: Exporting base player model")
    print("=" * 60)

    models, skeleton, selected_anims = load_shared_data(data_dir)
    body_data = build_body_mesh_data(models, skeleton)

    print("  Building base GLB...")
    glb_data = build_base_glb(skeleton, body_data, selected_anims)

    output_path = os.path.join(output_dir, 'player_base.glb')
    with open(output_path, 'wb') as f:
        f.write(glb_data)
    print(f"  Base GLB exported: {output_path} ({len(glb_data)/1024:.1f} KB)")
    return skeleton, body_data, selected_anims


def export_customized_avatars(output_dir: str, skeleton, body_data, selected_anims, max_avatars: int = 256):
    print("\n" + "=" * 60)
    print(f"  STEP 2: Exporting customized avatars (max {max_avatars})")
    print("=" * 60)

    avatars_dir = os.path.join(output_dir, 'avatars')
    os.makedirs(avatars_dir, exist_ok=True)

    all_avatars = generate_all_combinations(max_count=max_avatars)

    for i, avatar in enumerate(all_avatars):
        safe_name = avatar.id
        print(f"  [{i+1:3d}/{len(all_avatars)}] {avatar.id} {avatar.name}...", end=' ', flush=True)

        glb_data = build_avatar_glb(avatar, skeleton, body_data, selected_anims)

        output_path = os.path.join(avatars_dir, f'{safe_name}.glb')
        with open(output_path, 'wb') as f:
            f.write(glb_data)
        print(f"{len(glb_data)/1024:.0f} KB")

    catalog_path = os.path.join(output_dir, 'avatar_catalog.json')
    export_avatar_catalog(catalog_path, all_avatars)

    print(f"\n  {len(all_avatars)} avatar GLBs in: {avatars_dir}")


def export_animations_binary(selected_anims: dict, output_path: str):
    """Export selected animation clips to a compact binary file for Android runtime."""
    print("\n" + "=" * 60)
    print("  Exporting animation binary for Android runtime")
    print("=" * 60)

    # Order clips by anim_id 0..16
    ordered_clips = []
    for i in range(17):
        if i in selected_anims:
            ordered_clips.append(selected_anims[i])
        else:
            # Fallback to idle if missing
            ordered_clips.append(selected_anims.get(0, list(selected_anims.values())[0]))

    bone_names = [
        'player', 'body', 'middle',
        'left_thigh', 'left_knee', 'left_ankle',
        'right_thigh', 'right_knee', 'right_ankle',
        'left_shoulder', 'left_elbow',
        'right_shoulder', 'right_elbow',
        'head'
    ]

    with open(output_path, 'wb') as f:
        # Header
        f.write(struct.pack('<4sHHHH', b'DZAN', 1, len(ordered_clips), len(bone_names), 0))

        for clip in ordered_clips:
            name = clip.name.encode('utf-8')[:31].ljust(32, b'\x00')
            duration = (clip.frame_count / 60.0) if clip.frame_count > 0 else 0.0
            num_tracks = len(bone_names)
            f.write(struct.pack('<32sfB', name, duration, num_tracks))

            for bone_name in bone_names:
                track = clip.tracks.get(bone_name)
                kfs = track.keyframes if track else []
                track_name = bone_name.encode('utf-8')[:31].ljust(32, b'\x00')
                f.write(struct.pack('<32sH', track_name, len(kfs)))
                for kf in kfs:
                    t = kf.frame / 60.0
                    pos = kf.position or (0.0, 0.0, 0.0)
                    rot = kf.rotation
                    f.write(struct.pack('<fffffffff', t, pos[0], pos[1], pos[2], rot[0], rot[1], rot[2], rot[3], 0.0))

    print(f"  Animation binary exported: {output_path}")
    size_kb = os.path.getsize(output_path) / 1024.0
    print(f"  Size: {size_kb:.1f} KB ({len(ordered_clips)} clips, {len(bone_names)} bones each)")


def main():
    parser = argparse.ArgumentParser(description='GF Assets -> GLB Exporter')
    parser.add_argument('--data-dir', type=str, help='Path to GameplayFootball/data')
    parser.add_argument('--output-dir', type=str, default='./output')
    parser.add_argument('--skip-base', action='store_true')
    parser.add_argument('--skip-avatars', action='store_true')
    parser.add_argument('--skip-static', action='store_true')
    parser.add_argument('--max-avatars', type=int, default=256, help='Maximum number of combinatorial avatars to export (default: 256)')
    args = parser.parse_args()

    data_dir = args.data_dir or find_data_dir()
    if not data_dir:
        print("ERROR: Could not find GameplayFootball data directory. Use --data-dir PATH")
        sys.exit(1)

    print(f"Data directory: {data_dir}")
    print(f"Output directory: {args.output_dir}")
    os.makedirs(args.output_dir, exist_ok=True)

    skeleton = body_data = selected_anims = None

    if not args.skip_base:
        skeleton, body_data, selected_anims = export_base_player(data_dir, args.output_dir)

    if selected_anims:
        export_animations_binary(selected_anims, os.path.join(args.output_dir, 'anim_templates.bin'))

    if not args.skip_avatars:
        if skeleton is None:
            models, skeleton, selected_anims = load_shared_data(data_dir)
            body_data = build_body_mesh_data(models, skeleton)
        export_customized_avatars(args.output_dir, skeleton, body_data, selected_anims, max_avatars=args.max_avatars)

    if not args.skip_static:
        export_static_objects(data_dir, args.output_dir)

    print("\n" + "=" * 60)
    print("  EXPORT COMPLETE")
    print("=" * 60)
    print(f"\n  {args.output_dir}/player_base.glb  - Base model (no customizations)")
    print(f"  {args.output_dir}/avatars/*.glb     - {args.max_avatars} customized avatars")
    print(f"  {args.output_dir}/avatar_catalog.json")
    print(f"  {args.output_dir}/static/**/*.glb     - Reusable static objects")


if __name__ == '__main__':
    main()
