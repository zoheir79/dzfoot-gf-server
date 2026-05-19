# dzfoot-gf-server

Dedicated game server for DZFoot — Football 3D AR multiplayer.

## Stack
- C++17 · CMake 3.22+
- GameplayFootball (physics + AI) — **included as submodule**
- LiveKit C++ SDK (data channels)
- ENet fallback UDP

## Included Source

This repo includes the full GameplayFootball source code (**vi3itor fork, branche `google_brain`**) under `GameplayFootball/`:
- 390+ source files (C++ headers & implementations)
- Physics engine (Blunted2)
- AI systems (player AI, goalkeeper)
- Animation system
- Data files (teams, stadiums, textures)

**No external download required — works offline.**

## GF Versions — Clarification

### Arbre généalogique

```
BazkieBumpercar/GameplayFootball  — Original. Abandonné. Unlicense.
        ↓
google-research/football           — Fork Google Brain 2019. Rendu supprimé, API RL ajoutée. Apache 2.0.
        ↓
vi3itor/GameplayFootball         — Fork complet. Réintègre améliorations Google + garde menus/audio/rendu. Multi-plateforme.
        └── branche google_brain   ← ON UTILISE CELLE-CI (headless inclus)
```

### Comparaison des 3 versions

| Version | Headless | Rendu | Audio | Menus | Licence | Usage |
|---|---|---|---|---|---|---|
| BazkieBumpercar original | ❌ | ✅ | ✅ | ✅ | Unlicense | Code vieux, pas de CMake propre |
| google-research/football | ✅ (`render=False`) | ❌ Supprimé | ❌ Supprimé | ❌ Supprimés | Apache 2.0 | Focalisé RL, pas un jeu jouable |
| **vi3itor `google_brain`** | **✅** | **✅** | **✅** | **✅** | **Unlicense** | **✅ Notre choix** |

### Pourquoi vi3itor `google_brain` ?
- **vi3itor master** n'a **pas** de mode headless natif — le rendu SDL/OpenGL est obligatoire.
- **vi3itor `google_brain`** intègre les améliorations techniques de Google (dont `render=False`) tout en conservant le code complet (menus, audio, rendu).
- **Licence Unlicense** = domaine public. Téléchargeable, modifiable, commercial — aucune restriction.
- On ne utilise **pas** le fork Google directement (Apache 2.0, rendu supprimé, focalisé RL).

### Commandes exactes — récupérer le bon code

```bash
# vi3itor — branche google_brain (mode headless inclus)
git clone https://github.com/vi3itor/GameplayFootball.git
cd GameplayFootball
git checkout google_brain
git submodule update --init

# Compiler en headless sur Linux
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DGF_RENDER=OFF
make -j$(nproc)
```

> Google Research fork (`google-research/football`) sert uniquement de **référence** pour l'API `observations()` / `step()`. On ne l'intègre pas directement.

## GF Headless Mode — What Already Exists

La branche `google_brain` de vi3itor intègre le mode **headless complet** originellement ajouté par Google Brain.  
Le flag `render=False` désactive tout le rendu SDL/OpenGL et garde uniquement la simulation physique et la logique de match.

### What GF handles natively (no code to write)
- Simulation pure sans rendu — compile et tourne sans GPU, sans SDL display, sans X11
- Boucle de jeu autonome tick par tick (60 tick/s)
- IA joueurs + gardien
- Logique buts / hors-jeu / corners
- Score, timer, fin de match
- Cartons jaunes / rouges
- Fatigue joueurs
- Formations équipes
- API `observations()` → positions, vélocités, balle, score, cartons, fatigue, rôles
- Interface `step(actions)` → injecter les actions à chaque tick

### What we must add (~600 lignes C++ total)

| Component | Description | Est. Lines |
|---|---|---|
| `main_server.cpp` | Entry point + CLI args, load team config from Catalog Service | ~80 |
| `LiveKitBridge.h/.cpp` | LiveKit C++ SDK data channel: listen `"in"`, broadcast `"gs"` / `"ev"` | ~300 |
| `AnimationStateDetector` | Deduce `anim_id` from internal player state for GameState | ~80 |
| `StatsAccumulator` | Accumulate match stats from `observations()` | ~100 |
| `MatchResultPoster` | POST final result JSON to Stats Service (libcurl) | ~50 |

### Final Server Architecture

```
Session Service (Python)
   └── spawn ./gf_server --room-id=X --team-a=uuid --team-b=uuid --livekit-token=xxx

GF main_server.cpp
   ├── Parse args
   ├── Fetch team config from Catalog Service
   ├── Create GF env in headless mode (render=False)
   └── Connect LiveKit room (identity="gf-server")

Game Loop (60 tick/s)
   ├── 1) Read inputs from LiveKit topic "in"
   ├── 2) env.step(actions)
   ├── 3) observations() → positions, ball velocity, score
   ├── 4) Detect anim_ids (AnimationStateDetector)
   ├── 5) Serialize GameState (timestamp + tick + 22 players + ball)
   ├── 6) Broadcast GameState (20 Hz) via LiveKit topic "gs"
   └── 7) Accumulate stats (StatsAccumulator)

End of Match
   ├── env.done() == true
   ├── Serialize MatchResult (score + stats)
   ├── POST HTTP → Stats Service
   └── Process exits
```

> ⚠️ **Avertissement auteur original** (BazkieBumpercar) : *"This game has pretty good potential gameplay-wise, but has architectural problems regarding the code. I would advise you not to continue development, but rather use the source as inspiration for your own game."*
> 
> En pratique : le code fonctionne, Google l'utilise en prod pour du RL massif, mais le modifier en profondeur (ajouter réseau, attributs custom) demande du temps pour comprendre l'architecture Blunted2.

## Build

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

## Run

```bash
./gf_server \
  --room-id=match-abc \
  --team-a=algeria \
  --team-b=morocco \
  --stadium=algiers \
  --duration=600 \
  --livekit-url=wss://your-livekit.com \
  --livekit-token=xxx \
  --stats-url=http://stats:8000 \
  --redis-url=redis://localhost:6379
```

## Architecture

- `main_server.cpp` — entry point, CLI args
- `GameServer.h/.cpp` — 60 tick/s simulation loop
- `LiveKitBridge.h/.cpp` — WebRTC data channel (topics `gs`, `ev`, `in`)

## GameState Protocol

| Field | Size | Notes |
|---|---|---|
| `timestamp` | 8 bytes | Server tick timestamp (µs or ms) for client reconciliation |
| `tick` | 4 bytes | Simulation tick index |
| PlayerState[22] | 22 × 28 bytes | Position + anim_id |
| BallState | 24 bytes | Position + velocity (needed for client dead reckoning) |
| score[2] | 8 bytes | |
| timer | 4 bytes | |
| gameMode | 1 byte | |
| **Total** | **~648 bytes** |

Broadcast **20×/s** (20 Hz) via topic `"gs"` (unreliable).  
Why 20 Hz? Mobile jitter makes 60 Hz wasteful; client interpolation covers the gaps.  
Server simulation still runs at 60 tick/s.  
Player inputs are received at 60 Hz on topic `"in"`.

## Anti-Lag Server Responsibilities

The server is **authoritative**. It receives inputs with client timestamps, simulates physics at 60 tick/s, and broadcasts the official `GameState`.

### Reconciliation support
- Include `timestamp` of the last processed tick in every `GameState`.
- Clients use this to rewind and re-apply unacknowledged inputs.

### Ball dead reckoning support
- `BallState` must include **velocity** (`vx`, `vy`, `vz`) so the client can extrapolate between updates.

### Broadcast strategy
- `GameState` → 20 Hz unreliable topic `"gs"`.
- `Events` (goal, halftime, end) → reliable topic `"ev"` as needed.
- `Inputs` ← 60 Hz from clients on topic `"in"`.

## License

MIT
