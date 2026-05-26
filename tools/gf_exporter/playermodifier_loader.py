"""Load head + eyes from playermodifier.json (Three.js Editor export).

Extracts the merged head (mesh_neck + mesh_neck_1) and eye meshes with
all node transforms baked into vertex data, ready for GLB export.
"""
import json
import os
import numpy as np

_JSON_PATH = os.path.join(os.path.dirname(__file__), 'output', 'textures', 'playermodifier.json')

with open(_JSON_PATH, 'r', encoding='utf-8') as f:
    _DATA = json.load(f)


def _geometry_by_uuid(uuid: str):
    """Return the BufferGeometry dict matching uuid."""
    for g in _DATA.get('geometries', []):
        if g.get('uuid') == uuid:
            return g
    raise KeyError(f'Geometry {uuid} not found')


def _node_by_name(name: str):
    """DFS through the object tree to find a node by name."""
    stack = [_DATA['object']]
    while stack:
        node = stack.pop()
        if node.get('name') == name:
            return node
        stack.extend(node.get('children', []))
    raise KeyError(f'Node {name} not found')


def _parse_buffer(arr_dict):
    """Convert a JSON Float32Array / Uint32Array dict to a numpy array."""
    t = arr_dict['type']
    raw = arr_dict['array']
    if t == 'Float32Array':
        return np.array(raw, dtype=np.float32)
    if t == 'Uint32Array':
        return np.array(raw, dtype=np.uint32)
    raise TypeError(f'Unsupported array type {t}')


def _extract_geometry(geo_uuid: str):
    """Return (positions, normals, uvs, indices) for a geometry UUID."""
    geo = _geometry_by_uuid(geo_uuid)
    attrs = geo['data']['attributes']
    pos = _parse_buffer(attrs['position']).reshape(-1, 3)
    norm = _parse_buffer(attrs['normal']).reshape(-1, 3)
    uv = _parse_buffer(attrs['uv']).reshape(-1, 2)
    idx = _parse_buffer(geo['data']['index'])
    return pos, norm, uv, idx


def _apply_matrix(verts: np.ndarray, norms: np.ndarray, matrix: list):
    """Apply a Three.js 4x4 column-major matrix to vertices and normals."""
    # Extract 3x3 linear part and translation
    m = np.array(matrix, dtype=np.float64).reshape(4, 4, order='F')
    lin = m[:3, :3]
    trans = m[:3, 3]

    # Transform vertices
    new_verts = verts @ lin.T + trans

    # Transform normals (inverse-transpose of linear part, then renormalize)
    # For simplicity, apply linear part and renormalize; works for scale/rot
    new_norms = norms @ lin.T
    lengths = np.linalg.norm(new_norms, axis=1, keepdims=True)
    lengths[lengths == 0] = 1
    new_norms /= lengths

    return new_verts.astype(np.float32), new_norms.astype(np.float32)


def _get_mesh_data(node_name: str):
    """Return transformed (verts, norms, uvs, indices) for a named mesh node."""
    node = _node_by_name(node_name)
    geo_uuid = node['geometry']
    pos, norm, uv, idx = _extract_geometry(geo_uuid)
    matrix = node.get('matrix')
    if matrix and not np.allclose(matrix, np.eye(4).flatten()):
        pos, norm = _apply_matrix(pos, norm, matrix)
    return pos, norm, uv, idx


# ------------------------------------------------------------------
# Cached data
# ------------------------------------------------------------------

def _load_head_merged():
    """Merge mesh_neck (head) + mesh_neck_1 (neck connector)."""
    head_pos, head_norm, head_uv, head_idx = _get_mesh_data('mesh_neck')
    neck_pos, neck_norm, neck_uv, neck_idx = _get_mesh_data('mesh_neck_1')

    # Offset neck indices so they don't overlap with head indices
    neck_idx_offset = head_pos.shape[0]
    neck_idx = neck_idx + neck_idx_offset

    # Concatenate
    merged_pos = np.vstack([head_pos, neck_pos])
    merged_norm = np.vstack([head_norm, neck_norm])
    merged_uv = np.vstack([head_uv, neck_uv])
    merged_idx = np.concatenate([head_idx, neck_idx])

    return merged_pos, merged_norm, merged_uv, merged_idx


_HEAD_POS, _HEAD_NORM, _HEAD_UV, _HEAD_IDX = _load_head_merged()

_WHITE_POS, _WHITE_NORM, _WHITE_UV, _WHITE_IDX = _get_mesh_data('eye_white_node')
_BLACK_POS, _BLACK_NORM, _BLACK_UV, _BLACK_IDX = _get_mesh_data('eye_black_node')
_BROWN_POS, _BROWN_NORM, _BROWN_UV, _BROWN_IDX = _get_mesh_data('eye_brown_node')


def get_head_mesh():
    """Return the merged head+neck mesh.

    Tuple: (vertices, normals, uvs, indices)
    """
    return _HEAD_POS, _HEAD_NORM, _HEAD_UV, _HEAD_IDX


def get_eye_meshes():
    """Return three eye meshes (white, black, brown) positioned for the head.

    Each mesh is a tuple: (vertices, normals, uvs, indices, material_name)
    """
    return [
        (_WHITE_POS, _WHITE_NORM, _WHITE_UV, _WHITE_IDX, 'eye_white'),
        (_BLACK_POS, _BLACK_NORM, _BLACK_UV, _BLACK_IDX, 'eye_black'),
        (_BROWN_POS, _BROWN_NORM, _BROWN_UV, _BROWN_IDX, 'eye_brown'),
    ]


def get_eye_offset():
    """Return the approximate (x, y, z) center of the eyes in neck space."""
    # All three eye nodes have nearly the same translation; use white as base
    node = _node_by_name('eye_white_node')
    m = np.array(node['matrix'], dtype=np.float64).reshape(4, 4)
    return (float(m[0, 3]), float(m[1, 3]), float(m[2, 3]))


if __name__ == '__main__':
    print('Head mesh:', _HEAD_POS.shape[0], 'verts,', len(_HEAD_IDX) // 3, 'faces')
    print('  bounds X=', _HEAD_POS[:, 0].min(), '..', _HEAD_POS[:, 0].max())
    print('         Y=', _HEAD_POS[:, 1].min(), '..', _HEAD_POS[:, 1].max())
    print('         Z=', _HEAD_POS[:, 2].min(), '..', _HEAD_POS[:, 2].max())
    print()
    for pos, _, _, idx, name in get_eye_meshes():
        print(f'{name}: {pos.shape[0]} verts, {len(idx) // 3} faces')
        print(f'  center = ({pos[:,0].mean():.4f}, {pos[:,1].mean():.4f}, {pos[:,2].mean():.4f})')
    print(f'eye offset: {get_eye_offset()}')
