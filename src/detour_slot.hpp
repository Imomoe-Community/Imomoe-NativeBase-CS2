// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// Shared process-wide detours belong to PolyHook, not to the last callback.
// Keep a known handle until the final callback is gone or PolyHook removes it.
#include <polyhook/polyhook.hpp>

namespace funrounds::native::detail {

class DetourSlot {
public:
    DetourSlot(void*& address, polyhook::HookHandle& hook, bool* attempted = nullptr) noexcept
        : address_(address), hook_(hook), attempted_(attempted) {}

    bool Refresh() {
        if (!address_) return !hook_;
        if (!hook_) return true;
        const auto current = polyhook::FindDetour(address_);
        if (!current) {
            hook_ = nullptr;
            if (attempted_) *attempted_ = false;
            return true;
        }
        return current == hook_;
    }

    template <typename Create>
    bool Acquire(void* target, Create create, bool joinExisting = false) {
        if (!target || (!address_ && hook_) || (address_ && address_ != target)) return false;
        if (!Refresh()) {
            // Only explicitly compatible callers may join a replacement.
            const auto current = polyhook::FindDetour(target);
            if (!joinExisting || !current || current == hook_) return false;
            hook_ = current;
            if (attempted_) *attempted_ = false;
            return true;
        }
        if (hook_) return true;
        const auto current = polyhook::FindDetour(target);
        if (current && !joinExisting) return false;
        address_ = target;
        if (current) {
            hook_ = current;
        } else {
            if (attempted_) *attempted_ = true;
            hook_ = create();
        }
        return hook_ != nullptr;
    }

    template <typename Callback>
    bool Add(polyhook::CallbackType phase, Callback callback) {
        if (!hook_) return false;
        if (!polyhook::IsCallbackRegistered(hook_, phase, callback)) {
            (void)polyhook::AddCallback(hook_, phase, callback);
        }
        return polyhook::IsCallbackRegistered(hook_, phase, callback);
    }

    template <typename Callback>
    bool Remove(polyhook::CallbackType phase, Callback callback) {
        polyhook::HookHandle current{};
        const auto state = StateForRemoval(current);
        if (state == RemovalState::Invalid) return false;
        if (state == RemovalState::Replaced) {
            // Never modify the replacement. A callback into this plugin on it
            // must remain a cleanup failure until its actual owner removes it.
            return !polyhook::IsCallbackRegistered(current, phase, callback);
        }
        if (state == RemovalState::Known && polyhook::IsCallbackRegistered(hook_, phase, callback)) {
            (void)polyhook::RemoveCallback(hook_, phase, callback);
            if (polyhook::IsCallbackRegistered(hook_, phase, callback)) return false;
        }
        return true;
    }

    // Call only after every callback owned by this consumer has been removed.
    bool Release() {
        polyhook::HookHandle current{};
        const auto state = StateForRemoval(current);
        if (state == RemovalState::Invalid) return false;
        if (state == RemovalState::Empty || state == RemovalState::Replaced) {
            address_ = nullptr;
            hook_ = nullptr;
            if (attempted_) *attempted_ = false;
            return true;
        }
        if (polyhook::AreCallbacksRegistered(hook_)) return true;
        (void)polyhook::UnhookDetour(address_);
        if (polyhook::FindDetour(address_)) return false;
        address_ = nullptr;
        hook_ = nullptr;
        if (attempted_) *attempted_ = false;
        return true;
    }

private:
    enum class RemovalState { Empty, Known, Replaced, Invalid };

    RemovalState StateForRemoval(polyhook::HookHandle& current) {
        if (!address_) return hook_ ? RemovalState::Invalid : RemovalState::Empty;
        current = polyhook::FindDetour(address_);
        if (!hook_ && attempted_ && *attempted_) hook_ = current;
        if (!hook_) return RemovalState::Empty;
        if (!current) {
            hook_ = nullptr;
            if (attempted_) *attempted_ = false;
            return RemovalState::Empty;
        }
        return current == hook_ ? RemovalState::Known : RemovalState::Replaced;
    }

    void*& address_;
    polyhook::HookHandle& hook_;
    bool* attempted_;
};

} // namespace funrounds::native::detail
