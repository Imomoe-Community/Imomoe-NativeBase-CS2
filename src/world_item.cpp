// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "world_item.hpp"

#include <cmath>
#include <cstdint>
#include <string_view>

#include <mathlib/vector.h>
#include <plugin_export.h>
#include <s2sdk/s2sdk.hpp>

#include "engine.hpp"
#include "standard_grenade_policy.hpp"

namespace funrounds::native::detail {
namespace {

using s2sdk::WeaponDefIndex;
constexpr std::int32_t kInvalid = -1;
using Economy = void* (*)();
using Factory = void* (*)(void*, std::uint32_t, const Vector&, const QAngle&);
using PrimeAmmo = void (*)(void*);

struct WorldItem {
    Factory create{};
    void* economy{};
    PrimeAmmo primeAmmo{};
} g_item;

bool ResolveWorldItem(bool withGrenadeAmmo) {
    if (!g_item.create) {
        if (!__s2sdk_GetGameConfigSignature || !__s2sdk_GetWeaponItemDefinition || !__s2sdk_GetEntityClassname ||
            !__s2sdk_GetEntityOwner || !__s2sdk_RemoveEntity || !EnsureEngine()) {
            return false;
        }
        const auto create = reinterpret_cast<Factory>(ProbeSignature(SignatureKey{"FunRounds::CreateWorldItem"}));
        const auto economy = reinterpret_cast<Economy>(ProbeSignature(SignatureKey{"FunRounds::WorldItemEconomy"}));
        if (!create || !economy) return false;
        g_item.economy = economy();
        if (!g_item.economy) return false;
        g_item.create = create;
    }
    if (withGrenadeAmmo && !g_item.primeAmmo) {
        g_item.primeAmmo = reinterpret_cast<PrimeAmmo>(ProbeSignature(SignatureKey{"FunRounds::PrimeWorldGrenadeAmmo"}));
    }
    return !withGrenadeAmmo || g_item.primeAmmo;
}

std::int32_t SpawnWorldItem(WeaponDefIndex definition, const plg::vec3& position, std::string_view grenadeClass) {
    const bool primeGrenadeAmmo = !grenadeClass.empty();
    if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z) ||
        !ResolveWorldItem(primeGrenadeAmmo)) {
        return kInvalid;
    }
    // Stock grenade-drop split uses this ownerless econ factory, not a
    // naked entity + DispatchSpawn (weapon Spawn needs initialized VData).
    auto* item = g_item.create(g_item.economy, static_cast<std::uint16_t>(definition), Vector{position.x, position.y, position.z}, QAngle{});
    const auto handle = HandleFromEntity(item);
    if (handle == kInvalid) return kInvalid;
    const auto name = s2sdk::GetEntityClassname(handle);
    const std::string_view classname{name.data(), name.size()};
    const bool expectedClass = primeGrenadeAmmo ? classname == grenadeClass : classname == "weapon_knife" || classname == "weapon_knife_t";
    if (!expectedClass || s2sdk::GetEntityOwner(handle) != kInvalid || s2sdk::GetWeaponItemDefinition(handle) != definition) {
        s2sdk::RemoveEntity(handle);
        return kInvalid;
    }
    if (primeGrenadeAmmo) g_item.primeAmmo(item);
    return handle;
}

WeaponDefIndex GrenadeDefinition(grenade::Kind kind) noexcept {
    using grenade::Kind;
    switch (kind) {
        case Kind::He: return WeaponDefIndex::HEGrenade;
        case Kind::Flash: return WeaponDefIndex::Flashbang;
        case Kind::Smoke: return WeaponDefIndex::SmokeGrenade;
        case Kind::Decoy: return WeaponDefIndex::Decoy;
        case Kind::Molotov: return WeaponDefIndex::Molotov;
        case Kind::Incendiary: return WeaponDefIndex::IncGrenade;
    }
    return WeaponDefIndex::Invalid;
}

} // namespace

void ResetWorldItemMetadata() noexcept {
    g_item = {};
}

} // namespace funrounds::native::detail

extern "C" PLUGIN_API bool HasWorldItemFactorySupport(bool withGrenadeAmmo) noexcept {
    try {
        return funrounds::native::detail::ResolveWorldItem(withGrenadeAmmo);
    } catch (...) {
        return false;
    }
}

extern "C" PLUGIN_API std::int32_t CreateWorldItem(std::uint16_t definition, const plg::vec3& position, bool primeGrenadeAmmo) noexcept {
    using namespace funrounds::native;
    using namespace funrounds::native::detail;
    try {
        const auto item = static_cast<WeaponDefIndex>(definition);
        if (primeGrenadeAmmo) {
            if (item != WeaponDefIndex::HEGrenade) return kInvalid;
            return SpawnWorldItem(item, position, grenade::WeaponClass(grenade::Kind::He));
        }
        if (item != WeaponDefIndex::Knife && item != WeaponDefIndex::KnifeT &&
            (item < WeaponDefIndex::Bayonet || item > WeaponDefIndex::KnifeKukri)) {
            return kInvalid;
        }
        return SpawnWorldItem(item, position, {});
    } catch (...) {
        return kInvalid;
    }
}

extern "C" PLUGIN_API std::int32_t CreateGrenadeDud(std::int32_t value, const plg::vec3& position) noexcept {
    using namespace funrounds::native;
    using namespace funrounds::native::detail;
    try {
        if (!grenade::ValidKind(value)) return kInvalid;
        const auto kind = static_cast<grenade::Kind>(value);
        return SpawnWorldItem(GrenadeDefinition(kind), position, grenade::WeaponClass(kind));
    } catch (...) {
        return kInvalid;
    }
}
