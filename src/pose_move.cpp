// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "pose_move.hpp"

#include <cstdint>
#include <cstring>
#include <thread>

#include <cstrike15/cs_movedata.h>
#include <entity2/entityinstance.h>
#include <entity2/entitynetwork.h>
#include <plugin_export.h>
#include <polyhook/polyhook.hpp>
#include <s2sdk/s2sdk.hpp>

#include "detour_slot.hpp"
#include "engine.hpp"
#include "entity_handle.hpp"

namespace funrounds::native::detail {
namespace {

constexpr auto kStep = SignatureKey{"FunRounds::PoseMoveStep"};
using Assist = bool (*)(std::int32_t, std::int32_t, std::int32_t, std::uint64_t, bool, float&, float&, float&);
using Phase = polyhook::CallbackType;
using Result = polyhook::ResultType;

struct State {
    void* entry{};
    void* address{};
    polyhook::HookHandle hook{};
    SchemaField movementOwner{}, controller{}, health{}, life{}, ground{};
    Assist assist{};
    std::int32_t world{-1};
    std::thread::id gameThread{};
} g_state;

struct Step {
    CMoveData* move{};
    float priorForward{}, priorSide{}, writtenForward{}, writtenSide{};
    unsigned nested{};
};
thread_local Step g_step;

template <typename T>
T Read(const std::byte* object, SchemaField field) noexcept {
    T value;
    std::memcpy(&value, object + field.offset, sizeof(value));
    return value;
}

bool ResolveStep() {
    if (g_state.entry) return true;
    if (!__polyhook_HookDetour || !__polyhook_FindDetour || !__polyhook_UnhookDetour ||
        !__polyhook_AddCallback || !__polyhook_RemoveCallback || !__polyhook_IsCallbackRegistered ||
        !__polyhook_AreCallbacksRegistered || !__polyhook_GetArgumentPointer ||
        !__s2sdk_GetClientFlags || !__s2sdk_GetMaxClients ||
        !__s2sdk_PlayerSlotToEntHandle || !__s2sdk_IsClientInGame || !__s2sdk_GetClientPawn ||
        !EnsureEngine() || !ResolvePlayerPawnComponentChain(g_state.movementOwner) ||
        !ResolveSchemaField("CBasePlayerPawn", "m_hController", sizeof(std::uint32_t), g_state.controller) ||
        !ResolveSchemaField("CBaseEntity", "m_lifeState", sizeof(std::uint8_t), g_state.life) ||
        !ResolveSchemaField("CBaseEntity", "m_iHealth", sizeof(std::int32_t), g_state.health)) {
        return false;
    }
    g_state.entry = ProbeSignature(kStep);
    return g_state.entry != nullptr;
}

bool ResolveAssist() {
    return __s2sdk_GetClientButtons && ResolveStep();
}

bool ResolveGroundSupport() {
    return __s2sdk_EntIndexToEntHandle && __s2sdk_GetEntityClassname && __s2sdk_IsEntityPlayerPawn &&
        __s2sdk_EntHandleToEntPointer && __s2sdk_EntPointerToEntHandle && ResolveStep() &&
        (g_state.ground.size ||
         ResolveSchemaField("CBaseEntity", "m_hGroundEntity", sizeof(std::uint32_t), g_state.ground));
}

std::byte* MovementPawn(polyhook::ParametersHandle params) {
    auto* services = static_cast<std::byte*>(polyhook::GetArgument<void*>(params, 0));
    if (!services) return nullptr;
    auto* chain = reinterpret_cast<CNetworkVarChainer*>(services + g_state.movementOwner.offset);
    return reinterpret_cast<std::byte*>(chain->GetObject());
}

int LivingPlayerSlot(const std::byte* pawn, std::int32_t controllerHandle) {
    if (!pawn || Read<std::uint8_t>(pawn, g_state.life) != 0 || Read<std::int32_t>(pawn, g_state.health) <= 0) return -1;
    auto* controller = static_cast<CEntityInstance*>(EntityFromHandle(controllerHandle));
    if (!controller) return -1;
    const int slot = controller->GetRefEHandle().GetEntryIndex() - 1;
    return slot >= 0 && slot < s2sdk::GetMaxClients() && s2sdk::IsClientInGame(slot) &&
        s2sdk::PlayerSlotToEntHandle(slot) == controllerHandle && s2sdk::GetClientPawn(slot) == pawn ? slot : -1;
}

Result MoveStep(polyhook::HookHandle, polyhook::ParametersHandle params, std::int32_t,
                polyhook::ReturnHandle, Phase phase) noexcept {
    if (std::this_thread::get_id() != g_state.gameThread) return Result::Ignored;
    if (phase == Phase::Post) {
        if (g_step.nested) {
            --g_step.nested;
        } else if (auto* move = g_step.move) {
            // Later cooperating hooks may replace an axis; don't overwrite their value.
            if (move->m_flForwardMove == g_step.writtenForward) move->m_flForwardMove = g_step.priorForward;
            if (move->m_flSideMove == g_step.writtenSide) move->m_flSideMove = g_step.priorSide;
            g_step = {};
        }
        return Result::Ignored;
    }
    if (g_step.move) {
        ++g_step.nested;
        return Result::Ignored;
    }
    try {
        if (!g_state.assist || !params) return Result::Ignored;
        auto* move = static_cast<CMoveData*>(polyhook::GetArgument<void*>(params, 1));
        auto* pawn = MovementPawn(params);
        if (!move || !pawn) return Result::Ignored;
        const auto pawnHandle = HandleFromEntity(pawn);
        const auto controllerHandle = ToApiHandle(Read<std::uint32_t>(pawn, g_state.controller));
        const int slot = LivingPlayerSlot(pawn, controllerHandle);
        if (slot < 0 || pawnHandle == -1) return Result::Ignored;

        float forward = move->m_flForwardMove;
        float side = move->m_flSideMove;
        float vertical = move->m_vecVelocity.z;
        const auto buttons = s2sdk::GetClientButtons(slot, 0);
        const bool grounded = (s2sdk::GetClientFlags(slot) & 1) != 0;
        if (g_state.assist(slot, pawnHandle, controllerHandle, buttons, grounded, forward, side, vertical)) {
            g_step = {move, move->m_flForwardMove, move->m_flSideMove, forward, side, 0};
            move->m_flForwardMove = forward;
            move->m_flSideMove = side;
            // Velocity is an input/output of this step, unlike its temporary command axes.
            move->m_vecVelocity.z = vertical;
        }
    } catch (...) {
        // Keep native movement unchanged when identity lookup or the consumer fails.
    }
    return Result::Ignored;
}

Result GroundStep(polyhook::HookHandle, polyhook::ParametersHandle params, std::int32_t,
                  polyhook::ReturnHandle, Phase) noexcept {
    if (g_state.world == -1 || std::this_thread::get_id() != g_state.gameThread || !params) return Result::Ignored;
    try {
        auto* pawn = MovementPawn(params);
        if (!pawn) return Result::Ignored;
        const auto supportHandle = ToApiHandle(Read<std::uint32_t>(pawn, g_state.ground));
        if (supportHandle == g_state.world || !IsPlayerPawn(supportHandle)) return Result::Ignored;
        auto* support = static_cast<std::byte*>(EntityFromHandle(supportHandle));
        if (!support || support == pawn || HandleFromEntity(support) != supportHandle ||
            s2sdk::GetEntityClassname(supportHandle) != "player") return Result::Ignored;
        const auto pawnHandle = HandleFromEntity(pawn);
        if (EntityFromHandle(pawnHandle) != pawn || s2sdk::GetEntityClassname(pawnHandle) != "player") return Result::Ignored;
        const int slot = LivingPlayerSlot(pawn, ToApiHandle(Read<std::uint32_t>(pawn, g_state.controller)));
        if (slot < 0 || (s2sdk::GetClientFlags(slot) & 1) == 0 ||
            LivingPlayerSlot(support, ToApiHandle(Read<std::uint32_t>(support, g_state.controller))) < 0) return Result::Ignored;
        auto* world = EntityFromHandle(g_state.world);
        if (!world || HandleFromEntity(world) != g_state.world) return Result::Ignored;
        // The engine retains grounded state and categorizes it again when moving or jumping.
        const auto raw = ToNativeHandle(g_state.world);
        std::memcpy(pawn + g_state.ground.offset, &raw, sizeof(raw));
    } catch (...) {
        // Leave engine support unchanged if live identity cannot be verified.
    }
    return Result::Ignored;
}

bool DetachAssist() {
    g_state.assist = nullptr;
    if (g_step.move) return false;
    DetourSlot slot(g_state.address, g_state.hook);
    return slot.Remove(Phase::Pre, MoveStep) && slot.Remove(Phase::Post, MoveStep) && slot.Release();
}

bool DetachGroundSupport() {
    g_state.world = -1;
    DetourSlot slot(g_state.address, g_state.hook);
    return slot.Remove(Phase::Post, GroundStep) && slot.Release();
}

bool AcquireStep(DetourSlot& slot) {
    auto* entry = g_state.entry;
    using T = polyhook::DataType;
    if (!slot.Acquire(entry, [&] { return polyhook::HookDetour(entry, T::Void, {T::Pointer, T::Pointer, T::UInt8}, -1, kStep.name().data()); })) {
        return false;
    }
    g_state.gameThread = std::this_thread::get_id();
    return true;
}

bool SetAssist(bool active, Assist assist) {
    if (!active) return (!g_state.assist || g_state.assist == assist) && DetachAssist();
    if (!assist || !ResolveAssist()) return false;
    if (g_state.assist) return g_state.assist == assist;
    DetourSlot slot(g_state.address, g_state.hook);
    if (!AcquireStep(slot)) return false;
    if (!slot.Add(Phase::Post, MoveStep) || !slot.Add(Phase::Pre, MoveStep)) {
        (void)DetachAssist();
        return false;
    }
    g_state.assist = assist;
    return true;
}

bool SetGroundSupport(bool active) {
    if (!active) return DetachGroundSupport();
    if (g_state.world != -1) return true;
    if (!ResolveGroundSupport()) return false;
    const auto handle = s2sdk::EntIndexToEntHandle(0);
    auto* world = EntityFromHandle(handle);
    if (!world || HandleFromEntity(world) != handle || s2sdk::GetEntityClassname(handle) != "worldent") return false;
    DetourSlot slot(g_state.address, g_state.hook);
    if (!AcquireStep(slot)) return false;
    if (!slot.Add(Phase::Post, GroundStep)) {
        (void)DetachGroundSupport();
        return false;
    }
    g_state.world = handle;
    return true;
}

} // namespace

bool ShutdownPoseMove() noexcept {
    try {
        const bool assist = DetachAssist();
        const bool ground = DetachGroundSupport();
        if (!assist || !ground) return false;
        g_state = {};
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace funrounds::native::detail

extern "C" PLUGIN_API bool HasPoseMoveSupport() noexcept {
    try {
        return funrounds::native::detail::ResolveAssist();
    } catch (...) {
        return false;
    }
}

extern "C" PLUGIN_API bool HasPlayerGroundSupport() noexcept {
    try {
        return funrounds::native::detail::ResolveGroundSupport();
    } catch (...) {
        return false;
    }
}

extern "C" PLUGIN_API bool SetPlayerGroundSupport(bool active) noexcept {
    try {
        return funrounds::native::detail::SetGroundSupport(active);
    } catch (...) {
        return false;
    }
}

extern "C" PLUGIN_API bool SetPoseMoveAssist(bool active, funrounds::native::detail::Assist assist) noexcept {
    try {
        return funrounds::native::detail::SetAssist(active, assist);
    } catch (...) {
        return false;
    }
}
