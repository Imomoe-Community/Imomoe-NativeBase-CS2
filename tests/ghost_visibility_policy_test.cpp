// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <source_location>
#include <vector>

#include "ghost_visibility_policy.hpp"

namespace {

void require(bool condition, const std::source_location& location = std::source_location::current()) {
    if (!condition) {
        std::cerr << "ghost visibility policy check failed at line " << location.line() << '\n';
        std::abort();
    }
}

} // namespace

int main() {
    using funrounds::native::ghost_visibility::BuildTargetSet;
    using funrounds::native::ghost_visibility::ContainsTarget;

    const std::vector<std::int32_t> handles{7, -2, -1, 7, std::numeric_limits<std::int32_t>::min()};
    const auto targets = BuildTargetSet(handles);
    require(targets.size() == 3);
    require(ContainsTarget(targets, 7));
    require(ContainsTarget(targets, UINT32_C(0xFFFFFFFE)));
    require(ContainsTarget(targets, UINT32_C(0x80000000)));
    require(!ContainsTarget(targets, UINT32_C(0xFFFFFFFF)));
    require(!ContainsTarget(targets, 8));
    require(BuildTargetSet({}).empty());
    return 0;
}
