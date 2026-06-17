#include <iostream>
#include <cstring>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>
#include <algorithm>
#include <unistd.h>
#include "GameServer.h"
#include "RedisClient.h"

static std::atomic<bool> gRunning{true};

void signalHandler(int sig) {
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
    std::string statsUrl;
    std::string redisUrl;
    uint8_t gameMode = 1;
    std::string configFile;
    std::string configJson;
};

static std::string getenvOr(const char* name, const std::string& fallback) {
    const char* val = std::getenv(name);
    return val ? std::string(val) : fallback;
}

Args parseArgs(int argc, char* argv[]) {
    Args cfg;
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
        if (modeStr == "1v1" || modeStr == "1") cfg.gameMode = 0;
        else if (modeStr == "ai_vs_ai" || modeStr == "2") cfg.gameMode = 2;
        else cfg.gameMode = 1; // vs_ai
    } catch (...) { cfg.gameMode = 1; }
    cfg.statsUrl       = getenvOr("STATS_URL", "");
    cfg.redisUrl       = getenvOr("REDIS_URL", "");
    cfg.configFile     = getenvOr("CONFIG_FILE", "");
    cfg.configJson     = getenvOr("CONFIG_JSON", "");

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
        else if (arg.find("--stats-url=") == 0) cfg.statsUrl = arg.substr(12);
        else if (arg.find("--redis-url=") == 0) cfg.redisUrl = arg.substr(12);
        else if (arg.find("--mode=") == 0) {
            std::string m = arg.substr(7);
            if (m == "1v1" || m == "1") cfg.gameMode = 0;
            else if (m == "ai_vs_ai" || m == "2") cfg.gameMode = 2;
            else cfg.gameMode = 1;
        }
        else if (arg.find("--config-file=") == 0) cfg.configFile = arg.substr(14);
        else if (arg.find("--config-json=") == 0) cfg.configJson = arg.substr(14);
    }
    return cfg;
}

static GameServer::PlayerInputPacket parseInputPacket(const uint8_t* data, size_t len) {
    GameServer::PlayerInputPacket inp{};
    if (len < sizeof(dzfoot::PlayerInputPacket)) {
        std::cerr << "[GameServer] Input packet too short: " << len
                  << " bytes (expected " << sizeof(dzfoot::PlayerInputPacket) << ")" << std::endl;
        return inp;
    }
    std::memcpy(&inp, data, sizeof(dzfoot::PlayerInputPacket));
    // Validate header (ignore if malformed, input will be zeroed)
    if (inp.header.magic != dzfoot::DZ_MAGIC ||
        inp.header.version != dzfoot::DZ_PROTOCOL_VERSION ||
        inp.header.type != dzfoot::PACKET_PLAYER_INPUT) {
        std::cerr << "[GameServer] Invalid input packet header (magic=" << inp.header.magic
                  << " ver=" << inp.header.version << " type=" << inp.header.type << ")" << std::endl;
        std::memset(&inp, 0, sizeof(inp));
        return inp;
    }
    return inp;
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    Args args = parseArgs(argc, argv);
    if (args.roomId.empty()) {
        std::cerr << "Usage: gf_server --room-id=ID [--mode=1v1|vs_ai|ai_vs_ai] [--redis-url=...] [--broadcast-hz=20]" << std::endl;
        return 1;
    }

    // 1. Connect to Redis (required for game state broadcast + input receive)
    RedisClient redis;
    if (!args.redisUrl.empty()) {
        if (!redis.configure(args.redisUrl)) {
            std::cerr << "[GF Server] Redis configure failed for " << args.redisUrl << std::endl;
        } else {
            std::cout << "[GF Server] Redis connected" << std::endl;
        }
    }

    // 2. Start GameServer
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
    gscfg.redis = &redis;
    gscfg.gameMode = args.gameMode;
    gscfg.shutdownFlag = &gRunning;
    gscfg.matchConfigPath = args.configFile;
    gscfg.matchConfigJson = args.configJson;

    GameServer::Server server(gscfg);

    std::cout << "[gamestates] GF_SERVER_START room=" << args.roomId
              << " mode=" << (int)args.gameMode
              << " teamA=" << args.teamA
              << " teamB=" << args.teamB
              << " redis=" << (args.redisUrl.empty() ? "none" : "connected")
              << std::endl;

    // 3. Start input listener thread (Redis subscription for player inputs)
    std::string roomId = args.roomId;
    std::thread inputThread([&]() {
        while (gRunning) {
            auto raw = redis.subscribeNext("gf.input", 100);
            if (raw.size() >= 37) {
                // First 36 bytes: room_id prefix
                std::string msgRoomId(raw.begin(), raw.begin() + 36);
                msgRoomId.erase(std::find(msgRoomId.begin(), msgRoomId.end(), '\0'), msgRoomId.end());
                if (roomId.rfind(msgRoomId, 0) == 0) {
                    auto input = parseInputPacket(raw.data() + 36, raw.size() - 36);
                    server.receiveInput(input);
                }
            }
        }
    });

    server.run();

    gRunning = false;
    if (inputThread.joinable()) inputThread.join();

    // 4. Post match result to Stats Service
    server.postMatchResult(args.statsUrl);

    std::cout << "GF Server exited cleanly" << std::endl;
    return 0;
}
