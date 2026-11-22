// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace funrounds::native {

template <typename Callback>
class DamageCallScope {
public:
    DamageCallScope(const void* info, Callback callback, bool skipRules) noexcept
        : info_(info), callback_(callback), skipRules_(skipRules), previous_(current_) {
        current_ = this;
    }

    ~DamageCallScope() { current_ = previous_; }

    DamageCallScope(const DamageCallScope&) = delete;
    DamageCallScope& operator=(const DamageCallScope&) = delete;

    static bool SkipRules(const void* info, Callback callback) noexcept {
        const auto* scope = Find(info, callback);
        return scope && scope->skipRules_;
    }

    static bool IsSynthetic(const void* info, Callback callback) noexcept {
        return Find(info, callback) != nullptr;
    }

private:
    static const DamageCallScope* Find(const void* info, Callback callback) noexcept {
        for (auto* scope = current_; scope; scope = scope->previous_) {
            if (scope->info_ == info && scope->callback_ == callback) {
                return scope;
            }
        }
        return nullptr;
    }
    const void* info_;
    Callback callback_;
    bool skipRules_;
    DamageCallScope* previous_;
    static inline thread_local DamageCallScope* current_ = nullptr;
};

} // namespace funrounds::native
