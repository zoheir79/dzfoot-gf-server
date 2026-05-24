"""
Avatar combination generator — produces preset player appearances.
"""
from dataclasses import dataclass, field
from typing import List, Dict, Optional
import json


@dataclass
class AvatarConfig:
    """Complete player appearance configuration."""
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


# ─── Preset Combinations ─────────────────────────────────────────

PRESET_AVATARS = [
    AvatarConfig("classic_european", "light", "short", "blonde", "none", "blonde", "blue",
                 1.0, "average", "Joueur européen classique, cheveux blonds courts"),
    AvatarConfig("mediterranean", "olive", "curly", "dark_brown", "stubble", "dark_brown", "brown",
                 0.98, "average", "Joueur méditerranéen, cheveux bruns bouclés"),
    AvatarConfig("african_athletic", "dark", "short", "black", "short", "black", "brown",
                 1.02, "muscular", "Joueur africain athlétique, cheveux noirs courts"),
    AvatarConfig("north_african", "tan", "curly", "black", "full", "black", "brown",
                 0.99, "average", "Joueur maghrébin, cheveux noirs bouclés, barbe"),
    AvatarConfig("latin_american", "medium", "long", "dark_brown", "none", "dark_brown", "brown",
                 0.97, "average", "Joueur latino, cheveux longs bruns"),
    AvatarConfig("scandinavian", "light", "short", "blonde", "none", "blonde", "blue",
                 1.03, "muscular", "Joueur scandinave, grand, blond"),
    AvatarConfig("asian", "olive", "short", "black", "none", "black", "brown",
                 0.96, "average", "Joueur asiatique, cheveux noirs"),
    AvatarConfig("redhead_warrior", "light", "curly", "red", "stubble", "red", "green",
                 0.98, "average", "Joueur roux aux yeux verts"),
    AvatarConfig("veteran_grey", "medium", "short", "grey", "short", "grey", "hazel",
                 0.99, "average", "Vétéran expérimenté, cheveux gris"),
    AvatarConfig("young_talent", "medium", "mohawk", "brown", "none", "brown", "blue",
                 0.95, "thin", "Jeune talent avec crête"),
    AvatarConfig("powerful_striker", "brown", "bald", "black", "full", "black", "brown",
                 1.04, "heavy", "Attaquant puissant, chauve et barbu"),
    AvatarConfig("elegant_playmaker", "olive", "ponytail", "dark_brown", "stubble", "dark_brown", "hazel",
                 0.98, "average", "Meneur de jeu élégant, queue de cheval"),
    AvatarConfig("desert_warrior", "tan", "curly", "black", "full", "black", "brown",
                 1.0, "muscular", "Guerrier du désert, barbe fournie"),
    AvatarConfig("island_talent", "dark", "long", "black", "none", "black", "brown",
                 1.01, "muscular", "Talent des îles, cheveux longs"),
    AvatarConfig("street_footballer", "medium", "short", "light_brown", "stubble", "light_brown", "green",
                 0.97, "thin", "Footballeur de rue, look décontracté"),
    AvatarConfig("iron_defender", "light", "short", "red", "short", "red", "blue",
                 1.05, "heavy", "Défenseur imposant, roux"),
]

# ─── Team generation ─────────────────────────────────────────────

def generate_team_avatars(team_name: str, num_players: int = 11) -> List[AvatarConfig]:
    """Generate a diverse set of avatars for a team."""
    import random
    random.seed(hash(team_name) % (2**31))

    # Ensure variety: pick from presets with some randomization
    pool = list(PRESET_AVATARS)
    random.shuffle(pool)

    result = []
    for i in range(min(num_players, len(pool))):
        avatar = pool[i]
        # Slight randomization of height
        avatar.height_factor = round(random.uniform(0.93, 1.07), 2)
        result.append(avatar)

    # If we need more, generate random ones
    while len(result) < num_players:
        base = random.choice(PRESET_AVATARS)
        result.append(base)

    return result


def avatar_to_dict(avatar: AvatarConfig) -> dict:
    return {
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


def export_avatar_catalog(output_path: str):
    """Export all preset avatars as a JSON catalog."""
    catalog = {
        'presets': [avatar_to_dict(a) for a in PRESET_AVATARS],
        'skin_tones': list(SKIN_TONES.keys()),
        'hair_styles': ['bald', 'short', 'long', 'mohawk', 'curly', 'ponytail'],
        'hair_colors': ['black', 'dark_brown', 'brown', 'light_brown', 'blonde', 'red', 'grey', 'white'],
        'beard_styles': ['none', 'stubble', 'short', 'full'],
        'eye_colors': ['brown', 'blue', 'green', 'hazel', 'grey'],
        'body_types': ['thin', 'average', 'muscular', 'heavy'],
    }
    with open(output_path, 'w', encoding='utf-8') as f:
        json.dump(catalog, f, indent=2, ensure_ascii=False)
    print(f"  [AVATAR] Catalog exported: {output_path}")


from customize import SKIN_TONES
