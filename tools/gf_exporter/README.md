# GF Exporter — GameplayFootball → glTF 2.0 (GLB)

Exporte les assets GameplayFootball (.ase, .anim, .object) vers le format GLB
standard avec customisations procédurales (cheveux, barbe, oreilles, doigts, visage).

## Installation

```bash
pip install -r requirements.txt
```

## Utilisation

```bash
# Export tout (base + avatars)
python export_all.py

# Spécifier les dossiers
python export_all.py --data-dir ../../GameplayFootball/data --output-dir ./output

# Seulement le modèle de base
python export_all.py --skip-avatars

# Seulement les avatars
python export_all.py --skip-base
```

## Fichiers générés

| Fichier | Contenu |
|---|---|
| `player_base.glb` | Modèle joueur + squelette 14 os + 16 animations |
| `avatar_catalog.json` | Catalogue des 16 avatars prédéfinis |
| `avatar_configs.json` | Configs de customisation par avatar |
| `textures/face_*.png` | Textures de visage par teinte de peau |

## Architecture

```
parse_ase.py    → Parse .ase (3DS Max ASCII) → vertices, faces, UVs
parse_anim.py   → Parse .anim (GF animation) → keyframes quaternion
parse_object.py → Parse .object (scene graph) → squelette 14 bones
write_glb.py    → Assemble GLB binaire (glTF 2.0)
customize.py    → Géométrie procédurale (cheveux, barbe, oreilles, doigts, visage)
avatar_combinations.py → 16 avatars prédéfinis + générateur d'équipe
export_all.py   → Script principal d'orchestration
```

## Intégration Android

Le fichier `player_base.glb` contient :
- 14 bones (squelette GF standard)
- 16 animations mappées sur les IDs Android (0-15)
- Mesh unique avec skinning (bone weights)

Les customisations sont appliquées au runtime côté Android :
- Hauteur → `node.scale` sur le bone racine
- Couleur peau/cheveux → `material.baseColorFactor`
- Cheveux/barbe/oreilles/doigts → meshes additionnels générés
