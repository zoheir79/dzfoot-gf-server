// Copyright 2019 Google LLC & Bastiaan Konings
// DZFoot 100Hz refactor — centralized timestep configuration
//
// MODE HYBRIDE (actuel): le moteur physique reste calibré à 10ms/tick.
// Seul kSimFrequencyHz est utilisé (cadence de la boucle serveur = 100 steps/s).
// match.cpp incrémente actualTime_ms de +10 à chaque step, donc les
// comparaisons exactes du referee restent valides.
//
// MODE NATIF 60Hz (futur, branche séparée) : appliquer kTimeStepFactor
// dans ball.cpp, humanoid, etc., et convertir referee en intervalles.
// Voir REFONTE_60HZ_PLAN.md.

#ifndef _HPP_TIMESTEP_CONFIG
#define _HPP_TIMESTEP_CONFIG

// Fréquence de la boucle serveur (steps/s)
constexpr int kSimFrequencyHz = 100;

// Timestep nominal en ms (~10 ms). En mode hybride le moteur physique reste à 10ms/tick.
constexpr float kTimeStepMs = 1000.0f / static_cast<float>(kSimFrequencyHz);

// Timestep nominal en secondes.
constexpr float kTimeStepS = kTimeStepMs / 1000.0f;

// Facteur de conversion 10ms → 10ms (native 100Hz, factor = 1.0).
constexpr float kTimeStepFactor = kTimeStepMs / 10.0f;

// Échelle position→vitesse.
constexpr float kPositionToVelocityScale = 1.0f / kTimeStepS;

static_assert(kTimeStepMs > 9.0f && kTimeStepMs < 11.0f,
              "kTimeStepMs must be approximately 10.0");
static_assert(kTimeStepFactor > 0.9f && kTimeStepFactor < 1.1f,
              "kTimeStepFactor must be approximately 1.0");

#endif
