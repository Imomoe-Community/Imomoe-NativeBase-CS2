// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "grenade_factory.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <string_view>

#include <plugin_export.h>
#include <s2sdk/s2sdk.hpp>

#include "engine.hpp"
#include "entity_handle.hpp"
#include "fire_rain.hpp"
#include "standard_grenade_policy.hpp"

namespace funrounds::native::detail {
namespace {

using grenade::Kind;
constexpr std::int32_t kInvalid = -1;
using HEFactory = void* (*)(const float*, const float*, const float*, const float*, void*, std::uint16_t);
using SmokeFactory = void* (*)(const float*, const float*, const float*, const float*, void*, std::uint16_t, int);
#if defined(_WIN32)
using MolotovItem = std::uint32_t;
#else
using MolotovItem = std::uint16_t;
#endif
using MolotovFactory = void* (*)(const float*, const float*, const float*, const float*, void*, MolotovItem);

HEFactory g_he{};
SmokeFactory g_smoke{};
MolotovFactory g_molotov{};

bool ResolveFactory(Kind kind) {
    if (!__s2sdk_GetGameConfigSignature || !__s2sdk_IsValidEntHandle || !__s2sdk_EntHandleToEntPointer ||
        !__s2sdk_EntPointerToEntHandle || !__s2sdk_GetEntityClassname || !__s2sdk_GetEntityOwner ||
        !__s2sdk_RemoveEntity || !EnsureEngine()) return false;
    switch (kind) {
        case Kind::He:
            if (!g_he) g_he = reinterpret_cast<HEFactory>(ProbeSignature(SignatureKey{"FunRounds::HEProjectileFactory"}));
            return g_he != nullptr;
        case Kind::Smoke:
            if (!g_smoke) g_smoke = reinterpret_cast<SmokeFactory>(ProbeSignature(SignatureKey{"FunRounds::SmokeProjectileFactory"}));
            return g_smoke && __s2sdk_GetEntSchema2 && __s2sdk_GetSchemaOffset &&
                s2sdk::GetSchemaOffset("CBaseEntity", "m_iTeamNum") >= 0;
        case Kind::Molotov:
            if (!g_molotov) g_molotov = reinterpret_cast<MolotovFactory>(ProbeSignature(SignatureKey{"FunRounds::MolotovProjectileFactory"}));
            return g_molotov != nullptr;
        default:
            return false;
    }
}

bool Finite(const plg::vec3& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool CanCreate(Kind kind, bool forceRainSelfDamage) noexcept {
    try {
        return ResolveFactory(kind) && (!forceRainSelfDamage || (kind == Kind::Molotov && MolotovRainSelfDamageAvailable()));
    } catch (...) {
        return false;
    }
}

std::int32_t Create(Kind kind, const plg::vec3& position, const plg::vec3& velocity,
                    std::int32_t throwerPawn, bool forceRainSelfDamage) noexcept {
    try {
        if (!Finite(position) || !Finite(velocity) || !ResolveFactory(kind)) return kInvalid;
        void* thrower{};
        if (throwerPawn == kInvalid) {
            if (kind != Kind::Molotov || forceRainSelfDamage) return kInvalid;
        } else {
            if (!IsPlayerPawn(throwerPawn)) return kInvalid;
            thrower = EntityFromHandle(throwerPawn);
            if (!thrower || HandleFromEntity(thrower) != throwerPawn) return kInvalid;
        }
        if (forceRainSelfDamage && (kind != Kind::Molotov || !PrepareMolotovRainSelfDamage())) return kInvalid;

        const std::array<float, 3> origin{position.x, position.y, position.z};
        const std::array<float, 3> direction{velocity.x, velocity.y, velocity.z};
        constexpr auto radiansToDegrees = 180.f / std::numbers::pi_v<float>;
        const std::array<float, 3> angles{
            std::atan2(-velocity.z, std::hypot(velocity.x, velocity.y)) * radiansToDegrees,
            std::atan2(velocity.y, velocity.x) * radiansToDegrees, 0.f};
        const std::array<float, 3> angular{};
        void* projectile{};
        switch (kind) {
            case Kind::He:
                projectile = g_he(origin.data(), angles.data(), direction.data(), angular.data(), thrower, 44);
                break;
            case Kind::Smoke: {
                const auto team = static_cast<int>(s2sdk::GetEntSchema2(thrower, "CBaseEntity", "m_iTeamNum", 0));
                projectile = g_smoke(origin.data(), angles.data(), direction.data(), angular.data(), thrower, 45, team);
                break;
            }
            case Kind::Molotov:
                projectile = g_molotov(origin.data(), angles.data(), direction.data(), angular.data(), thrower,
                    MolotovItem{46});
                break;
            default:
                return kInvalid;
        }
        if (!projectile) return kInvalid;
        const auto handle = HandleFromEntity(projectile);
        if (handle == kInvalid || !s2sdk::IsValidEntHandle(handle) || EntityFromHandle(handle) != projectile) return kInvalid;
        const auto classname = s2sdk::GetEntityClassname(handle);
        if (std::string_view(classname.c_str()) != grenade::ProjectileClass(kind) ||
            s2sdk::GetEntityOwner(handle) != throwerPawn ||
            (forceRainSelfDamage && !TrackMolotovRainSelfDamage(handle, throwerPawn))) {
            s2sdk::RemoveEntity(handle);
            return kInvalid;
        }
        return handle;
    } catch (...) {
        return kInvalid;
    }
}

} // namespace

void ResetGrenadeFactories() noexcept {
    g_he = nullptr;
    g_smoke = nullptr;
    g_molotov = nullptr;
}

} // namespace funrounds::native::detail

extern "C" PLUGIN_API bool CanCreateGrenadeProjectile(std::int32_t kind, bool forceRainSelfDamage) noexcept {
    return funrounds::native::detail::CanCreate(static_cast<funrounds::native::grenade::Kind>(kind), forceRainSelfDamage);
}

extern "C" PLUGIN_API std::int32_t CreateGrenadeProjectile(std::int32_t kind, const plg::vec3& position,
                                                          const plg::vec3& velocity, std::int32_t throwerPawn,
                                                          bool forceRainSelfDamage) noexcept {
    return funrounds::native::detail::Create(static_cast<funrounds::native::grenade::Kind>(kind),
                                            position, velocity, throwerPawn, forceRainSelfDamage);
}
