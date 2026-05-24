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
import numpy as np
from pathlib import Path

sys.path.insert(0, os.path.dirname(__file__))

from parse_ase import parse_all_models, ASEMesh
from parse_anim import parse_all_animations, select_best_animations, AnimClip
from parse_object import build_skeleton_from_object, BODY_PART_ORDER, BONE_PARENTS
from write_glb import GLBBuilder
from customize import (
    generate_hair_mesh, generate_beard_mesh, generate_ears,
    generate_fingers, generate_face_features,
    generate_face_texture, SKIN_TONES, HAIR_COLORS
)
from avatar_combinations import (
    PRESET_AVATARS, AvatarConfig, avatar_to_dict, export_avatar_catalog
)
from coords import vec3_zup_to_yup, quat_zup_to_yup, normalize_quat


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


def build_body_mesh_data(models: dict, skeleton) -> dict:
    """Build combined body mesh vertex data (shared by all avatars)."""
    bone_mesh_map = {}
    for i, bone in enumerate(skeleton):
        if bone.mesh_file:
            mesh_key = os.path.basename(bone.mesh_file).replace('.ase', '')
            bone_mesh_map[bone.name] = (mesh_key, i)

    all_verts = []
    all_faces = []
    all_normals = []
    all_uvs = []
    all_joints = []
    all_weights = []
    vertex_offset = 0

    for bone_name in BODY_PART_ORDER:
        if bone_name not in bone_mesh_map:
            continue
        mesh_key, bone_idx = bone_mesh_map[bone_name]
        if mesh_key not in models or 'fullbody' in mesh_key:
            continue

        for ase_mesh in models[mesh_key]:
            nv = len(ase_mesh.vertices)
            all_verts.extend([vec3_zup_to_yup(v) for v in ase_mesh.vertices])

            if ase_mesh.normals and len(ase_mesh.normals) == nv:
                all_normals.extend([vec3_zup_to_yup(n) for n in ase_mesh.normals])
            else:
                all_normals.extend([(0, 1, 0)] * nv)

            if ase_mesh.uvs and len(ase_mesh.uvs) == nv:
                all_uvs.extend(ase_mesh.uvs)
            else:
                all_uvs.extend([(0, 0)] * nv)

            for _ in range(nv):
                all_joints.extend([bone_idx, 0, 0, 0])
                all_weights.extend([1.0, 0.0, 0.0, 0.0])

            for f in ase_mesh.faces:
                all_faces.append((f[0] + vertex_offset, f[1] + vertex_offset, f[2] + vertex_offset))
            vertex_offset += nv

    return {
        'verts': all_verts, 'faces': all_faces, 'normals': all_normals,
        'uvs': all_uvs, 'joints': all_joints, 'weights': all_weights,
    }


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

def build_base_glb(skeleton, body_data, selected_anims) -> bytes:
    """Build the base player GLB (no customizations)."""
    builder = GLBBuilder()
    skin_mat = builder.add_material('skin', [0.82, 0.65, 0.50, 1.0])
    bone_nodes = builder.add_skeleton_nodes(skeleton)
    ibm_acc = builder.add_inverse_bind_matrices(skeleton)
    skin_idx = builder.add_skin(builder._skin_joints, ibm_acc)

    prim = builder.add_primitive_geometry(
        body_data['verts'], body_data['faces'],
        body_data['normals'], body_data['uvs'],
        material_idx=skin_mat,
        joints_data=body_data['joints'],
        weights_data=body_data['weights'],
    )
    mesh_idx = builder.add_mesh('player_body', [prim])
    builder.gltf['nodes'][bone_nodes[0]]['mesh'] = mesh_idx
    builder.gltf['nodes'][bone_nodes[0]]['skin'] = skin_idx

    skeleton_names = [b.name for b in skeleton]
    for anim_id in range(16):
        clip = selected_anims.get(anim_id)
        if clip:
            builder.add_animation(f"anim_{anim_id:02d}", clip, skeleton_names, bone_nodes)

    return builder.finalize()


def build_avatar_glb(avatar: AvatarConfig, skeleton, body_data, selected_anims) -> bytes:
    """Build a fully customized GLB for one avatar preset."""
    builder = GLBBuilder()

    # ── Textures ──
    face_tex = generate_face_texture(skin_tone=avatar.skin_tone, eye_color=avatar.eye_color)
    face_png = png_bytes_from_texture(face_tex)
    face_tex_idx = builder.add_image(face_png)

    # ── Materials ──
    skin_color = SKIN_TONES.get(avatar.skin_tone, SKIN_TONES['medium'])
    hair_color = HAIR_COLORS.get(avatar.hair_color, HAIR_COLORS['black'])
    beard_color = HAIR_COLORS.get(avatar.beard_color, HAIR_COLORS['black'])

    skin_mat = builder.add_material('skin', list(skin_color), texture_idx=face_tex_idx)
    hair_mat = builder.add_material('hair', list(hair_color))
    beard_mat = builder.add_material('beard', list(beard_color))
    ear_mat = builder.add_material('ears', list(skin_color))
    finger_mat = builder.add_material('fingers', list(skin_color))

    # ── Skeleton ──
    bone_nodes = builder.add_skeleton_nodes(skeleton)
    ibm_acc = builder.add_inverse_bind_matrices(skeleton)
    skin_idx = builder.add_skin(builder._skin_joints, ibm_acc)

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

    # ── Body mesh ──
    prim = builder.add_primitive_geometry(
        body_data['verts'], body_data['faces'],
        body_data['normals'], body_data['uvs'],
        material_idx=skin_mat,
        joints_data=body_data['joints'],
        weights_data=body_data['weights'],
    )
    mesh_idx = builder.add_mesh('player_body', [prim])
    builder.gltf['nodes'][bone_nodes[0]]['mesh'] = mesh_idx
    builder.gltf['nodes'][bone_nodes[0]]['skin'] = skin_idx

    # ── Customization meshes (attached to neck bone) ──
    neck_idx = BODY_PART_ORDER.index('neck')
    neck_node_idx = bone_nodes[neck_idx]

    def attach_to_neck(node_idx: int):
        neck = builder.gltf['nodes'][neck_node_idx]
        if 'children' not in neck:
            neck['children'] = []
        neck['children'].append(node_idx)

    # Hair (vertices already contain offset, no node translation needed)
    if avatar.hair_style != 'bald':
        hv, hf, _ = generate_hair_mesh(avatar.hair_style)
        if hv:
            hp = builder.add_primitive_geometry(hv, hf, material_idx=hair_mat)
            hm = builder.add_mesh('hair', [hp])
            hn = builder.add_node('hair_node', mesh_idx=hm)
            attach_to_neck(hn)

    # Beard
    if avatar.beard_style != 'none':
        bv, bf, _ = generate_beard_mesh(avatar.beard_style)
        if bv:
            bp = builder.add_primitive_geometry(bv, bf, material_idx=beard_mat)
            bm = builder.add_mesh('beard', [bp])
            bn = builder.add_node('beard_node', mesh_idx=bm)
            attach_to_neck(bn)

    # Ears
    ev, ef, _ = generate_ears()
    if ev:
        ep = builder.add_primitive_geometry(ev, ef, material_idx=ear_mat)
        em = builder.add_mesh('ears', [ep])
        en = builder.add_node('ears_node', mesh_idx=em)
        attach_to_neck(en)

    # Face features (nose, eyes, mouth)
    fv, ff, _ = generate_face_features()
    if fv:
        fp = builder.add_primitive_geometry(fv, ff, material_idx=skin_mat)
        fm = builder.add_mesh('face_features', [fp])
        fn = builder.add_node('face_features_node', mesh_idx=fm)
        attach_to_neck(fn)

    # Fingers (attached to elbow bones)
    for side, elbow_name in [('left', 'left_elbow'), ('right', 'right_elbow')]:
        fgv, fgf, _ = generate_fingers(side)
        if fgv:
            fgp = builder.add_primitive_geometry(fgv, fgf, material_idx=finger_mat)
            fgm = builder.add_mesh(f'fingers_{side}', [fgp])
            fgn = builder.add_node(f'fingers_{side}_node', mesh_idx=fgm)
            elbow_idx = BODY_PART_ORDER.index(elbow_name)
            elbow_node = builder.gltf['nodes'][bone_nodes[elbow_idx]]
            if 'children' not in elbow_node:
                elbow_node['children'] = []
            elbow_node['children'].append(fgn)

    # ── Animations ──
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

    print("\n" + "=" * 60)
    print("  EXPORT COMPLETE")
    print("=" * 60)
    print(f"\n  {args.output_dir}/player_base.glb  - Base model (no customizations)")
    print(f"  {args.output_dir}/avatars/*.glb     - 16 customized avatars")
    print(f"  {args.output_dir}/avatar_catalog.json")


if __name__ == '__main__':
    main()
