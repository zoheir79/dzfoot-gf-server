#!/usr/bin/env python3
"""
DZFoot Android Client Simulator & LiveKit Monitor
Connects to the full backend flow, creates a 2x3 minutes match,
connects to LiveKit via WebRTC, receives the binary GameState packets,
decodes the physics of the ball and the 22 players, and displays real-time statistics.
"""

import asyncio
import json
import time
import httpx
import struct
import os
from typing import Optional

# LiveKit Python SDK
try:
    from livekit import rtc
except ImportError:
    print("ERROR: livekit package not found. Install it using: pip install livekit")
    import sys
    sys.exit(1)

# URLs
ACCOUNT_URL = "http://localhost:8001"
SESSION_URL = "http://localhost:8002"
PLAYER_USERNAME = f"android_test_{int(time.time())}"
PLAYER_PASSWORD = "test1234"
TEAM_A = "chlef"
TEAM_B = "mco"

# Animation names mapping
ANIM_NAMES = {
    0: "IDLE",
    1: "WALK",
    2: "RUN",
    3: "SPRINT",
    4: "SHOOT_R",
    5: "SHOOT_L",
    6: "PASS_S",
    7: "PASS_L",
    8: "HEADER",
    9: "TACKLE",
    10: "DRIBBLE",
    11: "FALL",
    12: "CELEBRATE",
    13: "GK_IDLE",
    14: "GK_DIVE_L",
    15: "GK_DIVE_R",
    16: "GK_CATCH"
}

# Struct formats for C++ alignment (x64 natural alignment)
HEADER_FMT = "<I4xQBBBBf"      # 24 bytes
BALL_FMT = "<3f3f3fbb2x"       # 40 bytes
PLAYER_FMT = "<3f3f3ffBBBBf"   # 48 bytes
EXPECTED_SIZE = 24 + 40 + (22 * 48)  # 1120 bytes

def unpack_game_state(data: bytes) -> Optional[dict]:
    if len(data) != EXPECTED_SIZE:
        # Try unpacking with alternative alignments if size differs slightly
        return None

    try:
        # 1. Unpack Header
        header_data = struct.unpack(HEADER_FMT, data[:24])
        tick = header_data[0]
        timestamp = header_data[1]
        mode = header_data[2]
        flags = header_data[3]
        score_a = header_data[4]
        score_b = header_data[5]
        timer = header_data[6]

        # 2. Unpack Ball
        ball_data = struct.unpack(BALL_FMT, data[24:64])
        ball_pos = ball_data[0:3]
        ball_vel = ball_data[3:6]
        ball_rot = ball_data[6:9]
        ball_owned_team = ball_data[9]
        ball_owned_player = ball_data[10]

        # 3. Unpack 22 Players
        players = []
        offset = 64
        for _ in range(22):
            p_data = struct.unpack(PLAYER_FMT, data[offset : offset + 48])
            offset += 48
            players.append({
                "pos": p_data[0:3],
                "vel": p_data[3:6],
                "dir": p_data[6:9],
                "rotY": p_data[9],
                "anim": p_data[10],
                "team": p_data[11],
                "role": p_data[12],
                "flags": p_data[13],
                "tired": p_data[14]
            })

        return {
            "tick": tick,
            "timestamp": timestamp,
            "mode": mode,
            "flags": flags,
            "score": [score_a, score_b],
            "timer": timer,
            "ball": {
                "pos": ball_pos,
                "vel": ball_vel,
                "rot": ball_rot,
                "owned_team": ball_owned_team,
                "owned_player": ball_owned_player
            },
            "players": players
        }
    except Exception as e:
        print(f"[Monitor] Struct unpack failed: {e}")
        return None

async def main():
    async with httpx.AsyncClient(timeout=30.0) as client:
        # 1. Authenticate / Login
        print(f"[Android] Registering {PLAYER_USERNAME}...")
        try:
            r = await client.post(f"{ACCOUNT_URL}/auth/register", json={
                "username": PLAYER_USERNAME,
                "password": PLAYER_PASSWORD,
                "pseudo": f"Test_{PLAYER_USERNAME}",
                "email": f"{PLAYER_USERNAME}@test.dzfoot",
            })
        except Exception as e:
            print(f"[Android] Register request failed: {e}")

        print("[Android] Logging in...")
        r = await client.post(f"{ACCOUNT_URL}/auth/login", json={
            "username": PLAYER_USERNAME,
            "password": PLAYER_PASSWORD,
            "email": f"{PLAYER_USERNAME}@test.dzfoot",
        })
        if r.status_code != 200:
            print(f"[Android] Login failed: {r.status_code} {r.text}")
            return
        token_data = r.json()
        token = token_data.get("access_token") or token_data.get("token")
        player_id = token_data.get("user_id") or token_data.get("player_id") or PLAYER_USERNAME
        print(f"[Android] Logged in! Player ID: {player_id}")

        # 2. Create a vs_ai match of 2x3 min = 6 minutes (360 seconds)
        print(f"[Android] Creating full 6-minute (360s) match: {TEAM_A} vs {TEAM_B}...")
        r = await client.post(f"{SESSION_URL}/internal/create-match", json={
            "player_a": player_id,
            "player_b": "ai_opponent",
            "team_a": TEAM_A,
            "team_b": TEAM_B,
            "mode": "vs_ai",
            "duration": 360, # 6 minutes total
        })
        if r.status_code != 200:
            print(f"[Android] Session service error: {r.text}")
            return
        
        match_info = r.json()
        room_id = match_info.get("room_id")
        livekit_url = match_info.get("livekit_url")
        client_token = match_info.get("token")

        print(f"\n[Android] Match Created! Room: {room_id}")
        print(f"[Android] LiveKit signaling URL: {livekit_url}")

        # LiveKit python library expects ws/wss URLs
        ws_url = livekit_url
        if ws_url.startswith("https://"):
            ws_url = "wss://" + ws_url[len("https://"):]
        elif ws_url.startswith("http://"):
            ws_url = "ws://" + ws_url[len("http://"):]

        print(f"[Android] Connecting to LiveKit WebRTC Room at {ws_url}...")
        
        room = rtc.Room()
        
        state_counter = 0
        last_print = 0

        @room.on("data_received")
        def on_data_received(data_packet: rtc.DataPacket):
            nonlocal state_counter, last_print
            payload = data_packet.payload
            topic = data_packet.topic
            
            if topic == "gs":
                state_counter += 1
                gs = unpack_game_state(payload)
                if not gs:
                    # If size doesn't match, print actual size received
                    if state_counter % 20 == 1:
                        print(f"[LiveKit Data] Received unknown format on 'gs' of size: {len(payload)} bytes")
                    return
                
                now = time.time()
                # Print stats every 2 seconds to avoid flooding terminal
                if now - last_print >= 2.0:
                    last_print = now
                    ball = gs["ball"]
                    in_play = "IN PLAY" if (gs["flags"] & 1) else "OUT OF PLAY"
                    goal_scored = "GOAL SCORED!" if (gs["flags"] & 2) else ""
                    
                    print("\n" + "="*80)
                    print(f" DZFOOT LIVE ENGINE MONITOR | Room: {room_id}")
                    print(f" Status: {in_play} {goal_scored}")
                    print(f" Time: {gs['timer']:.1f}s / 360s | Tick: {gs['tick']} | Score: {gs['score'][0]} - {gs['score'][1]}")
                    print(f" Ball: Pos=({ball['pos'][0]:.2f}, {ball['pos'][1]:.2f}, {ball['pos'][2]:.2f}) "
                          f"Vel=({ball['vel'][0]:.2f}, {ball['vel'][1]:.2f}, {ball['vel'][2]:.2f}) "
                          f"OwnedTeam={ball['owned_team']} OwnedPlayer={ball['owned_player']}")
                    
                    print("-"*80)
                    print(f" PLAYER DETAILS (First 5 of Team 0 & Team 1):")
                    # Team 0
                    team_0 = [p for p in gs["players"] if p["team"] == 0][:5]
                    print("  [Team 0 - Left]")
                    for idx, p in enumerate(team_0):
                        anim_name = ANIM_NAMES.get(p["anim"], f"UNKNOWN({p['anim']})")
                        has_ball = "⚽" if p["flags"] & 8 else ""
                        print(f"    Player {idx+1:2d}: Pos=({p['pos'][0]:.2f}, {p['pos'][2]:.2f}) Anim={anim_name:10s} Role={p['role']:2d} Tired={p['tired']:.2f} {has_ball}")
                    
                    # Team 1
                    team_1 = [p for p in gs["players"] if p["team"] == 1][:5]
                    print("  [Team 1 - Right]")
                    for idx, p in enumerate(team_1):
                        anim_name = ANIM_NAMES.get(p["anim"], f"UNKNOWN({p['anim']})")
                        has_ball = "⚽" if p["flags"] & 8 else ""
                        print(f"    Player {idx+12:2d}: Pos=({p['pos'][0]:.2f}, {p['pos'][2]:.2f}) Anim={anim_name:10s} Role={p['role']:2d} Tired={p['tired']:.2f} {has_ball}")
                    print("="*80)

        try:
            await room.connect(ws_url, client_token)
            print("[Android] WebRTC Peer Connection active! Listening for GameStates...")
            
            # Start publishing player inputs to trigger LiveKit SFU offer & maintain session active
            print("[Android] Starting player input emulation loop...")
            
            # Pack a dummy 56-byte input packet (all zeros, but with valid layout)
            # 10 floats (40 bytes) + playerIdx(1), team(1), pad(2) + clientTick(4) + clientTimeUs(8)
            dummy_input = struct.pack("<10fBB2xIQ", 
                0.0, 0.0, # dirX, dirZ
                0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, # actions (kick, pass, etc.)
                0, # playerIdx
                0, # team (0=left, 1=right)
                1, # clientTick
                int(time.time() * 1000000) # clientTimeUs
            )

            # Emulate an active client for 30 seconds
            for i in range(300): # 300 * 100ms = 30s
                # Send periodic input to LiveKit on topic "in" (reliable)
                await room.local_participant.publish_data(
                    payload=dummy_input,
                    topic="in",
                    reliable=True
                )
                await asyncio.sleep(0.1)
            
        except Exception as e:
            print(f"[Android] LiveKit connection failed: {e}")
        finally:
            await room.disconnect()
            print("[Android] Disconnected from room. Inspection finished.")

if __name__ == "__main__":
    asyncio.run(main())
