#!/usr/bin/env python3
"""
DZFoot Android Client Simulator — Test 1 vs AI (Chlef vs MCO)
Simule une app Android : login → matchmaking → LiveKit → GameState
"""

import asyncio
import json
import time
import httpx

ACCOUNT_URL = "http://localhost:8001"
MATCHMAKING_URL = "http://localhost:8003"
SESSION_URL = "http://localhost:8002"
PLAYER_USERNAME = f"android_test_{int(time.time())}"
PLAYER_PASSWORD = "test1234"
TEAM_A = "chlef"
TEAM_B = "mco"

async def main():
    async with httpx.AsyncClient(timeout=30.0) as client:
        # 1. Register / Login
        print(f"[Android] Registering {PLAYER_USERNAME}...")
        try:
            r = await client.post(f"{ACCOUNT_URL}/auth/register", json={
                "username": PLAYER_USERNAME,
                "password": PLAYER_PASSWORD,
                "pseudo": f"Test_{PLAYER_USERNAME}",
                "email": f"{PLAYER_USERNAME}@test.dzfoot",
            })
            if r.status_code == 409:
                print("[Android] User exists, logging in...")
            elif r.status_code == 200:
                print(f"[Android] Registered: {r.json()}")
            else:
                print(f"[Android] Register response: {r.status_code} {r.text}")
        except Exception as e:
            print(f"[Android] Register failed: {e}")

        # Login
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

        headers = {"Authorization": f"Bearer {token}"}

        # 2. Create match directly via Session Service (vs_ai bypasses matchmaking)
        print(f"[Android] Creating match: {TEAM_A} vs {TEAM_B} (vs_ai)...")
        r = await client.post(f"{SESSION_URL}/internal/create-match", json={
            "player_a": player_id,
            "player_b": "ai_opponent",
            "team_a": TEAM_A,
            "team_b": TEAM_B,
            "mode": "vs_ai",
            "duration": 120,
        })
        print(f"[Android] Session response: {r.status_code}")
        if r.status_code == 200:
            match_info = r.json()
            print(f"[Android] Match created: {json.dumps(match_info, indent=2)}")
        else:
            print(f"[Android] Session error: {r.text}")
            return

        room_id = match_info.get("room_id")
        livekit_url = match_info.get("livekit_url")
        print(f"\n[Android] MATCH READY!")
        print(f"  Room: {room_id}")
        print(f"  LiveKit: {livekit_url}")
        print(f"\n[Android] Connect to LiveKit room '{room_id}' for GameState")

if __name__ == "__main__":
    asyncio.run(main())
