#!/usr/bin/env python3
"""
DZFoot 60Hz Refactor — Server Integration Test
Launches gf_server in headless mode, measures tick rate,
validates GameState frequency, and checks timer consistency.

Usage:
    python3 test_server_tickrate.py

Requires:
    - Compiled gf_server binary in ../build/gf_server or current dir
    - No LiveKit / Redis needed (runs with --room-id=test)
"""

import subprocess
import time
import sys
import os
import signal
import re

# ------------------------------------------------------------------
# Config
# ------------------------------------------------------------------
SERVER_BIN = "./build/gf_server"
DURATION_S = 10          # How long to run the server (seconds)
EXPECTED_TICKRATE = 60   # Hz
TOLERANCE = 0.15         # 15% tolerance (51-69 Hz acceptable)

# ------------------------------------------------------------------
# Helpers
# ------------------------------------------------------------------
def find_server_binary():
    candidates = [
        "./gf_server",
        "./build/gf_server",
        "../build/gf_server",
        "build/gf_server",
    ]
    for c in candidates:
        if os.path.isfile(c) and os.access(c, os.X_OK):
            return c
    return None

def run_server_test(bin_path: str):
    cmd = [
        bin_path,
        "--room-id=test_60hz",
        "--duration=60",
        "--broadcast-hz=20",
        "--mode=vs_ai",
    ]
    print(f"[TEST] Launching: {' '.join(cmd)}")

    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )

    start_time = time.time()
    tick_count = 0
    last_timer = 0.0
    timer_ok = True
    heartbeat_count = 0
    errors = []

    try:
        while time.time() - start_time < DURATION_S:
            # Read line with timeout to avoid blocking forever
            import select
            if proc.poll() is not None:
                break

            ready, _, _ = select.select([proc.stdout], [], [], 0.5)
            if not ready:
                continue

            line = proc.stdout.readline()
            if not line:
                break
            line = line.strip()

            # Count ticks by looking for "[GameServer] tick" or similar
            # Adjust regex if server log format changed
            if "tick" in line.lower() or "Tick" in line:
                tick_count += 1

            # Check for errors / crashes
            if "error" in line.lower() or "fatal" in line.lower() or "assert" in line.lower():
                errors.append(line)

            # Heartbeat check (1 Hz)
            if "heartbeat" in line.lower():
                heartbeat_count += 1

            # Timer sanity: if server prints timer, verify monotonic increase
            m = re.search(r"timer[=:\s]+([0-9.]+)", line)
            if m:
                t = float(m.group(1))
                if t < last_timer:
                    timer_ok = False
                last_timer = t

    except KeyboardInterrupt:
        pass
    finally:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()

    elapsed = time.time() - start_time
    measured_hz = tick_count / elapsed if elapsed > 0 else 0

    return {
        "elapsed": elapsed,
        "tick_count": tick_count,
        "measured_hz": measured_hz,
        "heartbeat_count": heartbeat_count,
        "timer_ok": timer_ok,
        "errors": errors,
    }

# ------------------------------------------------------------------
# Tests
# ------------------------------------------------------------------
def test_tick_rate(result):
    print("\n[TEST] Server Tick Rate")
    print(f"  Elapsed wall time: {result['elapsed']:.2f}s")
    print(f"  Ticks observed:    {result['tick_count']}")
    print(f"  Measured Hz:       {result['measured_hz']:.2f}")
    print(f"  Expected Hz:       {EXPECTED_TICKRATE}")

    low = EXPECTED_TICKRATE * (1 - TOLERANCE)
    high = EXPECTED_TICKRATE * (1 + TOLERANCE)
    ok = low <= result["measured_hz"] <= high
    print(f"  Acceptable range:  {low:.1f} - {high:.1f} Hz")
    print(f"  {'PASS' if ok else 'FAIL'}")
    return ok

def test_heartbeat(result):
    print("\n[TEST] Heartbeat Frequency")
    expected_hb = max(1, int(result["elapsed"]) - 1)
    print(f"  Heartbeats observed: {result['heartbeat_count']}")
    print(f"  Expected approx:     {expected_hb}")
    ok = abs(result["heartbeat_count"] - expected_hb) <= 2
    print(f"  {'PASS' if ok else 'FAIL'}")
    return ok

def test_timer_monotonic(result):
    print("\n[TEST] Timer Monotonicity")
    print(f"  Timer strictly increasing: {result['timer_ok']}")
    print(f"  {'PASS' if result['timer_ok'] else 'FAIL'}")
    return result["timer_ok"]

def test_no_errors(result):
    print("\n[TEST] No Fatal Errors / Asserts")
    if result["errors"]:
        print(f"  Errors found ({len(result['errors'])}):")
        for e in result["errors"][:5]:
            print(f"    {e}")
        print("  FAIL")
        return False
    else:
        print("  No errors detected")
        print("  PASS")
        return True

# ------------------------------------------------------------------
# Main
# ------------------------------------------------------------------
def main():
    print("=" * 60)
    print("DZFoot 60Hz Refactor — Server Integration Test")
    print("=" * 60)

    bin_path = find_server_binary()
    if not bin_path:
        print("ERROR: gf_server binary not found. Please build first:")
        print("  mkdir -p build && cd build && cmake .. && make -j$(nproc)")
        sys.exit(1)

    print(f"Using binary: {bin_path}")
    result = run_server_test(bin_path)

    all_ok = True
    all_ok &= test_tick_rate(result)
    all_ok &= test_heartbeat(result)
    all_ok &= test_timer_monotonic(result)
    all_ok &= test_no_errors(result)

    print("\n" + "=" * 60)
    if all_ok:
        print("ALL TESTS PASSED")
        sys.exit(0)
    else:
        print("SOME TESTS FAILED")
        sys.exit(1)

if __name__ == "__main__":
    main()
