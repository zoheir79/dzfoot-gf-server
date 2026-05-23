// Copyright 2019 Google LLC & Bastiaan Konings
// DZFoot 60Hz refactor — centralized timestep configuration
// This file is the SINGLE SOURCE OF TRUTH for all timestep-dependent values.
// Any change to simulation frequency MUST go through here.

#ifndef _HPP_TIMESTEP_CONFIG
#define _HPP_TIMESTEP_CONFIG

// Target simulation frequency: 60 Hz
constexpr int kSimFrequencyHz = 60;

// Timestep in milliseconds (~16.6667 ms)
constexpr float kTimeStepMs = 1000.0f / static_cast<float>(kSimFrequencyHz);

// Timestep in seconds (~0.0166667 s)
constexpr float kTimeStepS = kTimeStepMs / 1000.0f;

// Conversion factor from old 10ms timestep to new timestep.
// All per-frame coefficients calibrated for 10ms must be multiplied by this.
// Example: old drag = 0.015f  →  new drag = 0.015f * kTimeStepFactor
constexpr float kTimeStepFactor = kTimeStepMs / 10.0f;

// Old timestep values kept for reference during migration.
// Remove once all coefficients have been migrated.
static_assert(kTimeStepMs > 15.0f && kTimeStepMs < 17.0f,
              "kTimeStepMs must be approximately 16.6667");
static_assert(kTimeStepFactor > 1.5f && kTimeStepFactor < 1.8f,
              "kTimeStepFactor must be approximately 1.6667");

#endif
