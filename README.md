# dzfoot-gf-server

Dedicated game server for DZFoot — Football 3D AR multiplayer.

## Stack
- C++17 · CMake 3.22+
- GameplayFootball (physics + AI) — **included as submodule**
- LiveKit C++ SDK (data channels)
- ENet fallback UDP

## Included Source

This repo includes the full GameplayFootball source code (vi3itor fork) under `GameplayFootball/`:
- 390+ source files (C++ headers & implementations)
- Physics engine (Blunted2)
- AI systems (player AI, goalkeeper)
- Animation system
- Data files (teams, stadiums, textures)

**No external download required — works offline.**

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
  --stats-url=http://stats:8000
```

## Architecture

- `main_server.cpp` — entry point, CLI args
- `GameServer.h/.cpp` — 60 tick/s simulation loop
- `LiveKitBridge.h/.cpp` — WebRTC data channel (topics `gs`, `ev`, `in`)

## GameState Protocol

| Field | Size |
|---|---|
| PlayerState[22] | 22 × 28 bytes |
| BallState | 24 bytes |
| score[2] | 8 bytes |
| timer | 4 bytes |
| gameMode | 1 byte |
| tick | 4 bytes |
| **Total** | **~640 bytes** |

Broadcast 60×/s via topic `"gs"` (unreliable).

## License
MIT
