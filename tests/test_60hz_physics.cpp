// DZFoot 60Hz Refactor — Compile-time Constants Validation
// Build:  cmake .. -DBUILD_60HZ_TEST=ON && cmake --build . --target test_60hz_physics
// Run:    ./build/tests/test_60hz_physics
//
// This test validates compile-time constants only.
// For runtime physics validation (ball friction, player sprint, determinism),
// use test_server_tickrate.py which launches the full gf_server binary.

#include <iostream>
#include <cmath>
#include <sstream>
#include <iomanip>

#include "timestep_config.hpp"
#include "main.hpp"

static bool floatEq(float a, float b, float eps = 0.001f) {
    return std::fabs(a - b) < eps;
}

static std::string fmt(float v) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(4) << v;
    return ss.str();
}

static bool testConstants() {
    std::cout << "[TEST 1] Timestep constants\n";
    bool ok = true;

    ok &= (kSimFrequencyHz == 60);
    std::cout << "  kSimFrequencyHz = " << kSimFrequencyHz
              << "  " << (ok ? "PASS" : "FAIL") << "\n";

    ok &= floatEq(kTimeStepMs, 16.6667f);
    std::cout << "  kTimeStepMs     = " << fmt(kTimeStepMs)
              << "  " << (floatEq(kTimeStepMs, 16.6667f) ? "PASS" : "FAIL") << "\n";

    ok &= floatEq(kTimeStepS, 0.0166667f);
    std::cout << "  kTimeStepS      = " << fmt(kTimeStepS)
              << "  " << (floatEq(kTimeStepS, 0.0166667f) ? "PASS" : "FAIL") << "\n";

    ok &= floatEq(kTimeStepFactor, 1.6667f);
    std::cout << "  kTimeStepFactor = " << fmt(kTimeStepFactor)
              << "  " << (floatEq(kTimeStepFactor, 1.6667f) ? "PASS" : "FAIL") << "\n";

    return ok;
}

static bool testGameConfig() {
    std::cout << "\n[TEST 2] GameConfig defaults\n";
    auto cfg = GameConfig::make();
    bool ok = (cfg->physics_steps_per_frame == 1);
    std::cout << "  physics_steps_per_frame = " << cfg->physics_steps_per_frame
              << " (expected 1)  " << (ok ? "PASS" : "FAIL") << "\n";
    return ok;
}

int main(int, char**) {
    std::cout << "========================================\n";
    std::cout << "DZFoot 60Hz Refactor — Compile Tests\n";
    std::cout << "========================================\n";

    bool allOk = true;
    allOk &= testConstants();
    allOk &= testGameConfig();

    std::cout << "\n========================================\n";
    if (allOk) {
        std::cout << "ALL TESTS PASSED\n";
        return 0;
    } else {
        std::cout << "SOME TESTS FAILED\n";
        return 1;
    }
}
