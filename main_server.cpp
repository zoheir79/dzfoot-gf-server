#include <iostream>
#include <cstring>
#include <csignal>
#include <atomic>
#include "GameServer.h"
#include "LiveKitBridge.h"

static std::atomic<bool> gRunning{true};

void signalHandler(int sig) {
    std::cout << "Received signal " << sig << ", shutting down..." << std::endl;
    gRunning = false;
}

struct Config {
    std::string roomId;
    std::string teamA;
    std::string teamB;
    std::string stadium;
    int duration = 600;
    std::string livekitUrl;
    std::string livekitToken;
    std::string statsUrl;
};

Config parseArgs(int argc, char* argv[]) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.find("--room-id=") == 0) cfg.roomId = arg.substr(10);
        else if (arg.find("--team-a=") == 0) cfg.teamA = arg.substr(9);
        else if (arg.find("--team-b=") == 0) cfg.teamB = arg.substr(9);
        else if (arg.find("--stadium=") == 0) cfg.stadium = arg.substr(10);
        else if (arg.find("--duration=") == 0) cfg.duration = std::stoi(arg.substr(11));
        else if (arg.find("--livekit-url=") == 0) cfg.livekitUrl = arg.substr(14);
        else if (arg.find("--livekit-token=") == 0) cfg.livekitToken = arg.substr(16);
        else if (arg.find("--stats-url=") == 0) cfg.statsUrl = arg.substr(12);
        else if (arg.find("--redis-url=") == 0) cfg.redisUrl = arg.substr(12);
    }
    return cfg;
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    Config cfg = parseArgs(argc, argv);
    if (cfg.roomId.empty()) {
        std::cerr << "Usage: gf_server --room-id=... --livekit-url=... --livekit-token=..." << std::endl;
        return 1;
    }

    // 1. Connect to LiveKit as server bot
    LiveKitBridge lkBridge;
    if (!lkBridge.connect(cfg.livekitUrl, cfg.livekitToken)) {
        std::cerr << "Failed to connect to LiveKit" << std::endl;
        return 1;
    }

    // 2. Start GameServer (runs in this thread)
    GameServer::Config gscfg;
    gscfg.roomId = cfg.roomId;
    gscfg.teamA = cfg.teamA;
    gscfg.teamB = cfg.teamB;
    gscfg.stadium = cfg.stadium;
    gscfg.duration = cfg.duration;
    gscfg.statsUrl = cfg.statsUrl;
    gscfg.redisUrl = cfg.redisUrl;
    gscfg.livekitBridge = &lkBridge;

    GameServer::Server server(gscfg);
    server.run();

    // 3. Post match result to Stats Service
    server.postMatchResult(cfg.statsUrl);

    lkBridge.disconnect();
    std::cout << "GF Server exited cleanly" << std::endl;
    return 0;
}
