// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "target_attack.hpp"

#include <cstdint>
#include <thread>
#include <cstrike15/usercmd.h>
#include <networkvar.h>
#include <plugin_export.h>
#include <polyhook/polyhook.hpp>

#include "detour_slot.hpp"
#include "engine.hpp"

namespace funrounds::native::detail {
namespace {

using BlockAttack = bool (*)(std::int32_t pawn);
using Phase = polyhook::CallbackType;
using Result = polyhook::ResultType;
constexpr auto kRunCommand = SignatureKey{"FunRounds::RunPlayerCommand"};

void* entry{};
polyhook::HookHandle hook{};
BlockAttack policy{};
std::thread::id gameThread{};
SchemaField chain{};

void* Resolve() {
    if (!(EnsureEngine() && __polyhook_HookDetour && __polyhook_FindDetour &&
        __polyhook_AddCallback && __polyhook_RemoveCallback && __polyhook_IsCallbackRegistered &&
        __polyhook_AreCallbacksRegistered && __polyhook_UnhookDetour && __polyhook_GetArgumentPointer &&
        ResolvePlayerPawnComponentChain(chain))) return nullptr;
    return ProbeSignature(kRunCommand);
}

Result FilterAttack(polyhook::HookHandle, polyhook::ParametersHandle args, std::int32_t,
                    polyhook::ReturnHandle, Phase) noexcept {
    if (!args || !policy || gameThread != std::this_thread::get_id()) return Result::Ignored;
    try {
        auto* service = static_cast<std::byte*>(polyhook::GetArgument<void*>(args, 0));
        auto* cmd = static_cast<CCSGOUserCmd*>(polyhook::GetArgument<void*>(args, 1));
        if (!service || !cmd) return Result::Ignored;
        auto* pawn = reinterpret_cast<CNetworkVarChainer*>(service + chain.offset)->GetObject();
        const auto handle = HandleFromEntity(pawn);
        if (handle == -1 || !policy(handle)) return Result::Ignored;
        constexpr std::uint64_t attack = IN_ATTACK | IN_ATTACK2;
        // Keep the live and serialized inputs consistent, including subtick button edges.
        for (auto& state : cmd->m_ButtonStates.m_pButtonStates) state &= ~attack;
        if (cmd->has_base()) {
            auto* base = cmd->mutable_base();
            if (base->has_buttons_pb()) {
                auto* buttons = base->mutable_buttons_pb();
                buttons->set_buttonstate1(buttons->buttonstate1() & ~attack);
                buttons->set_buttonstate2(buttons->buttonstate2() & ~attack);
                buttons->set_buttonstate3(buttons->buttonstate3() & ~attack);
            }
            for (auto& step : *base->mutable_subtick_moves()) {
                if ((step.button() & attack) == 0) continue;
                if (const auto keep = step.button() & ~attack) step.set_button(keep);
                else step.clear_button();
            }
        }
        cmd->set_attack1_start_history_index(-1);
        cmd->set_attack2_start_history_index(-1);
    } catch (...) {
        // A command callback cannot unwind through the engine.
    }
    return Result::Ignored;
}

bool Attach(BlockAttack callback) {
    if (!callback) return false;
    if (policy) return policy == callback;
    auto* address = Resolve();
    DetourSlot slot(entry, hook);
    using T = polyhook::DataType;
    if (!slot.Acquire(address, [&] {
        return polyhook::HookDetour(address, T::Pointer, {T::Pointer, T::Pointer}, -1, kRunCommand.name().data());
    })) return false;
    if (!slot.Add(Phase::Pre, FilterAttack)) {
        (void)ShutdownTargetAttack();
        return false;
    }
    gameThread = std::this_thread::get_id();
    policy = callback;
    return true;
}

}  // namespace

bool ShutdownTargetAttack() noexcept {
    try {
        policy = nullptr;
        DetourSlot slot(entry, hook);
        return slot.Remove(Phase::Pre, FilterAttack) && slot.Release();
    } catch (...) {
        return false;
    }
}

}  // namespace funrounds::native::detail

extern "C" PLUGIN_API bool HasTargetAttackSupport() noexcept {
    try {
        return funrounds::native::detail::Resolve() != nullptr;
    } catch (...) {
        return false;
    }
}

extern "C" PLUGIN_API bool SetTargetAttackCallback(bool enabled, funrounds::native::detail::BlockAttack callback) noexcept {
    try {
        return enabled ? funrounds::native::detail::Attach(callback) : funrounds::native::detail::ShutdownTargetAttack();
    } catch (...) {
        return false;
    }
}
