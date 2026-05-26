import json, struct, sys

with open(sys.argv[1], 'rb') as f:
    data = f.read()

magic, version, total_len = struct.unpack('<III', data[:12])
json_len, json_type = struct.unpack('<II', data[12:20])
json_bytes = data[20:20+json_len]
gltf = json.loads(json_bytes)

print('Meshes:')
for i, mesh in enumerate(gltf.get('meshes', [])):
    print(f'  {i}: {mesh["name"]}')

print()
print('Nodes with meshes:')
for i, node in enumerate(gltf.get('nodes', [])):
    if 'mesh' in node:
        mesh_idx = node['mesh']
        mesh_name = gltf['meshes'][mesh_idx]['name']
        print(f'  {i}: {node["name"]} -> mesh {mesh_idx} ({mesh_name})')
