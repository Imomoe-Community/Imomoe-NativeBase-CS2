// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "team_flash.hpp"

#include <optional>
#include <string_view>

#include <plugin_export.h>
#include <polyhook/polyhook.hpp>

#include "detour_slot.hpp"
#include "engine.hpp"
#include "entity_handle.hpp"
#include "team_flash_policy.hpp"

namespace funrounds::native::detail {
namespace {

using team_flash::Attribution;
using Phase = polyhook::CallbackType;
using Result = polyhook::ResultType;

constexpr auto kJudge = SignatureKey{"FunRounds::FlashbangJudge"};
const plg::string kGrenade{"CBaseGrenade"};
const plg::string kEntity{"CBaseEntity"};
const plg::string kPawn{"CBasePlayerPawn"};
const plg::string kThrower{"m_hThrower"};
const plg::string kOwner{"m_hOwnerEntity"};
const plg::string kController{"m_hController"};

void* g_judge{};
std::uint64_t g_ffa{};
void* g_entry{};
polyhook::HookHandle g_hook{};
bool g_attempted{};
bool g_spawned{}, g_deleted{}, g_mapEnd{};
team_flash::AttributionTable g_attributions;
// The controls and entity listeners run on the game thread.
thread_local bool g_active{};

bool RequiredImports() {
    return __s2sdk_GetGameConfigSignature && __s2sdk_FindConVar && __s2sdk_GetConVarBool &&
        __s2sdk_EntHandleToEntPointer && __s2sdk_EntPointerToEntHandle && __s2sdk_IsEntityPlayerPawn &&
        __s2sdk_EntHandleToEntIndex && __s2sdk_GetMaxClients && __s2sdk_GetClientPawn &&
        __s2sdk_PlayerSlotToEntHandle && __s2sdk_IsClientInGame && __s2sdk_GetEntityTeam &&
        __s2sdk_GetEntityHealth && __s2sdk_GetEntityClassname && __s2sdk_GetEntSchemaEnt2 &&
        __s2sdk_OnEntitySpawned_Register && __s2sdk_OnEntitySpawned_Unregister &&
        __s2sdk_OnEntityDeleted_Register && __s2sdk_OnEntityDeleted_Unregister &&
        __s2sdk_OnMapEnd_Register && __s2sdk_OnMapEnd_Unregister &&
        __polyhook_HookDetour && __polyhook_FindDetour && __polyhook_AddCallback &&
        __polyhook_IsCallbackRegistered && __polyhook_RemoveCallback &&
        __polyhook_AreCallbacksRegistered && __polyhook_UnhookDetour &&
        __polyhook_GetArgumentPointer && __polyhook_SetReturnFloat;
}

bool Resolve() {
    if (g_judge) return true;
    if (!RequiredImports() || !EnsureEngine() ||
        !ResolveSchemaHandle(kGrenade.c_str(), kThrower.c_str()) ||
        !ResolveSchemaHandle(kEntity.c_str(), kOwner.c_str()) ||
        !ResolveSchemaHandle(kPawn.c_str(), kController.c_str())) return false;
    g_ffa = s2sdk::FindConVar("mp_teammates_are_enemies");
    if (!g_ffa) return false;
    g_judge = ProbeSignature(kJudge);
    return g_judge != nullptr;
}

struct Player {
    std::int32_t slot;
    std::int32_t team;
    std::uint32_t pawn;
};

std::optional<Player> CurrentPlayer(void* pawn, std::int32_t handle) {
    if (!pawn || !IsPlayerPawn(handle)) return std::nullopt;
    const auto controller = s2sdk::GetEntSchemaEnt2(pawn, kPawn, kController, 0);
    const auto slot = s2sdk::EntHandleToEntIndex(controller) - 1;
    if (slot < 0 || slot >= s2sdk::GetMaxClients() || !s2sdk::IsClientInGame(slot) ||
        s2sdk::PlayerSlotToEntHandle(slot) != controller || s2sdk::GetClientPawn(slot) != pawn)
        return std::nullopt;
    return Player{slot, static_cast<std::int32_t>(s2sdk::GetEntityTeam(handle)), ToNativeHandle(handle)};
}

std::optional<Attribution> LiveThrower(std::int32_t handle) {
    auto player = CurrentPlayer(EntityFromHandle(handle), handle);
    if (!player || !team_flash::IsPlayingTeam(player->team) || s2sdk::GetEntityHealth(handle) <= 0)
        return std::nullopt;
    return Attribution{player->slot, player->pawn, player->team};
}

std::optional<Attribution> Capture(void* grenade, std::uint32_t fullHandle) {
    auto attribution = LiveThrower(s2sdk::GetEntSchemaEnt2(grenade, kGrenade, kThrower, 0));
    if (!attribution)
        attribution = LiveThrower(s2sdk::GetEntSchemaEnt2(grenade, kEntity, kOwner, 0));
    if (attribution) g_attributions.Record(fullHandle, *attribution);
    return attribution;
}

void OnSpawned(std::int32_t handle) noexcept {
    if (!g_active) return;
    try {
        auto* grenade = EntityFromHandle(handle);
        if (grenade && std::string_view(s2sdk::GetEntityClassname(handle).c_str()) == "flashbang_projectile")
            (void)Capture(grenade, ToNativeHandle(handle));
    } catch (...) {
        // Unknown attribution leaves the native flash unchanged.
    }
}

void OnDeleted(std::int32_t handle) noexcept {
    if (g_active) g_attributions.Erase(ToNativeHandle(handle));
}

void OnMapEnd() noexcept {
    if (g_active) g_attributions.Clear();
}

#if defined(_WIN32)
constexpr std::int32_t kGrenadeArg = 2;
#else
constexpr std::int32_t kGrenadeArg = 1;
#endif

Result JudgePre(polyhook::HookHandle, polyhook::ParametersHandle args, std::int32_t,
                polyhook::ReturnHandle ret, Phase) noexcept {
    if (!g_active) return Result::Ignored;
    try {
        auto* victim = polyhook::GetArgumentPointer(args, 0);
        auto* grenade = polyhook::GetArgumentPointer(args, kGrenadeArg);
        const auto handle = HandleFromEntity(grenade);
        if (handle == -1) return Result::Ignored;
        const auto fullHandle = ToNativeHandle(handle);
        auto attribution = g_attributions.Find(fullHandle);
        // The thrower may be assigned after the SDK spawn notification.
        if (!attribution) attribution = Capture(grenade, fullHandle);
        const auto player = CurrentPlayer(victim, HandleFromEntity(victim));
        if (!player || !team_flash::ShouldVetoTeamFlash(s2sdk::GetConVarBool(g_ffa),
                player->slot, player->team, player->pawn, attribution)) return Result::Ignored;

        polyhook::SetReturnFloat(ret, 0.0f);
        return Result::Supercede;
    } catch (...) {
        return Result::Ignored;
    }
}

bool Unregister(bool& registered, auto unregister, auto callback) noexcept {
    if (!registered) return true;
    try {
        unregister(callback);
        registered = false;
        return true;
    } catch (...) {
        return false;
    }
}

bool Attach() {
    if (g_active) return true;
    if (!ShutdownTeamFlash() || !Resolve()) return false;
    using T = polyhook::DataType;
    auto slot = DetourSlot{g_entry, g_hook, &g_attempted};
    try {
        if (slot.Acquire(g_judge, [] {
#if defined(_WIN32)
                // Win64 passes the 12-byte origin by pointer; SysV splits it across XMM0/XMM1.
                return polyhook::HookDetour(g_judge, T::Float, {T::Pointer, T::Pointer, T::Pointer}, -1, kJudge.name().data());
#else
                return polyhook::HookDetour(g_judge, T::Float, {T::Pointer, T::Pointer, T::Double, T::Float}, -1, kJudge.name().data());
#endif
            }) && slot.Add(Phase::Pre, JudgePre)) {
            g_spawned = true;
            s2sdk::OnEntitySpawned_Register(OnSpawned);
            g_deleted = true;
            s2sdk::OnEntityDeleted_Register(OnDeleted);
            g_mapEnd = true;
            s2sdk::OnMapEnd_Register(OnMapEnd);
            g_active = true;
            return true;
        }
    } catch (...) {
    }
    (void)ShutdownTeamFlash();
    return false;
}

} // namespace

bool ShutdownTeamFlash() noexcept {
    g_active = false;
    g_attributions.Clear();
    const bool mapClean = Unregister(g_mapEnd, s2sdk::OnMapEnd_Unregister, OnMapEnd);
    const bool deletedClean = Unregister(g_deleted, s2sdk::OnEntityDeleted_Unregister, OnDeleted);
    const bool spawnedClean = Unregister(g_spawned, s2sdk::OnEntitySpawned_Unregister, OnSpawned);
    try {
        auto slot = DetourSlot{g_entry, g_hook, &g_attempted};
        const bool detourClean = slot.Remove(Phase::Pre, JudgePre) && slot.Release();
        return mapClean && deletedClean && spawnedClean && detourClean;
    } catch (...) {
        return false;
    }
}

} // namespace funrounds::native::detail

extern "C" PLUGIN_API bool HasTeamFlashProtectionSupport() noexcept {
    try {
        return funrounds::native::detail::Resolve();
    } catch (...) {
        return false;
    }
}

extern "C" PLUGIN_API bool SetTeamFlashProtection(bool active) noexcept {
    try {
        using namespace funrounds::native::detail;
        return active ? Attach() : ShutdownTeamFlash();
    } catch (...) {
        return false;
    }
}
