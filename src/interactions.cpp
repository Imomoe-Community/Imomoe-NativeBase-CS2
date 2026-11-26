// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "interactions.hpp"

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>

#include <polyhook/polyhook.hpp>
#include <plugin_export.h>
#include <s2sdk/s2sdk.hpp>

#include "button_state.hpp"
#include "chicken_follow_policy.hpp"
#include "detour_slot.hpp"
#include "engine.hpp"
#include "entity_handle.hpp"

namespace funrounds::native::detail {
namespace {

constexpr auto kUseTargetSignature = SignatureKey{"OuterUseTargetSelector"};
constexpr auto kUseTargetOffset = OffsetKey{"UseServicesLastTargetHandle"};
constexpr auto kHealthshotCanUseSignature = SignatureKey{"HealthshotCanUse"};
constexpr auto kHealthshotEffectSignature = SignatureKey{"HealthshotSuccessEffect"};
constexpr auto kChickenFollowUseSignature = SignatureKey{"CChickenChickenUse"};
constexpr std::string_view kUseTargetHookName = "FunRounds::PlayerUseTargetSelector";
constexpr std::string_view kHealthshotCanUseHookName = "FunRounds::HealthshotCanUse";
constexpr std::string_view kHealthshotEffectHookName = "FunRounds::HealthshotSuccessEffect";
constexpr std::string_view kChickenFollowUseHookName = "FunRounds::ChickenFollowUse";
constexpr std::int32_t kInvalidHandle = -1;
constexpr std::uint64_t kInUseButton = 0x20;
constexpr std::uint32_t kInvalidEntityBits = std::numeric_limits<std::uint32_t>::max();
constexpr std::array<std::string_view, 3> kPawnClasses{
    "CBasePlayerPawn",
    "CCSPlayerPawnBase",
    "CCSPlayerPawn",
};

struct UseTargetState {
    void* address{};
    polyhook::HookHandle hook{};
    UseTargetCallback owner{};
    UseTargetCallback callback{};
    std::array<std::size_t, kPawnClasses.size()> schemaClassIndices{};
    std::array<std::int32_t, kPawnClasses.size()> useServicesOffsets{};
    std::size_t schemaCount{};
    std::int32_t pawnControllerOffset{-1};
    std::int32_t committedTargetOffset{-1};
    bool metadataReady{};
    bool callbackInstalled{};
};

struct HealthshotState {
    void* canUseAddress{};
    polyhook::HookHandle canUseHook{};
    void* effectAddress{};
    polyhook::HookHandle effectHook{};
    HealthshotCanUseCallback canUseOwner{};
    HealthshotCanUseCallback canUseCallback{};
    HealthshotEffectCallback effectOwner{};
    HealthshotEffectCallback effectCallback{};
    bool canUseCallbackInstalled{};
    bool effectCallbackInstalled{};
};

struct ChickenFollowUseState {
    void* address{};
    polyhook::HookHandle hook{};
    bool callbackInstalled{};
    ChickenFollowPolicy policy{};
};

UseTargetState g_useTarget;
HealthshotState g_healthshot;
ChickenFollowUseState g_chickenFollowUse;
#if defined(FUNROUNDS_NATIVE_LIFECYCLE_PROBE)
std::atomic<std::int32_t> g_chickenFollowProbeCallbacks{};
std::atomic<std::int32_t> g_chickenFollowProbeForeignCalls{};
std::atomic<std::int32_t> g_chickenFollowProbeTargetCalls{};
#endif

bool UseTargetEmpty() noexcept {
    return !g_useTarget.address && !g_useTarget.hook && !g_useTarget.owner && !g_useTarget.callback &&
           !g_useTarget.callbackInstalled;
}

bool HealthshotEmpty() noexcept {
    return !g_healthshot.canUseAddress && !g_healthshot.canUseHook && !g_healthshot.effectAddress &&
           !g_healthshot.effectHook && !g_healthshot.canUseOwner && !g_healthshot.canUseCallback &&
           !g_healthshot.effectOwner && !g_healthshot.effectCallback && !g_healthshot.canUseCallbackInstalled &&
           !g_healthshot.effectCallbackInstalled;
}

bool ChickenFollowUseEmpty() noexcept {
    return !g_chickenFollowUse.address && !g_chickenFollowUse.hook &&
           !g_chickenFollowUse.callbackInstalled && !g_chickenFollowUse.policy.WantsHook();
}

polyhook::ResultType UseTargetPost(polyhook::HookHandle,
                                   polyhook::ParametersHandle,
                                   std::int32_t,
                                   polyhook::ReturnHandle,
                                   polyhook::CallbackType) noexcept;
polyhook::ResultType HealthshotCanUsePre(polyhook::HookHandle,
                                         polyhook::ParametersHandle,
                                         std::int32_t,
                                         polyhook::ReturnHandle,
                                         polyhook::CallbackType) noexcept;
polyhook::ResultType HealthshotEffectPost(polyhook::HookHandle,
                                          polyhook::ParametersHandle,
                                          std::int32_t,
                                          polyhook::ReturnHandle,
                                          polyhook::CallbackType) noexcept;
polyhook::ResultType ChickenFollowUsePre(polyhook::HookHandle,
                                         polyhook::ParametersHandle,
                                         std::int32_t,
                                         polyhook::ReturnHandle,
                                         polyhook::CallbackType) noexcept;

bool HookImportsAvailable() noexcept {
    return __polyhook_HookDetour && __polyhook_FindDetour && __polyhook_AddCallback &&
           __polyhook_IsCallbackRegistered && __polyhook_AreCallbacksRegistered &&
           __polyhook_RemoveCallback && __polyhook_UnhookDetour &&
           __polyhook_GetArgumentPointer && __polyhook_GetArgumentFloat && __polyhook_GetArgumentInt32 &&
           __polyhook_GetReturnPointer && __polyhook_SetReturnBool && __s2sdk_GetGameConfigSignature &&
           __s2sdk_GetGameConfigOffset && __s2sdk_GetSchemaOffset && __s2sdk_GetEntSchema2 && __s2sdk_GetEntSchemaEnt2 &&
           __s2sdk_GetMaxClients && __s2sdk_IsClientInGame && __s2sdk_GetClientPawn &&
           __s2sdk_PlayerSlotToEntHandle &&
           __s2sdk_GetClientButtons && __s2sdk_GetClientWeapons && __s2sdk_GetEntityClassname &&
           __s2sdk_EntHandleToEntPointer && __s2sdk_EntPointerToEntHandle && __s2sdk_IsValidEntHandle &&
           __s2sdk_IsEntityPlayerPawn;
}

bool ChickenFollowImportsAvailable() noexcept {
    return __s2sdk_GetGameConfigSignature &&
           __polyhook_HookDetour && __polyhook_FindDetour && __polyhook_AddCallback &&
           __polyhook_IsCallbackRegistered && __polyhook_AreCallbacksRegistered &&
           __polyhook_RemoveCallback && __polyhook_UnhookDetour && __polyhook_GetArgumentPointer &&
           __s2sdk_GetEntityClassname && __s2sdk_EntHandleToEntPointer &&
           __s2sdk_EntPointerToEntHandle && __s2sdk_IsValidEntHandle;
}

bool ResolvePawnControllerSchema(std::int32_t& offset) noexcept {
    offset = -1;
    if (!EnsureEngine() || !__s2sdk_GetSchemaOffset) {
        return false;
    }
    offset = ProbeSchemaOffset("CBasePlayerPawn", "m_hController");
    return offset >= 0;
}

bool ResolveUseTargetMetadata(std::int32_t& pawnControllerOffset) noexcept {
    if (g_useTarget.metadataReady) {
        pawnControllerOffset = g_useTarget.pawnControllerOffset;
        return true;
    }
    if (!EnsureEngine() || !__s2sdk_GetGameConfigOffset || !__s2sdk_GetSchemaOffset) {
        return false;
    }
    if (g_useTarget.committedTargetOffset < 0) {
        g_useTarget.committedTargetOffset = ProbeOffset(kUseTargetOffset);
    }
    if (g_useTarget.committedTargetOffset < 0) {
        return false;
    }
    if (!ResolvePawnControllerSchema(pawnControllerOffset)) {
        return false;
    }
    std::array<std::int32_t, kPawnClasses.size()> serviceOffsets{};
    std::array<std::size_t, kPawnClasses.size()> serviceClassIndices{};
    std::size_t serviceCount{};
    for (std::size_t index = 0; index < kPawnClasses.size(); ++index) {
        const auto className = kPawnClasses[index];
        const auto offset = ProbeSchemaOffset(className, "m_pUseServices", false);
        if (offset >= 0) {
            serviceOffsets[index] = offset;
            serviceClassIndices[serviceCount++] = index;
        }
    }
    if (serviceCount == 0) {
        for (const auto className : kPawnClasses) MissingSchema(className, "m_pUseServices");
        return false;
    }
    g_useTarget.pawnControllerOffset = pawnControllerOffset;
    g_useTarget.schemaClassIndices = serviceClassIndices;
    g_useTarget.useServicesOffsets = serviceOffsets;
    g_useTarget.schemaCount = serviceCount;
    g_useTarget.metadataReady = true;
    return true;
}

void* Signature(SignatureKey name) noexcept {
    if (!EnsureEngine() || !__s2sdk_GetGameConfigSignature) {
        return nullptr;
    }
    return ProbeSignature(name);
}

bool SlotOwnsPawn(std::int32_t slot, void* pawn, std::int32_t pawnControllerOffset) noexcept {
    if (!pawn || !s2sdk::IsClientInGame(slot) || s2sdk::GetClientPawn(slot) != pawn) {
        return false;
    }
    const auto controller = s2sdk::PlayerSlotToEntHandle(slot);
    if (!s2sdk::IsValidEntHandle(controller) || pawnControllerOffset < 0) {
        return false;
    }
    // A stale BOT slot may retain m_hPawn after takeover. The pawn's reverse link is authoritative.
    std::uint32_t controllerBits{};
    std::memcpy(&controllerBits, static_cast<const std::byte*>(pawn) + pawnControllerOffset, sizeof(controllerBits));
    return ToApiHandle(controllerBits) == controller;
}

bool ResolveUseServiceOwner(void* services, std::int32_t& pawnHandle, std::int32_t& slot) noexcept {
    std::int32_t pawnControllerOffset{-1};
    if (!services || !ResolveUseTargetMetadata(pawnControllerOffset)) {
        return false;
    }
    std::size_t matches = 0;
    const auto maximum = s2sdk::GetMaxClients();
    if (maximum <= 0) {
        return false;
    }
    for (std::int32_t candidateSlot = 0; candidateSlot < maximum; ++candidateSlot) {
        if (!s2sdk::IsClientInGame(candidateSlot)) {
            continue;
        }
        void* const pawn = s2sdk::GetClientPawn(candidateSlot);
        const auto candidateHandle = HandleFromEntity(pawn);
        if (!pawn || candidateHandle == kInvalidHandle || !IsPlayerPawn(candidateHandle) ||
            EntityFromHandle(candidateHandle) != pawn || !SlotOwnsPawn(candidateSlot, pawn, pawnControllerOffset)) {
            continue;
        }
        bool serviceMatches = false;
        for (std::size_t index = 0; index < g_useTarget.schemaCount; ++index) {
            void* serviceAddress{};
            std::memcpy(&serviceAddress, static_cast<const std::byte*>(pawn) +
                        g_useTarget.useServicesOffsets[g_useTarget.schemaClassIndices[index]], sizeof(serviceAddress));
            if (serviceAddress == services) {
                serviceMatches = true;
                break;
            }
        }
        if (serviceMatches) {
            ++matches;
            pawnHandle = candidateHandle;
            slot = candidateSlot;
        }
    }
    return matches == 1 && SlotOwnsPawn(slot, EntityFromHandle(pawnHandle), pawnControllerOffset);
}

bool UseButtonDown(std::int32_t slot) noexcept {
    const auto currentButtons = s2sdk::GetClientButtons(
        slot, static_cast<std::int32_t>(ButtonStateGroup::CurrentDown));
    return IsButtonCurrentlyDown(currentButtons, kInUseButton);
}

std::int32_t TargetHandle(void* target) noexcept {
    if (!target) {
        return kInvalidHandle;
    }
    const auto handle = HandleFromEntity(target);
    if (handle == kInvalidHandle || !s2sdk::IsValidEntHandle(handle) || EntityFromHandle(handle) != target) {
        return kInvalidHandle;
    }
    return handle;
}

bool ResolveHealthshotOwner(void* weapon, void* pawn, std::int32_t& pawnHandle,
                            std::int32_t& weaponHandle) noexcept {
    if (!weapon || !pawn) {
        return false;
    }
    const auto resolvedPawn = HandleFromEntity(pawn);
    if (resolvedPawn == kInvalidHandle || !IsPlayerPawn(resolvedPawn) || EntityFromHandle(resolvedPawn) != pawn) {
        return false;
    }
    std::int32_t pawnControllerOffset{-1};
    if (!ResolvePawnControllerSchema(pawnControllerOffset)) {
        return false;
    }
    std::size_t ownerMatches = 0;
    std::int32_t matchedSlot = -1;
    std::int32_t matchedWeapon = kInvalidHandle;
    const auto maximum = s2sdk::GetMaxClients();
    if (maximum <= 0) {
        return false;
    }
    for (std::int32_t candidateSlot = 0; candidateSlot < maximum; ++candidateSlot) {
        if (!SlotOwnsPawn(candidateSlot, pawn, pawnControllerOffset)) {
            continue;
        }
        std::size_t weaponMatches = 0;
        for (const auto handle : s2sdk::GetClientWeapons(candidateSlot)) {
            if (handle == kInvalidHandle || !s2sdk::IsValidEntHandle(handle) ||
                s2sdk::GetEntityClassname(handle) != "weapon_healthshot") {
                continue;
            }
            if (EntityFromHandle(handle) == weapon) {
                ++weaponMatches;
                matchedWeapon = handle;
            }
        }
        if (weaponMatches == 1) {
            ++ownerMatches;
            matchedSlot = candidateSlot;
        }
    }
    if (ownerMatches != 1 || matchedSlot < 0 || matchedWeapon == kInvalidHandle ||
        !SlotOwnsPawn(matchedSlot, pawn, pawnControllerOffset) || HandleFromEntity(pawn) != resolvedPawn) {
        return false;
    }
    pawnHandle = resolvedPawn;
    weaponHandle = matchedWeapon;
    return true;
}

bool RemoveUseTargetHook() noexcept {
    g_useTarget.callback = nullptr;
    try {
        DetourSlot slot(g_useTarget.address, g_useTarget.hook);
        if (!slot.Remove(polyhook::CallbackType::Post, UseTargetPost)) return false;
        g_useTarget.callbackInstalled = false;
        if (!slot.Release()) return false;
        g_useTarget.owner = nullptr;
        if (!g_useTarget.hook) g_useTarget = {};
        return true;
    } catch (...) {
        return false;
    }
}

bool RemoveHealthshotHooks() noexcept {
    g_healthshot.canUseCallback = nullptr;
    g_healthshot.effectCallback = nullptr;
    auto remove = [](polyhook::HookHandle& hook, void*& address, polyhook::CallbackType phase,
                     auto callback, bool& installed) noexcept {
        try {
            DetourSlot slot(address, hook);
            if (!slot.Remove(phase, callback)) return false;
            installed = false;
            return slot.Release();
        } catch (...) {
            return false;
        }
    };
    const bool canUseClean = remove(g_healthshot.canUseHook, g_healthshot.canUseAddress,
                                    polyhook::CallbackType::Pre, HealthshotCanUsePre,
                                    g_healthshot.canUseCallbackInstalled);
    const bool effectClean = remove(g_healthshot.effectHook, g_healthshot.effectAddress,
                                    polyhook::CallbackType::Post, HealthshotEffectPost,
                                    g_healthshot.effectCallbackInstalled);
    if (!canUseClean || !effectClean) return false;
    g_healthshot.canUseOwner = nullptr;
    g_healthshot.effectOwner = nullptr;
    if (!g_healthshot.canUseHook && !g_healthshot.effectHook) g_healthshot = {};
    return true;
}

std::int32_t CurrentChickenUseHandle(void* entity) {
    if (!entity) {
        return kInvalidHandle;
    }
    const auto handle = s2sdk::EntPointerToEntHandle(entity);
    return handle != kInvalidHandle && s2sdk::IsValidEntHandle(handle) &&
           s2sdk::EntHandleToEntPointer(handle) == entity && s2sdk::GetEntityClassname(handle) == "chicken"
        ? handle : kInvalidHandle;
}

bool RemoveChickenFollowUseHook() noexcept {
    try {
        DetourSlot slot(g_chickenFollowUse.address, g_chickenFollowUse.hook);
        if (!slot.Remove(polyhook::CallbackType::Pre, ChickenFollowUsePre)) return false;
        g_chickenFollowUse.callbackInstalled = false;
        if (!slot.Release()) return false;
        if (!g_chickenFollowUse.hook) g_chickenFollowUse = {};
        return true;
    } catch (...) {
        return false;
    }
}

bool InstallChickenFollowUseHookAt(void* address) {
    // Preserve the full CChickenChickenUse ABI: (this, const EntityUseInput_t*) -> void.
    const std::array arguments{polyhook::DataType::Pointer, polyhook::DataType::Pointer};
    DetourSlot slot(g_chickenFollowUse.address, g_chickenFollowUse.hook);
    try {
        if (!slot.Acquire(address, [&] {
                return polyhook::HookDetour(address, polyhook::DataType::Void,
                    plg::vector<polyhook::DataType>(arguments.begin(), arguments.end()), -1,
                    plg::string(kChickenFollowUseHookName));
            }, true)) return false;
        g_chickenFollowUse.callbackInstalled = slot.Add(polyhook::CallbackType::Pre, ChickenFollowUsePre);
        if (!g_chickenFollowUse.callbackInstalled) {
            (void)RemoveChickenFollowUseHook();
            return false;
        }
        return true;
    } catch (...) {
        (void)RemoveChickenFollowUseHook();
        return false;
    }
}

bool EnableChickenFollowUseHook() {
    if (g_chickenFollowUse.policy.WantsHook()) {
        return g_chickenFollowUse.callbackInstalled;
    }
    return (ChickenFollowUseEmpty() || RemoveChickenFollowUseHook()) &&
           InstallChickenFollowUseHookAt(Signature(kChickenFollowUseSignature));
}

bool ReleaseChickenFollowUseHook() noexcept {
    return g_chickenFollowUse.policy.WantsHook() || RemoveChickenFollowUseHook();
}

bool InstallUseTargetHook(UseTargetCallback callback) {
    std::int32_t pawnControllerOffset{-1};
    if (!callback || !HookImportsAvailable() || !ResolveUseTargetMetadata(pawnControllerOffset) ||
        (g_useTarget.owner && g_useTarget.owner != callback)) return false;
    const auto address = Signature(kUseTargetSignature);
    const std::array arguments{polyhook::DataType::Pointer, polyhook::DataType::Float};
    DetourSlot slot(g_useTarget.address, g_useTarget.hook);
    try {
        if (!slot.Acquire(address, [&] {
                return polyhook::HookDetour(address, polyhook::DataType::Pointer,
                    plg::vector<polyhook::DataType>(arguments.begin(), arguments.end()), -1,
                    plg::string(kUseTargetHookName));
            }, true)) return false;
        g_useTarget.callbackInstalled = slot.Add(polyhook::CallbackType::Post, UseTargetPost);
        if (!g_useTarget.callbackInstalled) {
            (void)RemoveUseTargetHook();
            return false;
        }
        g_useTarget.owner = callback;
        g_useTarget.callback = callback;
        return true;
    } catch (...) {
        (void)RemoveUseTargetHook();
        return false;
    }
}

bool InstallHealthshotHooks(HealthshotCanUseCallback canUse, HealthshotEffectCallback effect) {
    if (!canUse || !effect || !HookImportsAvailable() ||
        (g_healthshot.canUseOwner && g_healthshot.canUseOwner != canUse) ||
        (g_healthshot.effectOwner && g_healthshot.effectOwner != effect)) return false;
    std::int32_t pawnControllerOffset{-1};
    if (!ResolvePawnControllerSchema(pawnControllerOffset)) return false;
    const auto canUseAddress = Signature(kHealthshotCanUseSignature);
    const auto effectAddress = Signature(kHealthshotEffectSignature);
    if (!canUseAddress || !effectAddress ||
        (g_healthshot.canUseAddress && g_healthshot.canUseAddress != canUseAddress) ||
        (g_healthshot.effectAddress && g_healthshot.effectAddress != effectAddress)) return false;
    const std::array canUseArguments{polyhook::DataType::Pointer, polyhook::DataType::Pointer};
#if defined(_WIN32)
    const std::array effectArguments{polyhook::DataType::Pointer, polyhook::DataType::Pointer,
                                     polyhook::DataType::Pointer, polyhook::DataType::Int32};
#elif defined(__linux__)
    const std::array effectArguments{polyhook::DataType::Pointer, polyhook::DataType::Pointer};
#else
#error "Healthshot interaction hook supports Windows x64 and Linux x64 only"
#endif
    DetourSlot canUseSlot(g_healthshot.canUseAddress, g_healthshot.canUseHook);
    DetourSlot effectSlot(g_healthshot.effectAddress, g_healthshot.effectHook);
    try {
        if (!canUseSlot.Acquire(canUseAddress, [&] {
                return polyhook::HookDetour(canUseAddress, polyhook::DataType::Bool,
                    plg::vector<polyhook::DataType>(canUseArguments.begin(), canUseArguments.end()), -1,
                    plg::string(kHealthshotCanUseHookName));
            }, true)) return false;
        g_healthshot.canUseCallbackInstalled = canUseSlot.Add(polyhook::CallbackType::Pre, HealthshotCanUsePre);
        if (!g_healthshot.canUseCallbackInstalled ||
            !effectSlot.Acquire(effectAddress, [&] {
                return polyhook::HookDetour(effectAddress, polyhook::DataType::Void,
                    plg::vector<polyhook::DataType>(effectArguments.begin(), effectArguments.end()), -1,
                    plg::string(kHealthshotEffectHookName));
            }, true)) {
            (void)RemoveHealthshotHooks();
            return false;
        }
        g_healthshot.effectCallbackInstalled = effectSlot.Add(polyhook::CallbackType::Post, HealthshotEffectPost);
        if (!g_healthshot.effectCallbackInstalled) {
            (void)RemoveHealthshotHooks();
            return false;
        }
        g_healthshot.canUseOwner = canUse;
        g_healthshot.effectOwner = effect;
        g_healthshot.canUseCallback = canUse;
        g_healthshot.effectCallback = effect;
        return true;
    } catch (...) {
        (void)RemoveHealthshotHooks();
        return false;
    }
}

polyhook::ResultType UseTargetPost(polyhook::HookHandle,
                                   polyhook::ParametersHandle parameters,
                                   std::int32_t,
                                   polyhook::ReturnHandle result,
                                   polyhook::CallbackType type) noexcept {
    const auto callback = g_useTarget.callback;
    if (type != polyhook::CallbackType::Post || !callback || !parameters || !result) {
        return polyhook::ResultType::Ignored;
    }
    try {
        auto* const services = polyhook::GetArgument<void*>(parameters, 0);
        std::int32_t pawnHandle = kInvalidHandle;
        std::int32_t slot = -1;
        if (!ResolveUseServiceOwner(services, pawnHandle, slot)) {
            return polyhook::ResultType::Ignored;
        }
        const auto inUse = UseButtonDown(slot);
        if (!inUse) {
            callback(pawnHandle, kInvalidHandle, false);
            return polyhook::ResultType::Ignored;
        }
        auto* const selectedTarget = polyhook::GetReturn<void*>(result);
        const auto selectedHandle = TargetHandle(selectedTarget);
        std::uint32_t committedBits{};
        if (services && g_useTarget.committedTargetOffset >= 0) {
            std::memcpy(&committedBits, static_cast<const std::byte*>(services) + g_useTarget.committedTargetOffset,
                        sizeof(committedBits));
        }
        const auto committedHandle = std::bit_cast<std::int32_t>(committedBits);
        const auto target = selectedHandle != kInvalidHandle && selectedHandle == committedHandle
            ? selectedHandle
            : kInvalidHandle;
        callback(pawnHandle, target, true);
    } catch (...) {
        return polyhook::ResultType::Ignored;
    }
    return polyhook::ResultType::Ignored;
}

polyhook::ResultType HealthshotCanUsePre(polyhook::HookHandle,
                                         polyhook::ParametersHandle parameters,
                                         std::int32_t,
                                         polyhook::ReturnHandle result,
                                         polyhook::CallbackType type) noexcept {
    const auto callback = g_healthshot.canUseCallback;
    if (type != polyhook::CallbackType::Pre || !callback || !parameters || !result) {
        return polyhook::ResultType::Ignored;
    }
    try {
        auto* const weapon = polyhook::GetArgument<void*>(parameters, 0);
        auto* const pawn = polyhook::GetArgument<void*>(parameters, 1);
        std::int32_t pawnHandle = kInvalidHandle;
        std::int32_t weaponHandle = kInvalidHandle;
        if (!ResolveHealthshotOwner(weapon, pawn, pawnHandle, weaponHandle) ||
            callback(pawnHandle, weaponHandle) == 0) {
            return polyhook::ResultType::Ignored;
        }
        polyhook::SetReturn<bool>(result, false);
        return polyhook::ResultType::Supercede;
    } catch (...) {
        return polyhook::ResultType::Ignored;
    }
}

polyhook::ResultType HealthshotEffectPost(polyhook::HookHandle,
                                          polyhook::ParametersHandle parameters,
                                          std::int32_t,
                                          polyhook::ReturnHandle,
                                          polyhook::CallbackType type) noexcept {
    const auto callback = g_healthshot.effectCallback;
    if (type != polyhook::CallbackType::Post || !callback || !parameters) {
        return polyhook::ResultType::Ignored;
    }
    try {
        auto* const weapon = polyhook::GetArgument<void*>(parameters, 0);
        auto* const pawn = polyhook::GetArgument<void*>(parameters, 1);
        std::int32_t pawnHandle = kInvalidHandle;
        std::int32_t weaponHandle = kInvalidHandle;
        if (ResolveHealthshotOwner(weapon, pawn, pawnHandle, weaponHandle)) {
            callback(pawnHandle, weaponHandle);
        }
    } catch (...) {
        return polyhook::ResultType::Ignored;
    }
    return polyhook::ResultType::Ignored;
}

polyhook::ResultType ChickenFollowUsePre(polyhook::HookHandle,
                                         polyhook::ParametersHandle parameters,
                                         std::int32_t,
                                         polyhook::ReturnHandle,
                                         polyhook::CallbackType type) noexcept {
    if (type != polyhook::CallbackType::Pre || !g_chickenFollowUse.policy.WantsHook() || !parameters) {
        return polyhook::ResultType::Ignored;
    }
#if defined(FUNROUNDS_NATIVE_LIFECYCLE_PROBE)
    g_chickenFollowProbeCallbacks.fetch_add(1, std::memory_order_relaxed);
#endif
    try {
        auto* const chicken = polyhook::GetArgument<void*>(parameters, 0);
        if (!g_chickenFollowUse.policy.Blocks(CurrentChickenUseHandle(chicken))) {
            return polyhook::ResultType::Ignored;
        }
        return polyhook::ResultType::Supercede;
    } catch (...) {
        return polyhook::ResultType::Ignored;
    }
}

#if defined(FUNROUNDS_NATIVE_LIFECYCLE_PROBE)
#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
void ChickenFollowSharedProbeTarget(void* first, void* second) {
    if (first) {
        g_chickenFollowProbeTargetCalls.fetch_add(8, std::memory_order_relaxed);
    }
    if (second) {
        g_chickenFollowProbeTargetCalls.fetch_add(16, std::memory_order_relaxed);
    }
    g_chickenFollowProbeTargetCalls.fetch_add(1, std::memory_order_relaxed);
}

void CallChickenFollowSharedProbeTarget() {
    void (*volatile target)(void*, void*) = &ChickenFollowSharedProbeTarget;
    target(nullptr, nullptr);
}

polyhook::ResultType ChickenFollowForeignProbe(polyhook::HookHandle,
                                                polyhook::ParametersHandle,
                                                std::int32_t,
                                                polyhook::ReturnHandle,
                                                polyhook::CallbackType type) {
    if (type == polyhook::CallbackType::Pre) {
        g_chickenFollowProbeForeignCalls.fetch_add(1, std::memory_order_relaxed);
    }
    return polyhook::ResultType::Ignored;
}
#endif

} // namespace

#if defined(FUNROUNDS_NATIVE_LIFECYCLE_PROBE)
bool ProbeChickenFollowSharedDetour() noexcept {
    void* const target = reinterpret_cast<void*>(&ChickenFollowSharedProbeTarget);
    if (!ChickenFollowUseEmpty() || polyhook::FindDetour(target)) {
        return false;
    }
    g_chickenFollowProbeCallbacks.store(0, std::memory_order_relaxed);
    g_chickenFollowProbeForeignCalls.store(0, std::memory_order_relaxed);
    g_chickenFollowProbeTargetCalls.store(0, std::memory_order_relaxed);

    const auto cleanup = [&]() noexcept {
        g_chickenFollowUse.policy = {};
        if (const auto current = polyhook::FindDetour(target)) {
            if (polyhook::IsCallbackRegistered(current, polyhook::CallbackType::Pre,
                                                ChickenFollowForeignProbe)) {
                (void)polyhook::RemoveCallback(current, polyhook::CallbackType::Pre,
                                               ChickenFollowForeignProbe);
            }
        }
        const bool clean = RemoveChickenFollowUseHook();
        const auto current = polyhook::FindDetour(target);
        if (current && !polyhook::AreCallbacksRegistered(current)) {
            (void)polyhook::UnhookDetour(target);
        }
        return clean && polyhook::FindDetour(target) == nullptr;
    };

    try {
        if (!InstallChickenFollowUseHookAt(target)) {
            (void)cleanup();
            return false;
        }
        g_chickenFollowUse.policy.all = true;
        const auto original = polyhook::FindDetour(target);
        if (!original || !polyhook::AddCallback(original, polyhook::CallbackType::Pre,
                                                 ChickenFollowForeignProbe) ||
            !polyhook::IsCallbackRegistered(original, polyhook::CallbackType::Pre,
                                             ChickenFollowForeignProbe)) {
            (void)cleanup();
            return false;
        }

        CallChickenFollowSharedProbeTarget();
        const bool bothRanInitially =
            g_chickenFollowProbeCallbacks.load(std::memory_order_relaxed) == 1 &&
            g_chickenFollowProbeForeignCalls.load(std::memory_order_relaxed) == 1;

        g_chickenFollowUse.policy.all = false;
        const bool detached = RemoveChickenFollowUseHook();
        const auto afterDetach = polyhook::FindDetour(target);
        const bool foreignSurvived = detached && !g_chickenFollowUse.policy.WantsHook() && !g_chickenFollowUse.callbackInstalled &&
            g_chickenFollowUse.address == target && g_chickenFollowUse.hook == original && afterDetach == original &&
            polyhook::IsCallbackRegistered(afterDetach, polyhook::CallbackType::Pre,
                                            ChickenFollowForeignProbe);
        CallChickenFollowSharedProbeTarget();
        const bool foreignStillRuns =
            g_chickenFollowProbeCallbacks.load(std::memory_order_relaxed) == 1 &&
            g_chickenFollowProbeForeignCalls.load(std::memory_order_relaxed) == 2;

        const bool reattached = InstallChickenFollowUseHookAt(target) &&
            InstallChickenFollowUseHookAt(target) &&
            polyhook::FindDetour(target) == original &&
            polyhook::IsCallbackRegistered(original, polyhook::CallbackType::Pre,
                                           ChickenFollowUsePre);
        g_chickenFollowUse.policy.all = reattached;
        CallChickenFollowSharedProbeTarget();
        const bool eachRanOnceAfterReenable = reattached &&
            g_chickenFollowProbeCallbacks.load(std::memory_order_relaxed) == 2 &&
            g_chickenFollowProbeForeignCalls.load(std::memory_order_relaxed) == 3;

        const bool foreignRemoved = polyhook::RemoveCallback(original, polyhook::CallbackType::Pre,
                                                              ChickenFollowForeignProbe) &&
            !polyhook::IsCallbackRegistered(original, polyhook::CallbackType::Pre,
                                            ChickenFollowForeignProbe) &&
            polyhook::IsCallbackRegistered(original, polyhook::CallbackType::Pre,
                                           ChickenFollowUsePre);
        CallChickenFollowSharedProbeTarget();
        const bool onlyOwnStillRuns = foreignRemoved &&
            g_chickenFollowProbeCallbacks.load(std::memory_order_relaxed) == 3 &&
            g_chickenFollowProbeForeignCalls.load(std::memory_order_relaxed) == 3;
        g_chickenFollowUse.policy.all = false;
        const bool released = RemoveChickenFollowUseHook() && ChickenFollowUseEmpty() &&
            polyhook::FindDetour(target) == nullptr;
        CallChickenFollowSharedProbeTarget();
        const bool originalRunsWithoutCallbacks =
            g_chickenFollowProbeTargetCalls.load(std::memory_order_relaxed) == 5 &&
            g_chickenFollowProbeCallbacks.load(std::memory_order_relaxed) == 3 &&
            g_chickenFollowProbeForeignCalls.load(std::memory_order_relaxed) == 3;
        const bool passed = bothRanInitially && foreignSurvived && foreignStillRuns &&
            eachRanOnceAfterReenable && onlyOwnStillRuns && released &&
            originalRunsWithoutCallbacks;
        if (!passed) {
            (void)cleanup();
        }
        return passed;
    } catch (...) {
        (void)cleanup();
        return false;
    }
}
#endif

bool HasUseTargetSupport() noexcept {
    try {
        std::int32_t pawnControllerOffset{-1};
        return HookImportsAvailable() && ResolveUseTargetMetadata(pawnControllerOffset) && Signature(kUseTargetSignature);
    } catch (...) {
        return false;
    }
}

bool HookUseTarget(UseTargetCallback callback) noexcept {
    if (!callback || (g_useTarget.owner && g_useTarget.owner != callback)) {
        return false;
    }
    g_useTarget.owner = callback;
    try {
        return InstallUseTargetHook(callback);
    } catch (...) {
        (void)RemoveUseTargetHook();
        return false;
    }
}

bool UnhookUseTarget(UseTargetCallback callback) noexcept {
    if (!callback) {
        return false;
    }
    if (UseTargetEmpty()) {
        return true;
    }
    if (!g_useTarget.owner && !g_useTarget.callback) return RemoveUseTargetHook();
    if (callback != g_useTarget.owner) {
        return false;
    }
    return RemoveUseTargetHook();
}

bool HasHealthshotSupport() noexcept {
    try {
        std::int32_t pawnControllerOffset{-1};
        return HookImportsAvailable() && ResolvePawnControllerSchema(pawnControllerOffset) &&
               Signature(kHealthshotCanUseSignature) &&
               Signature(kHealthshotEffectSignature);
    } catch (...) {
        return false;
    }
}

bool HookHealthshot(HealthshotCanUseCallback canUse, HealthshotEffectCallback effect) noexcept {
    if (!canUse || !effect ||
        (g_healthshot.canUseOwner && g_healthshot.canUseOwner != canUse) ||
        (g_healthshot.effectOwner && g_healthshot.effectOwner != effect)) {
        return false;
    }
    g_healthshot.canUseOwner = canUse;
    g_healthshot.effectOwner = effect;
    try {
        return InstallHealthshotHooks(canUse, effect);
    } catch (...) {
        (void)RemoveHealthshotHooks();
        return false;
    }
}

bool UnhookHealthshot(HealthshotCanUseCallback canUse, HealthshotEffectCallback effect) noexcept {
    if (!canUse || !effect) {
        return false;
    }
    if (HealthshotEmpty()) {
        return true;
    }
    if (!g_healthshot.canUseOwner && !g_healthshot.effectOwner) return RemoveHealthshotHooks();
    if (canUse != g_healthshot.canUseOwner || effect != g_healthshot.effectOwner) {
        return false;
    }
    return RemoveHealthshotHooks();
}

bool HasChickenFollowBlockSupport() noexcept {
    try {
        return ChickenFollowImportsAvailable() && Signature(kChickenFollowUseSignature);
    } catch (...) {
        return false;
    }
}

bool SetChickenFollowBlocking(bool active) noexcept {
    if (!active) {
        g_chickenFollowUse.policy.all = false;
        return ReleaseChickenFollowUseHook();
    }
    try {
        if (ChickenFollowImportsAvailable() && EnableChickenFollowUseHook()) {
            g_chickenFollowUse.policy.all = true;
            return true;
        }
    } catch (...) {
    }
    (void)ReleaseChickenFollowUseHook();
    return false;
}

bool SetChickenUseBlocked(std::int32_t handle, bool blocked) noexcept {
    try {
        if (!blocked) {
            g_chickenFollowUse.policy.chickens.erase(handle);
            return ReleaseChickenFollowUseHook();
        }
        if (handle != kInvalidHandle && ChickenFollowImportsAvailable() &&
            CurrentChickenUseHandle(s2sdk::EntHandleToEntPointer(handle)) == handle &&
            EnableChickenFollowUseHook()) {
            g_chickenFollowUse.policy.chickens.insert(handle);
            return true;
        }
    } catch (...) {
    }
    (void)ReleaseChickenFollowUseHook();
    return false;
}

bool ShutdownInteractions() noexcept {
    const bool useClean = RemoveUseTargetHook();
    const bool healthshotClean = RemoveHealthshotHooks();
    g_chickenFollowUse.policy = {};
    const bool chickenFollowClean = ReleaseChickenFollowUseHook();
    return useClean && healthshotClean && chickenFollowClean;
}

} // namespace funrounds::native::detail

extern "C" PLUGIN_API bool HasUseTargetSupport() {
    return funrounds::native::detail::HasUseTargetSupport();
}

extern "C" PLUGIN_API bool HookUseTarget(funrounds::native::detail::UseTargetCallback callback) {
    return funrounds::native::detail::HookUseTarget(callback);
}

extern "C" PLUGIN_API bool UnhookUseTarget(funrounds::native::detail::UseTargetCallback callback) {
    return funrounds::native::detail::UnhookUseTarget(callback);
}

extern "C" PLUGIN_API bool HasHealthshotSupport() {
    return funrounds::native::detail::HasHealthshotSupport();
}

extern "C" PLUGIN_API bool HookHealthshot(funrounds::native::detail::HealthshotCanUseCallback canUse,
                                          funrounds::native::detail::HealthshotEffectCallback effect) {
    return funrounds::native::detail::HookHealthshot(canUse, effect);
}

extern "C" PLUGIN_API bool UnhookHealthshot(funrounds::native::detail::HealthshotCanUseCallback canUse,
                                            funrounds::native::detail::HealthshotEffectCallback effect) {
    return funrounds::native::detail::UnhookHealthshot(canUse, effect);
}

extern "C" PLUGIN_API bool HasChickenFollowBlockSupport() {
    return funrounds::native::detail::HasChickenFollowBlockSupport();
}

extern "C" PLUGIN_API bool SetChickenFollowBlocking(bool active) {
    return funrounds::native::detail::SetChickenFollowBlocking(active);
}

extern "C" PLUGIN_API bool SetChickenUseBlocked(std::int32_t handle, bool blocked) {
    return funrounds::native::detail::SetChickenUseBlocked(handle, blocked);
}
