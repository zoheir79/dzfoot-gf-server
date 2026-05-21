#pragma once
#include <string>

namespace GameServer {
struct MatchStats;
struct Config;
}

// POSTs final match result as JSON to the Stats Service.
// Returns true on HTTP 2xx. Logs errors otherwise.
class StatsPoster {
public:
    static bool post(const std::string& url,
                     const GameServer::Config& cfg,
                     const GameServer::MatchStats& stats);
};
