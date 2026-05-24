"""
Parser for .anim (GameplayFootball animation) files.
Format: CSV-like with quaternion keyframes per body part.
"""
import os
import re
from dataclasses import dataclass, field
from typing import List, Dict, Tuple, Optional


@dataclass
class AnimKeyFrame:
    """A single keyframe for one body part at one frame index."""
    frame: int
    rotation: Tuple[float, float, float, float]  # quaternion (x, y, z, w)
    position: Optional[Tuple[float, float, float]] = None  # only for 'player'


@dataclass
class AnimTrack:
    """Animation track for one body part."""
    body_part: str
    keyframes: List[AnimKeyFrame] = field(default_factory=list)


@dataclass
class AnimClip:
    """A complete animation clip."""
    name: str
    tracks: Dict[str, AnimTrack] = field(default_factory=dict)
    frame_count: int = 0
    anim_type: str = ""  # movement, shot, pass, etc.
    metadata: Dict[str, str] = field(default_factory=dict)


def parse_anim(filepath: str) -> AnimClip:
    """Parse a single .anim file into an AnimClip."""
    with open(filepath, 'r', encoding='utf-8', errors='replace') as f:
        lines = f.readlines()

    clip = AnimClip(name=os.path.basename(filepath).replace('.anim', ''))

    xml_section = False
    xml_lines = []

    for line in lines:
        line = line.strip()
        if not line:
            continue

        if line.startswith('<'):
            xml_section = True

        if xml_section:
            xml_lines.append(line)
            continue

        # Parse CSV line: bodypart,frame,qx,qy,qz,qw,frame,qx,qy,qz,qw,...
        parts = line.split(',')
        if len(parts) < 2:
            continue

        body_part = parts[0].strip()
        track = clip.tracks.get(body_part, AnimTrack(body_part=body_part))

        i = 1
        while i < len(parts):
            try:
                frame = int(parts[i].strip())
            except (ValueError, IndexError):
                break

            if body_part == 'player':
                # player track has positions: frame, x, y, z
                if i + 3 < len(parts):
                    try:
                        x = float(parts[i + 1].strip())
                        y = float(parts[i + 2].strip())
                        z = float(parts[i + 3].strip())
                        kf = AnimKeyFrame(
                            frame=frame,
                            rotation=(0.0, 0.0, 0.0, 1.0),
                            position=(x, y, z)
                        )
                        track.keyframes.append(kf)
                        clip.frame_count = max(clip.frame_count, frame + 1)
                    except ValueError:
                        pass
                    i += 4
                else:
                    break
            else:
                # body part tracks have rotations: frame, qx, qy, qz, qw
                if i + 4 < len(parts):
                    try:
                        qx = float(parts[i + 1].strip())
                        qy = float(parts[i + 2].strip())
                        qz = float(parts[i + 3].strip())
                        qw = float(parts[i + 4].strip())
                        kf = AnimKeyFrame(
                            frame=frame,
                            rotation=(qx, qy, qz, qw)
                        )
                        track.keyframes.append(kf)
                        clip.frame_count = max(clip.frame_count, frame + 1)
                    except ValueError:
                        pass
                    i += 5
                else:
                    break

        clip.tracks[body_part] = track

    # Parse XML metadata
    xml_text = '\n'.join(xml_lines)
    type_match = re.search(r'<type>\s*(\w+)\s*</type>', xml_text)
    if type_match:
        clip.anim_type = type_match.group(1)

    # Extract other metadata
    for key in ['incomingvelocity', 'outgoingvelocity', 'incomingbodyangle',
                'outgoingbodyangle', 'angle', 'idlelevel', 'specialvar1', 'specialvar2']:
        match = re.search(rf'<{key}>\s*([^<]+)\s*</{key}>', xml_text)
        if match:
            clip.metadata[key] = match.group(1).strip()

    return clip


def parse_all_animations(anim_dir: str) -> Dict[str, AnimClip]:
    """Recursively parse all .anim files in a directory tree.
    Returns {relative_path: AnimClip}.
    """
    result = {}
    for root, dirs, files in os.walk(anim_dir):
        for fname in files:
            if fname.endswith('.anim'):
                filepath = os.path.join(root, fname)
                rel_path = os.path.relpath(filepath, anim_dir)
                try:
                    clip = parse_anim(filepath)
                    result[rel_path] = clip
                except Exception as e:
                    print(f"  [ANIM] {rel_path}: ERROR - {e}")

    print(f"  [ANIM] Parsed {len(result)} animation files")
    return result


# Mapping from GF animation types/paths to Android anim_id
ANIM_TYPE_TO_ID = {
    'movement': {
        'idle': 0,
        'walk': 1,
        'dribble': 9,
        'sprint': 3,
        'run': 2,
    },
    'shot': 4,       # shoot_r (mirror for shoot_l = 5)
    'pass': 6,       # pass_short
    'longpass': 7,   # pass_long
    'highpass': 7,
    'sliding': 8,    # tackle
    'trip': 14,      # fall
    'deflect': 15,   # header
    'catch': 13,     # gk_catch
    'interfere': 14,
    'special': 10,   # celebrate
    'ballcontrol': 9,  # dribble
    'trap': 9,
}


def classify_animation(clip: AnimClip, filepath: str) -> int:
    """Map a GF animation to Android anim_id (0-15)."""
    atype = clip.anim_type.lower()
    fname = os.path.basename(filepath).lower()

    # Check for goalkeeper-specific
    if 'catch' in atype or 'catch' in fname:
        return 13  # gk_catch

    if atype == 'movement':
        # Determine velocity from metadata or filename
        velocity = clip.metadata.get('incomingvelocity', '')
        path_lower = filepath.lower()

        if 'idle' in path_lower:
            return 0
        elif 'sprint' in path_lower:
            return 3
        elif 'dribble' in path_lower:
            return 9
        elif 'walk' in path_lower:
            return 1
        else:
            # Default based on velocity
            try:
                vel = float(velocity)
                if vel < 1.0:
                    return 0
                elif vel < 4.0:
                    return 9
                elif vel < 6.0:
                    return 1
                else:
                    return 3
            except ValueError:
                return 0

    mapping = {
        'shot': 4,
        'pass': 6,
        'shortpass': 6,
        'longpass': 7,
        'highpass': 7,
        'sliding': 8,
        'trip': 14,
        'deflect': 15,
        'header': 15,
        'catch': 13,
        'interfere': 14,
        'special': 10,
        'celebration': 10,
        'ballcontrol': 9,
        'trap': 9,
    }

    return mapping.get(atype, 0)


def select_best_animations(animations: Dict[str, AnimClip]) -> Dict[int, AnimClip]:
    """Select the best animation for each Android anim_id (0-15).
    Picks the animation with the most keyframes for each type.
    """
    candidates = {i: [] for i in range(16)}

    for path, clip in animations.items():
        anim_id = classify_animation(clip, path)
        candidates[anim_id].append((path, clip))

    selected = {}
    for anim_id, clips in candidates.items():
        if clips:
            # Pick the one with most keyframes (most detailed)
            best = max(clips, key=lambda x: sum(
                len(t.keyframes) for t in x[1].tracks.values()
            ))
            selected[anim_id] = best[1]
            print(f"  [ANIM] id={anim_id:2d} -> {best[0]} "
                  f"({sum(len(t.keyframes) for t in best[1].tracks.values())} kf)")

    return selected
