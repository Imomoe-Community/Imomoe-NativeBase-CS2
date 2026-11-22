// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cstdint>

#include <plg/plugin.hpp>

namespace funrounds::native::detail {

bool CanCreateMolotovRain(bool requireForceSelfDamage) noexcept;
std::int32_t CreateMolotovRainProjectile(const plg::vec3& position,
                                        std::int32_t throwerPawn,
                                        bool forceSelfDamage) noexcept;
bool ShouldBypassSelfDamageForRain(std::int32_t inflictorHandle,
                                   std::int32_t victimPawnHandle) noexcept;
void ClearMolotovRainTracking() noexcept;
bool ShutdownMolotovRain() noexcept;

// Shared standard-grenade Detonate callbacks bracket the original Molotov call.
void BeginRainMolotovDetonation(std::int32_t projectileHandle, void* entity) noexcept;
void EndRainMolotovDetonation(void* entity) noexcept;

} // namespace funrounds::native::detail
