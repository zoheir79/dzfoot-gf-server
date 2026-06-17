#!/usr/bin/env python3
"""DZFoot GF Worker - Docker mode via docker-py"""
import os, sys, json, time, signal, threading

REDIS_URL = os.getenv("REDIS_URL")
STATS_URL = os.getenv("STATS_URL")
LIVEKIT_URL = os.getenv("LIVEKIT_URL", "")
GF_IMAGE = os.getenv("GF_IMAGE", "dzfoot-gf-server:prod")
GF_NETWORK = os.getenv("GF_NETWORK", "dzfoot-backend_default")
GF_LOGS_HOST_PATH = os.getenv("GF_LOGS_HOST_PATH", "/var/log/dzfoot/gf")
WORKER_ID = os.getenv("HOSTNAME", "worker-docker")

missing = []
if not REDIS_URL:
    missing.append("REDIS_URL")
if not STATS_URL:
    missing.append("STATS_URL")
if missing:
    print(f"[GF Docker Worker] FATAL: Required environment variables not set: {', '.join(missing)}", file=sys.stderr)
    sys.exit(1)

print(f"[GF Docker Worker {WORKER_ID}] Starting. Redis: {REDIS_URL}")

import redis
import docker
from docker.types import Mount
import tarfile, io

r = redis.from_url(REDIS_URL)

try:
    client = docker.DockerClient(base_url="unix:///var/run/docker.sock")
    client.ping()
    print(f"[GF Docker Worker {WORKER_ID}] Docker OK")
except Exception as e:
    print(f"ERROR: Cannot access Docker: {e}")
    sys.exit(1)

running = True
active_containers = {}  # room_id -> container


def _stop_and_remove(container, room_id="unknown"):
    """Force-stop then remove a container, swallowing all errors."""
    try:
        container.stop(timeout=2)
    except Exception:
        pass
    try:
        container.remove(force=True)
        print(f"[GF Docker Worker {WORKER_ID}] Removed container for {room_id}", flush=True)
    except Exception:
        pass


def shutdown(sig, frame):
    global running
    running = False

signal.signal(signal.SIGINT, shutdown)
signal.signal(signal.SIGTERM, shutdown)


GF_CONTAINER_LABEL = "dzfoot.gf_worker"


def reap_stopped():
    """Periodically remove any gf containers spawned by this worker that are not running."""
    while running:
        time.sleep(30)
        try:
            for c in client.containers.list(all=True, filters={"label": GF_CONTAINER_LABEL}):
                if c.status != "running":
                    _stop_and_remove(c, c.name)
        except Exception as e:
            print(f"[GF Docker Worker {WORKER_ID}] Reaper error: {e}", flush=True)


def cleanup_listener():
    """Listen to gf.crashed / gf.finished events on Redis and kill matching containers."""
    while running:
        try:
            cleanup_r = redis.from_url(REDIS_URL)
            pubsub = cleanup_r.pubsub()
            pubsub.subscribe("gf.crashed", "gf.finished")
            print(f"[GF Docker Worker {WORKER_ID}] Cleanup listener started", flush=True)
            for message in pubsub.listen():
                if not running:
                    break
                if message["type"] != "message":
                    continue
                try:
                    room_id = message["data"].decode("utf-8") if isinstance(message["data"], bytes) else message["data"]
                    container_name = f"gf-{room_id}"
                    active_containers.pop(room_id, None)
                    try:
                        container = client.containers.get(container_name)
                        _stop_and_remove(container, room_id)
                        print(f"[GF Docker Worker {WORKER_ID}] Cleaned up container {container_name}", flush=True)
                    except docker.errors.NotFound:
                        pass  # Already gone
                    except Exception as e:
                        print(f"[GF Docker Worker {WORKER_ID}] Cleanup error for {container_name}: {e}", flush=True)
                except Exception as e:
                    print(f"[GF Docker Worker {WORKER_ID}] Cleanup listener error: {e}", flush=True)
        except redis.exceptions.ConnectionError as e:
            print(f"[GF Docker Worker {WORKER_ID}] Cleanup Redis error: {e}", flush=True)
            time.sleep(5)
        except Exception as e:
            print(f"[GF Docker Worker {WORKER_ID}] Cleanup listener fatal error: {e}", flush=True)
            time.sleep(5)


# Clean up any orphaned gf containers on startup
print(f"[GF Docker Worker {WORKER_ID}] Removing orphaned containers...", flush=True)
try:
    for c in client.containers.list(all=True, filters={"label": GF_CONTAINER_LABEL}):
        if c.status != "running":
            _stop_and_remove(c, c.name)
except Exception as e:
    print(f"[GF Docker Worker {WORKER_ID}] Startup cleanup error: {e}", flush=True)

# Start background threads
threading.Thread(target=cleanup_listener, daemon=True).start()
threading.Thread(target=reap_stopped, daemon=True).start()

while running:
    try:
        result = r.brpop("gf.spawn", timeout=10)
        if result is None:
            continue
        _, payload_bytes = result
        payload = json.loads(payload_bytes)
        room_id = payload.get("room_id", "")
        token = payload.get("token", "")
        if not room_id or not token:
            continue
        team_a = payload.get("team_a", "default-a")
        team_b = payload.get("team_b", "default-b")
        stadium = payload.get("stadium_id", "default-stadium")
        duration = payload.get("duration", 600)
        mode = payload.get("mode", "vs_ai")
        player_a = payload.get("player_a", "")
        player_b = payload.get("player_b", "")
        match_config = payload.get("match_config", "")
        print(f"[GF Docker Worker {WORKER_ID}] Spawning {room_id} ({team_a} vs {team_b}, {duration}s)")
        cmd = [
            f"--room-id={room_id}", f"--team-a={team_a}", f"--team-b={team_b}",
            f"--stadium={stadium}", f"--duration={duration}", f"--mode={mode}",
            "--broadcast-hz=20", f"--livekit-url={LIVEKIT_URL}",
            f"--livekit-token={token}", f"--stats-url={STATS_URL}",
            f"--redis-url={REDIS_URL}",
        ]
        if player_a: cmd.append(f"--player-a={player_a}")
        if player_b: cmd.append(f"--player-b={player_b}")
        log_file = f"/app/logs/gf_server_{room_id}.log"
        env = {
            "LIVEKIT_URL": LIVEKIT_URL,
            "LIVEKIT_TOKEN": token,
            "REDIS_URL": REDIS_URL,
            "STATS_URL": STATS_URL,
            "GF_LOG_FILE": log_file
        }
        mounts = [Mount(target="/app/logs", source=GF_LOGS_HOST_PATH, type="bind")]
        volumes = {}
        config_files_to_cleanup = []
        if match_config:
            config_host_path = f"/tmp/gf_{room_id}.json"
            with open(config_host_path, "w") as f:
                json.dump(match_config, f)
            volumes[config_host_path] = {"bind": "/tmp/match_config.json", "mode": "ro"}
            cmd.append("--config-file=/tmp/match_config.json")
            config_files_to_cleanup.append(config_host_path)
            print(f"[GF Docker Worker {WORKER_ID}] Match config written to {config_host_path}")

        container = client.containers.run(
            image=GF_IMAGE, command=cmd, name=f"gf-{room_id}",
            network=GF_NETWORK, environment=env, volumes=volumes, mounts=mounts,
            labels={GF_CONTAINER_LABEL: "true"},
            entrypoint="/app/run_logged.sh",
            detach=True, remove=False)
        active_containers[room_id] = container
        print(f"[GF Docker Worker {WORKER_ID}] Container {container.id[:12]} launched")

        # Wait a few seconds then check if container is still alive
        time.sleep(5)
        container.reload()
        if container.status != "running":
            crash_logs = ""
            try:
                bits, _ = container.get_archive(log_file)
                data = b"".join(bits)
                tar = tarfile.open(fileobj=io.BytesIO(data), mode="r:*")
                for member in tar:
                    crash_logs = tar.extractfile(member).read().decode("utf-8", errors="replace")[-4000:]
                    break
            except Exception:
                crash_logs = container.logs(stdout=True, stderr=True, tail=100).decode("utf-8", errors="replace")
            print(f"[GF Docker Worker {WORKER_ID}] CRASH detected for {room_id}! Logs:\n{crash_logs}", flush=True)
            active_containers.pop(room_id, None)
            r.publish("gf.crashed", room_id)
            try:
                container.remove(force=True)
            except Exception:
                pass
            for path in config_files_to_cleanup:
                try:
                    os.remove(path)
                except Exception:
                    pass
            continue
        r.publish("gf.ready", room_id)
    except redis.exceptions.ConnectionError as e:
        print(f"[GF Docker Worker {WORKER_ID}] Redis error: {e}")
        time.sleep(5)
        try: r = redis.from_url(REDIS_URL)
        except: pass
    except Exception as e:
        print(f"[GF Docker Worker {WORKER_ID}] Error: {e}")
        time.sleep(1)
