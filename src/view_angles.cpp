// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "engine.hpp"

#include <cmath>

#include <mathlib/vector.h>
#include <plugin_export.h>

extern "C" PLUGIN_API bool SnapPlayerViewAngles(std::int32_t pawnHandle, const plg::vec3& viewAngles) {
    using namespace funrounds::native::detail;
    using SnapViewAngles = void (*)(void*, QAngle*);
    constexpr SignatureKey kSnapViewAngles{"PlayerPawn::SnapViewAngles"};

    if (!std::isfinite(viewAngles.x) || !std::isfinite(viewAngles.y) || !std::isfinite(viewAngles.z)) {
        return false;
    }
    try {
        if (!EnsureEngine() || !IsPlayerPawn(pawnHandle)) return false;
        auto* pawn = EntityFromHandle(pawnHandle);
        if (!pawn || HandleFromEntity(pawn) != pawnHandle) return false;
        const auto snap = reinterpret_cast<SnapViewAngles>(ProbeSignature(kSnapViewAngles));
        if (!snap) return false;

        QAngle angles(viewAngles.x, viewAngles.y, viewAngles.z);
        snap(pawn, &angles);
        return true;
    } catch (...) {
        return false;
    }
}
