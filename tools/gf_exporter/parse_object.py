"""
Parser for .object (GameplayFootball scene graph) files.
Extracts the skeleton hierarchy with bone positions and rotations.
"""
import os
import re
import math
from dataclasses import dataclass, field
from typing import List, Dict, Tuple, Optional
import xml.etree.ElementTree as ET
from coords import axis_zup_to_yup, vec3_zup_to_yup


def axis_angle_to_quaternion(angle_deg: float, ax: float, ay: float, az: float) -> Tuple[float, float, float, float]:
    """Convert axis-angle (degrees) to normalized quaternion (x, y, z, w)."""
    # Normalize axis
    length = math.sqrt(ax * ax + ay * ay + az * az)
    if length < 1e-10:
        return (0.0, 0.0, 0.0, 1.0)
    ax /= length
    ay /= length
    az /= length

    half = math.radians(angle_deg) / 2.0
    s = math.sin(half)
    return (ax * s, ay * s, az * s, math.cos(half))


@dataclass
class Bone:
    name: str
    parent_index: int = -1  # -1 = root
    local_position: Tuple[float, float, float] = (0.0, 0.0, 0.0)
    local_rotation: Tuple[float, float, float, float] = (0.0, 0.0, 0.0, 1.0)
    mesh_file: str = ""  # referenced .ase file
    mesh_name: str = ""  # geometry name in object


# Standard GF body part order (must match BodyPart enum in animation.hpp)
BODY_PART_ORDER = [
    'player',
    'body',
    'middle',
    'neck',
    'left_thigh',
    'right_thigh',
    'left_knee',
    'right_knee',
    'left_ankle',
    'right_ankle',
    'left_shoulder',
    'right_shoulder',
    'left_elbow',
    'right_elbow',
]

BODY_PART_INDEX = {name: i for i, name in enumerate(BODY_PART_ORDER)}

# Parent mapping for GF skeleton
BONE_PARENTS = {
    'body': 'player',
    'middle': 'body',
    'neck': 'middle',
    'left_thigh': 'body',
    'right_thigh': 'body',
    'left_knee': 'left_thigh',
    'right_knee': 'right_thigh',
    'left_ankle': 'left_knee',
    'right_ankle': 'right_knee',
    'left_shoulder': 'middle',
    'right_shoulder': 'middle',
    'left_elbow': 'left_shoulder',
    'right_elbow': 'right_shoulder',
    'player': None,  # root
}


def parse_object_xml(filepath: str) -> List[Bone]:
    """Parse a .object XML file to extract the skeleton hierarchy."""
    with open(filepath, 'r', encoding='utf-8', errors='replace') as f:
        content = f.read()

    # The .object files are not strict XML (they have unquoted attributes sometimes)
    # Use regex-based parsing for robustness
    bones = []
    bone_stack = []  # stack of (bone_name, depth)

    # Find all nodes and geometries
    lines = content.split('\n')
    depth = 0
    current_bone = None

    for line in lines:
        stripped = line.strip()

        # Track depth via braces
        if '{' in stripped and not stripped.startswith('{'):
            depth += 1
        if stripped == '}' or stripped.startswith('}'):
            depth -= 1
            if bone_stack and bone_stack[-1][1] >= depth:
                bone_stack.pop()

        # Parse <name>
        name_match = re.search(r'<name>\s*(\S+)\s*</name>', stripped)
        if name_match:
            bone_name = name_match.group(1)
            parent_idx = -1
            if bone_stack:
                parent_name = bone_stack[-1][0]
                parent_idx = BODY_PART_INDEX.get(parent_name, -1)

            current_bone = Bone(
                name=bone_name,
                parent_index=parent_idx,
            )
            bone_stack.append((bone_name, depth))
            bones.append(current_bone)
            continue

        # Parse <position>
        pos_match = re.search(r'<position>\s*([^<]+)\s*</position>', stripped)
        if pos_match and current_bone:
            parts = pos_match.group(1).strip().split(',')
            if len(parts) >= 3:
                try:
                    current_bone.local_position = (
                        float(parts[0].strip()),
                        float(parts[1].strip()),
                        float(parts[2].strip())
                    )
                except ValueError:
                    pass
            continue

        # Parse <rotation> — GF uses axis-angle in DEGREES: (angle, axisX, axisY, axisZ) in Z-up
        rot_match = re.search(r'<rotation>\s*([^<]+)\s*</rotation>', stripped)
        if rot_match and current_bone:
            parts = rot_match.group(1).strip().split(',')
            if len(parts) >= 4:
                try:
                    angle = float(parts[0].strip())
                    ax = float(parts[1].strip())
                    ay = float(parts[2].strip())
                    az = float(parts[3].strip())
                    # Convert axis from Z-up to Y-up
                    ax, ay, az = axis_zup_to_yup(ax, ay, az)
                    current_bone.local_rotation = axis_angle_to_quaternion(angle, ax, ay, az)
                except ValueError:
                    pass
            continue

        # Parse <filename> inside <geometry>
        file_match = re.search(r'<filename>\s*([^<]+)\s*</filename>', stripped)
        if file_match and current_bone:
            current_bone.mesh_file = file_match.group(1).strip()
            continue

        # Parse geometry <name>
        geom_name_match = re.search(r'<geometry>.*?<name>\s*(\S+)\s*</name>', stripped, re.DOTALL)
        if not geom_name_match:
            # Try simpler pattern
            pass

    return bones


def build_skeleton_from_object(object_path: str) -> List[Bone]:
    """Build the complete 14-bone skeleton from a player.object file.
    Ensures all 14 body parts are present in the correct order.
    """
    parsed = parse_object_xml(object_path)

    # Build lookup by name
    bone_map = {b.name: b for b in parsed}

    # Build ordered skeleton
    skeleton = []
    for bp_name in BODY_PART_ORDER:
        if bp_name in bone_map:
            bone = bone_map[bp_name]
            # Set parent index from our known hierarchy
            parent_name = BONE_PARENTS.get(bp_name)
            if parent_name:
                bone.parent_index = BODY_PART_INDEX.get(parent_name, -1)
            else:
                bone.parent_index = -1
            # Convert position from Z-up to Y-up
            bone.local_position = vec3_zup_to_yup(bone.local_position)
            skeleton.append(bone)
        else:
            # Create placeholder bone
            parent_name = BONE_PARENTS.get(bp_name)
            parent_idx = BODY_PART_INDEX.get(parent_name, -1) if parent_name else -1
            skeleton.append(Bone(
                name=bp_name,
                parent_index=parent_idx,
            ))

    print(f"  [OBJ] Skeleton: {len(skeleton)} bones")
    for b in skeleton:
        print(f"        [{b.parent_index:2d}] -> {b.name} "
              f"pos={b.local_position} mesh={b.mesh_file}")

    return skeleton


def get_bone_mesh_mapping(skeleton: List[Bone]) -> Dict[str, str]:
    """Return mapping from bone name to .ase mesh file name (without extension)."""
    mapping = {}
    for bone in skeleton:
        if bone.mesh_file:
            mesh_key = os.path.basename(bone.mesh_file).replace('.ase', '')
            mapping[bone.name] = mesh_key
    return mapping
