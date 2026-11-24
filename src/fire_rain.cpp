// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "fire_rain.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <unordered_map>

#include <plugin_export.h>
#include <plg/plugin.hpp>
#include <polyhook/polyhook.hpp>
#include <s2sdk/s2sdk.hpp>

#include "engine.hpp"
#include "entity_handle.hpp"
#include "standard_grenade.hpp"
#include "standard_grenade_policy.hpp"

namespace funrounds::native::detail {
namespace {

constexpr std::int32_t kInvalid = -1;
constexpr char kInfernoClass[] = "inferno";

struct RainProjectile {
    std::int32_t thrower{kInvalid};
    bool forceSelfDamage{};
};

struct RainInferno {
    std::int32_t thrower{kInvalid};
    bool forceSelfDamage{};
};

struct DetonationScope {
    std::int32_t handle{kInvalid};
    void* entity{};
};

std::unordered_map<std::int32_t, RainProjectile> g_projectiles;
std::unordered_map<std::int32_t, RainInferno> g_infernos;
std::array<DetonationScope, 16> g_detonating{};
std::size_t g_detonationDepth{};
std::size_t g_overflowDepth{};
bool g_startBurnHooked{};
bool g_expireHooked{};
bool g_mapEndRegistered{};
bool g_eventCallbacksActive{};
SchemaField g_ownerField{};
SchemaField g_throwerField{};
bool g_lineageSchemaReady{};

bool HasLineageImports() noexcept {
    return __s2sdk_GetGameConfigVTable != nullptr && __s2sdk_HookEvent != nullptr && __s2sdk_UnhookEvent != nullptr &&
           __s2sdk_GetEventInt != nullptr && __s2sdk_EntIndexToEntHandle != nullptr &&
           __s2sdk_OnMapEnd_Register != nullptr && __s2sdk_OnMapEnd_Unregister != nullptr &&
           __polyhook_HookDetour != nullptr && __polyhook_FindDetour != nullptr &&
           __polyhook_AddCallback != nullptr && __polyhook_IsCallbackRegistered != nullptr &&
           __polyhook_AreCallbacksRegistered != nullptr && __polyhook_RemoveCallback != nullptr &&
           __polyhook_UnhookDetour != nullptr && __polyhook_GetArgumentPointer != nullptr;
}

bool ResolveLineageSchema() noexcept {
    if (g_lineageSchemaReady) return true;
    if (!ResolveSchemaField("CBaseEntity", "m_hOwnerEntity", sizeof(std::uint32_t), g_ownerField) ||
        !ResolveSchemaField("CBaseGrenade", "m_hThrower", sizeof(std::uint32_t), g_throwerField)) {
        return false;
    }
    g_lineageSchemaReady = true;
    return true;
}

bool ReadDetonateTarget() noexcept {
    try {
        if (!HasLineageImports() || !EnsureEngine()) return false;
        const auto slot = ProbeOffset(OffsetKey{"CSGrenadeProjectileBase::Detonate"});
        auto** const table = static_cast<void**>(ProbeVTable(VTableKey{"CMolotovProjectile"}));
        return slot >= 0 && slot < 4096 && table && table[slot] != nullptr;
    } catch (...) {
        return false;
    }
}

bool ValidEntity(std::int32_t handle, void*& entity) noexcept {
    entity = nullptr;
    try {
        if (handle == kInvalid || !s2sdk::IsValidEntHandle(handle)) return false;
        entity = EntityFromHandle(handle);
        return entity && HandleFromEntity(entity) == handle && s2sdk::IsValidEntHandle(handle);
    } catch (...) {
        entity = nullptr;
        return false;
    }
}

bool HasClass(std::int32_t handle, std::string_view expected) noexcept {
    try {
        if (!s2sdk::IsValidEntHandle(handle)) return false;
        const auto name = s2sdk::GetEntityClassname(handle);
        return std::string_view(name.c_str()) == expected;
    } catch (...) {
        return false;
    }
}

std::int32_t InfernoOwner(std::int32_t handle) noexcept {
    try {
        if (!s2sdk::IsValidEntHandle(handle) || !ResolveLineageSchema()) {
            return kInvalid;
        }
        auto* entity = EntityFromHandle(handle);
        if (!entity || HandleFromEntity(entity) != handle) return kInvalid;
        std::uint32_t owner{};
        std::memcpy(&owner, static_cast<const std::byte*>(entity) + g_ownerField.offset, sizeof(owner));
        return ToApiHandle(owner);
    } catch (...) {
        return kInvalid;
    }
}

std::int32_t GrenadeThrower(std::int32_t handle) noexcept {
    try {
        if (!s2sdk::IsValidEntHandle(handle) || !ResolveLineageSchema()) {
            return kInvalid;
        }
        auto* entity = EntityFromHandle(handle);
        if (!entity || HandleFromEntity(entity) != handle) return kInvalid;
        std::uint32_t thrower{};
        std::memcpy(&thrower, static_cast<const std::byte*>(entity) + g_throwerField.offset, sizeof(thrower));
        return ToApiHandle(thrower);
    } catch (...) {
        return kInvalid;
    }
}

std::int32_t CurrentDetonatingProjectile() noexcept {
    if (g_overflowDepth != 0 || g_detonationDepth == 0) return kInvalid;
    return g_detonating[g_detonationDepth - 1].handle;
}

s2sdk::ResultType OnInfernoStart(const plg::string&, void* event, bool) noexcept {
    if (!g_eventCallbacksActive || !event) return s2sdk::ResultType::Continue;
    try {
        const auto projectileHandle = CurrentDetonatingProjectile();
        const auto projectile = g_projectiles.find(projectileHandle);
        if (projectile == g_projectiles.end() || !projectile->second.forceSelfDamage) {
            return s2sdk::ResultType::Continue;
        }
        const auto entityIndex = s2sdk::GetEventInt(event, "entityid");
        const auto infernoHandle = s2sdk::EntIndexToEntHandle(entityIndex);
        if (infernoHandle == kInvalid || !HasClass(infernoHandle, kInfernoClass)) return s2sdk::ResultType::Continue;

        const auto owner = InfernoOwner(infernoHandle);
        if (owner != projectile->second.thrower) return s2sdk::ResultType::Continue;

        g_infernos.insert_or_assign(infernoHandle, RainInferno{owner, true});
    } catch (...) {
    }
    return s2sdk::ResultType::Continue;
}

s2sdk::ResultType OnInfernoExpire(const plg::string&, void* event, bool) noexcept {
    if (!g_eventCallbacksActive || !event) return s2sdk::ResultType::Continue;
    try {
        const auto entityIndex = s2sdk::GetEventInt(event, "entityid");
        const auto infernoHandle = s2sdk::EntIndexToEntHandle(entityIndex);
        if (infernoHandle != kInvalid) g_infernos.erase(infernoHandle);
    } catch (...) {
    }
    return s2sdk::ResultType::Continue;
}

void OnMapEnd() noexcept {
    ClearMolotovRainTracking();
}

bool RemoveEventHooks() noexcept {
    bool clean = true;
    try {
        if (g_expireHooked) {
            if (s2sdk::UnhookEvent("inferno_expire", OnInfernoExpire, s2sdk::HookMode::Post) == s2sdk::EventHookError::Okay) {
                g_expireHooked = false;
            } else {
                clean = false;
            }
        }
        if (g_startBurnHooked) {
            if (s2sdk::UnhookEvent("inferno_startburn", OnInfernoStart, s2sdk::HookMode::Post) == s2sdk::EventHookError::Okay) {
                g_startBurnHooked = false;
            } else {
                clean = false;
            }
        }
        if (g_mapEndRegistered) {
            s2sdk::OnMapEnd_Unregister(OnMapEnd);
            g_mapEndRegistered = false;
        }
    } catch (...) {
        clean = false;
    }
    return clean;
}

bool EnsureLineageCallbacks() noexcept {
    if (g_eventCallbacksActive) return true;
    if (!HasLineageImports() || !ReadDetonateTarget()) return false;
    try {
        if (!g_startBurnHooked) {
            if (s2sdk::HookEvent("inferno_startburn", OnInfernoStart, s2sdk::HookMode::Post) !=
                s2sdk::EventHookError::Okay) return false;
            g_startBurnHooked = true;
        }
        if (!g_expireHooked) {
            if (s2sdk::HookEvent("inferno_expire", OnInfernoExpire, s2sdk::HookMode::Post) !=
                s2sdk::EventHookError::Okay) {
                (void)RemoveEventHooks();
                return false;
            }
            g_expireHooked = true;
        }
        if (!g_mapEndRegistered) {
            s2sdk::OnMapEnd_Register(OnMapEnd);
            g_mapEndRegistered = true;
        }
        if (!EnableRainMolotovLineage()) {
            (void)RemoveEventHooks();
            return false;
        }
        g_eventCallbacksActive = true;
        return true;
    } catch (...) {
        (void)RemoveEventHooks();
        return false;
    }
}

} // namespace

bool MolotovRainSelfDamageAvailable() noexcept {
    return ReadDetonateTarget() && ResolveLineageSchema();
}

bool PrepareMolotovRainSelfDamage() noexcept {
    return MolotovRainSelfDamageAvailable() && EnsureLineageCallbacks();
}

bool TrackMolotovRainSelfDamage(std::int32_t projectile, std::int32_t thrower) noexcept {
    if (GrenadeThrower(projectile) != thrower) return false;
    try {
        g_projectiles.insert_or_assign(projectile, RainProjectile{thrower, true});
        return true;
    } catch (...) {
        return false;
    }
}

bool ShouldBypassSelfDamageForRain(std::int32_t inflictorHandle,
                                   std::int32_t victimPawnHandle) noexcept {
    if (inflictorHandle == kInvalid || victimPawnHandle == kInvalid) return false;
    try {
        void* entity{};
        if (!ValidEntity(inflictorHandle, entity) || !HasClass(inflictorHandle, kInfernoClass) ||
            !IsPlayerPawn(victimPawnHandle)) return false;
        const auto found = g_infernos.find(inflictorHandle);
        if (found == g_infernos.end() || !found->second.forceSelfDamage || found->second.thrower != victimPawnHandle) {
            return false;
        }
        return InfernoOwner(inflictorHandle) == victimPawnHandle;
    } catch (...) {
        return false;
    }
}

void ClearMolotovRainTracking() noexcept {
    g_projectiles.clear();
    g_infernos.clear();
    g_detonationDepth = 0;
    g_overflowDepth = 0;
}

bool ShutdownMolotovRain() noexcept {
    g_eventCallbacksActive = false;
    ClearMolotovRainTracking();
    const bool clean = RemoveEventHooks();
    if (clean) {
        g_ownerField = {};
        g_throwerField = {};
        g_lineageSchemaReady = false;
    }
    return clean;
}

void BeginRainMolotovDetonation(std::int32_t projectileHandle, void* entity) noexcept {
    if (!g_eventCallbacksActive || !entity) return;
    // Even an untracked Molotov masks an outer rain scope if Detonate nests.
    if (g_overflowDepth != 0) {
        ++g_overflowDepth;
        return;
    }
    if (g_detonationDepth == g_detonating.size()) {
        g_overflowDepth = 1;
        return;
    }
    g_detonating[g_detonationDepth++] = DetonationScope{projectileHandle, entity};
}

void EndRainMolotovDetonation(void* entity) noexcept {
    if (g_overflowDepth != 0) {
        --g_overflowDepth;
        return;
    }
    for (std::size_t index = g_detonationDepth; index > 0; --index) {
        const auto slot = index - 1;
        if (g_detonating[slot].entity != entity) continue;
        g_projectiles.erase(g_detonating[slot].handle);
        for (std::size_t move = slot + 1; move < g_detonationDepth; ++move) {
            g_detonating[move - 1] = g_detonating[move];
        }
        --g_detonationDepth;
        return;
    }
}

} // namespace funrounds::native::detail

extern "C" PLUGIN_API bool ShouldBypassSelfDamageForRain(std::int32_t inflictorHandle,
                                                         std::int32_t victimPawnHandle) noexcept {
    return funrounds::native::detail::ShouldBypassSelfDamageForRain(inflictorHandle, victimPawnHandle);
}

extern "C" PLUGIN_API void ClearMolotovRainTracking() noexcept {
    funrounds::native::detail::ClearMolotovRainTracking();
}
