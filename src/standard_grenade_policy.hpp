// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <optional>

#include "gamedata_key.hpp"
#include <cstdint>
#include <string_view>

namespace funrounds::native::grenade {

enum class Kind : std::int32_t {
    Smoke = 1,
    Molotov = 2,
    Incendiary = 3,
    He = 4,
    Flash = 5,
    Decoy = 6,
};

enum class RequestResult : std::int32_t {
    Failed = -1,
    AlreadyStartedOrGone = 0,
    Submitted = 1,
};

enum class StateResult : std::int32_t {
    Failed = -1,
    NotStarted = 0,
    StartedOrGone = 1,
};

constexpr std::int32_t ResultValue(RequestResult value) noexcept {
    return static_cast<std::int32_t>(value);
}

constexpr std::int32_t StateValue(StateResult value) noexcept {
    return static_cast<std::int32_t>(value);
}

constexpr bool ValidKind(std::int32_t value) noexcept {
    return value >= static_cast<std::int32_t>(Kind::Smoke) && value <= static_cast<std::int32_t>(Kind::Decoy);
}

constexpr std::string_view ProjectileClass(Kind kind) noexcept {
    switch (kind) {
        case Kind::Smoke: return "smokegrenade_projectile";
        case Kind::Molotov:
        case Kind::Incendiary: return "molotov_projectile";
        case Kind::He: return "hegrenade_projectile";
        case Kind::Flash: return "flashbang_projectile";
        case Kind::Decoy: return "decoy_projectile";
    }
    return {};
}

constexpr std::optional<detail::VTableKey> VTableName(Kind kind) noexcept {
    switch (kind) {
        case Kind::Smoke: return detail::VTableKey{"CSmokeGrenadeProjectile"};
        case Kind::Molotov:
        case Kind::Incendiary: return detail::VTableKey{"CMolotovProjectile"};
        case Kind::He: return detail::VTableKey{"CHEGrenadeProjectile"};
        case Kind::Flash: return detail::VTableKey{"CFlashbangProjectile"};
        case Kind::Decoy: return {};
    }
    return {};
}

constexpr bool UsesDetonationHold(Kind kind) noexcept {
    return kind == Kind::Smoke || kind == Kind::Molotov || kind == Kind::Incendiary || kind == Kind::Decoy;
}

constexpr std::string_view WeaponClass(Kind kind) noexcept {
    switch (kind) {
        case Kind::Smoke: return "weapon_smokegrenade";
        case Kind::Molotov: return "weapon_molotov";
        case Kind::Incendiary: return "weapon_incgrenade";
        case Kind::He: return "weapon_hegrenade";
        case Kind::Flash: return "weapon_flashbang";
        case Kind::Decoy: return "weapon_decoy";
    }
    return {};
}

} // namespace funrounds::native::grenade
