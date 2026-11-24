// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "engine.hpp"

#include <baseentity.h>
#include <plugin_export.h>

class CBasePlayerController;
class CBasePlayerPawn;

namespace {

using namespace funrounds::native::detail;

void* SchemaEntity(void* entity, const plg::string& name, const plg::string& field) {
    return EntityFromHandle(s2sdk::GetEntSchemaEnt2(entity, name, field, 0));
}

} // namespace

extern "C" PLUGIN_API void ForceRespawnClient(std::int32_t playerSlot) {
    if (playerSlot < 0 || playerSlot >= s2sdk::GetMaxClients()) return;

    const auto controllerHandle = s2sdk::EntIndexToEntHandle(playerSlot + 1);
    if (!IsPlayerController(controllerHandle)) return;
    auto* const controller = EntityFromHandle(controllerHandle);
    if (!controller) return;

    auto* const current = SchemaEntity(controller, "CBasePlayerController", "m_hPawn");
    auto* const player = SchemaEntity(controller, "CCSPlayerController", "m_hPlayerPawn");
    // S2SDK's IsEntityPlayerPawn also accepts an observer pawn.
    const bool currentIsPlayer = current && s2sdk::GetEntityClassname(HandleFromEntity(current)) == "player";
    auto* const pawn = currentIsPlayer ? current : player;
    if (!pawn) return;

    auto* const owner = SchemaEntity(pawn, "CBasePlayerPawn", "m_hController");
    if (currentIsPlayer) {
        if (owner != controller || (player && player != pawn)) return;
    } else {
        if (static_cast<CBaseEntityVTable*>(pawn)->IsAlive() || (owner && owner != controller)) return;
        // A dead pawn without a current owner can only be restored from this controller's own observer.
        if (!owner && (!current ||
                       current != SchemaEntity(controller, "CCSPlayerController", "m_hObserverPawn") ||
                       SchemaEntity(current, "CBasePlayerPawn", "m_hController") != controller ||
                       SchemaEntity(controller, "CCSPlayerController", "m_hOriginalControllerOfCurrentPawn") != controller ||
                       SchemaEntity(pawn, "CCSPlayerPawn", "m_hOriginalController") != controller)) return;
    }

    if (!EnsureEngine()) return;
    using SetPawn = void (*)(CBasePlayerController*, CBasePlayerPawn*, bool, bool, bool);
    const auto setPawn = reinterpret_cast<SetPawn>(ProbeSignature(SignatureKey{"CBasePlayerController::SetPawn"}));
    const auto respawnSlot = ProbeOffset(OffsetKey{"CBasePlayerController::RoundRespawn"});
    if (!setPawn || respawnSlot < 0) return;

    auto* const typedController = static_cast<CBasePlayerController*>(controller);
    setPawn(typedController, static_cast<CBasePlayerPawn*>(pawn), true, false, false);
    using RoundRespawn = void (*)(CBasePlayerController*);
    reinterpret_cast<RoundRespawn>((*static_cast<void***>(controller))[respawnSlot])(typedController);
}
