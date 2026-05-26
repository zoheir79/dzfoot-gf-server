"""
Procedural geometry generator for player customization.
Adds hair, beard, ears, fingers, nose, eyes, mouth without Blender.
"""
import math
import numpy as np
from typing import List, Tuple, Dict, Optional


# ─── Geometric Primitives ────────────────────────────────────────

def create_sphere(radius: float, segments: int = 8, rings: int = 6,
                  center: Tuple[float, float, float] = (0, 0, 0)) -> Tuple[List, List]:
    """Create a UV sphere. Returns (vertices, faces)."""
    verts = []
    for i in range(rings + 1):
        phi = math.pi * i / rings
        for j in range(segments + 1):
            theta = 2.0 * math.pi * j / segments
            x = center[0] + radius * math.sin(phi) * math.cos(theta)
            y = center[1] + radius * math.cos(phi)
            z = center[2] + radius * math.sin(phi) * math.sin(theta)
            verts.append((x, y, z))

    faces = []
    for i in range(rings):
        for j in range(segments):
            a = i * (segments + 1) + j
            b = a + segments + 1
            faces.append((a, b, a + 1))
            faces.append((b, b + 1, a + 1))
    return verts, faces


def create_half_sphere(radius: float, segments: int = 8, rings: int = 4,
                       center: Tuple = (0, 0, 0),
                       direction: Tuple = (1, 0, 0)) -> Tuple[List, List]:
    """Create a half-sphere pointing in a direction (for ears)."""
    verts = []
    for i in range(rings + 1):
        phi = math.pi * 0.5 * i / rings
        for j in range(segments + 1):
            theta = 2.0 * math.pi * j / segments
            x = center[0] + radius * math.sin(phi) * math.cos(theta) * direction[0]
            y = center[1] + radius * math.cos(phi)
            z = center[2] + radius * math.sin(phi) * math.sin(theta)
            verts.append((x, y, z))

    faces = []
    for i in range(rings):
        for j in range(segments):
            a = i * (segments + 1) + j
            b = a + segments + 1
            faces.append((a, b, a + 1))
            faces.append((b, b + 1, a + 1))
    return verts, faces


def create_ellipsoid(rx: float, ry: float, rz: float,
                     segments: int = 10, rings: int = 8,
                     center: Tuple = (0, 0, 0)) -> Tuple[List, List]:
    """Create an ellipsoid (stretched sphere)."""
    verts = []
    for i in range(rings + 1):
        phi = math.pi * i / rings
        for j in range(segments + 1):
            theta = 2.0 * math.pi * j / segments
            x = center[0] + rx * math.sin(phi) * math.cos(theta)
            y = center[1] + ry * math.cos(phi)
            z = center[2] + rz * math.sin(phi) * math.sin(theta)
            verts.append((x, y, z))

    faces = []
    for i in range(rings):
        for j in range(segments):
            a = i * (segments + 1) + j
            b = a + segments + 1
            faces.append((a, b, a + 1))
            faces.append((b, b + 1, a + 1))
    return verts, faces


def create_cylinder(radius: float, length: float, segments: int = 8,
                    center: Tuple = (0, 0, 0),
                    axis: Tuple = (0, 1, 0)) -> Tuple[List, List]:
    """Create a cylinder along an axis."""
    verts = []
    half_len = length / 2.0
    # Two end caps
    for j in range(segments):
        angle = 2.0 * math.pi * j / segments
        x = radius * math.cos(angle)
        z = radius * math.sin(angle)
        # Top ring
        verts.append((center[0] + x, center[1] + half_len, center[2] + z))
        # Bottom ring
        verts.append((center[0] + x, center[1] - half_len, center[2] + z))

    # Top center
    verts.append((center[0], center[1] + half_len, center[2]))
    # Bottom center
    verts.append((center[0], center[1] - half_len, center[2]))

    faces = []
    top_center = len(verts) - 2
    bot_center = len(verts) - 1

    for j in range(segments):
        next_j = (j + 1) % segments
        # Side faces
        t0 = j * 2
        b0 = j * 2 + 1
        t1 = next_j * 2
        b1 = next_j * 2 + 1
        faces.append((t0, b0, t1))
        faces.append((b0, b1, t1))
        # Top cap
        faces.append((t0, t1, top_center))
        # Bottom cap
        faces.append((b1, b0, bot_center))

    return verts, faces


def create_cone(radius: float, height: float, segments: int = 8,
                center: Tuple = (0, 0, 0),
                direction: Tuple = (0, 1, 0)) -> Tuple[List, List]:
    """Create a cone pointing in direction (for nose)."""
    dx, dy, dz = direction
    length = math.sqrt(dx * dx + dy * dy + dz * dz)
    if length < 1e-10:
        dx, dy, dz = 0, 1, 0
    else:
        dx, dy, dz = dx / length, dy / length, dz / length

    # Build orthonormal basis (u, v) perpendicular to direction d
    # Pick an arbitrary vector not parallel to d
    if abs(dx) < 0.9:
        ax, ay, az = 1, 0, 0
    else:
        ax, ay, az = 0, 1, 0
    # u = normalize(cross(d, a))
    ux = dy * az - dz * ay
    uy = dz * ax - dx * az
    uz = dx * ay - dy * ax
    ul = math.sqrt(ux * ux + uy * uy + uz * uz)
    ux, uy, uz = ux / ul, uy / ul, uz / ul
    # v = cross(d, u)
    vx = dy * uz - dz * uy
    vy = dz * ux - dx * uz
    vz = dx * uy - dy * ux

    verts = []
    # Base ring
    for j in range(segments):
        angle = 2.0 * math.pi * j / segments
        ca = radius * math.cos(angle)
        sa = radius * math.sin(angle)
        bx = center[0] + ca * ux + sa * vx
        by = center[1] + ca * uy + sa * vy
        bz = center[2] + ca * uz + sa * vz
        verts.append((bx, by, bz))
    # Tip
    tx = center[0] + height * dx
    ty = center[1] + height * dy
    tz = center[2] + height * dz
    verts.append((tx, ty, tz))
    # Base center
    verts.append((center[0], center[1], center[2]))

    tip_idx = len(verts) - 2
    base_center = len(verts) - 1

    faces = []
    for j in range(segments):
        next_j = (j + 1) % segments
        faces.append((j, next_j, tip_idx))
        faces.append((next_j, j, base_center))

    return verts, faces


def create_ridge(width: float, height: float, length: float,
                 center: Tuple = (0, 0, 0)) -> Tuple[List, List]:
    """Create a ridge shape (for mohawk hair)."""
    verts = []
    hw = width / 2.0
    hl = length / 2.0

    # 8 vertices: 4 base corners, 4 top corners
    verts.append((center[0] - hw, center[1] - hl, center[2]))      # 0: base BL
    verts.append((center[0] + hw, center[1] - hl, center[2]))      # 1: base BR
    verts.append((center[0] + hw, center[1] + hl, center[2]))      # 2: base TR
    verts.append((center[0] - hw, center[1] + hl, center[2]))      # 3: base TL
    verts.append((center[0] - hw * 0.3, center[1] - hl, center[2] + height))  # 4: top BL
    verts.append((center[0] + hw * 0.3, center[1] - hl, center[2] + height))  # 5: top BR
    verts.append((center[0] + hw * 0.3, center[1] + hl, center[2] + height))  # 6: top TR
    verts.append((center[0] - hw * 0.3, center[1] + hl, center[2] + height))  # 7: top TL

    faces = [
        (0, 1, 5), (0, 5, 4),  # front
        (1, 2, 6), (1, 6, 5),  # right
        (2, 3, 7), (2, 7, 6),  # back
        (3, 0, 4), (3, 4, 7),  # left
        (4, 5, 6), (4, 6, 7),  # top
        (3, 2, 1), (3, 1, 0),  # bottom
    ]
    return verts, faces


# ─── Customization Features ──────────────────────────────────────

def generate_hair_mesh(style: str = "short") -> Tuple[List, List, Tuple]:
    """
    Generate a hair mesh.
    Styles: 'short', 'long', 'mohawk', 'bald'
    Returns (vertices, faces, offset_from_head_center)
    """
    if style == 'bald':
        return [], [], (0, 0, 0)

    if style == 'short':
        # Cap on dome, thick enough to be visible
        verts, faces = create_ellipsoid(0.055, 0.030, 0.050, segments=10, rings=6,
                                        center=(0, 0.270, 0.000))
        return verts, faces, (0, 0, 0)

    elif style == 'long':
        # Longer cap down the back
        verts, faces = create_ellipsoid(0.055, 0.075, 0.050, segments=10, rings=8,
                                        center=(0, 0.225, -0.010))
        return verts, faces, (0, 0, 0)

    elif style == 'mohawk':
        # Narrow ridge on top
        verts, faces = create_ridge(0.022, 0.070, 0.120, center=(0, 0.275, 0.000))
        return verts, faces, (0, 0, 0)

    elif style == 'curly':
        # Sphere on top
        verts, faces = create_sphere(0.058, segments=10, rings=8,
                                     center=(0, 0.255, 0.000))
        return verts, faces, (0, 0, 0)

    elif style == 'ponytail':
        # Cap + tail
        top_verts, top_faces = create_ellipsoid(0.052, 0.030, 0.048, segments=8, rings=5,
                                                center=(0, 0.255, 0.000))
        tail_verts, tail_faces = create_cylinder(0.018, 0.100, segments=6,
                                                  center=(0, 0.180, -0.045))
        offset = len(top_verts)
        all_verts = top_verts + tail_verts
        all_faces = top_faces + [(f[0] + offset, f[1] + offset, f[2] + offset)
                                 for f in tail_faces]
        return all_verts, all_faces, (0, 0, 0)

    # Default: short
    verts, faces = create_ellipsoid(0.055, 0.030, 0.050, segments=10, rings=6,
                                    center=(0, 0.270, 0.000))
    return verts, faces, (0, 0, 0)


def generate_beard_mesh(style: str = "none") -> Tuple[List, List, Tuple]:
    """
    Generate a beard mesh.
    Styles: 'none', 'stubble', 'short', 'full'
    """
    if style == 'none':
        return [], [], (0, 0, 0)

    if style == 'stubble':
        # Thin layer on chin
        verts, faces = create_ellipsoid(0.020, 0.007, 0.010, segments=6, rings=4,
                                        center=(0, 0.066, 0.088))
        return verts, faces, (0, 0, 0)

    elif style == 'short':
        # Small beard on chin
        verts, faces = create_ellipsoid(0.028, 0.018, 0.016, segments=8, rings=5,
                                        center=(0, 0.060, 0.088))
        return verts, faces, (0, 0, 0)

    elif style == 'full':
        # Full beard
        verts, faces = create_ellipsoid(0.040, 0.028, 0.022, segments=10, rings=6,
                                        center=(0, 0.054, 0.086))
        return verts, faces, (0, 0, 0)

    return [], [], (0, 0, 0)


def _build_ear_mesh(sign: float) -> Tuple[List, List]:
    """Build a flat realistic ear mesh. sign=-1 for left, +1 for right.

    The ear is a very thin, slightly curved oval disc pressed against the
    side of the head.  It is ~0.06 m tall, ~0.035 m wide, and only ~0.004 m
    thick so it never looks like a bulky cylinder.
    """
    verts = []
    faces = []

    N_RINGS = 4
    N_SEGS = 12

    # Ear centre on the side of the head (flush with surface)
    cx = sign * 0.084
    cy = 0.142
    cz = 0.028

    for ring in range(N_RINGS):
        t = ring / (N_RINGS - 1)  # 0 = outer rim, 1 = inner/back
        # Thickness falls to almost zero at the back
        thickness = 0.003 * (1.0 - t)
        # Oval radii shrink a little toward the back
        ry = 0.023 - t * 0.004
        rz = 0.012 - t * 0.002

        for seg in range(N_SEGS):
            angle = 2.0 * math.pi * seg / N_SEGS
            ly = ry * math.cos(angle)
            lz = rz * math.sin(angle)
            # Very slight backward tilt so the ear follows the head curvature
            y_local = ly * 0.985 - lz * 0.174
            z_local = ly * 0.174 + lz * 0.985
            x_local = thickness
            verts.append((cx + x_local * sign, cy + y_local, cz + z_local))

    # Faces between rings
    for ring in range(N_RINGS - 1):
        for seg in range(N_SEGS):
            a = ring * N_SEGS + seg
            b = ring * N_SEGS + (seg + 1) % N_SEGS
            c = (ring + 1) * N_SEGS + seg
            d = (ring + 1) * N_SEGS + (seg + 1) % N_SEGS
            faces.append((a, b, d))
            faces.append((a, d, c))

    # Close the back with a tiny fan (inner ring -> centre point)
    back_centre = len(verts)
    verts.append((cx, cy, cz))
    inner_start = (N_RINGS - 1) * N_SEGS
    for seg in range(N_SEGS):
        a = inner_start + seg
        b = inner_start + (seg + 1) % N_SEGS
        faces.append((b, a, back_centre))

    return verts, faces


def generate_ears() -> Tuple[List, List, Tuple]:
    """Generate both realistic ears."""
    left_v, left_f = _build_ear_mesh(-1)
    right_v, right_f = _build_ear_mesh(1)
    offset = len(left_v)
    all_verts = left_v + right_v
    all_faces = left_f + [(f[0] + offset, f[1] + offset, f[2] + offset) for f in right_f]
    return all_verts, all_faces, (0, 0, 0)


def generate_fingers(side: str = 'left') -> Tuple[List, List, Tuple]:
    """
    Generate 5 simple fingers as small cylinders.
    side: 'left' or 'right'
    """
    sign = 1 if side == 'left' else -1
    all_verts = []
    all_faces = []

    # Finger positions relative to wrist
    finger_configs = [
        ('thumb',  (sign * 0.03, -0.02, 0.01),  0.010, 0.05),
        ('index',  (sign * 0.02, -0.06, 0.005), 0.009, 0.07),
        ('middle', (sign * 0.005, -0.07, 0.0),  0.010, 0.075),
        ('ring',   (sign * -0.01, -0.06, -0.005), 0.009, 0.065),
        ('pinky',  (sign * -0.02, -0.04, -0.01), 0.008, 0.05),
    ]

    for name, pos, radius, length in finger_configs:
        verts, faces = create_cylinder(radius, length, segments=6, center=pos)
        offset = len(all_verts)
        all_verts.extend(verts)
        all_faces.extend([(f[0] + offset, f[1] + offset, f[2] + offset) for f in faces])

    return all_verts, all_faces, (0, -0.06, 0)


def _build_nose() -> Tuple[List, List]:
    """Build a realistic nose with bridge, tip, nostrils, and alae."""
    verts = []
    faces = []
    N_SEG = 12  # angular resolution
    N_RING = 8  # vertical rings from bridge to base

    # Nose base position on outer dome of face
    bx, by, bz = 0.0, 0.14, 0.10

    for ring in range(N_RING):
        t = ring / (N_RING - 1)  # 0 = bridge top, 1 = nostril base
        # Height along nose
        y = by + t * 0.04  # nose is ~4cm tall
        # Forward protrusion: small bump
        protrusion = 0.010 * math.sin(t * math.pi)  # max 1cm at mid-nose
        # Width: narrow at bridge, wide at alae
        width = 0.008 + t * 0.010  # 0.8cm at bridge, 1.8cm at base
        # Z offset: nose protrudes outward from dome
        z_center = bz + protrusion

        for seg in range(N_SEG):
            angle = 2.0 * math.pi * seg / N_SEG
            # Cross-section: rounded triangle (wider at bottom, narrower at top)
            # Use a superellipse-like shape
            ca = math.cos(angle)
            sa = math.sin(angle)
            # Shape factor: makes bottom flatter (nostril area)
            shape_y = 1.0 - 0.3 * t * abs(sa)
            rx = width * ca * shape_y
            rz = protrusion * 0.5 * sa * (1.0 + 0.5 * abs(ca))
            verts.append((bx + rx, y, z_center + rz))

    # Build faces between rings
    for ring in range(N_RING - 1):
        for seg in range(N_SEG):
            a = ring * N_SEG + seg
            b = ring * N_SEG + (seg + 1) % N_SEG
            c = (ring + 1) * N_SEG + seg
            d = (ring + 1) * N_SEG + (seg + 1) % N_SEG
            faces.append((a, b, d))
            faces.append((a, d, c))

    # Add nostrils (two small indentations at the base)
    base_start = (N_RING - 1) * N_SEG
    for side_sign in [-1, 1]:
        nostril_center = len(verts)
        nx = bx + side_sign * 0.012
        ny = by + 0.04
        nz = bz - 0.008
        verts.append((nx, ny, nz))
        # Connect nostril center to nearby base vertices
        for seg in range(N_SEG):
            vi = base_start + seg
            vx, vy, vz = verts[vi]
            # Only connect vertices near this side
            if (side_sign < 0 and vx <= bx + 0.002) or (side_sign > 0 and vx >= bx - 0.002):
                ni = (seg + 1) % N_SEG
                vj = base_start + ni
                vjx, vjy, vjz = verts[vj]
                if (side_sign < 0 and vjx <= bx + 0.002) or (side_sign > 0 and vjx >= bx - 0.002):
                    faces.append((vi, vj, nostril_center))

    return verts, faces


def _build_eye(sign: float) -> Tuple[List, List]:
    """Build a single realistic eye with eyelid. sign=-1 for left, +1 for right."""
    verts = []
    faces = []
    N_SEG = 14
    N_RING = 5

    # Eye position on outer dome of face
    ex, ey, ez = sign * 0.035, 0.17, 0.09

    for ring in range(N_RING):
        t = ring / (N_RING - 1)  # 0 = back of eye, 1 = front (cornea)
        # Eye is a flattened sphere, ~2cm wide x 1.2cm tall
        rx = 0.010 * (1.0 - abs(t - 0.5) * 1.8)  # width
        ry = 0.006 * (1.0 - abs(t - 0.5) * 1.8)  # height
        rz = 0.006 * (t - 0.5)  # depth offset

        for seg in range(N_SEG):
            angle = 2.0 * math.pi * seg / N_SEG
            vx = ex + rx * math.cos(angle)
            vy = ey + ry * math.sin(angle)
            vz = ez + rz
            verts.append((vx, vy, vz))

    # Build faces
    for ring in range(N_RING - 1):
        for seg in range(N_SEG):
            a = ring * N_SEG + seg
            b = ring * N_SEG + (seg + 1) % N_SEG
            c = (ring + 1) * N_SEG + seg
            d = (ring + 1) * N_SEG + (seg + 1) % N_SEG
            faces.append((a, b, d))
            faces.append((a, d, c))

    # Add upper eyelid (a curved flap above the eye)
    lid_start = len(verts)
    lid_segments = 10
    lid_arc = 0.7  # how much of the eye the lid covers
    for i in range(lid_segments + 1):
        t = i / lid_segments
        angle = math.pi * (0.5 - lid_arc) + t * math.pi * lid_arc * 2
        lx = ex + 0.014 * math.cos(angle)
        ly = ey + 0.008 * math.sin(angle)
        # Lid protrudes slightly
        lz = ez - 0.003
        verts.append((lx, ly, lz))
        # Inner edge of lid (closer to eye surface)
        lz2 = ez - 0.001
        verts.append((lx, ly, lz2))

    # Build lid faces
    for i in range(lid_segments):
        a = lid_start + i * 2
        b = lid_start + i * 2 + 1
        c = lid_start + (i + 1) * 2
        d = lid_start + (i + 1) * 2 + 1
        faces.append((a, b, d))
        faces.append((a, d, c))

    return verts, faces


def _build_mouth() -> Tuple[List, List]:
    """Build realistic lips with Cupid's bow."""
    verts = []
    faces = []
    N_SEG = 16
    N_RING = 4

    # Mouth position on outer dome of face
    mx, my, mz = 0.0, 0.07, 0.08

    for ring in range(N_RING):
        t = ring / (N_RING - 1)  # 0 = inner (mouth opening), 1 = outer lip edge
        # Lip width and height
        rx = 0.022 + t * 0.006  # wider at outer edge
        ry = 0.005 + t * 0.006  # taller at outer edge
        # Forward protrusion
        prot = 0.008 * (1.0 - abs(t - 0.5) * 2.0)

        for seg in range(N_SEG):
            angle = 2.0 * math.pi * seg / N_SEG
            ca = math.cos(angle)
            sa = math.sin(angle)

            # Cupid's bow on upper lip (top half of ellipse)
            y_mod = 1.0
            if sa > 0:  # upper lip
                # Cupid's bow: dip in center
                bow_dip = 0.3 * (1.0 - abs(ca)) * (1.0 - abs(ca))
                y_mod = 1.0 - bow_dip * 0.6
            else:  # lower lip
                # Fuller lower lip
                y_mod = 1.0 + 0.15 * (1.0 - abs(ca))

            vx = mx + rx * ca
            vy = my + ry * sa * y_mod
            vz = mz - prot * (1.0 - 0.3 * abs(ca))

            verts.append((vx, vy, vz))

    # Build faces
    for ring in range(N_RING - 1):
        for seg in range(N_SEG):
            a = ring * N_SEG + seg
            b = ring * N_SEG + (seg + 1) % N_SEG
            c = (ring + 1) * N_SEG + seg
            d = (ring + 1) * N_SEG + (seg + 1) % N_SEG
            faces.append((a, b, d))
            faces.append((a, d, c))

    return verts, faces


def generate_face_features() -> Tuple[List, List, Tuple]:
    """Generate realistic nose, eyes, and mouth."""
    all_verts = []
    all_faces = []

    # Nose
    nv, nf = _build_nose()
    offset = len(all_verts)
    all_verts.extend(nv)
    all_faces.extend([(f[0] + offset, f[1] + offset, f[2] + offset) for f in nf])

    # Left eye
    lv, lf = _build_eye(-1)
    offset = len(all_verts)
    all_verts.extend(lv)
    all_faces.extend([(f[0] + offset, f[1] + offset, f[2] + offset) for f in lf])

    # Right eye
    rv, rf = _build_eye(1)
    offset = len(all_verts)
    all_verts.extend(rv)
    all_faces.extend([(f[0] + offset, f[1] + offset, f[2] + offset) for f in rf])

    # Mouth
    mv, mf = _build_mouth()
    offset = len(all_verts)
    all_verts.extend(mv)
    all_faces.extend([(f[0] + offset, f[1] + offset, f[2] + offset) for f in mf])

    return all_verts, all_faces, (0, 0, 0)


# ─── Skin Tone Colors ────────────────────────────────────────────

SKIN_TONES = {
    'light':      [0.95, 0.82, 0.72, 1.0],
    'medium':     [0.82, 0.65, 0.50, 1.0],
    'olive':      [0.75, 0.60, 0.42, 1.0],
    'tan':        [0.65, 0.48, 0.32, 1.0],
    'brown':      [0.45, 0.30, 0.18, 1.0],
    'dark':       [0.28, 0.18, 0.10, 1.0],
    'deep':       [0.18, 0.11, 0.06, 1.0],
}

HAIR_COLORS = {
    'black':      [0.05, 0.05, 0.05, 1.0],
    'dark_brown': [0.20, 0.12, 0.06, 1.0],
    'brown':      [0.35, 0.22, 0.12, 1.0],
    'light_brown':[0.45, 0.30, 0.18, 1.0],
    'blonde':     [0.85, 0.75, 0.45, 1.0],
    'red':        [0.70, 0.25, 0.15, 1.0],
    'grey':       [0.55, 0.55, 0.55, 1.0],
    'white':      [0.90, 0.90, 0.85, 1.0],
}

EYE_COLORS = {
    'brown':  [0.35, 0.20, 0.05, 1.0],
    'blue':   [0.20, 0.40, 0.70, 1.0],
    'green':  [0.15, 0.55, 0.30, 1.0],
    'hazel':  [0.50, 0.45, 0.20, 1.0],
    'grey':   [0.50, 0.55, 0.60, 1.0],
}


def generate_face_plane() -> Tuple[List, List, List, List]:
    """Generate a flat face decal plane with planar UVs.

    The quad is positioned just in front of the face dome so the
    procedural face texture (eyes, nose, mouth) is visible only on the
    front, never wrapping around the sides.
    """
    verts = [
        (-0.032, 0.148, 0.121),
        ( 0.032, 0.148, 0.121),
        (-0.032, 0.212, 0.121),
        ( 0.032, 0.212, 0.121),
    ]
    faces = [(0, 1, 2), (1, 3, 2)]
    uvs = [(0.0, 0.0), (1.0, 0.0), (0.0, 1.0), (1.0, 1.0)]
    normals = [(0.0, 0.0, 1.0)] * 4
    return verts, faces, uvs, normals


def generate_beard_plane() -> Tuple[List, List, List, List]:
    verts = [
        (-0.032, 0.074, 0.123),
        ( 0.032, 0.074, 0.123),
        (-0.032, 0.136, 0.117),
        ( 0.032, 0.136, 0.117),
    ]
    faces = [(0, 1, 2), (1, 3, 2)]
    uvs = [(0.0, 0.0), (1.0, 0.0), (0.0, 1.0), (1.0, 1.0)]
    normals = [(0.0, 0.0, 1.0)] * 4
    return verts, faces, uvs, normals


def generate_face_texture(skin_tone: str = 'medium',
                          eye_color: str = 'brown',
                          beard_style: str = 'none',
                          beard_color: str = 'black',
                          resolution: int = 256) -> np.ndarray:
    """
    Generate a simple face texture with eyes, nose, and mouth painted on.
    Returns a numpy array (H, W, 4) RGBA.
    """
    base_color = SKIN_TONES.get(skin_tone, SKIN_TONES['medium'])
    eye_col = EYE_COLORS.get(eye_color, EYE_COLORS['brown'])
    img = np.zeros((resolution, resolution, 4), dtype=np.float32)

    h, w = resolution, resolution
    cx, cy = w // 2, h // 2

    eye_y = int(h * 0.46)
    left_eye_x = int(w * 0.34)
    right_eye_x = int(w * 0.66)
    eye_rx = int(w * 0.070)
    eye_ry = int(h * 0.040)

    for ex, ey_pos in [(left_eye_x, eye_y), (right_eye_x, eye_y)]:
        for y in range(max(0, ey_pos - eye_ry), min(h, ey_pos + eye_ry + 1)):
            for x in range(max(0, ex - eye_rx), min(w, ex + eye_rx + 1)):
                dx = (x - ex) / max(1, eye_rx)
                dy = (y - ey_pos) / max(1, eye_ry)
                dist = dx * dx + dy * dy
                if dist < 1.0:
                    shade = 0.18 + 0.22 * (1.0 - dist)
                    img[y, x, :3] = [shade * eye_col[0], shade * eye_col[1], shade * eye_col[2]]
                    img[y, x, 3] = max(img[y, x, 3], 0.85)
                if dist < 0.08:
                    img[y, x, :3] = [0.03, 0.03, 0.03]
                    img[y, x, 3] = 0.95

    lid_y = eye_y - int(eye_ry * 0.55)
    for ex in [left_eye_x, right_eye_x]:
        for y in range(max(0, lid_y - 1), min(h, lid_y + 1)):
            for x in range(max(0, ex - int(eye_rx * 1.15)), min(w, ex + int(eye_rx * 1.15))):
                img[y, x, :3] = [0.16, 0.12, 0.09]
                img[y, x, 3] = 0.75

    brow_y = eye_y - int(h * 0.11)
    for ex in [left_eye_x, right_eye_x]:
        for y in range(max(0, brow_y - 1), min(h, brow_y + 2)):
            for x in range(max(0, ex - int(eye_rx * 1.10)), min(w, ex + int(eye_rx * 1.10))):
                img[y, x, :3] = [0.15, 0.10, 0.08]
                img[y, x, 3] = 0.70

    nose_y0 = int(h * 0.64)
    nose_y1 = int(h * 0.74)
    for y in range(max(0, nose_y0), min(h, nose_y1)):
        taper = 1.0 - abs((y - (nose_y0 + nose_y1) * 0.5) / max(1.0, (nose_y1 - nose_y0) * 0.5))
        half_w = max(1, int(1 + 2 * taper))
        for x in range(max(0, cx - half_w), min(w, cx + half_w + 1)):
            img[y, x, :3] = [base_color[0] * 0.78, base_color[1] * 0.78, base_color[2] * 0.78]
            img[y, x, 3] = max(img[y, x, 3], 0.10)

    return (img * 255).astype(np.uint8)


def generate_beard_texture(skin_tone: str = 'medium',
                           beard_style: str = 'none',
                           beard_color: str = 'black',
                           resolution: int = 256) -> np.ndarray:
    base_color = SKIN_TONES.get(skin_tone, SKIN_TONES['medium'])
    beard_col = HAIR_COLORS.get(beard_color, HAIR_COLORS['black'])
    beard_rgb = [
        base_color[0] * 0.40 + beard_col[0] * 0.60,
        base_color[1] * 0.40 + beard_col[1] * 0.60,
        base_color[2] * 0.40 + beard_col[2] * 0.60,
    ]
    img = np.zeros((resolution, resolution, 4), dtype=np.float32)
    if beard_style == 'none':
        return (img * 255).astype(np.uint8)

    h, w = resolution, resolution
    cx = w // 2
    if beard_style == 'stubble':
        center_y = int(h * 0.42)
        rx = int(w * 0.30)
        ry = int(h * 0.20)
        alpha_scale = 0.06
    elif beard_style == 'short':
        center_y = int(h * 0.44)
        rx = int(w * 0.33)
        ry = int(h * 0.24)
        alpha_scale = 0.11
    else:
        center_y = int(h * 0.46)
        rx = int(w * 0.36)
        ry = int(h * 0.28)
        alpha_scale = 0.16

    top_y = max(0, center_y - ry)
    bottom_y = min(h, center_y + ry + 1)
    left_x = max(0, cx - rx)
    right_x = min(w, cx + rx + 1)
    for y in range(top_y, bottom_y):
        for x in range(left_x, right_x):
            dx = (x - cx) / max(1, rx)
            dy = (y - center_y) / max(1, ry)
            jaw_center_y = center_y + ry * 0.10
            jaw_dy = (y - jaw_center_y) / max(1, ry)
            dist = dx * dx + jaw_dy * jaw_dy
            if dist > 1.0 or y < center_y - ry * 0.55:
                continue
            side_falloff = max(0.0, 1.0 - abs(dx) ** 1.6)
            vertical_falloff = max(0.0, 1.0 - abs(jaw_dy) ** 1.4)
            chin_mask = max(0.0, 1.0 - abs((y - (center_y + ry * 0.30)) / max(1.0, ry * 0.95)))
            alpha = alpha_scale * side_falloff * (0.35 + 0.65 * vertical_falloff) * (0.45 + 0.55 * chin_mask)
            if abs(dx) < 0.14 and y < center_y + ry * 0.08:
                continue
            if beard_style == 'stubble' and y < center_y - ry * 0.05:
                continue
            if beard_style == 'stubble':
                grain = ((x * 17 + y * 31) % 23) / 23.0
                alpha *= 0.18 + 0.55 * grain
            elif beard_style == 'short':
                grain = ((x * 11 + y * 19) % 17) / 17.0
                alpha *= 0.62 + 0.22 * grain
            else:
                grain = ((x * 7 + y * 13) % 9) / 9.0
                alpha *= 0.74 + 0.16 * grain
            alpha = min(0.24, max(0.0, alpha))
            img[y, x, :3] = beard_rgb
            img[y, x, 3] = max(img[y, x, 3], alpha)

    return (img * 255).astype(np.uint8)


def generate_skin_texture(skin_tone: str = 'medium',
                          eye_color: str = 'brown',
                          beard_style: str = 'none',
                          beard_color: str = 'black',
                          resolution: int = 512) -> np.ndarray:
    """Return a flat skin-tone texture. No facial features are painted
    because the head mesh UVs are too low-resolution to support readable
    face details. Eyes / mouth will be handled geometrically if needed."""
    base_color = SKIN_TONES.get(skin_tone, SKIN_TONES['medium'])
    h, w = resolution, resolution
    img = np.zeros((h, w, 4), dtype=np.float32)
    img[:, :, 0] = base_color[0]
    img[:, :, 1] = base_color[1]
    img[:, :, 2] = base_color[2]
    img[:, :, 3] = 1.0
    return (np.clip(img, 0.0, 1.0) * 255).astype(np.uint8)
