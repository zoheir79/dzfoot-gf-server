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
        'idle': 0,      # ANIM_IDLE
        'walk': 1,      # ANIM_WALK
        'run': 2,       # ANIM_RUN
        'sprint': 3,    # ANIM_SPRINT
        'dribble': 10,  # ANIM_DRIBBLE
    },
    'shot': 4,          # ANIM_SHOOT_R (mirror for shoot_l = 5)
    'pass': 6,          # ANIM_PASS_S
    'longpass': 7,      # ANIM_PASS_L
    'highpass': 7,      # ANIM_PASS_L
    'sliding': 9,       # ANIM_TACKLE
    'trip': 11,         # ANIM_FALL
    'deflect': 8,       # ANIM_HEADER
    'catch': 16,        # ANIM_GK_CATCH
    'interfere': 9,     # ANIM_TACKLE
    'special': 12,      # ANIM_CELEBRATE
    'ballcontrol': 10,  # ANIM_DRIBBLE
    'trap': 10,         # ANIM_DRIBBLE
}


def classify_animation(clip: AnimClip, filepath: str) -> int:
    """Map a GF animation to Android anim_id (0-16)."""
    atype = clip.anim_type.lower()
    fname = os.path.basename(filepath).lower()
    path_lower = filepath.lower().replace('\\', '/')

    if 'movement_special/' in path_lower:
        return 12

    # Check for goalkeeper-specific first
    if 'catch' in atype or 'catch' in fname:
        return 16  # ANIM_GK_CATCH

    if 'dive_l' in fname or 'divel' in fname:
        return 14  # ANIM_GK_DIVE_L
    if 'dive_r' in fname or 'diver' in fname:
        return 15  # ANIM_GK_DIVE_R
    if 'gk_idle' in fname or 'gkidle' in fname:
        return 13  # ANIM_GK_IDLE

    if atype == 'movement':
        # Determine velocity from metadata or filename
        velocity = clip.metadata.get('incomingvelocity', '')

        if 'idle' in path_lower:
            return 0  # ANIM_IDLE
        elif 'sprint' in path_lower:
            return 3  # ANIM_SPRINT
        elif 'dribble' in path_lower:
            return 10  # ANIM_DRIBBLE
        elif 'walk' in path_lower:
            return 1  # ANIM_WALK
        elif 'run' in path_lower:
            return 2  # ANIM_RUN
        else:
            # Default based on velocity
            try:
                vel = float(velocity)
                if vel < 1.0:
                    return 0
                elif vel < 2.5:
                    return 1
                elif vel < 4.5:
                    return 2
                else:
                    return 3
            except ValueError:
                return 0

    mapping = {
        'shot': 4,          # ANIM_SHOOT_R (mirror handles L)
        'pass': 6,          # ANIM_PASS_S
        'shortpass': 6,     # ANIM_PASS_S
        'longpass': 7,      # ANIM_PASS_L
        'highpass': 7,      # ANIM_PASS_L
        'sliding': 9,       # ANIM_TACKLE
        'trip': 11,         # ANIM_FALL
        'deflect': 8,       # ANIM_HEADER
        'header': 8,        # ANIM_HEADER
        'catch': 16,        # ANIM_GK_CATCH
        'interfere': 9,     # ANIM_TACKLE
        'special': 12,      # ANIM_CELEBRATE
        'celebration': 12,  # ANIM_CELEBRATE
        'ballcontrol': 10,  # ANIM_DRIBBLE
        'trap': 10,         # ANIM_DRIBBLE
    }

    return mapping.get(atype, 0)


def animation_selection_score(anim_id: int, path: str, clip: AnimClip) -> tuple:
    p = path.lower().replace('\\', '/')
    keyframes = sum(len(t.keyframes) for t in clip.tracks.values())
    preferred = {
        0: ['movement/idle/000_idlelevel1.anim', 'ballcontrol/idle/000.anim', 'movement/idle/000_accel.anim'],
        1: ['movement/walk/000.anim', 'movement/walk/045.anim', 'ballcontrol/walk/000.anim'],
        2: ['movement/sprint/000_idlelevel1.anim', 'movement/sprint/000_decel_decel_decel_todo.anim'],
        3: ['movement/sprint/000_idlelevel1.anim', 'ballcontrol/sprint/000.anim', 'movement/sprint/135_decel_decel_decel.anim'],
        4: ['shot/', 'shot/sprint/'],
        6: ['pass/walk/', 'pass/idle/', 'pass/sprint/'],
        7: ['highpass/walk/', 'highpass/idle/', 'longpass/'],
        8: ['deflect/idle/', 'deflect/walk/'],
        9: ['sliding/sprint/', 'sliding/walk/'],
        10: ['movement/dribble/135.anim', 'movement/dribble/045_decel.anim', 'ballcontrol/walk/000.anim'],
        11: ['trip/'],
        12: ['celebration/', 'movement_special/idle/special/000_stand_up_from_front.anim'],
        16: ['catch/'],
    }
    bad = ['stand_up_from_back', 'stand_up_from_front', 'highballs', 'headerdive', 'jump']
    penalty = sum(1 for token in bad if token in p)
    rank = 100
    for idx, token in enumerate(preferred.get(anim_id, [])):
        if token in p:
            rank = idx
            break
    if anim_id in (0, 1, 2, 3, 10) and 'movement_special/' in p:
        penalty += 10
    if anim_id == 10 and ('trap/' in p or 'highballs' in p):
        penalty += 5
    return (-penalty, -rank, keyframes)


def select_best_animations(animations: Dict[str, AnimClip]) -> Dict[int, AnimClip]:
    """Select the best animation for each Android anim_id (0-16).
    Picks the animation with the most keyframes for each type.
    """
    candidates = {i: [] for i in range(17)}

    for path, clip in animations.items():
        anim_id = classify_animation(clip, path)
        candidates[anim_id].append((path, clip))

    selected = {}
    for anim_id, clips in candidates.items():
        if clips:
            best = max(clips, key=lambda x: animation_selection_score(anim_id, x[0], x[1]))
            selected[anim_id] = best[1]
            print(f"  [ANIM] id={anim_id:2d} -> {best[0]} "
                  f"({sum(len(t.keyframes) for t in best[1].tracks.values())} kf)")

    return selected
