"""Load eye meshes from headproto.gltf + scene.bin and scale them to GF head size."""
import struct, json, os
import numpy as np

_GLTF_PATH = os.path.join(os.path.dirname(__file__), 'headproto.gltf')
_BIN_PATH = os.path.join(os.path.dirname(__file__), 'scene.bin')

with open(_GLTF_PATH, 'r') as f:
    _GLTF = json.load(f)

with open(_BIN_PATH, 'rb') as f:
    _BIN = f.read()


def _read_accessor(acc_idx: int) -> np.ndarray:
    acc = _GLTF['accessors'][acc_idx]
    bv = _GLTF['bufferViews'][acc['bufferView']]
    offset = bv.get('byteOffset', 0) + acc.get('byteOffset', 0)
    count = acc['count']
    ctype = acc['componentType']

    fmt_map = {5126: 'f', 5123: 'H', 5125: 'I'}
    size_map = {5126: 4, 5123: 2, 5125: 4}
    fmt = fmt_map[ctype]
    size = size_map[ctype]

    t = acc['type']
    comps = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4}[t]
    stride = bv.get('byteStride', size * comps)

    data = []
    for i in range(count):
        row = []
        for c in range(comps):
            pos = offset + i * stride + c * size
            row.append(struct.unpack('<' + fmt, _BIN[pos:pos + size])[0])
        data.append(row)
    return np.array(data, dtype=np.float32)


def _get_mesh_data(mesh_idx: int):
    prim = _GLTF['meshes'][mesh_idx]['primitives'][0]
    pos = _read_accessor(prim['attributes']['POSITION'])
    norm = _read_accessor(prim['attributes']['NORMAL'])
    uv = _read_accessor(prim['attributes']['TEXCOORD_0'])
    indices = _read_accessor(prim['indices']).astype(np.uint32).flatten()
    return pos, norm, uv, indices


# Pre-extract eye meshes
_HEAD_POS, _HEAD_NORM, _HEAD_UV, _HEAD_IDX = _get_mesh_data(0)
_WHITE_POS, _WHITE_NORM, _WHITE_UV, _WHITE_IDX = _get_mesh_data(1)
_BLACK_POS, _BLACK_NORM, _BLACK_UV, _BLACK_IDX = _get_mesh_data(2)
_BROWN_POS, _BROWN_NORM, _BROWN_UV, _BROWN_IDX = _get_mesh_data(3)

# Eyes_1 node translation from glTF
_EYES_NODE_MAT = _GLTF['nodes'][5]['matrix']
_EYES_TX, _EYES_TY, _EYES_TZ = _EYES_NODE_MAT[12], _EYES_NODE_MAT[13], _EYES_NODE_MAT[14]

# Proto head bounds
_PROTO_MIN = np.array([-0.071286, -0.000006, -0.103233], dtype=np.float32)
_PROTO_MAX = np.array([0.071262, 0.235409, 0.076609], dtype=np.float32)
_PROTO_SIZE = _PROTO_MAX - _PROTO_MIN

# GF head bounds (measured from head.ase)
_GF_MIN = np.array([-0.0881, 0.0207, -0.0748], dtype=np.float32)
_GF_MAX = np.array([0.0887, 0.2774, 0.1302], dtype=np.float32)
_GF_SIZE = _GF_MAX - _GF_MIN

# Scale factors
_SCALE = _GF_SIZE / _PROTO_SIZE  # [sx, sy, sz]

# Offset to align proto head with GF head bounds
# Proto base Y≈0, GF base Y≈0.0207  => offset Y≈0.0207
# Proto face Z≈0.0766, GF face Z≈0.1302, proto center Z≈-0.013, GF center Z≈0.028
# => need Z offset ≈ +0.043 to align centers
_COMMON_OFFSET = np.array([0.0, 0.0207, 0.043], dtype=np.float32)


def _transform_proto_points(points: np.ndarray) -> np.ndarray:
    """Scale proto points to GF size and apply common offset."""
    result = points.copy()
    result[:, 0] *= _SCALE[0]
    result[:, 1] *= _SCALE[1]
    result[:, 2] *= _SCALE[2]
    result += _COMMON_OFFSET
    return result


def _transform_proto_normals(normals: np.ndarray) -> np.ndarray:
    """Scale proto normals and renormalize."""
    result = normals.copy()
    result[:, 0] *= _SCALE[0]
    result[:, 1] *= _SCALE[1]
    result[:, 2] *= _SCALE[2]
    lengths = np.linalg.norm(result, axis=1, keepdims=True)
    lengths[lengths == 0] = 1
    result /= lengths
    return result


def get_head_mesh():
    """Return the full head mesh from headproto, scaled to GF head dimensions.
    Tuple: (vertices, normals, uvs, indices)
    """
    head_pos = _transform_proto_points(_HEAD_POS)
    head_norm = _transform_proto_normals(_HEAD_NORM)
    return head_pos, head_norm, _HEAD_UV, _HEAD_IDX


def get_eye_meshes():
    """Return three eye meshes (white, black, brown) positioned for GF head.
    Each mesh is a tuple: (vertices, normals, uvs, indices, material_name)
    Positions are in GF head local space.
    """
    white_pos = _transform_proto_points(_WHITE_POS)
    black_pos = _transform_proto_points(_BLACK_POS)
    brown_pos = _transform_proto_points(_BROWN_POS)

    white_norm = _transform_proto_normals(_WHITE_NORM)
    black_norm = _transform_proto_normals(_BLACK_NORM)
    brown_norm = _transform_proto_normals(_BROWN_NORM)

    return [
        (white_pos, white_norm, _WHITE_UV, _WHITE_IDX, 'eye_white'),
        (black_pos, black_norm, _BLACK_UV, _BLACK_IDX, 'eye_black'),
        (brown_pos, brown_norm, _BROWN_UV, _BROWN_IDX, 'eye_brown'),
    ]


def get_eye_offset():
    """Return the (x, y, z) offset of the eyes_1 node in GF space."""
    return (
        float(_EYES_TX * _SCALE[0] + _COMMON_OFFSET[0]),
        float(_EYES_TY * _SCALE[1] + _COMMON_OFFSET[1]),
        float(_EYES_TZ * _SCALE[2] + _COMMON_OFFSET[2]),
    )


if __name__ == '__main__':
    meshes = get_eye_meshes()
    for pos, norm, uv, idx, name in meshes:
        print(f'{name}: {len(pos)} verts, {len(idx)//3} faces')
        print(f'  bounds X={pos[:,0].min():.4f}..{pos[:,0].max():.4f}')
        print(f'         Y={pos[:,1].min():.4f}..{pos[:,1].max():.4f}')
        print(f'         Z={pos[:,2].min():.4f}..{pos[:,2].max():.4f}')
    print(f'eyes_1 offset: {get_eye_offset()}')
