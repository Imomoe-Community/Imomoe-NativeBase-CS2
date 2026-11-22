// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>

namespace funrounds::native::detail {

struct ShotScope {
    unsigned depth{};
    std::int32_t weapon{-1};
    const void* time{};

    void leave() noexcept {
        if (depth && --depth == 0) *this = {};
    }

    bool matches(const void* impulseTime) const noexcept {
        return depth == 1 && weapon != -1 && time && time == impulseTime;
    }
};

struct FirearmTypes {
    static constexpr std::array names{
        "WEAPONTYPE_PISTOL", "WEAPONTYPE_SUBMACHINEGUN", "WEAPONTYPE_SHOTGUN",
        "WEAPONTYPE_RIFLE", "WEAPONTYPE_SNIPER_RIFLE", "WEAPONTYPE_MACHINEGUN",
    };
    std::array<std::uint32_t, names.size()> values{};

    bool contains(std::uint32_t type) const noexcept {
        return std::ranges::find(values, type) != values.end();
    }
};

inline std::optional<float> ScaleImpulse(float magnitude, float factor) noexcept {
    if (!(factor >= -1.0F && factor <= 6.0F) || factor == 1.0F) return std::nullopt;
    const float scaled = magnitude * factor;
    return std::isfinite(scaled) ? std::optional{scaled} : std::nullopt;
}

} // namespace funrounds::native::detail
