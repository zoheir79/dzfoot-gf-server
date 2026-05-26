"""
GLB 2.0 writer — assembles a complete glTF binary file from parsed GF assets.
Uses raw struct packing (no external glTF library dependency).
"""
import struct
import json
import os
import base64
from dataclasses import dataclass, field
from typing import List, Dict, Tuple, Optional, Any
import numpy as np
from coords import quat_zup_to_yup, normalize_quat, vec3_zup_to_yup


# ─── GLB Binary Layout ───────────────────────────────────────────
#
# GLB file structure:
#   [12-byte header] [JSON chunk] [BIN chunk (optional)]
#
# Header:
#   uint32 magic = 0x46546C67 ("glTF")
#   uint32 version = 2
#   uint32 total_length
#
# Chunk:
#   uint32 chunk_length
#   uint32 chunk_type (0x4E4F534A = JSON, 0x004E4942 = BIN)
#   uint8[chunk_length] chunk_data


def pack_glb(json_data: dict, bin_data: bytes) -> bytes:
    """Pack JSON + binary buffer into a .glb file."""
    # Pad JSON to 4-byte alignment with spaces (valid JSON whitespace)
    json_str = json.dumps(json_data, separators=(',', ':'))
    while len(json_str) % 4 != 0:
        json_str += ' '
    json_bytes = json_str.encode('utf-8')

    # Pad binary to 4-byte alignment with null bytes
    bin_padded = bin_data
    while len(bin_padded) % 4 != 0:
        bin_padded += b'\x00'

    # Chunks
    json_chunk_type = 0x4E4F534A  # "JSON"
    bin_chunk_type = 0x004E4942   # "BIN\0"

    total_length = 12 + 8 + len(json_bytes) + 8 + len(bin_padded)

    header = struct.pack('<III', 0x46546C67, 2, total_length)
    json_chunk = struct.pack('<II', len(json_bytes), json_chunk_type) + json_bytes
    bin_chunk = struct.pack('<II', len(bin_padded), bin_chunk_type) + bin_padded

    return header + json_chunk + bin_chunk


# ─── Buffer Builder ──────────────────────────────────────────────

@dataclass
class BufferBuilder:
    """Accumulates binary data and tracks byte offsets for accessors."""
    data: bytearray = field(default_factory=bytearray)
    views: List[dict] = field(default_factory=list)
    accessors: List[dict] = field(default_factory=list)

    def add_view(self, byte_offset: int, byte_length: int, target: int,
                 byte_stride: int = 0) -> int:
        """Add a bufferView, return its index."""
        view = {
            'buffer': 0,
            'byteOffset': byte_offset,
            'byteLength': byte_length,
        }
        if byte_stride > 0:
            view['byteStride'] = byte_stride
        if target > 0:
            view['target'] = target
        self.views.append(view)
        return len(self.views) - 1

    def add_accessor(self, view_idx: int, component_type: int, count: int,
                     acc_type: str, byte_offset: int = 0,
                     min_vals=None, max_vals=None) -> int:
        """Add an accessor, return its index."""
        acc = {
            'bufferView': view_idx,
            'componentType': component_type,
            'count': count,
            'type': acc_type,
        }
        if byte_offset > 0:
            acc['byteOffset'] = byte_offset
        if min_vals is not None:
            acc['min'] = min_vals
        if max_vals is not None:
            acc['max'] = max_vals
        self.accessors.append(acc)
        return len(self.accessors) - 1

    def pack_floats(self, values: List[float]) -> int:
        """Append float32 array, return byte offset."""
        offset = len(self.data)
        for v in values:
            self.data.extend(struct.pack('<f', float(v)))
        return offset

    def pack_uint16(self, values: List[int]) -> int:
        """Append uint16 array, return byte offset."""
        offset = len(self.data)
        for v in values:
            self.data.extend(struct.pack('<H', int(v)))
        return offset

    def pack_uint32(self, values: List[int]) -> int:
        """Append uint32 array, return byte offset."""
        offset = len(self.data)
        for v in values:
            self.data.extend(struct.pack('<I', int(v)))
        return offset


# ─── GLTF Component Types ────────────────────────────────────────

FLOAT = 5126
UNSIGNED_SHORT = 5123
UNSIGNED_INT = 5125

ARRAY_BUFFER = 34962
ELEMENT_ARRAY_BUFFER = 34963


# ─── Main GLB Builder ────────────────────────────────────────────

class GLBBuilder:
    """Builds a complete glTF 2.0 GLB file."""

    def __init__(self):
        self.buf = BufferBuilder()
        self.gltf: Dict[str, Any] = {
            'asset': {'version': '2.0', 'generator': 'gf_exporter'},
            'scene': 0,
            'scenes': [{'nodes': []}],
            'nodes': [],
            'meshes': [],
            'skins': [],
            'materials': [],
            'accessors': [],
            'bufferViews': [],
            'buffers': [{'byteLength': 0}],
        }
        self._node_idx = 0
        self._skin_joints = []

    def add_node(self, name: str, translation=None, rotation=None, scale=None,
                 mesh_idx: int = -1, skin_idx: int = -1,
                 children: List[int] = None) -> int:
        """Add a node, return its index."""
        node = {'name': name}
        if translation:
            node['translation'] = list(translation)
        if rotation:
            node['rotation'] = list(rotation)
        if scale:
            node['scale'] = list(scale)
        if mesh_idx >= 0:
            node['mesh'] = mesh_idx
        if skin_idx >= 0:
            node['skin'] = skin_idx
        if children:
            node['children'] = children

        self.gltf['nodes'].append(node)
        idx = len(self.gltf['nodes']) - 1
        return idx

    def add_skeleton_nodes(self, bones: List[Any]) -> List[int]:
        """Add bone nodes, return list of node indices."""
        node_indices = []
        for bone in bones:
            idx = self.add_node(
                name=bone.name,
                translation=list(bone.local_position),
                rotation=list(bone.local_rotation),
            )
            node_indices.append(idx)
            self._skin_joints.append(idx)

        # Set up parent-child relationships
        for i, bone in enumerate(bones):
            if bone.parent_index >= 0 and bone.parent_index < len(node_indices):
                parent_node = self.gltf['nodes'][node_indices[bone.parent_index]]
                if 'children' not in parent_node:
                    parent_node['children'] = []
                parent_node['children'].append(node_indices[i])

        # Add root bones to scene
        for i, bone in enumerate(bones):
            if bone.parent_index < 0:
                self.gltf['scenes'][0]['nodes'].append(node_indices[i])

        return node_indices

    def add_skin(self, joints: List[int], ibm_accessor: int) -> int:
        """Add a skin, return its index."""
        skin = {
            'joints': joints,
            'inverseBindMatrices': ibm_accessor,
        }
        self.gltf['skins'].append(skin)
        return len(self.gltf['skins']) - 1

    def add_mesh(self, name: str, primitives: List[dict]) -> int:
        """Add a mesh, return its index."""
        mesh = {'name': name, 'primitives': primitives}
        self.gltf['meshes'].append(mesh)
        return len(self.gltf['meshes']) - 1

    def add_material(self, name: str, base_color: List[float],
                     metallic: float = 0.0, roughness: float = 0.8,
                     texture_idx: int = -1) -> int:
        """Add a PBR material, return its index."""
        pbr = {
            'baseColorFactor': base_color,
            'metallicFactor': metallic,
            'roughnessFactor': roughness,
        }
        if texture_idx >= 0:
            pbr['baseColorTexture'] = {'index': texture_idx}

        mat = {'name': name, 'pbrMetallicRoughness': pbr}
        self.gltf['materials'].append(mat)
        return len(self.gltf['materials']) - 1

    def add_image(self, png_bytes: bytes) -> int:
        """Embed a PNG image in the buffer, return image index."""
        if 'images' not in self.gltf:
            self.gltf['images'] = []
        if 'textures' not in self.gltf:
            self.gltf['textures'] = []

        # Store PNG bytes in buffer
        offset = len(self.buf.data)
        self.buf.data.extend(png_bytes)
        view_idx = self.buf.add_view(offset, len(png_bytes), 0)

        img_idx = len(self.gltf['images'])
        self.gltf['images'].append({
            'name': f'image_{img_idx}',
            'mimeType': 'image/png',
            'bufferView': view_idx,
        })

        # Create texture referencing this image
        tex_idx = len(self.gltf['textures'])
        self.gltf['textures'].append({
            'name': f'texture_{img_idx}',
            'source': img_idx,
        })

        return tex_idx

    def add_primitive_geometry(self, vertices: List[Tuple],
                               faces: List[Tuple],
                               normals: List[Tuple] = None,
                               uvs: List[Tuple] = None,
                               material_idx: int = 0,
                               joints_data: List[int] = None,
                               weights_data: List[float] = None) -> dict:
        """Pack vertex data into buffer and return a primitive dict."""
        n_verts = len(vertices)

        # Positions
        pos_flat = []
        pos_min = [float('inf')] * 3
        pos_max = [float('-inf')] * 3
        for v in vertices:
            pos_flat.extend(v)
            for j in range(3):
                pos_min[j] = min(pos_min[j], v[j])
                pos_max[j] = max(pos_max[j], v[j])

        pos_offset = self.buf.pack_floats(pos_flat)
        pos_len = n_verts * 3 * 4
        pos_view = self.buf.add_view(pos_offset, pos_len, ARRAY_BUFFER)
        pos_acc = self.buf.add_accessor(pos_view, FLOAT, n_verts, 'VEC3',
                                         min_vals=pos_min, max_vals=pos_max)

        primitive = {
            'attributes': {'POSITION': pos_acc},
            'mode': 4,  # TRIANGLES
            'material': material_idx,
        }

        # Normals
        if normals and len(normals) == n_verts:
            norm_flat = []
            for n in normals:
                norm_flat.extend(n)
            norm_offset = self.buf.pack_floats(norm_flat)
            norm_view = self.buf.add_view(norm_offset, n_verts * 3 * 4, ARRAY_BUFFER)
            norm_acc = self.buf.add_accessor(norm_view, FLOAT, n_verts, 'VEC3')
            primitive['attributes']['NORMAL'] = norm_acc
        else:
            # Generate flat normals
            norm_flat = self._generate_normals(vertices, faces)
            norm_offset = self.buf.pack_floats(norm_flat)
            norm_view = self.buf.add_view(norm_offset, n_verts * 3 * 4, ARRAY_BUFFER)
            norm_acc = self.buf.add_accessor(norm_view, FLOAT, n_verts, 'VEC3')
            primitive['attributes']['NORMAL'] = norm_acc

        # UVs
        if uvs and len(uvs) == n_verts:
            uv_flat = []
            for uv in uvs:
                uv_flat.extend([uv[0], 1.0 - uv[1]])  # flip V for glTF
            uv_offset = self.buf.pack_floats(uv_flat)
            uv_view = self.buf.add_view(uv_offset, n_verts * 2 * 4, ARRAY_BUFFER)
            uv_acc = self.buf.add_accessor(uv_view, FLOAT, n_verts, 'VEC2')
            primitive['attributes']['TEXCOORD_0'] = uv_acc

        # Joints (bone indices for skinning)
        if joints_data:
            joint_offset = self.buf.pack_uint16(joints_data)
            joint_view = self.buf.add_view(joint_offset, len(joints_data) * 2, ARRAY_BUFFER)
            joint_acc = self.buf.add_accessor(joint_view, UNSIGNED_SHORT,
                                               len(joints_data) // 4, 'VEC4')
            primitive['attributes']['JOINTS_0'] = joint_acc

        # Weights
        if weights_data:
            weight_offset = self.buf.pack_floats(weights_data)
            weight_view = self.buf.add_view(weight_offset, len(weights_data) * 4, ARRAY_BUFFER)
            weight_acc = self.buf.add_accessor(weight_view, FLOAT,
                                                len(weights_data) // 4, 'VEC4')
            primitive['attributes']['WEIGHTS_0'] = weight_acc

        # Indices
        idx_flat = []
        for f in faces:
            idx_flat.extend(f)
        idx_offset = self.buf.pack_uint32(idx_flat)
        idx_view = self.buf.add_view(idx_offset, len(idx_flat) * 4, ELEMENT_ARRAY_BUFFER)
        idx_acc = self.buf.add_accessor(idx_view, UNSIGNED_INT, len(idx_flat), 'SCALAR')
        primitive['indices'] = idx_acc

        return primitive

    def add_inverse_bind_matrices(self, bones: List[Any]) -> int:
        """Compute and pack correct inverse bind matrices for skinning from the bone hierarchy."""
        # Helper to convert translation and quaternion to 4x4 column-major matrix
        def compose_mat4(pos, rot):
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

        def mat4_mul(A, B):
            C = [0.0] * 16
            for col in range(4):
                for row in range(4):
                    val = 0.0
                    for k in range(4):
                        val += A[k * 4 + row] * B[col * 4 + k]
                    C[col * 4 + row] = val
            return C

        def mat4_inv(M):
            # Transposed rotation part (column-major)
            r00, r10, r20 = M[0], M[1], M[2]
            r01, r11, r21 = M[4], M[5], M[6]
            r02, r12, r22 = M[8], M[9], M[10]
            tx, ty, tz = M[12], M[13], M[14]
            
            ir00, ir10, ir20 = r00, r01, r02
            ir01, ir11, ir21 = r10, r11, r12
            ir02, ir12, ir22 = r20, r21, r22
            
            # Translation part ( -R^T * T )
            itx = -(ir00 * tx + ir01 * ty + ir02 * tz)
            ity = -(ir10 * tx + ir11 * ty + ir12 * tz)
            itz = -(ir20 * tx + ir21 * ty + ir22 * tz)
            
            return [
                ir00, ir10, ir20, 0.0,
                ir01, ir11, ir21, 0.0,
                ir02, ir12, ir22, 0.0,
                itx, ity, itz, 1.0
            ]

        # Compute world transforms for all bones
        world_transforms = [None] * len(bones)
        
        def get_world_transform(i):
            if world_transforms[i] is not None:
                return world_transforms[i]
            
            bone = bones[i]
            local_m = compose_mat4(bone.local_position, bone.local_rotation)
            
            if bone.parent_index < 0:
                world_transforms[i] = local_m
            else:
                parent_m = get_world_transform(bone.parent_index)
                world_transforms[i] = mat4_mul(parent_m, local_m)
            return world_transforms[i]

        for i in range(len(bones)):
            get_world_transform(i)

        # Invert all world transforms to get Inverse Bind Matrices
        ibm_flat = []
        for i in range(len(bones)):
            inv_m = mat4_inv(world_transforms[i])
            ibm_flat.extend(inv_m)

        offset = self.buf.pack_floats(ibm_flat)
        view = self.buf.add_view(offset, len(ibm_flat) * 4, 0)
        acc = self.buf.add_accessor(view, FLOAT, len(bones), 'MAT4')
        return acc

    def add_animation(self, name: str, clip, skeleton_bones: List[str],
                      bone_node_indices: List[int]):
        """Add a glTF animation from a GF AnimClip."""
        if not clip or not clip.tracks:
            return

        gltf_anim = {'name': name, 'channels': [], 'samplers': []}

        # For each bone that has animation data
        for bone_name in skeleton_bones:
            if bone_name not in clip.tracks:
                continue

            track = clip.tracks[bone_name]
            if not track.keyframes:
                continue

            bone_idx = skeleton_bones.index(bone_name)
            node_idx = bone_node_indices[bone_idx]

            sorted_keyframes = sorted(track.keyframes, key=lambda kf: kf.frame)
            fps = 60.0
            times = [kf.frame / fps for kf in sorted_keyframes]
            time_offset = self.buf.pack_floats(times)
            time_view = self.buf.add_view(time_offset, len(times) * 4, 0)
            time_acc = self.buf.add_accessor(time_view, FLOAT, len(times), 'SCALAR',
                                             min_vals=[times[0]], max_vals=[times[-1]])

            if bone_name == 'player':
                # Translation channel
                translations = []
                for kf in sorted_keyframes:
                    if kf.position:
                        translations.extend(vec3_zup_to_yup(kf.position))
                    else:
                        translations.extend([0.0, 0.0, 0.0])

                trans_offset = self.buf.pack_floats(translations)
                trans_view = self.buf.add_view(trans_offset, len(translations) * 4, 0)
                trans_acc = self.buf.add_accessor(trans_view, FLOAT, len(sorted_keyframes), 'VEC3')

                sampler = {'input': time_acc, 'output': trans_acc, 'interpolation': 'LINEAR'}
                gltf_anim['samplers'].append(sampler)
                gltf_anim['channels'].append({
                    'sampler': len(gltf_anim['samplers']) - 1,
                    'target': {'node': node_idx, 'path': 'translation'},
                })
            else:
                # Rotation channel
                rotations = []
                for kf in sorted_keyframes:
                    q = quat_zup_to_yup(kf.rotation)
                    q = normalize_quat(q)
                    rotations.extend(q)

                rot_offset = self.buf.pack_floats(rotations)
                rot_view = self.buf.add_view(rot_offset, len(rotations) * 4, 0)
                rot_acc = self.buf.add_accessor(rot_view, FLOAT, len(sorted_keyframes), 'VEC4')

                sampler = {'input': time_acc, 'output': rot_acc, 'interpolation': 'LINEAR'}
                gltf_anim['samplers'].append(sampler)
                gltf_anim['channels'].append({
                    'sampler': len(gltf_anim['samplers']) - 1,
                    'target': {'node': node_idx, 'path': 'rotation'},
                })

        if gltf_anim['channels']:
            if 'animations' not in self.gltf:
                self.gltf['animations'] = []
            self.gltf['animations'].append(gltf_anim)

    def finalize(self) -> bytes:
        """Finalize and return the GLB binary data."""
        # Update buffer byteLength
        self.gltf['buffers'][0]['byteLength'] = len(self.buf.data)

        # Copy accessors and bufferViews to glTF
        self.gltf['accessors'] = self.buf.accessors
        self.gltf['bufferViews'] = self.buf.views

        return pack_glb(self.gltf, bytes(self.buf.data))

    def _generate_normals(self, vertices, faces):
        """Generate per-vertex normals from face data."""
        normals = [[0.0, 0.0, 0.0] for _ in range(len(vertices))]
        for f in faces:
            v0 = vertices[f[0]]
            v1 = vertices[f[1]]
            v2 = vertices[f[2]]
            # Compute face normal
            e1 = [v1[i] - v0[i] for i in range(3)]
            e2 = [v2[i] - v0[i] for i in range(3)]
            nx = e1[1] * e2[2] - e1[2] * e2[1]
            ny = e1[2] * e2[0] - e1[0] * e2[2]
            nz = e1[0] * e2[1] - e1[1] * e2[0]
            length = (nx * nx + ny * ny + nz * nz) ** 0.5
            if length > 0:
                nx /= length
                ny /= length
                nz /= length
            for idx in f:
                normals[idx][0] += nx
                normals[idx][1] += ny
                normals[idx][2] += nz

        # Normalize
        result = []
        for n in normals:
            length = (n[0] ** 2 + n[1] ** 2 + n[2] ** 2) ** 0.5
            if length > 0:
                result.extend([n[0] / length, n[1] / length, n[2] / length])
            else:
                result.extend([0.0, 1.0, 0.0])
        return result
