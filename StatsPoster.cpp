#include "StatsPoster.h"
#include "GameServer.h"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <iostream>

using nlohmann::json;

static size_t writeNoop(void*, size_t size, size_t nmemb, void*) {
    return size * nmemb;
}

bool StatsPoster::post(const std::string& url,
                       const GameServer::Config& cfg,
                       const GameServer::MatchStats& stats) {
    if (url.empty()) {
        std::cout << "[StatsPoster] No stats-url configured, skipping POST" << std::endl;
        return false;
    }

    // Stats Service expects /internal/match-result format
    json payload = {
        {"room_id", cfg.roomId},
        {"player_a", cfg.playerA},
        {"player_b", cfg.playerB},
        {"team_a", cfg.teamA},
        {"team_b", cfg.teamB},
        {"stadium", cfg.stadium},
        {"score_a", stats.score[0]},
        {"score_b", stats.score[1]},
        {"duration_s", stats.duration_s},
        {"stats", {
            {
                {"goals", stats.goals[0]},
                {"shots", stats.shots[0]},
                {"shots_on_target", stats.shots_on_target[0]},
                {"passes", stats.passes[0]},
                {"passes_success", stats.passes_success[0]},
                {"tackles", stats.tackles[0]},
                {"yellow_cards", stats.yellow_cards[0]},
                {"red_cards", stats.red_cards[0]},
                {"possession_pct", stats.possession_ticks[0]},
                {"distance_m", stats.distance_m[0]}
            },
            {
                {"goals", stats.goals[1]},
                {"shots", stats.shots[1]},
                {"shots_on_target", stats.shots_on_target[1]},
                {"passes", stats.passes[1]},
                {"passes_success", stats.passes_success[1]},
                {"tackles", stats.tackles[1]},
                {"yellow_cards", stats.yellow_cards[1]},
                {"red_cards", stats.red_cards[1]},
                {"possession_pct", stats.possession_ticks[1]},
                {"distance_m", stats.distance_m[1]}
            }
        }}
    };

    std::string body = payload.dump();

    CURL* curl = curl_easy_init();
    if (!curl) {
        std::cerr << "[StatsPoster] curl_easy_init failed" << std::endl;
        return false;
    }

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json");

    std::string fullUrl = url;
    if (fullUrl.empty() || fullUrl.back() == '/') {
        fullUrl += "internal/match-result";
    } else {
        fullUrl += "/internal/match-result";
    }
    curl_easy_setopt(curl, CURLOPT_URL, fullUrl.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body.size());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeNoop);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        std::cerr << "[StatsPoster] POST failed: " << curl_easy_strerror(rc) << std::endl;
        return false;
    }
    if (code < 200 || code >= 300) {
        std::cerr << "[StatsPoster] HTTP " << code << " from " << url << std::endl;
        return false;
    }
    std::cout << "[StatsPoster] POST OK (" << code << ") " << url << std::endl;
    return true;
}
