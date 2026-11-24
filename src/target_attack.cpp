// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "target_attack.hpp"

#include <cstdint>
#include <cstring>
#include <thread>
#include <cstrike15/usercmd.h>
#include <entity2/entityinstance.h>
#include <networkvar.h>
#include <plugin_export.h>
#include <polyhook/polyhook.hpp>

#include "detour_slot.hpp"
#include "engine.hpp"
#include "entity_handle.hpp"

namespace funrounds::native::detail {
namespace {

using BlockAttack = bool (*)(std::int32_t pawn);
using WeaponCommand = void (*)(std::int32_t playerSlot, std::int32_t pawn);
using Phase = polyhook::CallbackType;
using Result = polyhook::ResultType;
constexpr auto kRunCommand = SignatureKey{"FunRounds::RunPlayerCommand"};

void* entry{};
polyhook::HookHandle hook{};
BlockAttack policy{};
WeaponCommand weaponCommand{};
std::thread::id gameThread{};
SchemaField chain{};
SchemaField pawnController{};
int currentCommandOffset{-1};
struct CommandIdentity {
    std::int32_t pawn{-1};
    std::int32_t slot{-1};
};
thread_local CommandIdentity currentWeaponCommand;

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

Result DispatchWeaponCommand(polyhook::HookHandle, polyhook::ParametersHandle args, std::int32_t,
                             polyhook::ReturnHandle, Phase) noexcept {
    if (!args || !weaponCommand || gameThread != std::this_thread::get_id()) return Result::Ignored;
    try {
        auto* service = static_cast<std::byte*>(polyhook::GetArgument<void*>(args, 0));
        const auto* cmd = polyhook::GetArgument<void*>(args, 1);
        if (!service || !cmd) return Result::Ignored;
        auto* pawn = reinterpret_cast<CNetworkVarChainer*>(service + chain.offset)->GetObject();
        const auto handle = HandleFromEntity(pawn);
        if (handle == -1) return Result::Ignored;
        std::uint32_t controllerHandle;
        std::memcpy(&controllerHandle, reinterpret_cast<const std::byte*>(pawn) + pawnController.offset, sizeof(controllerHandle));
        const auto controllerId = ToApiHandle(controllerHandle);
        if (!IsPlayerController(controllerId)) return Result::Ignored;
        auto* controller = static_cast<CEntityInstance*>(EntityFromHandle(controllerId));
        if (!controller) return Result::Ignored;
        const void* current{};
        std::memcpy(&current, reinterpret_cast<const std::byte*>(controller) + currentCommandOffset, sizeof(current));
        // The controller installs this exact command around the movement call.
        // Native hitscan lag compensation dereferences it for human players.
        if (current != cmd) return Result::Ignored;
        struct CommandScope {
            CommandIdentity previous{currentWeaponCommand};
            ~CommandScope() { currentWeaponCommand = previous; }
        } scope;
        const auto slot = controller->GetRefEHandle().GetEntryIndex() - 1;
        currentWeaponCommand = {handle, slot};
        weaponCommand(slot, handle);
    } catch (...) {
        // A command callback cannot unwind through the engine.
    }
    return Result::Ignored;
}

bool AcquireCommandHook(void* address) {
    DetourSlot slot(entry, hook);
    using T = polyhook::DataType;
    return slot.Acquire(address, [&] {
        return polyhook::HookDetour(address, T::Pointer, {T::Pointer, T::Pointer}, -1, kRunCommand.name().data());
    });
}

bool Attach(BlockAttack callback) {
    if (!callback) return false;
    if (policy) return policy == callback;
    if (!AcquireCommandHook(Resolve())) return false;
    if (!DetourSlot(entry, hook).Add(Phase::Pre, FilterAttack)) {
        (void)ShutdownTargetAttack();
        return false;
    }
    gameThread = std::this_thread::get_id();
    policy = callback;
    return true;
}

bool AttachWeaponCommand(WeaponCommand callback) {
    if (!callback) return false;
    if (weaponCommand) return weaponCommand == callback;
    if (!HasNativeWeaponCommand() || !AcquireCommandHook(Resolve())) return false;
    if (!DetourSlot(entry, hook).Add(Phase::Post, DispatchWeaponCommand)) {
        (void)ShutdownNativeWeaponCommand();
        return false;
    }
    gameThread = std::this_thread::get_id();
    weaponCommand = callback;
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

bool HasNativeWeaponCommand() noexcept {
    try {
        if (currentCommandOffset > 0) return true;
        if (!Resolve() || !ResolveSchemaField("CBasePlayerPawn", "m_hController", sizeof(std::uint32_t), pawnController))
            return false;
        currentCommandOffset = ProbeOffset(OffsetKey{"FunRounds::PlayerControllerCurrentCommand"});
        return currentCommandOffset > 0;
    } catch (...) {
        return false;
    }
}

std::int32_t NativeWeaponCommandSlot(std::int32_t pawn) noexcept {
    return pawn == currentWeaponCommand.pawn ? currentWeaponCommand.slot : -1;
}

bool ShutdownNativeWeaponCommand() noexcept {
    try {
        weaponCommand = nullptr;
        DetourSlot slot(entry, hook);
        return slot.Remove(Phase::Post, DispatchWeaponCommand) && slot.Release();
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

extern "C" PLUGIN_API bool SetNativeWeaponCommandCallback(bool enabled, funrounds::native::detail::WeaponCommand callback) noexcept {
    try {
        return enabled ? funrounds::native::detail::AttachWeaponCommand(callback) : funrounds::native::detail::ShutdownNativeWeaponCommand();
    } catch (...) {
        return false;
    }
}
