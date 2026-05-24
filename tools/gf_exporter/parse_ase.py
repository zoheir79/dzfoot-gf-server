"""
Parser for .ase (3DS Max ASCII Export) files.
Extracts vertices, normals, UVs, faces, and material references.
"""
import re
import os
from dataclasses import dataclass, field
from typing import List, Tuple, Optional


@dataclass
class ASEMesh:
    name: str
    vertices: List[Tuple[float, float, float]] = field(default_factory=list)
    normals: List[Tuple[float, float, float]] = field(default_factory=list)
    uvs: List[Tuple[float, float]] = field(default_factory=list)
    faces: List[Tuple[int, int, int]] = field(default_factory=list)
    material_name: str = ""
    texture_path: str = ""


def parse_ase(filepath: str) -> List[ASEMesh]:
    """Parse a .ase file and return all geometry objects found."""
    with open(filepath, 'r', encoding='utf-8', errors='replace') as f:
        content = f.read()

    meshes = []
    lines = content.split('\n')

    # Extract material info first
    materials = {}
    in_material_list = False
    in_material = False
    current_mtl_id = -1
    current_mtl_name = ""
    current_mtl_texture = ""

    for line in lines:
        line = line.strip()
        if '*MATERIAL_LIST' in line:
            in_material_list = True
            continue
        if in_material_list and '*MATERIAL_COUNT' in line:
            continue
        if in_material_list and line.startswith('*MATERIAL '):
            in_material = True
            parts = line.split()
            if len(parts) >= 2:
                try:
                    current_mtl_id = int(parts[1])
                except ValueError:
                    current_mtl_id = -1
            current_mtl_name = ""
            current_mtl_texture = ""
            continue
        if in_material and '*MATERIAL_NAME' in line:
            m = re.search(r'"([^"]+)"', line)
            current_mtl_name = m.group(1) if m else line.split()[-1].strip('"')
            continue
        if in_material and '*BITMAP' in line:
            m = re.search(r'"([^"]+)"', line)
            if m:
                current_mtl_texture = m.group(1)
            continue
        if in_material and line == '}':
            in_material = False
            if current_mtl_id >= 0:
                materials[current_mtl_id] = {
                    'name': current_mtl_name,
                    'texture': current_mtl_texture
                }
            continue
        if in_material_list and line == '}':
            in_material_list = False
            continue

    # Parse geometry objects
    i = 0
    while i < len(lines):
        line = lines[i].strip()

        if '*GEOMOBJECT' in line:
            mesh = ASEMesh(name="")
            i += 1

            # Find NODE_NAME
            while i < len(lines):
                l = lines[i].strip()
                if '*NODE_NAME' in l:
                    m = re.search(r'"([^"]+)"', l)
                    mesh.name = m.group(1) if m else l.split()[-1].strip('"')
                    break
                i += 1

            # Check TIMEVALUE - only take first frame (0)
            time_value = -1
            while i < len(lines):
                l = lines[i].strip()
                if '*TIMEVALUE' in l:
                    parts = l.split()
                    if len(parts) >= 2:
                        try:
                            time_value = int(parts[1])
                        except ValueError:
                            pass
                    break
                if '*MESH' in l:
                    break
                i += 1

            # Skip if not first time value
            if time_value > 0:
                # Skip to end of GEOMOBJECT
                depth = 1
                while i < len(lines) and depth > 0:
                    l = lines[i].strip()
                    depth += l.count('{') - l.count('}')
                    i += 1
                continue

            # Find MESH block
            while i < len(lines):
                l = lines[i].strip()
                if l == '*MESH {' or l.startswith('*MESH {'):
                    i += 1
                    break
                i += 1

            # Parse mesh contents
            in_vertices = False
            in_faces = False
            in_vertex_normals = False
            in_tverts = False
            vertex_normals_temp = []
            brace_depth = 1  # we entered MESH {, so depth=1

            while i < len(lines):
                l = lines[i].strip()

                # Track brace depth
                open_braces = l.count('{')
                close_braces = l.count('}')
                brace_depth += open_braces - close_braces

                if brace_depth <= 0:
                    # End of MESH block
                    break

                if '*MESH_VERTEX_LIST' in l:
                    in_vertices = True
                    i += 1
                    continue

                if in_vertices and l.startswith('*MESH_VERTEX'):
                    parts = l.split()
                    if len(parts) >= 5:
                        try:
                            x, y, z = float(parts[2]), float(parts[3]), float(parts[4])
                            mesh.vertices.append((x, y, z))
                        except ValueError:
                            pass
                    i += 1
                    continue

                if in_vertices and '*MESH_FACE_LIST' in l:
                    in_vertices = False
                    in_faces = True
                    i += 1
                    continue

                if in_faces and l.startswith('*MESH_FACE'):
                    ma = re.search(r'A:\s*(\d+)', l)
                    mb = re.search(r'B:\s*(\d+)', l)
                    mc = re.search(r'C:\s*(\d+)', l)
                    if ma and mb and mc:
                        mesh.faces.append((int(ma.group(1)), int(mb.group(1)), int(mc.group(1))))
                    i += 1
                    continue

                if in_faces and '*MESH_NORMALS' in l:
                    in_faces = False
                    in_vertex_normals = True
                    i += 1
                    continue

                if in_vertex_normals and l.startswith('*MESH_VERTEXNORMAL'):
                    parts = l.split()
                    if len(parts) >= 5:
                        try:
                            x, y, z = float(parts[2]), float(parts[3]), float(parts[4])
                            vertex_normals_temp.append((x, y, z))
                        except ValueError:
                            pass
                    i += 1
                    continue

                if in_vertex_normals and '*MESH_TVERTLIST' in l:
                    in_vertex_normals = False
                    in_tverts = True
                    i += 1
                    continue

                if in_tverts and l.startswith('*MESH_TVERT'):
                    parts = l.split()
                    if len(parts) >= 4:
                        try:
                            u, v = float(parts[2]), float(parts[3])
                            mesh.uvs.append((u, v))
                        except ValueError:
                            pass
                    i += 1
                    continue

                if in_tverts and '*MESH_TFACE' in l:
                    in_tverts = False
                    i += 1
                    continue

                i += 1

            mesh.normals = vertex_normals_temp

            if materials:
                first = list(materials.values())[0]
                mesh.material_name = first['name']
                mesh.texture_path = first['texture']

            meshes.append(mesh)
            continue

        i += 1

    return meshes


def parse_all_models(models_dir: str) -> dict:
    """Parse all .ase files in a directory. Returns {name: [ASEMesh]}."""
    result = {}
    for fname in os.listdir(models_dir):
        if fname.endswith('.ase'):
            name = fname.replace('.ase', '')
            filepath = os.path.join(models_dir, fname)
            try:
                meshes = parse_ase(filepath)
                result[name] = meshes
                total_v = sum(len(m.vertices) for m in meshes)
                total_f = sum(len(m.faces) for m in meshes)
                print(f"  [ASE] {fname}: {total_v} verts, {total_f} faces, {len(meshes)} mesh(es)")
            except Exception as e:
                print(f"  [ASE] {fname}: ERROR - {e}")
                import traceback
                traceback.print_exc()
    return result
