// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "recoil_impulse.hpp"

#include <cstring>
#include <limits>
#include <span>

#include <entity2/entityinstance.h>
#include <plugin_export.h>
#include <polyhook/polyhook.hpp>

#include "detour_slot.hpp"
#include "engine.hpp"
#include "entity_handle.hpp"
#include "recoil_impulse_policy.hpp"

namespace funrounds::native::detail {
namespace {

using Factor = float (*)(std::int32_t slot, std::int32_t pawn, std::int32_t controller);
using Phase = polyhook::CallbackType;
using Result = polyhook::ResultType;

constexpr auto kWeapon = SignatureKey{"FunRounds::RecoilImpulse::Weapon"};
constexpr auto kImpulse = SignatureKey{"FunRounds::RecoilImpulse::Apply"};
constexpr auto kCaller = SignatureKey{"FunRounds::RecoilImpulse::Callsite"};
constexpr auto kCallerOffset = OffsetKey{"FunRounds::RecoilImpulse::Callsite"};
constexpr auto kCallOffset = OffsetKey{"FunRounds::RecoilImpulse::CallOpcode"};

struct Metadata {
    void* weapon{};
    void* impulse{};
    SchemaField aimService{};
    SchemaField controller{};
    SchemaField subclassId{};
    SchemaField weaponType{};
    FirearmTypes firearms;
};

struct Hook {
    void* entry{};
    polyhook::HookHandle handle{};
    bool attempted{};
    DetourSlot slot() { return {entry, handle, &attempted}; }
};

Metadata g_metadata;
Hook g_weapon, g_impulse;
Factor g_owner{};
Factor g_factor{};

// The control interface is called on the Rust/game thread. No worker reads the policy
// or resolved engine entities, even if the detours are entered there.
thread_local bool g_game_thread{};
thread_local ShotScope g_shot;

bool RequiredImports() {
    return __s2sdk_GetGameConfigSignature && __s2sdk_GetGameConfigOffset &&
           __s2sdk_GetEntityOwner && __s2sdk_GetMaxClients &&
           __s2sdk_IsEntityPlayerPawn && __s2sdk_IsEntityPlayerController &&
           __s2sdk_EntHandleToEntPointer && __s2sdk_EntPointerToEntHandle &&
           __polyhook_HookDetour && __polyhook_FindDetour && __polyhook_AddCallback &&
           __polyhook_IsCallbackRegistered && __polyhook_RemoveCallback &&
           __polyhook_AreCallbacksRegistered && __polyhook_UnhookDetour &&
           __polyhook_GetArgumentPointer && __polyhook_GetArgumentFloat && __polyhook_SetArgumentFloat;
}

bool Resolve() {
    if (g_metadata.impulse) return true;
    if (!RequiredImports() || !EnsureEngine()) return false;
    Metadata next{};
    next.weapon = ProbeSignature(kWeapon);
    next.impulse = ProbeSignature(kImpulse);
    const auto* caller = static_cast<const std::byte*>(ProbeSignature(kCaller));
    const auto delta = ProbeOffset(kCallerOffset);
    const auto op = ProbeOffset(kCallOffset);
    if (!next.weapon || !next.impulse || !caller || delta < 0 || op < 0) return false;
    // kCaller spans the verified argument loads and complete direct call on both platforms.
    if (reinterpret_cast<std::uintptr_t>(caller) != reinterpret_cast<std::uintptr_t>(next.weapon) + static_cast<std::uintptr_t>(delta) ||
        op > 21 || caller[op] != std::byte{0xE8}) {
        InvalidGamedata(kCaller.name());
        return false;
    }
    std::int32_t relative{};
    std::memcpy(&relative, caller + op + 1, sizeof(relative));
    const auto target = reinterpret_cast<std::intptr_t>(caller + op + 5) + relative;
    if (target != reinterpret_cast<std::intptr_t>(next.impulse)) {
        InvalidGamedata(kCaller.name());
        return false;
    }
    if (!ResolveSchemaField("CCSPlayerPawn", "m_pAimPunchServices", sizeof(void*), next.aimService) ||
        !ResolveSchemaField("CBasePlayerPawn", "m_hController", sizeof(std::uint32_t), next.controller)) {
        return false;
    }
    // The shot wrapper also serves Zeus. Classify the actual weapon by VData.
    if (!ResolveSchemaField("CBaseEntity", "m_nSubclassID", sizeof(std::uint32_t), next.subclassId) ||
        !ResolveSchemaField("CCSWeaponBaseVData", "m_WeaponType", sizeof(std::uint32_t), next.weaponType)) return false;
    auto* scope = ServerSchema();
    const auto* types = scope ? scope->FindDeclaredEnum("CSWeaponType").Get() : nullptr;
    if (!types || types->m_nSize != sizeof(std::uint32_t) || !types->m_pEnumerators) {
        MissingSchema("CSWeaponType", {});
        return false;
    }
    const auto entries = std::span(types->m_pEnumerators, types->m_nEnumeratorCount);
    for (std::size_t i = 0; i < FirearmTypes::names.size(); ++i) {
        const auto name = std::string_view{FirearmTypes::names[i]};
        const auto entry = std::ranges::find_if(entries, [name](const auto& value) {
            return value.m_pszName && name == value.m_pszName;
        });
        if (entry == entries.end() || entry->m_nValue < 0 ||
            static_cast<std::uint64_t>(entry->m_nValue) > std::numeric_limits<std::uint32_t>::max()) {
            MissingSchema("CSWeaponType", name);
            return false;
        }
        next.firearms.values[i] = static_cast<std::uint32_t>(entry->m_nValue);
    }
    g_metadata = next;
    return true;
}

template <typename T>
T ReadField(const void* instance, SchemaField field) {
    T value;
    std::memcpy(&value, static_cast<const std::byte*>(instance) + field.offset, sizeof(value));
    return value;
}

Result WeaponScope(polyhook::HookHandle, polyhook::ParametersHandle args, std::int32_t,
                   polyhook::ReturnHandle, Phase phase) noexcept {
    if (!g_game_thread) return Result::Ignored;
    if (phase == Phase::Post) {
        g_shot.leave();
    } else if (++g_shot.depth == 1 && g_factor) {
        try {
            g_shot.weapon = HandleFromEntity(polyhook::GetArgument<void*>(args, 0));
            g_shot.time = polyhook::GetArgument<void*>(args, 2);
        } catch (...) {
            g_shot.weapon = -1;
        }
    }
    return Result::Ignored;
}

Result ApplyImpulse(polyhook::HookHandle, polyhook::ParametersHandle args, std::int32_t,
                    polyhook::ReturnHandle, Phase) noexcept {
    if (!g_game_thread || !g_factor || g_shot.depth != 1) return Result::Ignored;
    try {
        if (!g_shot.matches(polyhook::GetArgument<void*>(args, 1))) return Result::Ignored;
        const auto weapon = g_shot.weapon;
        const auto* entity = static_cast<const std::byte*>(EntityFromHandle(weapon));
        if (!entity) return Result::Ignored;
        // CBaseEntity's subclass ID is followed by its packed VData pointer.
        const auto vdata = ReadField<const void*>(entity + sizeof(std::uint32_t), g_metadata.subclassId);
        if (!vdata || !g_metadata.firearms.contains(ReadField<std::uint32_t>(vdata, g_metadata.weaponType))) return Result::Ignored;
        const auto pawn = s2sdk::GetEntityOwner(weapon);
        auto* pawnEntity = EntityFromHandle(pawn);
        if (!pawnEntity || !IsPlayerPawn(pawn) ||
            ReadField<void*>(pawnEntity, g_metadata.aimService) != polyhook::GetArgument<void*>(args, 0)) return Result::Ignored;
        const auto controller = ToApiHandle(ReadField<std::uint32_t>(pawnEntity, g_metadata.controller));
        auto* controllerEntity = EntityFromHandle(controller);
        if (!controllerEntity || !IsPlayerController(controller)) return Result::Ignored;
        const auto controllerRef = static_cast<CEntityInstance*>(controllerEntity)->GetRefEHandle();
        const auto slot = controllerRef.GetEntryIndex() - 1;
        if (slot < 0 || slot >= s2sdk::GetMaxClients() || controllerRef.ToInt() != ToNativeHandle(controller)) return Result::Ignored;

        const auto magnitude = ScaleImpulse(polyhook::GetArgumentFloat(args, 3), g_factor(slot, pawn, controller));
        if (!magnitude) return Result::Ignored;
        polyhook::SetArgumentFloat(args, 3, *magnitude);
        return Result::Handled;
    } catch (...) {
        return Result::Ignored;
    }
}

bool Attach(Factor factor) {
    if (!factor || (g_owner && g_owner != factor)) return false;
    if (g_factor) return g_game_thread;
    if (!Resolve()) return false;
    g_owner = factor;
    using T = polyhook::DataType;
    auto weapon = g_weapon.slot();
    auto impulse = g_impulse.slot();
    // Weapon: Win64 RCX/EDX/R8; SysV RDI/ESI/RDX.
    // Apply: Win64 RCX/RDX/XMM2/XMM3; SysV RDI/RSI/XMM0/XMM1.
    // Keep the tested detour return types; neither callback modifies the return.
    // Do not join an untyped or otherwise unknown consumer's pre-existing detour.
    if (!weapon.Acquire(g_metadata.weapon, [] {
            return polyhook::HookDetour(g_metadata.weapon, T::Int64, {T::Pointer, T::UInt32, T::Pointer}, -1, kWeapon.name().data());
        }) ||
        !impulse.Acquire(g_metadata.impulse, [] {
            return polyhook::HookDetour(g_metadata.impulse, T::Int64, {T::Pointer, T::Pointer, T::Float, T::Float}, -1, kImpulse.name().data());
        }) ||
        !weapon.Add(Phase::Pre, WeaponScope) || !weapon.Add(Phase::Post, WeaponScope) ||
        !impulse.Add(Phase::Pre, ApplyImpulse)) {
        (void)ShutdownRecoilImpulse();
        return false;
    }
    g_game_thread = true;
    g_factor = factor;
    return true;
}

} // namespace

bool ShutdownRecoilImpulse() noexcept {
    try {
        g_factor = nullptr;
        if (g_shot.depth) return false;
        g_game_thread = false;
        auto impulse = g_impulse.slot();
        auto weapon = g_weapon.slot();
        const bool impulseClean = impulse.Remove(Phase::Pre, ApplyImpulse) && impulse.Release();
        const bool weaponClean = weapon.Remove(Phase::Pre, WeaponScope) &&
            weapon.Remove(Phase::Post, WeaponScope) && weapon.Release();
        if (!impulseClean || !weaponClean) return false;
        g_owner = nullptr;
        return true;
    } catch (...) {
        return false;
    }
}

void ResetRecoilImpulseMetadata() noexcept { g_metadata = {}; }

} // namespace funrounds::native::detail

extern "C" PLUGIN_API bool HasRecoilImpulseSupport() noexcept {
    try {
        return funrounds::native::detail::Resolve();
    } catch (...) {
        return false;
    }
}

extern "C" PLUGIN_API bool SetRecoilImpulseCallback(bool enabled, funrounds::native::detail::Factor factor) noexcept {
    try {
        using namespace funrounds::native::detail;
        if (enabled) return Attach(factor);
        // A failed competing registration may ask to roll back; it owns nothing.
        return (g_owner && g_owner != factor) || ShutdownRecoilImpulse();
    } catch (...) {
        return false;
    }
}
