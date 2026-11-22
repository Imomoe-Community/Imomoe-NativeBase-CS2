// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "abi_probe.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <type_traits>

#include <polyhook/polyhook.hpp>

#if defined(_WIN32) && defined(_M_X64)
constexpr char kProbePlatform[] = "windows-x64";
constexpr char kPlatformReturnAbiField[] = "windows_output_buffer_rax";
#elif defined(__linux__) && defined(__x86_64__)
constexpr char kProbePlatform[] = "linux-x64";
constexpr char kPlatformReturnAbiField[] = "linux_xmm0_xmm1_vector";
#else
#error "The third-person ABI probe requires Windows x64 or Linux x64"
#endif

namespace third_person_abi_probe {
struct ProbeVector {
    float x{};
    float y{};
    float z{};
};

struct ProbePawn {
    const ProbeVector* camera{};
    ProbeVector nativeEye{};
    std::uint8_t forceNative{};
};

static_assert(std::is_standard_layout_v<ProbeVector>);
static_assert(sizeof(ProbeVector) == 12);
static_assert(std::is_standard_layout_v<ProbePawn>);
static_assert(offsetof(ProbePawn, camera) == 0);
static_assert(offsetof(ProbePawn, nativeEye) == 8);
static_assert(offsetof(ProbePawn, forceNative) == 20);
static_assert(sizeof(ProbePawn) == 24);
} // namespace third_person_abi_probe

extern "C" {
extern char ThirdPersonAbiSite[];

#if defined(_WIN32)
third_person_abi_probe::ProbeVector* ThirdPersonAbiTarget(
    third_person_abi_probe::ProbePawn* pawn, third_person_abi_probe::ProbeVector* output) noexcept;
#else
third_person_abi_probe::ProbeVector ThirdPersonAbiTarget(
    third_person_abi_probe::ProbePawn* pawn) noexcept;
#endif

bool ThirdPersonAbiCallWithSentinels(
    third_person_abi_probe::ProbePawn* pawn, third_person_abi_probe::ProbeVector* output) noexcept;
}

namespace {

using third_person_abi_probe::ProbePawn;
using third_person_abi_probe::ProbeVector;

constexpr ProbeVector kNestedCamera{77.0F, 88.0F, -99.0F};
constexpr ProbeVector kNestedNative{7.0F, 8.0F, 9.0F};

struct HookState {
    void* site{};
    polyhook::HookHandle hook{};
};

HookState g_hookState;
std::atomic<std::uint32_t> g_callbackCount{};
std::atomic<std::uint32_t> g_forceCallbackCount{};
std::atomic<std::uint32_t> g_nestedCallbackCount{};
std::atomic<bool> g_registerInputsMatch{true};
std::atomic<bool> g_cameraFieldsUntouched{true};
std::atomic<bool> g_nestedCallTriggered{};
std::atomic<bool> g_nestedCallPassed{};
std::atomic<bool> g_callbackThrew{};
thread_local bool g_insideNestedCall{};

bool HookControlImportsAvailable() noexcept {
    return __polyhook_HookDetour2 && __polyhook_FindDetour && __polyhook_AddCallback &&
        __polyhook_RemoveCallback && __polyhook_IsCallbackRegistered && __polyhook_UnhookDetour &&
        __polyhook_GetRegisterPointer && __polyhook_SetRegisterPointer;
}

bool SameVector(const ProbeVector& actual, const ProbeVector& expected) noexcept {
    return actual.x == expected.x && actual.y == expected.y && actual.z == expected.z;
}

bool InvokeNestedTarget(ProbePawn& pawn, ProbeVector& output) noexcept {
#if defined(_WIN32)
    return ThirdPersonAbiTarget(&pawn, &output) == &output;
#else
    output = ThirdPersonAbiTarget(&pawn);
    return true;
#endif
}

class NestedCallScope final {
public:
    NestedCallScope() noexcept : previous_(g_insideNestedCall) {
        g_insideNestedCall = true;
    }

    ~NestedCallScope() {
        g_insideNestedCall = previous_;
    }

    NestedCallScope(const NestedCallScope&) = delete;
    NestedCallScope& operator=(const NestedCallScope&) = delete;

private:
    bool previous_;
};

polyhook::ResultType ProbePre(polyhook::HookHandle, polyhook::ParametersHandle parameters,
                              std::int32_t, polyhook::ReturnHandle,
                              polyhook::CallbackType type) noexcept {
    try {
        if (parameters == nullptr || type != polyhook::CallbackType::Pre) {
            return polyhook::ResultType::Ignored;
        }

        const auto registers = static_cast<polyhook::RegistersHandle>(parameters);
#if defined(_WIN32)
        constexpr auto pawnRegister = polyhook::RegisterType::RDI;
        constexpr auto temporaryRegister = polyhook::RegisterType::RCX;
#else
        constexpr auto pawnRegister = polyhook::RegisterType::RBX;
        constexpr auto temporaryRegister = polyhook::RegisterType::RDI;
#endif
        auto* const pawn = static_cast<ProbePawn*>(polyhook::GetRegisterPointer(registers, pawnRegister));
        if (pawn == nullptr) {
            g_registerInputsMatch.store(false, std::memory_order_relaxed);
            return polyhook::ResultType::Ignored;
        }

        g_callbackCount.fetch_add(1, std::memory_order_relaxed);
        const ProbeVector* const originalCamera = pawn->camera;
        if (polyhook::GetRegisterPointer(registers, temporaryRegister) != originalCamera) {
            g_registerInputsMatch.store(false, std::memory_order_relaxed);
        }
        if (g_insideNestedCall) {
            g_nestedCallbackCount.fetch_add(1, std::memory_order_relaxed);
        }

        if (pawn->forceNative != 0) {
            g_forceCallbackCount.fetch_add(1, std::memory_order_relaxed);
            polyhook::SetRegisterPointer(registers, temporaryRegister, nullptr);
            if (pawn->camera != originalCamera) {
                g_cameraFieldsUntouched.store(false, std::memory_order_relaxed);
            }

            if (!g_insideNestedCall) {
                g_nestedCallTriggered.store(true, std::memory_order_relaxed);
                NestedCallScope nestedScope;
                ProbePawn nestedPawn{&kNestedCamera, kNestedNative, 0};
                ProbeVector nestedResult{};
                const bool returnedCorrectly = InvokeNestedTarget(nestedPawn, nestedResult);
                g_nestedCallPassed.store(returnedCorrectly && SameVector(nestedResult, kNestedCamera),
                                         std::memory_order_relaxed);
            }
        }
    } catch (...) {
        g_callbackThrew.store(true, std::memory_order_relaxed);
    }

    return polyhook::ResultType::Ignored;
}

bool PrepareProbeHook() {
    if (!HookControlImportsAvailable()) {
        return false;
    }

    void* const site = static_cast<void*>(ThirdPersonAbiSite);
    if (g_hookState.site != nullptr && g_hookState.site != site) {
        return false;
    }

    if (g_hookState.site != nullptr) {
        const auto current = polyhook::FindDetour(site);
        if (current == nullptr) {
            g_hookState = {};
        } else if (g_hookState.hook != nullptr && current != g_hookState.hook) {
            return false;
        } else if (g_hookState.hook == nullptr) {
            // The state only acquires this address after confirming it was free.
            // A hook observed here can therefore be ours after a partial install.
            g_hookState.hook = current;
        }
    }

    if (g_hookState.site == nullptr) {
        if (polyhook::FindDetour(site) != nullptr) {
            return false;
        }
        g_hookState.site = site;
        try {
            g_hookState.hook = polyhook::HookDetour2(site, plg::string("third-person-abi-probe"));
        } catch (...) {
            try {
                g_hookState.hook = polyhook::FindDetour(site);
            } catch (...) {
                // Keep the address so Stop can make a later ownership check.
            }
            return false;
        }
        if (g_hookState.hook == nullptr) {
            try {
                g_hookState.hook = polyhook::FindDetour(site);
            } catch (...) {
                // Keep the address so Stop can make a later ownership check.
            }
            return false;
        }
    }

    const auto current = polyhook::FindDetour(site);
    if (current == nullptr || current != g_hookState.hook) {
        return false;
    }
    if (!polyhook::IsCallbackRegistered(current, polyhook::CallbackType::Pre, ProbePre)) {
        (void)polyhook::AddCallback(current, polyhook::CallbackType::Pre, ProbePre);
    }
    return polyhook::IsCallbackRegistered(current, polyhook::CallbackType::Pre, ProbePre);
}

void ResetProbeMeasurements() noexcept {
    g_callbackCount.store(0, std::memory_order_relaxed);
    g_forceCallbackCount.store(0, std::memory_order_relaxed);
    g_nestedCallbackCount.store(0, std::memory_order_relaxed);
    g_registerInputsMatch.store(true, std::memory_order_relaxed);
    g_cameraFieldsUntouched.store(true, std::memory_order_relaxed);
    g_nestedCallTriggered.store(false, std::memory_order_relaxed);
    g_nestedCallPassed.store(false, std::memory_order_relaxed);
    g_callbackThrew.store(false, std::memory_order_relaxed);
}

std::string RunNotStartedResult(const char* detachStatus) {
    std::ostringstream result;
    result << "abi_probe=fail platform=" << kProbePlatform
           << " setup=fail callback=not_run null_camera=not_run own_camera_branch=not_run"
           << " external_camera=not_run branch_relocated=not_run nested=not_run"
           << " return_abi=not_run nonvolatile=not_run camera_unchanged=not_run"
           << ' ' << kPlatformReturnAbiField << "=not_run"
           << " detach=" << detachStatus
           << " post_detach_original=not_run";
    return result.str();
}

} // namespace

std::string RunThirdPersonAbiProbe() {
    try {
        if (!PrepareProbeHook()) {
            const bool hadOwnedState = g_hookState.site != nullptr;
            const bool detached = hadOwnedState ? StopThirdPersonAbiProbe() : true;
            const char* const detachStatus = !hadOwnedState ? "not_owned" : (detached ? "pass" : "fail");
            return RunNotStartedResult(detachStatus);
        }

        ResetProbeMeasurements();

        constexpr ProbeVector noCameraNative{101.25F, 202.5F, -303.75F};
        constexpr ProbeVector ownedCamera{-11.0F, 22.0F, 33.0F};
        constexpr ProbeVector externalCamera{44.0F, -55.0F, 66.0F};

        ProbePawn noCameraPawn{nullptr, noCameraNative, 0};
        ProbePawn ownedPawn{&ownedCamera, noCameraNative, 1};
        ProbePawn externalPawn{&externalCamera, noCameraNative, 0};
        ProbeVector noCameraResult{};
        ProbeVector ownedResult{};
        ProbeVector externalResult{};

        const bool noCameraAbi = ThirdPersonAbiCallWithSentinels(&noCameraPawn, &noCameraResult);
        const bool noCameraPath = noCameraAbi && SameVector(noCameraResult, noCameraNative);
        const bool ownedAbi = ThirdPersonAbiCallWithSentinels(&ownedPawn, &ownedResult);
        const bool ownCameraBranch = ownedAbi && SameVector(ownedResult, noCameraNative);
        const bool externalAbi = ThirdPersonAbiCallWithSentinels(&externalPawn, &externalResult);
        const bool externalCameraPath = externalAbi && SameVector(externalResult, externalCamera);

        const bool callbackInputs = g_callbackCount.load(std::memory_order_relaxed) == std::uint32_t{4} &&
            g_forceCallbackCount.load(std::memory_order_relaxed) == std::uint32_t{1} &&
            g_registerInputsMatch.load(std::memory_order_relaxed) &&
            !g_callbackThrew.load(std::memory_order_relaxed);
        const bool nested = g_nestedCallTriggered.load(std::memory_order_relaxed) &&
            g_nestedCallbackCount.load(std::memory_order_relaxed) == 1 &&
            g_nestedCallPassed.load(std::memory_order_relaxed);
        const bool cameraUnchanged = g_cameraFieldsUntouched.load(std::memory_order_relaxed) &&
            noCameraPawn.camera == nullptr && ownedPawn.camera == &ownedCamera &&
            externalPawn.camera == &externalCamera;
        const bool branchRelocated = noCameraPath && ownCameraBranch && externalCameraPath;
        const bool returnAbi = noCameraPath && ownCameraBranch && externalCameraPath;
        const bool nonvolatile = noCameraAbi && ownedAbi && externalAbi;
        const std::uint32_t callbacksBeforeDetach = g_callbackCount.load(std::memory_order_relaxed);

        const bool detached = StopThirdPersonAbiProbe();
        bool originalAfterDetach = false;
        if (detached) {
            ProbeVector originalResult{};
            const bool originalAbi = ThirdPersonAbiCallWithSentinels(&ownedPawn, &originalResult);
            originalAfterDetach = originalAbi && SameVector(originalResult, ownedCamera) &&
                g_callbackCount.load(std::memory_order_relaxed) == callbacksBeforeDetach &&
                ownedPawn.camera == &ownedCamera;
        }

        const bool passed = callbackInputs && noCameraPath && ownCameraBranch && externalCameraPath &&
            branchRelocated && nested && cameraUnchanged && returnAbi && nonvolatile && detached &&
            originalAfterDetach;

        std::ostringstream result;
        result << "abi_probe=" << (passed ? "pass" : "fail")
               << " platform=" << kProbePlatform
               << " setup=pass"
               << " callback=" << (callbackInputs ? "pass" : "fail")
               << " null_camera=" << (noCameraPath ? "pass" : "fail")
               << " own_camera_branch=" << (ownCameraBranch ? "pass" : "fail")
               << " external_camera=" << (externalCameraPath ? "pass" : "fail")
               << " branch_relocated=" << (branchRelocated ? "pass" : "fail")
               << " nested=" << (nested ? "pass" : "fail")
               << " return_abi=" << (returnAbi ? "pass" : "fail")
               << ' ' << kPlatformReturnAbiField << '=' << (returnAbi ? "pass" : "fail")
               << " nonvolatile=" << (nonvolatile ? "pass" : "fail")
               << " camera_unchanged=" << (cameraUnchanged ? "pass" : "fail")
               << " detach=" << (detached ? "pass" : "fail")
               << " post_detach_original=" << (originalAfterDetach ? "pass" : "fail");
        return result.str();
    } catch (...) {
        const bool detached = StopThirdPersonAbiProbe();
        std::ostringstream result;
        result << "abi_probe=fail platform=" << kProbePlatform
               << " reason=exception callback=not_run return_abi=not_run"
               << ' ' << kPlatformReturnAbiField << "=not_run"
               << " detach=" << (detached ? "pass" : "fail")
               << " post_detach_original=not_run";
        return result.str();
    }
}

bool StopThirdPersonAbiProbe() noexcept {
    try {
        if (g_hookState.site == nullptr) {
            return g_hookState.hook == nullptr;
        }
        if (!HookControlImportsAvailable()) {
            return false;
        }

        const auto current = polyhook::FindDetour(g_hookState.site);
        if (current == nullptr) {
            g_hookState = {};
            return true;
        }
        if (g_hookState.hook == nullptr) {
            // Only this probe records a site after verifying it was unowned.
            g_hookState.hook = current;
        }
        if (current != g_hookState.hook) {
            return false;
        }
        if (polyhook::IsCallbackRegistered(current, polyhook::CallbackType::Pre, ProbePre)) {
            (void)polyhook::RemoveCallback(current, polyhook::CallbackType::Pre, ProbePre);
            if (polyhook::IsCallbackRegistered(current, polyhook::CallbackType::Pre, ProbePre)) {
                return false;
            }
        }
        (void)polyhook::UnhookDetour(g_hookState.site);
        if (polyhook::FindDetour(g_hookState.site) != nullptr) {
            return false;
        }
        g_hookState = {};
        return true;
    } catch (...) {
        return false;
    }
}
