// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cstdint>

namespace funrounds::native::detail {

constexpr bool ShouldDispatchPlayerDamage(std::uint64_t flags, std::uint64_t forceDeathFlag) noexcept {
    return (flags & forceDeathFlag) == 0;
}

} // namespace funrounds::native::detail
