// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cstdint>
#include <optional>
#include <unordered_map>

namespace funrounds::native::team_flash {

struct Attribution {
    std::int32_t throwerSlot;
    std::uint32_t throwerPawnHandle;
    std::int32_t throwerTeam;
};

constexpr bool IsPlayingTeam(std::int32_t team) noexcept {
    return team == 2 || team == 3;
}

constexpr bool ShouldVetoTeamFlash(bool ffaEnabled, std::int32_t victimSlot, std::int32_t victimTeam,
                                  std::uint32_t victimPawnHandle, std::optional<Attribution> attribution) noexcept {
    return !ffaEnabled && attribution && IsPlayingTeam(victimTeam) && victimTeam == attribution->throwerTeam &&
        victimSlot != attribution->throwerSlot && victimPawnHandle != attribution->throwerPawnHandle;
}

class AttributionTable {
public:
    // Never carry attribution across serials on a reused entity index.
    void Record(std::uint32_t fullHandle, Attribution attribution) {
        entries_.insert_or_assign(fullHandle, attribution);
    }

    std::optional<Attribution> Find(std::uint32_t fullHandle) const {
        const auto entry = entries_.find(fullHandle);
        if (entry == entries_.end()) return std::nullopt;
        return entry->second;
    }

    void Erase(std::uint32_t fullHandle) noexcept { entries_.erase(fullHandle); }
    void Clear() noexcept { entries_.clear(); }

private:
    std::unordered_map<std::uint32_t, Attribution> entries_;
};

} // namespace funrounds::native::team_flash
