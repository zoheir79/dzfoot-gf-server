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

from parse_ase import parse_all_models, ASEMesh
from parse_anim import parse_all_animations, select_best_animations, AnimClip
from parse_object import build_skeleton_from_object, BODY_PART_ORDER, BONE_PARENTS, BODY_PART_INDEX, axis_angle_to_quaternion
from write_glb import GLBBuilder
from customize import (
    generate_hair_mesh, generate_beard_mesh, generate_ears,
    generate_fingers, generate_face_features,
    generate_face_texture, SKIN_TONES, HAIR_COLORS
)
from avatar_combinations import (
    PRESET_AVATARS, AvatarConfig, avatar_to_dict, export_avatar_catalog
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

    # Fill to 16 animations
    mirror_map = {4: 5, 12: 11}
    for src_id, dst_id in mirror_map.items():
        if dst_id not in selected_anims and src_id in selected_anims:
            selected_anims[dst_id] = mirror_animation(selected_anims[src_id])
    for missing_id in range(16):
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
    corresponding rigid node. Applying the inverse bind/world transform here would
    double-apply the node offset and explode the assembled character.
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

        verts_local = []
        faces_local = []
        normals_local = []
        uvs_local = []
        vertex_offset = 0

        for ase_mesh in models[mesh_key]:
            nv = len(ase_mesh.vertices)

            verts_local.extend([vec3_zup_to_yup(v) for v in ase_mesh.vertices])

            if ase_mesh.normals and len(ase_mesh.normals) == nv:
                normals_local.extend([vec3_zup_to_yup(n) for n in ase_mesh.normals])
            else:
                normals_local.extend([(0, 1, 0)] * nv)

            if ase_mesh.uvs and len(ase_mesh.uvs) == nv:
                # Do NOT flip here. GLBBuilder.add_primitive_geometry flips it automatically once.
                uvs_local.extend(ase_mesh.uvs)
            else:
                uvs_local.extend([(0, 0)] * nv)

            for f in ase_mesh.faces:
                faces_local.append((f[0] + vertex_offset, f[1] + vertex_offset, f[2] + vertex_offset))

            vertex_offset += nv

        bone_meshes[bone_name] = {
            'verts': verts_local,
            'faces': faces_local,
            'normals': normals_local,
            'uvs': uvs_local
        }

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
    skin_mat = builder.add_material('skin', [0.82, 0.65, 0.50, 1.0])
    kit_mat = builder.add_material('kit', [0.90, 0.90, 0.90, 1.0])
    shoe_mat = builder.add_material('shoe', [0.20, 0.20, 0.20, 1.0])

    bone_nodes = builder.add_skeleton_nodes(skeleton)

    categories = {
        'body': kit_mat,
        'middle': kit_mat,
        'left_ankle': shoe_mat,
        'right_ankle': shoe_mat,
    }

    for bone_name, mesh_data in bone_meshes.items():
        if bone_name not in BODY_PART_INDEX:
            continue
        bone_idx = BODY_PART_INDEX[bone_name]
        node_idx = bone_nodes[bone_idx]
        mat_idx = categories.get(bone_name, skin_mat)

        prim = builder.add_primitive_geometry(
            mesh_data['verts'], mesh_data['faces'],
            mesh_data['normals'], mesh_data['uvs'],
            material_idx=mat_idx
        )
        mesh_idx = builder.add_mesh(f'mesh_{bone_name}', [prim])
        builder.gltf['nodes'][node_idx]['mesh'] = mesh_idx

    skeleton_names = [b.name for b in skeleton]
    for anim_id in range(16):
        clip = selected_anims.get(anim_id)
        if clip:
            builder.add_animation(f"anim_{anim_id:02d}", clip, skeleton_names, bone_nodes)

    return builder.finalize()


def build_avatar_glb(avatar: AvatarConfig, skeleton, bone_meshes, selected_anims) -> bytes:
    """Build a fully customized GLB for one avatar preset with high quality textures using a clean rigid hierarchy."""
    builder = GLBBuilder()
    
    # Resolve correct skin texture based on skin tone preset
    skin_bmp_map = {
        'light': 'skin01.bmp',
        'medium': 'skin02.bmp',
        'olive': 'skin03.bmp',
        'dark': 'skin04.bmp',
        'black': 'skin04.bmp',
    }
    skin_bmp = skin_bmp_map.get(avatar.skin_tone, 'skin02.bmp')
    
    data_dir = find_data_dir()
    
    # Load BMP textures from game assets and convert to embedded PNGs
    skin_png = load_bmp_as_png_bytes(data_dir, skin_bmp) if data_dir else None
    kit_png = load_bmp_as_png_bytes(data_dir, 'kit_template.bmp') if data_dir else None
    shoe_png = load_bmp_as_png_bytes(data_dir, 'shoe.bmp') if data_dir else None

    # Fallbacks if files not found
    skin_color = SKIN_TONES.get(avatar.skin_tone, SKIN_TONES['medium'])
    hair_color = HAIR_COLORS.get(avatar.hair_color, HAIR_COLORS['black'])
    beard_color = HAIR_COLORS.get(avatar.beard_color, HAIR_COLORS['black'])

    skin_tex_idx = builder.add_image(skin_png) if skin_png else -1
    kit_tex_idx = builder.add_image(kit_png) if kit_png else -1
    shoe_tex_idx = builder.add_image(shoe_png) if shoe_png else -1

    skin_mat = builder.add_material('skin', list(skin_color), texture_idx=skin_tex_idx)
    kit_mat = builder.add_material('kit', [0.95, 0.95, 0.95, 1.0], texture_idx=kit_tex_idx)
    shoe_mat = builder.add_material('shoe', [0.85, 0.85, 0.85, 1.0], texture_idx=shoe_tex_idx)
    
    hair_mat = builder.add_material('hair', list(hair_color))
    beard_mat = builder.add_material('beard', list(beard_color))

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

    # Assign rigid meshes to skeleton nodes
    categories = {
        'body': kit_mat,
        'middle': kit_mat,
        'left_ankle': shoe_mat,
        'right_ankle': shoe_mat,
    }

    for bone_name, mesh_data in bone_meshes.items():
        if bone_name not in BODY_PART_INDEX:
            continue
        bone_idx = BODY_PART_INDEX[bone_name]
        node_idx = bone_nodes[bone_idx]
        mat_idx = categories.get(bone_name, skin_mat)

        prim = builder.add_primitive_geometry(
            mesh_data['verts'], mesh_data['faces'],
            mesh_data['normals'], mesh_data['uvs'],
            material_idx=mat_idx
        )
        mesh_idx = builder.add_mesh(f'mesh_{bone_name}', [prim])
        builder.gltf['nodes'][node_idx]['mesh'] = mesh_idx

    # Attach rigid customization elements (Hair, Beard, and Fingers) as direct children of the skeleton nodes
    neck_idx = BODY_PART_ORDER.index('neck')
    neck_node_idx = bone_nodes[neck_idx]

    def add_rigid_custom_part(name: str, verts: List[Tuple], faces: List[Tuple], material_idx: int, parent_node_idx: int):
        if not verts or not faces:
            return
        prim = builder.add_primitive_geometry(
            verts, faces,
            material_idx=material_idx
        )
        mesh_idx = builder.add_mesh(name, [prim])
        child_node_idx = builder.add_node(name + '_node', mesh_idx=mesh_idx)
        
        # Insert as child in the glTF node hierarchy
        p_node = builder.gltf['nodes'][parent_node_idx]
        if 'children' not in p_node:
            p_node['children'] = []
        p_node['children'].append(child_node_idx)

    # Hair
    if avatar.hair_style != 'bald':
        hv, hf, _ = generate_hair_mesh(avatar.hair_style)
        add_rigid_custom_part('hair', hv, hf, hair_mat, neck_node_idx)

    # Beard
    if avatar.beard_style != 'none':
        bv, bf, _ = generate_beard_mesh(avatar.beard_style)
        add_rigid_custom_part('beard', bv, bf, beard_mat, neck_node_idx)

    # Fingers (rigidly attached to left/right elbow nodes)
    for side, elbow_name in [('left', 'left_elbow'), ('right', 'right_elbow')]:
        fgv, fgf, _ = generate_fingers(side)
        elbow_idx = BODY_PART_ORDER.index(elbow_name)
        elbow_node_idx = bone_nodes[elbow_idx]
        add_rigid_custom_part(f'fingers_{side}', fgv, fgf, skin_mat, elbow_node_idx)

    # Animations
    skeleton_names = [b.name for b in skeleton]
    for anim_id in range(16):
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


def export_customized_avatars(output_dir: str, skeleton, body_data, selected_anims):
    print("\n" + "=" * 60)
    print("  STEP 2: Exporting customized avatars (1 GLB each)")
    print("=" * 60)

    avatars_dir = os.path.join(output_dir, 'avatars')
    os.makedirs(avatars_dir, exist_ok=True)

    for i, avatar in enumerate(PRESET_AVATARS):
        safe_name = avatar.name.replace(' ', '_').lower()
        print(f"  [{i+1:2d}/{len(PRESET_AVATARS)}] {avatar.name}...", end=' ', flush=True)

        glb_data = build_avatar_glb(avatar, skeleton, body_data, selected_anims)

        output_path = os.path.join(avatars_dir, f'{safe_name}.glb')
        with open(output_path, 'wb') as f:
            f.write(glb_data)
        print(f"{len(glb_data)/1024:.0f} KB")

    catalog_path = os.path.join(output_dir, 'avatar_catalog.json')
    export_avatar_catalog(catalog_path)

    print(f"\n  {len(PRESET_AVATARS)} avatar GLBs in: {avatars_dir}")


def main():
    parser = argparse.ArgumentParser(description='GF Assets -> GLB Exporter')
    parser.add_argument('--data-dir', type=str, help='Path to GameplayFootball/data')
    parser.add_argument('--output-dir', type=str, default='./output')
    parser.add_argument('--skip-base', action='store_true')
    parser.add_argument('--skip-avatars', action='store_true')
    parser.add_argument('--skip-static', action='store_true')
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

    if not args.skip_avatars:
        if skeleton is None:
            models, skeleton, selected_anims = load_shared_data(data_dir)
            body_data = build_body_mesh_data(models, skeleton)
        export_customized_avatars(args.output_dir, skeleton, body_data, selected_anims)

    if not args.skip_static:
        export_static_objects(data_dir, args.output_dir)

    print("\n" + "=" * 60)
    print("  EXPORT COMPLETE")
    print("=" * 60)
    print(f"\n  {args.output_dir}/player_base.glb  - Base model (no customizations)")
    print(f"  {args.output_dir}/avatars/*.glb     - 16 customized avatars")
    print(f"  {args.output_dir}/avatar_catalog.json")
    print(f"  {args.output_dir}/static/**/*.glb     - Reusable static objects")


if __name__ == '__main__':
    main()
