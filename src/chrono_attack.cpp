// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "chrono_attack.hpp"

#include <cstdint>
#include <thread>

#include <plugin_export.h>
#include <polyhook/polyhook.hpp>

#include "detour_slot.hpp"
#include "engine.hpp"

namespace funrounds::native::detail {
namespace {

using Attack = bool (*)(std::int32_t weapon, bool post);
using Phase = polyhook::CallbackType;
using Result = polyhook::ResultType;

constexpr auto kAttack = SignatureKey{"FunRounds::TaserPrimaryAttack"};

struct AttackHook {
    void* entry{};
    polyhook::HookHandle hook{};
    Attack callback{};
    std::thread::id gameThread{};
    unsigned active{};
};

AttackHook g_attack;

void* ResolveAttack() {
    if (!EnsureEngine() || !__s2sdk_GetGameConfigOffset || !__s2sdk_GetGameConfigSignature ||
        !__s2sdk_GetGameConfigVTable || !__polyhook_HookDetour || !__polyhook_FindDetour ||
        !__polyhook_AddCallback || !__polyhook_RemoveCallback || !__polyhook_IsCallbackRegistered ||
        !__polyhook_AreCallbacksRegistered || !__polyhook_UnhookDetour || !__polyhook_GetArgumentPointer) {
        return nullptr;
    }
    auto* entry = ProbeSignature(kAttack);
    auto** table = static_cast<void**>(ProbeVTable(VTableKey{"CWeaponTaser"}));
    constexpr auto slot = OffsetKey{"FunRounds::TaserPrimaryAttack"};
    const auto index = ProbeOffset(slot);
    if (!entry || !table || index < 0) return nullptr;
    if (table[index] != entry) {
        InvalidGamedata(slot.name());
        return nullptr;
    }
    return entry;
}

Result TaserAttack(polyhook::HookHandle, polyhook::ParametersHandle arguments, std::int32_t,
                   polyhook::ReturnHandle, Phase phase) noexcept {
    if (!g_attack.callback || g_attack.gameThread != std::this_thread::get_id()) return Result::Ignored;
    struct ActiveCall {
        ActiveCall() { ++g_attack.active; }
        ~ActiveCall() { --g_attack.active; }
    } active;
    try {
        auto* weapon = polyhook::GetArgument<void*>(arguments, 0);
        const auto handle = HandleFromEntity(weapon);
        if (!weapon || handle == -1 || EntityFromHandle(handle) != weapon) return Result::Ignored;
        const bool allow = g_attack.callback(handle, phase == Phase::Post);
        return phase == Phase::Pre && !allow ? Result::Supercede : Result::Ignored;
    } catch (...) {
        return phase == Phase::Pre ? Result::Supercede : Result::Ignored;
    }
}

bool Attach(Attack callback) {
    if (!callback) return false;
    if (g_attack.callback) return g_attack.callback == callback;
    auto* const entry = g_attack.entry ? g_attack.entry : ResolveAttack();
    DetourSlot slot(g_attack.entry, g_attack.hook);
    using T = polyhook::DataType;
    if (!slot.Acquire(entry, [&] { return polyhook::HookDetour(entry, T::Void, {T::Pointer}, -1, kAttack.name().data()); })) return false;
    g_attack.gameThread = std::this_thread::get_id();
    if (!slot.Add(Phase::Post, TaserAttack) || !slot.Add(Phase::Pre, TaserAttack)) {
        (void)ShutdownChronoAttack();
        return false;
    }
    g_attack.callback = callback;
    return true;
}

} // namespace

bool ShutdownChronoAttack() noexcept {
    try {
        if (g_attack.active) return false;
        g_attack.callback = nullptr;
        DetourSlot slot(g_attack.entry, g_attack.hook);
        if (!slot.Remove(Phase::Pre, TaserAttack) || !slot.Remove(Phase::Post, TaserAttack) || !slot.Release()) return false;
        if (!g_attack.hook) g_attack = {};
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace funrounds::native::detail

extern "C" PLUGIN_API bool HasChronoAttackSupport() noexcept {
    try {
        return funrounds::native::detail::ResolveAttack() != nullptr;
    } catch (...) {
        return false;
    }
}

extern "C" PLUGIN_API bool SetChronoAttackCallback(bool enabled, funrounds::native::detail::Attack callback) noexcept {
    try {
        return enabled ? funrounds::native::detail::Attach(callback) : funrounds::native::detail::ShutdownChronoAttack();
    } catch (...) {
        return false;
    }
}
