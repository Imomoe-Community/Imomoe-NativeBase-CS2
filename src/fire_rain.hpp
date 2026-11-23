// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cstdint>


namespace funrounds::native::detail {

bool MolotovRainSelfDamageAvailable() noexcept;
bool PrepareMolotovRainSelfDamage() noexcept;
bool TrackMolotovRainSelfDamage(std::int32_t projectile, std::int32_t thrower) noexcept;
bool ShouldBypassSelfDamageForRain(std::int32_t inflictorHandle,
                                   std::int32_t victimPawnHandle) noexcept;
void ClearMolotovRainTracking() noexcept;
bool ShutdownMolotovRain() noexcept;

// Shared standard-grenade Detonate callbacks bracket the original Molotov call.
void BeginRainMolotovDetonation(std::int32_t projectileHandle, void* entity) noexcept;
void EndRainMolotovDetonation(void* entity) noexcept;

} // namespace funrounds::native::detail
