// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "recoil_impulse_policy.hpp"

#include <limits>

using namespace funrounds::native::detail;

int main() {
    const FirearmTypes types{{1, 2, 3, 4, 5, 6}};
    for (std::uint32_t type = 1; type <= 6; ++type) {
        if (!types.contains(type)) return 1;
    }
    for (const auto type : {0u, 7u, 8u, 9u, 10u, 11u, 12u}) {
        if (types.contains(type)) return 2;
    }
    if (ScaleImpulse(2.0F, -1.0F) != -2.0F ||
        ScaleImpulse(2.0F, 0.0F) != 0.0F ||
        ScaleImpulse(2.0F, 0.5F) != 1.0F ||
        ScaleImpulse(2.0F, 6.0F) != 12.0F) return 3;
    if (ScaleImpulse(2.0F, 1.0F) ||
        ScaleImpulse(2.0F, -1.1F) ||
        ScaleImpulse(2.0F, 6.1F) ||
        ScaleImpulse(2.0F, std::numeric_limits<float>::quiet_NaN()) ||
        ScaleImpulse(2.0F, std::numeric_limits<float>::infinity()) ||
        ScaleImpulse(std::numeric_limits<float>::max(), 6.0F)) return 4;

    int shot{}, other{};
    ShotScope scope{1, 0x10001, &shot};
    if (!scope.matches(&shot) || scope.matches(&other)) return 5;
    ++scope.depth;
    if (scope.matches(&shot)) return 6;
    scope.leave();
    if (!scope.matches(&shot)) return 7;
    scope.leave();
    if (scope.matches(&shot) || scope.weapon != -1 || scope.time) return 8;
}
