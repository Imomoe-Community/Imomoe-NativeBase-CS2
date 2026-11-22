// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace funrounds::native {

class DamageFrameStack {
public:
    static void SetEnabled(bool enabled) noexcept {
        enabled_.store(enabled, std::memory_order_release);
    }

    static std::uint64_t Push() noexcept {
        activeCalls_.fetch_add(1, std::memory_order_acq_rel);
        if (overflowDepth_ != 0) {
            ++overflowDepth_;
            return 0;
        }
        const bool tracking = enabled_.load(std::memory_order_acquire);
        if (!tracking && frames_.empty()) {
            return 0;
        }

        std::uint64_t token = nextToken_.fetch_add(1, std::memory_order_relaxed);
        if (token == 0) {
            token = nextToken_.fetch_add(1, std::memory_order_relaxed);
        }
        try {
            frames_.push_back({tracking ? token : 0});
        } catch (...) {
            overflowDepth_ = 1;
            return 0;
        }
        return tracking ? token : 0;
    }

    static void Pop() noexcept {
        if (overflowDepth_ != 0) {
            --overflowDepth_;
        } else if (!frames_.empty()) {
            frames_.pop_back();
        }
        activeCalls_.fetch_sub(1, std::memory_order_acq_rel);
    }

    static std::uint64_t Current() noexcept {
        return overflowDepth_ == 0 && !frames_.empty() ? frames_.back().token : 0;
    }

    static void SetDirectFirearmContactGroup(std::int32_t group) noexcept {
        if (Current() != 0) frames_.back().directFirearmContactGroup = group;
    }

    static std::int32_t DirectFirearmContactGroup() noexcept {
        return Current() != 0 ? frames_.back().directFirearmContactGroup : -1;
    }

    static void SetDirectFirearmRay(float x, float y) noexcept {
        if (Current() != 0) frames_.back().directFirearmRay = {x, y};
    }

    static std::array<float, 2> DirectFirearmRay() noexcept {
        return Current() != 0 ? frames_.back().directFirearmRay : std::array<float, 2>{};
    }

    static void MarkDirectKnifeContact() noexcept {
        if (Current() != 0) frames_.back().directKnifeContact = true;
    }

    static bool HasDirectKnifeContact() noexcept {
        return Current() != 0 && frames_.back().directKnifeContact;
    }

    static std::uint64_t ActiveCalls() noexcept {
        return activeCalls_.load(std::memory_order_acquire);
    }

private:
    struct Frame {
        std::uint64_t token;
        std::int32_t directFirearmContactGroup = -1;
        bool directKnifeContact = false;
        std::array<float, 2> directFirearmRay{};
    };

    static inline std::atomic<bool> enabled_{false};
    static inline std::atomic<std::uint64_t> activeCalls_{0};
    static inline std::atomic<std::uint64_t> nextToken_{1};
    static inline thread_local std::vector<Frame> frames_;
    static inline thread_local std::size_t overflowDepth_ = 0;
};

class DamageFrameScope {
public:
    DamageFrameScope() noexcept : token_(DamageFrameStack::Push()) {}
    ~DamageFrameScope() { DamageFrameStack::Pop(); }

    DamageFrameScope(const DamageFrameScope&) = delete;
    DamageFrameScope& operator=(const DamageFrameScope&) = delete;

    std::uint64_t Token() const noexcept { return token_; }

private:
    std::uint64_t token_;
};

} // namespace funrounds::native
