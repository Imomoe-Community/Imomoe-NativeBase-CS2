// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "player_movement_policy.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <thread>

#include <entity2/entityinstance.h>
#include <entity2/entitynetwork.h>
#include <plugin_export.h>
#include <polyhook/polyhook.hpp>
#include <s2sdk/s2sdk.hpp>
#include <tier0/convar.h>

#include "detour_slot.hpp"
#include "engine.hpp"
#include "entity_handle.hpp"

namespace funrounds::native::detail {
namespace {

using Eligibility = bool (*)(std::int32_t, std::int32_t, std::int32_t);
using Phase = polyhook::CallbackType;
using Result = polyhook::ResultType;

struct Hook {
    void* entry{};
    void* address{};
    polyhook::HookHandle handle{};
};

struct State {
    Hook permission{}, movement{}, legacyJump{}, modernJump{};
    SchemaField freeze{}, movementOwner{}, controller{}, legacyContext{}, modernContext{};
    ConVarData** jumpRef{};
    Eligibility freezeEligible{}, jumpEligible{};
    std::thread::id gameThread{};
} g_state;

struct FreezeRead {
    std::byte* rules{};
    unsigned nested{};
    std::uint8_t before{}, written{};
};

// The original movement and permission functions continue to apply native
// restrictions such as planting. Change only their view of the freeze flag.
thread_local FreezeRead g_permission;
thread_local FreezeRead g_movement;

bool ResolveActor() {
    if (g_state.movementOwner.size) return true;
    return __polyhook_HookDetour && __polyhook_FindDetour && __polyhook_UnhookDetour &&
        __polyhook_AddCallback && __polyhook_RemoveCallback && __polyhook_IsCallbackRegistered &&
        __polyhook_AreCallbacksRegistered && __polyhook_GetArgumentPointer &&
        __s2sdk_GetMaxClients && __s2sdk_IsClientInGame && __s2sdk_PlayerSlotToEntHandle && __s2sdk_GetClientPawn &&
        EnsureEngine() && ResolveSchemaField("CBasePlayerPawn", "m_hController", sizeof(std::uint32_t), g_state.controller) &&
        ResolvePlayerPawnComponentChain(g_state.movementOwner);
}

bool ResolveFreeze() {
    if (g_state.permission.entry && g_state.movement.entry) return true;
    if (!ResolveActor() || !__s2sdk_GetGameRules ||
        !ResolveSchemaField("CCSGameRules", "m_bFreezePeriod", sizeof(std::uint8_t), g_state.freeze)) return false;
    g_state.permission.entry = ProbeSignature(SignatureKey{"FunRounds::PlayerFreezePermission"});
    g_state.movement.entry = ProbeSignature(SignatureKey{"FunRounds::PlayerFreezeProcessMovement"});
    return g_state.permission.entry && g_state.movement.entry;
}

std::byte* MovementPawn(std::byte* services) {
    if (!services) return nullptr;
    auto* chain = reinterpret_cast<CNetworkVarChainer*>(services + g_state.movementOwner.offset);
    return reinterpret_cast<std::byte*>(chain->GetObject());
}

bool Eligible(std::byte* pawn, Eligibility eligible) {
    if (!pawn || !eligible) return false;
    const auto pawnHandle = HandleFromEntity(pawn);
    if (!IsPlayerPawn(pawnHandle)) return false;
    std::uint32_t owner{};
    std::memcpy(&owner, pawn + g_state.controller.offset, sizeof(owner));
    const auto controllerHandle = ToApiHandle(owner);
    auto* controller = static_cast<CEntityInstance*>(EntityFromHandle(controllerHandle));
    if (!controller) return false;
    const auto slot = controller->GetRefEHandle().GetEntryIndex() - 1;
    return slot >= 0 && slot < s2sdk::GetMaxClients() && s2sdk::IsClientInGame(slot) &&
        s2sdk::PlayerSlotToEntHandle(slot) == controllerHandle && s2sdk::GetClientPawn(slot) == pawn &&
        eligible(slot, pawnHandle, controllerHandle);
}

template <bool Movement>
Result FreezeGate(polyhook::HookHandle, polyhook::ParametersHandle params, std::int32_t,
                  polyhook::ReturnHandle, Phase phase) noexcept {
    if (std::this_thread::get_id() != g_state.gameThread) return Result::Ignored;
    auto& scope = Movement ? g_movement : g_permission;
    const auto& other = Movement ? g_permission : g_movement;
    if (phase == Phase::Post) {
        if (scope.nested) {
            --scope.nested;
        } else if (scope.rules) {
            if (s2sdk::GetGameRules() == scope.rules) {
                auto& freeze = reinterpret_cast<std::uint8_t*>(scope.rules)[g_state.freeze.offset];
                if (freeze == scope.written) freeze = scope.before;
            }
            scope = {};
        }
        return Result::Ignored;
    }
    if (scope.rules) {
        ++scope.nested;
        return Result::Ignored;
    }
    try {
        if (!params || !g_state.freezeEligible) return Result::Ignored;
        auto* rules = static_cast<std::byte*>(s2sdk::GetGameRules());
        if (!rules) return Result::Ignored;
        auto& freeze = reinterpret_cast<std::uint8_t*>(rules)[g_state.freeze.offset];
        if (freeze != 1 && other.rules != rules) return Result::Ignored;

        auto* pawn = static_cast<std::byte*>(polyhook::GetArgument<void*>(params, 0));
        if constexpr (Movement) pawn = MovementPawn(pawn);
        const auto effective = static_cast<std::uint8_t>(Eligible(pawn, g_state.freezeEligible) ? 0 : 1);
        if (effective != freeze) {
            scope = {rules, 0, freeze, effective};
            freeze = effective;
        }
    } catch (...) {
        // Do not take ownership if identity or the consumer cannot be resolved.
    }
    return Result::Ignored;
}

template <bool Movement>
bool Remove(Hook& hook) {
    DetourSlot slot(hook.address, hook.handle);
    return slot.Remove(Phase::Pre, FreezeGate<Movement>) &&
        slot.Remove(Phase::Post, FreezeGate<Movement>) && slot.Release();
}

bool DetachFreeze() {
    g_state.freezeEligible = nullptr;
    if (g_permission.rules || g_movement.rules) return false;
    const bool movement = Remove<true>(g_state.movement);
    return Remove<false>(g_state.permission) && movement;
}

template <bool Movement>
bool Install(Hook& hook) {
    using T = polyhook::DataType;
    DetourSlot slot(hook.address, hook.handle);
    const auto create = [&] {
        if constexpr (Movement) {
            return polyhook::HookDetour(hook.entry, T::Void, {T::Pointer, T::Pointer}, -1, "FunRounds::PlayerFreezeProcessMovement");
        } else {
            return polyhook::HookDetour(hook.entry, T::Bool, {T::Pointer}, -1, "FunRounds::PlayerFreezePermission");
        }
    };
    return slot.Acquire(hook.entry, create) && slot.Add(Phase::Post, FreezeGate<Movement>) && slot.Add(Phase::Pre, FreezeGate<Movement>);
}

bool SetFreeze(bool active, Eligibility eligibility) {
    if (!active) return (!g_state.freezeEligible || g_state.freezeEligible == eligibility) && DetachFreeze();
    if (!eligibility || !ResolveFreeze()) return false;
    if (g_state.freezeEligible) return g_state.freezeEligible == eligibility;
    g_state.gameThread = std::this_thread::get_id();
    if (!Install<false>(g_state.permission) || !Install<true>(g_state.movement)) {
        (void)DetachFreeze();
        return false;
    }
    g_state.freezeEligible = eligibility;
    return true;
}

// The engine's two jump implementations read this private ConVarData reference.
// Supply a read-only view during the selected pawn's native jump evaluation;
// neither the registered ConVar value nor its replication/callbacks are changed.
constexpr std::size_t kBoolOffset = 0x58;
constexpr auto kJumpRef = SignatureKey{"FunRounds::PlayerAutoJumpConVarLoad"};
constexpr auto kLegacyJump = SignatureKey{"FunRounds::PlayerAutoJumpLegacy"};
constexpr auto kModernJump = SignatureKey{"FunRounds::PlayerAutoJumpModern"};

struct JumpRead {
    alignas(ConVarData) std::array<std::byte, sizeof(ConVarData)> view{};
    ConVarData* original{};
    ConVarData* written{};
    unsigned nested{};
};
thread_local JumpRead g_legacy, g_modern;

bool ResolveJump() {
    if (g_state.jumpRef) return true;
    if (!ResolveActor() ||
        !ResolveSchemaField("CCSPlayer_MovementServices", "m_LegacyJump", 0, g_state.legacyContext) ||
        !ResolveSchemaField("CCSPlayer_MovementServices", "m_ModernJump", 0, g_state.modernContext)) return false;
    g_state.legacyJump.entry = ProbeSignature(kLegacyJump);
    g_state.modernJump.entry = ProbeSignature(kModernJump);
    const auto* load = static_cast<const std::byte*>(ProbeSignature(kJumpRef));
    if (!g_state.legacyJump.entry || !g_state.modernJump.entry || !load) return false;
    // Both verified builds use MOV RAX,[RIP+rel32]; the following compare is
    // the native first bool slot read at +0x58.
    std::int32_t disp{};
    std::memcpy(&disp, load + 3, sizeof(disp));
    auto** ref = reinterpret_cast<ConVarData**>(reinterpret_cast<std::intptr_t>(load + 7) + disp);
    const auto* data = *ref;
    if (!data || data->GetType() != EConVarType_Bool || !data->GetName() || std::strcmp(data->GetName(), "sv_autobunnyhopping") != 0) {
        InvalidGamedata(kJumpRef.name());
        return false;
    }
    g_state.jumpRef = ref;
    return true;
}

template <bool Legacy>
Result JumpGate(polyhook::HookHandle, polyhook::ParametersHandle params, std::int32_t,
                polyhook::ReturnHandle, Phase phase) noexcept {
    if (std::this_thread::get_id() != g_state.gameThread) return Result::Ignored;
    auto& read = Legacy ? g_legacy : g_modern;
    const auto& other = Legacy ? g_modern : g_legacy;
    if (phase == Phase::Post) {
        if (read.nested) {
            --read.nested;
        } else if (read.original) {
            if (*g_state.jumpRef == read.written) *g_state.jumpRef = read.original;
            read.original = nullptr;
            read.written = nullptr;
        }
        return Result::Ignored;
    }
    if (read.original) {
        ++read.nested;
        return Result::Ignored;
    }
    try {
        if (!params || !g_state.jumpEligible || !*g_state.jumpRef) return Result::Ignored;
        auto* context = static_cast<std::byte*>(polyhook::GetArgument<void*>(params, 0));
        const auto offset = Legacy ? g_state.legacyContext.offset : g_state.modernContext.offset;
        if (!context) return Result::Ignored;
        auto* original = *g_state.jumpRef;
        auto* base = original == other.written ? other.original : original;
        if (!base) return Result::Ignored;
        auto* effective = base;
        if (Eligible(MovementPawn(context - offset), g_state.jumpEligible) &&
            reinterpret_cast<const std::byte*>(base)[kBoolOffset] == std::byte{0}) {
            // SourceSDK notes the registered allocation is trimmed to the type;
            // copy only through the first bool read, never sizeof(ConVarData).
            std::memcpy(read.view.data(), base, kBoolOffset + 1);
            read.view[kBoolOffset] = std::byte{1};
            effective = reinterpret_cast<ConVarData*>(read.view.data());
        }
        if (original != effective) {
            read.original = original;
            read.written = effective;
            *g_state.jumpRef = effective;
        }
    } catch (...) {
        // An unavailable actor never receives the per-player view.
    }
    return Result::Ignored;
}

template <bool Legacy>
bool ConfigureJump(bool active, Hook& hook) {
    DetourSlot slot(hook.address, hook.handle);
    if (!active) {
        return slot.Remove(Phase::Pre, JumpGate<Legacy>) && slot.Remove(Phase::Post, JumpGate<Legacy>) && slot.Release();
    }
    using T = polyhook::DataType;
    const auto create = [&] {
        return polyhook::HookDetour(hook.entry, T::Void, {T::Pointer, T::Pointer}, -1,
                                   (Legacy ? kLegacyJump : kModernJump).name().data());
    };
    return slot.Acquire(hook.entry, create) && slot.Add(Phase::Post, JumpGate<Legacy>) && slot.Add(Phase::Pre, JumpGate<Legacy>);
}

bool DetachJump() {
    g_state.jumpEligible = nullptr;
    if (g_legacy.original || g_modern.original) return false;
    const bool legacy = ConfigureJump<true>(false, g_state.legacyJump);
    return ConfigureJump<false>(false, g_state.modernJump) && legacy;
}

bool SetJump(bool active, Eligibility eligibility) {
    if (!active) return (!g_state.jumpEligible || g_state.jumpEligible == eligibility) && DetachJump();
    if (!eligibility || !ResolveJump()) return false;
    if (g_state.jumpEligible) return g_state.jumpEligible == eligibility;
    g_state.gameThread = std::this_thread::get_id();
    if (!ConfigureJump<true>(true, g_state.legacyJump) || !ConfigureJump<false>(true, g_state.modernJump)) {
        (void)DetachJump();
        return false;
    }
    g_state.jumpEligible = eligibility;
    return true;
}

}

bool ShutdownPlayerMovementPolicies() noexcept {
    try {
        const auto freeze = DetachFreeze();
        if (!DetachJump() || !freeze) return false;
        g_state = {};
        return true;
    } catch (...) {
        return false;
    }
}

}

extern "C" PLUGIN_API bool HasPlayerFreezeMovementSupport() noexcept {
    try { return funrounds::native::detail::ResolveFreeze(); } catch (...) { return false; }
}

extern "C" PLUGIN_API bool SetPlayerFreezeMovement(bool active, funrounds::native::detail::Eligibility eligibility) noexcept {
    try { return funrounds::native::detail::SetFreeze(active, eligibility); } catch (...) { return false; }
}

extern "C" PLUGIN_API bool HasPlayerAutoJumpSupport() noexcept {
    try { return funrounds::native::detail::ResolveJump(); } catch (...) { return false; }
}

extern "C" PLUGIN_API bool SetPlayerAutoJump(bool active, funrounds::native::detail::Eligibility eligibility) noexcept {
    try { return funrounds::native::detail::SetJump(active, eligibility); } catch (...) { return false; }
}
