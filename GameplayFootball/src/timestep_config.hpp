// Copyright 2019 Google LLC & Bastiaan Konings
// DZFoot 60Hz refactor — centralized timestep configuration
//
// MODE HYBRIDE (actuel): le moteur physique reste calibré à 10ms/tick.
// Seul kSimFrequencyHz est utilisé (cadence de la boucle serveur = 60 steps/s).
// match.cpp incrémente actualTime_ms de +10 à chaque step, donc les
// comparaisons exactes du referee restent valides.
//
// MODE NATIF 60Hz (futur, branche séparée) : appliquer kTimeStepFactor
// dans ball.cpp, humanoid, etc., et convertir referee en intervalles.
// Voir REFONTE_60HZ_PLAN.md.

#ifndef _HPP_TIMESTEP_CONFIG
#define _HPP_TIMESTEP_CONFIG

// Fréquence de la boucle serveur (steps/s)
constexpr int kSimFrequencyHz = 60;

// Timestep nominal en ms (~16.667 ms). NON utilisé en mode hybride.
constexpr float kTimeStepMs = 1000.0f / static_cast<float>(kSimFrequencyHz);

// Timestep nominal en secondes. NON utilisé en mode hybride.
constexpr float kTimeStepS = kTimeStepMs / 1000.0f;

// Facteur de conversion 10ms → 16.667ms. Réservé pour migration native 60Hz.
constexpr float kTimeStepFactor = kTimeStepMs / 10.0f;

// Échelle position→vitesse. Réservé pour migration native 60Hz.
constexpr float kPositionToVelocityScale = 1.0f / kTimeStepS;

static_assert(kTimeStepMs > 15.0f && kTimeStepMs < 17.0f,
              "kTimeStepMs must be approximately 16.6667");
static_assert(kTimeStepFactor > 1.5f && kTimeStepFactor < 1.8f,
              "kTimeStepFactor must be approximately 1.6667");

#endif
