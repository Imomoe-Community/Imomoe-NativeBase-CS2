// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace funrounds::native::reload_sync {

constexpr int kMaxPlayers = 64;
constexpr int kRequestTicks = 128; // Two seconds at CS2's 64 Hz simulation rate.
constexpr std::uint64_t kReloadBit = std::uint64_t{1} << 13;
using ButtonStates = std::uint64_t[3];

constexpr bool ReloadButtonDown(const ButtonStates& states) noexcept {
    return ((states[0] | states[2]) & kReloadBit) != 0;
}

constexpr bool ReadyToReject(std::int32_t clip, std::int32_t nextPrimaryAttackTick, int currentTick) noexcept {
    // The native deadline can be in a later subtick of the same tick.
    return clip >= 0 && nextPrimaryAttackTick < currentTick;
}

struct PlayerLife {
    int slot{-1};
    int controller{-1};
    int pawnHandle{-1};
    void* pawn{};

    bool operator==(const PlayerLife&) const = default;
};

struct PlayerIdentity : PlayerLife {
    int weapon{-1};

    bool operator==(const PlayerIdentity&) const = default;
};

struct PendingRequest {
    PlayerLife life;
    std::int64_t deadlineTick;
};

enum class PlayerEvent { Death, Spawn, Disconnect, BotTakeover, Equip };

class PolicyState {
public:
    std::array<std::optional<PlayerIdentity>, kMaxPlayers> fullAttempts{};
    std::array<std::optional<PlayerIdentity>, kMaxPlayers> fullReceipts{};
    std::size_t fullReceiptCount{};

    void Queue(const PlayerLife& life, int tick, bool reloading) noexcept {
        if (!reloading) Request(life) = PendingRequest{life, static_cast<std::int64_t>(tick) + kRequestTicks};
    }

    bool Begin(const PlayerLife& life, int tick, bool reloading) noexcept {
        auto& request = Request(life);
        if (!request) return false;
        if (request->life == life && tick < request->deadlineTick && !reloading) return true;
        request.reset();
        return false;
    }

    void Finish(const PlayerLife& life, int tick, bool accepted, bool rejectedByReadyWeapon) noexcept {
        auto& request = Request(life);
        if (!request || request->life != life) return;
        if (accepted || rejectedByReadyWeapon || tick >= request->deadlineTick) request.reset();
    }

    void SetReceipt(std::size_t slot, const PlayerIdentity& player) noexcept {
        auto& receipt = fullReceipts[slot];
        if (!receipt) ++fullReceiptCount;
        receipt = player;
    }

    void ClearReceipt(std::size_t slot) noexcept {
        auto& receipt = fullReceipts[slot];
        if (!receipt) return;
        receipt.reset();
        --fullReceiptCount;
    }

    void OnPlayerEvent(PlayerEvent event, int slot) noexcept {
        if (slot < 0 || slot >= kMaxPlayers) return;
        const auto index = static_cast<std::size_t>(slot);
        if (event != PlayerEvent::Equip) pending_[index].reset();
        fullAttempts[index].reset();
        ClearReceipt(index);
    }

    void OnRoundEnd() noexcept {
        ClearPending();
        fullAttempts.fill(std::nullopt);
    }

    void ClearPending() noexcept { pending_.fill(std::nullopt); }

private:
    std::optional<PendingRequest>& Request(const PlayerLife& life) noexcept {
        return pending_[static_cast<std::size_t>(life.slot)];
    }

    std::array<std::optional<PendingRequest>, kMaxPlayers> pending_{};
};

struct DispatchFrame {
    void* weapon{};
    std::optional<PlayerIdentity> player;
    std::byte* editedButtons{};
    bool synthetic{};
    bool attempted{};
    bool wasReloading{};
    bool weaponReloadFired{};
    bool nestedReloadStarted{};
    bool ownsFullAttempt{};

    bool ShouldBroadcast(bool nowReloading) const noexcept {
        return !synthetic && !weaponReloadFired && !nestedReloadStarted && !wasReloading && nowReloading;
    }
};

class DispatchStack {
public:
    DispatchFrame& Push(void* weapon) {
        frames_.emplace_back();
        frames_.back().weapon = weapon;
        return frames_.back();
    }

    DispatchFrame* Current() noexcept { return frames_.empty() ? nullptr : &frames_.back(); }
    bool IsNested() const noexcept { return frames_.size() > 1; }

    DispatchFrame Pop() noexcept {
        const auto frame = frames_.back();
        frames_.pop_back();
        return frame;
    }

    bool IsInjected(const PlayerLife& player, const std::byte* buttons) const noexcept {
        return std::any_of(frames_.begin(), frames_.end(), [&](const DispatchFrame& frame) {
            return frame.editedButtons && frame.editedButtons == buttons && *frame.player == player;
        });
    }

    bool ObserveReloadEvent(const PlayerIdentity& player) noexcept {
        const auto* const frame = Current();
        const bool synthetic = frame && frame->player == player && frame->synthetic;
        for (auto& active : frames_) {
            if (active.player == player) active.weaponReloadFired = true;
        }
        return synthetic;
    }

    void ObserveNestedReload(const PlayerIdentity& player) noexcept {
        for (auto& active : frames_) {
            if (active.player == player) active.nestedReloadStarted = true;
        }
    }

private:
    std::vector<DispatchFrame> frames_;
};

} // namespace funrounds::native::reload_sync
