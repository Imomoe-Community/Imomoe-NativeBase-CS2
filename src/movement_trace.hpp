// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cstdint>

#include <plugin_export.h>

#include <mathlib/vector.h>

namespace funrounds::native::detail {
void ResetMovementTraceMetadata() noexcept;

struct WorldShotContact {
    Vector position;
    Vector start;
    Vector normal;
    std::uint32_t surface;
    std::int32_t entity;
};

bool TraceWorldShotContact(std::int32_t shooter, const Vector& muzzle, const Vector& position,
                           WorldShotContact& contact) noexcept;
}

// Both methods are game-thread-only. The query accepts a complete player pawn
// handle and the pawn's real local collision bounds. A true return means a
// finite query completed; `outHit == false` is a successful miss.
extern "C" PLUGIN_API bool HasPlayerMovementHullSupport();

extern "C" PLUGIN_API bool TracePlayerMovementHull(std::int32_t pawnHandle,
                                                   const Vector& start,
                                                   const Vector& end,
                                                   const Vector& hullMins,
                                                   const Vector& hullMaxs,
                                                   Vector& outPosition,
                                                   float& outFraction,
                                                   bool& outHit,
                                                   std::int32_t& outEntityHandle,
                                                   bool& outStartSolid,
                                                   Vector& outNormal);
