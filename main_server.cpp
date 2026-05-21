#include <iostream>
#include <cstring>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>
#include <unistd.h>
#include "GameServer.h"
#include "LiveKitBridge.h"

static std::atomic<bool> gRunning{true};

void signalHandler(int sig) {
    // async-signal-safe: write() instead of std::cout
    const char msg[] = "Received signal, shutting down...\n";
    (void)sig;
    (void)::write(STDOUT_FILENO, msg, sizeof(msg) - 1);
    gRunning = false;
}

struct Args {
    std::string roomId;
    std::string teamA;
    std::string teamB;
    std::string stadium;
    std::string playerA;
    std::string playerB;
    int duration = 600;
    int broadcastRateHz = 20;
    std::string livekitUrl;
    std::string livekitToken;
    std::string statsUrl;
    std::string redisUrl;
    uint8_t gameMode = 1; // 0=1v1, 1=vs_AI
    std::string configFile; // external JSON match config
};

static std::string getenvOr(const char* name, const std::string& fallback) {
    const char* val = std::getenv(name);
    return val ? std::string(val) : fallback;
}

Args parseArgs(int argc, char* argv[]) {
    Args cfg;
    // Environment-variable defaults (useful for Docker / k8s standalone deployment)
    cfg.roomId         = getenvOr("ROOM_ID", "");
    cfg.teamA          = getenvOr("TEAM_A", "");
    cfg.teamB          = getenvOr("TEAM_B", "");
    cfg.stadium        = getenvOr("STADIUM", "");
    cfg.playerA        = getenvOr("PLAYER_A", "");
    cfg.playerB        = getenvOr("PLAYER_B", "");
    try { cfg.duration = std::stoi(getenvOr("DURATION", "600")); } catch (...) { cfg.duration = 600; }
    try { cfg.broadcastRateHz = std::stoi(getenvOr("BROADCAST_HZ", "20")); } catch (...) { cfg.broadcastRateHz = 20; }
    try {
        std::string modeStr = getenvOr("MODE", "vs_ai");
        cfg.gameMode = (modeStr == "1v1" || modeStr == "1") ? 0 : 1;
    } catch (...) { cfg.gameMode = 1; }
    cfg.livekitUrl     = getenvOr("LIVEKIT_URL", "");
    cfg.livekitToken   = getenvOr("LIVEKIT_TOKEN", "");
    cfg.statsUrl       = getenvOr("STATS_URL", "");
    cfg.redisUrl       = getenvOr("REDIS_URL", "");
    cfg.configFile     = getenvOr("CONFIG_FILE", "");

    // CLI arguments override environment variables
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.find("--room-id=") == 0) cfg.roomId = arg.substr(10);
        else if (arg.find("--team-a=") == 0) cfg.teamA = arg.substr(9);
        else if (arg.find("--team-b=") == 0) cfg.teamB = arg.substr(9);
        else if (arg.find("--stadium=") == 0) cfg.stadium = arg.substr(10);
        else if (arg.find("--player-a=") == 0) cfg.playerA = arg.substr(11);
        else if (arg.find("--player-b=") == 0) cfg.playerB = arg.substr(11);
        else if (arg.find("--duration=") == 0) {
            try { cfg.duration = std::stoi(arg.substr(11)); } catch (...) {}
        }
        else if (arg.find("--broadcast-hz=") == 0) {
            try { cfg.broadcastRateHz = std::stoi(arg.substr(15)); } catch (...) {}
        }
        else if (arg.find("--livekit-url=") == 0) cfg.livekitUrl = arg.substr(14);
        else if (arg.find("--livekit-token=") == 0) cfg.livekitToken = arg.substr(16);
        else if (arg.find("--stats-url=") == 0) cfg.statsUrl = arg.substr(12);
        else if (arg.find("--redis-url=") == 0) cfg.redisUrl = arg.substr(12);
        else if (arg.find("--mode=") == 0) {
            std::string m = arg.substr(7);
            cfg.gameMode = (m == "1v1" || m == "1") ? 0 : 1;
        }
        else if (arg.find("--config-file=") == 0) cfg.configFile = arg.substr(14);
    }
    return cfg;
}

// Parse raw input packet into PlayerInput.
// Layout: dirX(4), dirZ(4), kick(4), pass(4), highPass(4), shot(4), sliding(4),
//         dribble(4), sprint(4), switchPlayer(4), playerIdx(1), team(1), pad(2),
//         clientTick(4), clientTimeUs(8)
// Total 56 bytes
static GameServer::PlayerInput parseInputPacket(const uint8_t* data, size_t len) {
    GameServer::PlayerInput inp{};
    if (len < 48) return inp; // need up to clientTick at offset 44 (4 bytes)
    float vals[10];
    std::memcpy(vals, data, 40); // 10 floats = 40 bytes
    inp.dirX = vals[0];
    inp.dirZ = vals[1];
    inp.kick = vals[2] > 0.5f;
    inp.pass = vals[3] > 0.5f;
    inp.highPass = vals[4] > 0.5f;
    inp.shot = vals[5] > 0.5f;
    inp.sliding = vals[6] > 0.5f;
    inp.dribble = vals[7] > 0.5f;
    inp.sprint = vals[8] > 0.5f;
    inp.switchPlayer = vals[9] > 0.5f;
    inp.playerIdx = data[40];
    inp.team = data[41];
    std::memcpy(&inp.clientTick, data + 44, 4);
    if (len >= 56) {
        std::memcpy(&inp.clientTimeUs, data + 48, 8);
    }
    return inp;
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    Args args = parseArgs(argc, argv);
    if (args.roomId.empty()) {
        std::cerr << "Usage: gf_server --room-id=ID [--mode=1v1|vs_ai] [--livekit-url=... --livekit-token=...] [--broadcast-hz=20]" << std::endl;
        return 1;
    }

    // 1. Connect to LiveKit as server bot (stub until C++ SDK integrated)
    LiveKitBridge lkBridge;
    if (!args.livekitUrl.empty() && !args.livekitToken.empty()) {
        if (!lkBridge.connect(args.livekitUrl, args.livekitToken, args.roomId)) {
            std::cerr << "[GF Server] LiveKit connect failed, running without network" << std::endl;
        }
    }

    // 2. Start GameServer (runs in this thread)
    GameServer::Config gscfg;
    gscfg.roomId = args.roomId;
    gscfg.teamA = args.teamA;
    gscfg.teamB = args.teamB;
    gscfg.stadium = args.stadium;
    gscfg.playerA = args.playerA;
    gscfg.playerB = args.playerB;
    gscfg.duration = args.duration;
    gscfg.broadcastRateHz = args.broadcastRateHz;
    gscfg.statsUrl = args.statsUrl;
    gscfg.redisUrl = args.redisUrl;
    gscfg.livekitBridge = &lkBridge;
    gscfg.gameMode = args.gameMode;
    gscfg.shutdownFlag = &gRunning;
    gscfg.matchConfigPath = args.configFile;

    GameServer::Server server(gscfg);

    // Wire LiveKit input callback to GameServer (topic "in")
    lkBridge.setOnDataReceived([&server](const std::string& topic, const uint8_t* data, size_t len) {
        if (topic != "in") return;
        auto input = parseInputPacket(data, len);
        server.receiveInput(input);
    });

    server.run();

    // 3. Post match result to Stats Service
    server.postMatchResult(args.statsUrl);

    lkBridge.disconnect();
    std::cout << "GF Server exited cleanly" << std::endl;
    return 0;
}
