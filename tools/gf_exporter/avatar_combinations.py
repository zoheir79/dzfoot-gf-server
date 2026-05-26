"""
Avatar combination generator — produces preset player appearances.
Supports combinatorial generation with unique IDs for Android dynamic kit replacement.
"""
from dataclasses import dataclass, field
from typing import List, Dict, Optional
import json
import hashlib


@dataclass
class AvatarConfig:
    """Complete player appearance configuration."""
    id: str
    name: str
    skin_tone: str = "medium"
    hair_style: str = "short"
    hair_color: str = "black"
    beard_style: str = "none"
    beard_color: str = "black"
    eye_color: str = "brown"
    height_factor: float = 1.0
    body_type: str = "average"  # thin, average, muscular, heavy
    description: str = ""


# ─── Attribute Pools ─────────────────────────────────────────────

SKIN_TONES_LIST = ['light', 'medium', 'olive', 'tan', 'brown', 'dark', 'deep']
HAIR_STYLES_LIST = ['bald', 'short', 'long', 'mohawk', 'curly', 'ponytail']
HAIR_COLORS_LIST = ['black', 'dark_brown', 'brown', 'light_brown', 'blonde', 'red', 'grey', 'white']
BEARD_STYLES_LIST = ['none', 'stubble', 'short', 'full']
EYE_COLORS_LIST = ['brown', 'blue', 'green', 'hazel', 'grey']
BODY_TYPES_LIST = ['thin', 'average', 'muscular', 'heavy']


# ─── Preset Combinations (original 16) ───────────────────────────

PRESET_AVATARS = [
    AvatarConfig("av_001", "classic_european", "light", "short", "blonde", "none", "blonde", "blue", 1.0, "average", "Joueur européen classique, cheveux blonds courts"),
    AvatarConfig("av_002", "mediterranean", "olive", "curly", "dark_brown", "stubble", "dark_brown", "brown", 0.98, "average", "Joueur méditerranéen, cheveux bruns bouclés"),
    AvatarConfig("av_003", "african_athletic", "dark", "short", "black", "short", "black", "brown", 1.02, "muscular", "Joueur africain athlétique, cheveux noirs courts"),
    AvatarConfig("av_004", "north_african", "tan", "curly", "black", "full", "black", "brown", 0.99, "average", "Joueur maghrébin, cheveux noirs bouclés, barbe"),
    AvatarConfig("av_005", "latin_american", "medium", "long", "dark_brown", "none", "dark_brown", "brown", 0.97, "average", "Joueur latino, cheveux longs bruns"),
    AvatarConfig("av_006", "scandinavian", "light", "short", "blonde", "none", "blonde", "blue", 1.03, "muscular", "Joueur scandinave, grand, blond"),
    AvatarConfig("av_007", "asian", "olive", "short", "black", "none", "black", "brown", 0.96, "average", "Joueur asiatique, cheveux noirs"),
    AvatarConfig("av_008", "redhead_warrior", "light", "curly", "red", "stubble", "red", "green", 0.98, "average", "Joueur roux aux yeux verts"),
    AvatarConfig("av_009", "veteran_grey", "medium", "short", "grey", "short", "grey", "hazel", 0.99, "average", "Vétéran expérimenté, cheveux gris"),
    AvatarConfig("av_010", "young_talent", "medium", "mohawk", "brown", "none", "brown", "blue", 0.95, "thin", "Jeune talent avec crête"),
    AvatarConfig("av_011", "powerful_striker", "brown", "bald", "black", "full", "black", "brown", 1.04, "heavy", "Attaquant puissant, chauve et barbu"),
    AvatarConfig("av_012", "elegant_playmaker", "olive", "ponytail", "dark_brown", "stubble", "dark_brown", "hazel", 0.98, "average", "Meneur de jeu élégant, queue de cheval"),
    AvatarConfig("av_013", "desert_warrior", "tan", "curly", "black", "full", "black", "brown", 1.0, "muscular", "Guerrier du désert, barbe fournie"),
    AvatarConfig("av_014", "island_talent", "dark", "long", "black", "none", "black", "brown", 1.01, "muscular", "Talent des îles, cheveux longs"),
    AvatarConfig("av_015", "street_footballer", "medium", "short", "light_brown", "stubble", "light_brown", "green", 0.97, "thin", "Footballeur de rue, look décontracté"),
    AvatarConfig("av_016", "iron_defender", "light", "short", "red", "short", "red", "blue", 1.05, "heavy", "Défenseur imposant, roux"),
]


# ─── Combinatorial Generator ─────────────────────────────────────

def generate_avatar_id(skin: str, hair_style: str, hair_color: str,
                       beard: str, beard_color: str, eye: str,
                       body: str, height: float) -> str:
    """Generate a unique deterministic ID from avatar attributes."""
    raw = f"{skin}_{hair_style}_{hair_color}_{beard}_{beard_color}_{eye}_{body}_{height:.2f}"
    h = hashlib.md5(raw.encode('utf-8')).hexdigest()[:8]
    return f"av_{h}"


def generate_all_combinations(max_count: int = 256) -> List[AvatarConfig]:
    """Generate a diverse set of avatar combinations."""
    import random
    random.seed(42)

    # Generate combinatorial pool
    pool = []
    for skin in SKIN_TONES_LIST:
        for hair_style in HAIR_STYLES_LIST:
            for hair_color in HAIR_COLORS_LIST:
                for beard in BEARD_STYLES_LIST:
                    for eye in EYE_COLORS_LIST:
                        for body in BODY_TYPES_LIST:
                            # Beard color usually matches hair color
                            beard_color = hair_color if beard != 'none' else 'black'
                            # Height varies by body type
                            height = round(random.uniform(0.93, 1.07), 2)
                            avatar_id = generate_avatar_id(skin, hair_style, hair_color,
                                                           beard, beard_color, eye, body, height)
                            name = f"{skin}_{hair_style}_{hair_color}_{beard}_{body}"
                            desc = f"Avatar {skin} skin, {hair_color} {hair_style} hair, {beard} beard, {eye} eyes, {body} build"
                            pool.append(AvatarConfig(avatar_id, name, skin, hair_style, hair_color,
                                                     beard, beard_color, eye, height, body, desc))

    # Shuffle and limit
    random.shuffle(pool)
    selected = pool[:max_count]

    # Always include the 16 presets at the beginning
    all_avatars = list(PRESET_AVATARS)
    existing_ids = {a.id for a in all_avatars}
    for a in selected:
        if a.id not in existing_ids:
            all_avatars.append(a)
            if len(all_avatars) >= max_count:
                break

    return all_avatars[:max_count]


# ─── Team generation ─────────────────────────────────────────────

def generate_team_avatars(team_name: str, num_players: int = 11) -> List[AvatarConfig]:
    """Generate a diverse set of avatars for a team."""
    import random
    random.seed(hash(team_name) % (2**31))

    pool = generate_all_combinations(256)
    random.shuffle(pool)
    return pool[:num_players]


def avatar_to_dict(avatar: AvatarConfig) -> dict:
    return {
        'id': avatar.id,
        'name': avatar.name,
        'skin_tone': avatar.skin_tone,
        'hair_style': avatar.hair_style,
        'hair_color': avatar.hair_color,
        'beard_style': avatar.beard_style,
        'beard_color': avatar.beard_color,
        'eye_color': avatar.eye_color,
        'height_factor': avatar.height_factor,
        'body_type': avatar.body_type,
        'description': avatar.description,
    }


def export_avatar_catalog(output_path: str, avatars: List[AvatarConfig]):
    """Export avatar catalog with kit material metadata for Android dynamic replacement."""
    catalog = {
        'version': '2.0',
        'avatars': [avatar_to_dict(a) for a in avatars],
        'attributes': {
            'skin_tones': SKIN_TONES_LIST,
            'hair_styles': HAIR_STYLES_LIST,
            'hair_colors': HAIR_COLORS_LIST,
            'beard_styles': BEARD_STYLES_LIST,
            'eye_colors': EYE_COLORS_LIST,
            'body_types': BODY_TYPES_LIST,
        },
        'android_kit_mapping': {
            'kit_upper_material_name': 'kit_upper',
            'kit_lower_material_name': 'kit_lower',
            'skin_material_name': 'skin',
            'shoe_material_name': 'shoe',
            'hair_material_name': 'hair',
            'beard_material_name': 'beard',
            'description': 'In Android, find these material names in the loaded GLB and replace their baseColorTexture at runtime to apply team jerseys/shorts dynamically.',
        },
    }
    with open(output_path, 'w', encoding='utf-8') as f:
        json.dump(catalog, f, indent=2, ensure_ascii=False)
    print(f"  [AVATAR] Catalog exported: {output_path} ({len(avatars)} avatars)")


from customize import SKIN_TONES, HAIR_COLORS
