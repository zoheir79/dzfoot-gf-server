#include <iostream>
#include <fstream>
#include <cstring>
#include "protocol/DZFootProtocol.h"

using namespace dzfoot;

int main() {
    std::cout << "DZFoot Protocol v" << DZ_PROTOCOL_VERSION << " layout tests" << std::endl;

    // 1. Static sizes
    static_assert(sizeof(PacketHeader) == 12, "PacketHeader size");
    static_assert(sizeof(NetworkBallState) == 40, "NetworkBallState size");
    static_assert(sizeof(NetworkPlayerState) == 48, "NetworkPlayerState size");
    static_assert(sizeof(GameStatePacket) < 1200, "GameStatePacket fits datagram");
    static_assert(sizeof(MatchEventPacket) == 12 + 1 + 1 + 1 + 1 + 12 + 4 + 2 + 2, "MatchEventPacket size");
    static_assert(sizeof(PlayerInputPacket) == 12 + 4 + 4 + 2 + 1 + 1 + 4 + 8, "PlayerInputPacket size");
    static_assert(sizeof(PlayerStaticInfo) == 3 + 32 + 4 + 4 + 32 + (4 * 21), "PlayerStaticInfo size");
    static_assert(sizeof(MatchSetupPacket) == 12 + 1 + 64 + 1 + 1 + (sizeof(PlayerStaticInfo) * 22), "MatchSetupPacket size");

    std::cout << "sizeof(PacketHeader)       = " << sizeof(PacketHeader) << std::endl;
    std::cout << "sizeof(PlayerStaticInfo)   = " << sizeof(PlayerStaticInfo) << std::endl;
    std::cout << "sizeof(MatchSetupPacket)   = " << sizeof(MatchSetupPacket) << std::endl;
    std::cout << "sizeof(NetworkBallState)   = " << sizeof(NetworkBallState) << std::endl;
    std::cout << "sizeof(NetworkPlayerState) = " << sizeof(NetworkPlayerState) << std::endl;
    std::cout << "sizeof(GameStatePacket)    = " << sizeof(GameStatePacket) << std::endl;
    std::cout << "sizeof(MatchEventPacket)   = " << sizeof(MatchEventPacket) << std::endl;
    std::cout << "sizeof(PlayerInputPacket)  = " << sizeof(PlayerInputPacket) << std::endl;

    // 2. Build a golden GameStatePacket
    GameStatePacket gs{};
    gs.header.magic = DZ_MAGIC;
    gs.header.version = DZ_PROTOCOL_VERSION;
    gs.header.type = PACKET_GAME_STATE;
    gs.header.size = sizeof(gs);
    gs.header.flags = 0;
    gs.tick = 12345;
    gs.timestampUs = 9876543210ULL;
    gs.gameMode = 1;
    gs.gameFlags = 0;
    gs.score[0] = 2;
    gs.score[1] = 1;
    gs.timer = 42.5f;
    gs.ball.pos[0] = 1.0f; gs.ball.pos[1] = 0.2f; gs.ball.pos[2] = -0.5f;
    gs.ball.vel[0] = 0.1f; gs.ball.vel[1] = 0.0f; gs.ball.vel[2] = 0.05f;
    gs.ball.ownedTeam = 0;
    gs.ball.ownedPlayer = 3;
    for (int i = 0; i < DZ_MAX_PLAYERS; ++i) {
        gs.players[i].pos[0] = float(i);
        gs.players[i].pos[1] = float(i) * 0.1f;
        gs.players[i].pos[2] = -float(i);
        gs.players[i].anim = static_cast<uint8_t>(i % ANIM_COUNT);
        gs.players[i].team = (i < 11) ? 0 : 1;
    }

    std::ofstream fout("golden_gamestate_v1.bin", std::ios::binary);
    fout.write(reinterpret_cast<const char*>(&gs), sizeof(gs));
    fout.close();
    std::cout << "Wrote golden_gamestate_v1.bin (" << sizeof(gs) << " bytes)" << std::endl;

    // 3. Validation tests
    uint8_t buf[sizeof(gs)];
    std::memcpy(buf, &gs, sizeof(gs));

    if (!validateGameStatePacket(buf, sizeof(buf))) {
        std::cerr << "FAIL: valid packet rejected" << std::endl;
        return 1;
    }
    if (validateGameStatePacket(buf, sizeof(buf) - 1)) {
        std::cerr << "FAIL: truncated packet accepted" << std::endl;
        return 1;
    }
    buf[4] = 99; // corrupt version
    if (validateGameStatePacket(buf, sizeof(buf))) {
        std::cerr << "FAIL: bad version accepted" << std::endl;
        return 1;
    }
    buf[4] = 1; // restore version
    if (!validateGameStatePacket(buf, sizeof(buf))) {
        std::cerr << "FAIL: restored packet rejected" << std::endl;
        return 1;
    }

    // 4. NaN rejection
    GameStatePacket gsNaN = gs;
    gsNaN.players[0].pos[0] = std::numeric_limits<float>::quiet_NaN();
    std::memcpy(buf, &gsNaN, sizeof(gsNaN));
    if (isFiniteVec3(gsNaN.players[0].pos)) {
        std::cerr << "FAIL: NaN not detected" << std::endl;
        return 1;
    }

    // 5. MatchSetupPacket golden + validation
    MatchSetupPacket ms{};
    ms.header.magic = DZ_MAGIC;
    ms.header.version = DZ_PROTOCOL_VERSION;
    ms.header.type = PACKET_MATCH_SETUP;
    ms.header.size = sizeof(ms);
    ms.header.flags = 0;
    ms.playerCount = DZ_MAX_PLAYERS;
    std::strcpy(ms.teamAName, "CR Belouizdad");
    std::strcpy(ms.teamBName, "MC Alger");
    ms.stadiumId = 1;
    ms.durationMinutes = 90;
    for (int i = 0; i < DZ_MAX_PLAYERS; ++i) {
        ms.players[i].index = i;
        ms.players[i].team = (i < 11) ? 0 : 1;
        ms.players[i].role = 0;
        std::strcpy(ms.players[i].lastName, "Player");
        ms.players[i].height = 1.80f;
        ms.players[i].skinColor = 0;
        std::strcpy(ms.players[i].hairStyle, "short");
        std::strcpy(ms.players[i].hairColor, "black");
        for (int s = 0; s < kNumPlayerStats; ++s) ms.players[i].stats[s] = 50.0f;
    }
    {
        std::ofstream fout("golden_matchsetup_v1.bin", std::ios::binary);
        fout.write(reinterpret_cast<const char*>(&ms), sizeof(ms));
        fout.close();
        std::cout << "Wrote golden_matchsetup_v1.bin (" << sizeof(ms) << " bytes)" << std::endl;
    }
    uint8_t msBuf[sizeof(ms)];
    std::memcpy(msBuf, &ms, sizeof(ms));
    if (!validateMatchSetupPacket(msBuf, sizeof(msBuf))) {
        std::cerr << "FAIL: valid MatchSetupPacket rejected" << std::endl;
        return 1;
    }
    if (validateMatchSetupPacket(msBuf, sizeof(msBuf) - 1)) {
        std::cerr << "FAIL: truncated MatchSetupPacket accepted" << std::endl;
        return 1;
    }

    std::cout << "All tests PASSED" << std::endl;
    return 0;
}
