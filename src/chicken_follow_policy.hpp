// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cstdint>
#include <unordered_set>

namespace funrounds::native::detail {

struct ChickenFollowPolicy {
    bool all{};
    std::unordered_set<std::int32_t> chickens;

    bool WantsHook() const noexcept { return all || !chickens.empty(); }
    bool Blocks(std::int32_t chicken) const {
        return chicken != -1 && (all || chickens.contains(chicken));
    }
};

} // namespace funrounds::native::detail
