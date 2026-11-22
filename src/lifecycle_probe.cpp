// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "lifecycle_probe.hpp"

#include <array>
#include <atomic>
#include <cstdint>

#include <polyhook/polyhook.hpp>

namespace funrounds::native::detail {
namespace {

#if defined(_MSC_VER)
#define FUNROUNDS_NOINLINE __declspec(noinline)
#else
#define FUNROUNDS_NOINLINE __attribute__((noinline))
#endif

std::atomic<std::int32_t> probeMultiplier{3};
std::atomic<std::int32_t> probeCallbackCount{0};
std::atomic<bool> probeCallbackLive{false};
polyhook::HookHandle probeHook = nullptr;
const char* lifecycleProbeFailure = "probe did not run";

extern "C" FUNROUNDS_NOINLINE std::int32_t LifecycleProbeTarget(std::int32_t value) {
    return value * probeMultiplier.load(std::memory_order_relaxed);
}

polyhook::ResultType LifecycleProbeCallback(polyhook::HookHandle,
                                             polyhook::ParametersHandle,
                                             std::int32_t,
                                             polyhook::ReturnHandle,
                                             polyhook::CallbackType type) {
    if (type == polyhook::CallbackType::Pre && probeCallbackLive.load(std::memory_order_acquire)) {
        probeCallbackCount.fetch_add(1, std::memory_order_relaxed);
    }
    return polyhook::ResultType::Ignored;
}

} // namespace

bool StartLifecycleProbe() {
    probeCallbackCount.store(0, std::memory_order_relaxed);
    lifecycleProbeFailure = "unexpected existing lifecycle detour";
    if (probeHook != nullptr || polyhook::FindDetour(reinterpret_cast<void*>(&LifecycleProbeTarget)) != nullptr) {
        return false;
    }

    lifecycleProbeFailure = "unhooked lifecycle target returned an unexpected value";
    if (LifecycleProbeTarget(4) != 12 || probeCallbackCount.load(std::memory_order_relaxed) != 0) {
        return false;
    }

    constexpr std::array arguments{polyhook::DataType::Int32};
    const plg::vector<polyhook::DataType> argumentTypes(arguments.begin(), arguments.end());
    probeHook = polyhook::HookDetour(reinterpret_cast<void*>(&LifecycleProbeTarget),
                                     polyhook::DataType::Int32,
                                     argumentTypes,
                                     -1,
                                     "FunRounds::LifecycleProbe");
    if (probeHook == nullptr) {
        lifecycleProbeFailure = "PolyHook failed to create the lifecycle detour";
        (void)StopLifecycleProbe();
        return false;
    }

    probeCallbackLive.store(true, std::memory_order_release);
    if (!polyhook::AddCallback(probeHook, polyhook::CallbackType::Pre, LifecycleProbeCallback)) {
        lifecycleProbeFailure = "PolyHook failed to register the lifecycle callback";
        (void)StopLifecycleProbe();
        return false;
    }
    if (!polyhook::IsCallbackRegistered(probeHook, polyhook::CallbackType::Pre, LifecycleProbeCallback)) {
        lifecycleProbeFailure = "PolyHook did not report the lifecycle callback as registered";
        (void)StopLifecycleProbe();
        return false;
    }

    const std::int32_t result = LifecycleProbeTarget(4);
    if (result != 12 || probeCallbackCount.load(std::memory_order_relaxed) != 1) {
        lifecycleProbeFailure = "lifecycle target did not traverse its registered pre-callback";
        (void)StopLifecycleProbe();
        return false;
    }

    lifecycleProbeFailure = "";
    return true;
}

const char* LifecycleProbeFailure() noexcept {
    return lifecycleProbeFailure;
}

bool StopLifecycleProbe() noexcept {
    probeCallbackLive.store(false, std::memory_order_release);
    void* const target = reinterpret_cast<void*>(&LifecycleProbeTarget);

    try {
        auto remainingHook = polyhook::FindDetour(target);
        if (remainingHook != nullptr) {
            if (polyhook::IsCallbackRegistered(remainingHook, polyhook::CallbackType::Pre, LifecycleProbeCallback)) {
                (void)polyhook::RemoveCallback(remainingHook, polyhook::CallbackType::Pre, LifecycleProbeCallback);
                if (polyhook::IsCallbackRegistered(remainingHook, polyhook::CallbackType::Pre, LifecycleProbeCallback)) {
                    return false;
                }
            }
            if (!polyhook::UnhookDetour(target) && polyhook::FindDetour(target) != nullptr) {
                return false;
            }
        }

        if (polyhook::FindDetour(target) != nullptr) {
            return false;
        }
        probeHook = nullptr;

        const std::int32_t countBefore = probeCallbackCount.load(std::memory_order_relaxed);
        return LifecycleProbeTarget(4) == 12 &&
               probeCallbackCount.load(std::memory_order_relaxed) == countBefore;
    } catch (...) {
        return false;
    }
}

} // namespace funrounds::native::detail
