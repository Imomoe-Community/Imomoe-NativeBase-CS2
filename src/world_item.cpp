// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "world_item.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string_view>
#include <vector>

#include <const.h>
#include <engine2/igameeventsystem.h>
#include <google/protobuf/message_lite.h>
#include <mathlib/vector.h>
#include <networkstringtabledefs.h>
#include <plugin_export.h>
#include <s2sdk/s2sdk.hpp>
#include <soundsystem/soundflags.h>
#include <tier0/bufferstring.h>
#include <tier0/utlmap.h>

#include "engine.hpp"
#include "entity_handle.hpp"
#include "movement_trace.hpp"
#include "standard_grenade_policy.hpp"
#include "target_attack.hpp"

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
    int primaryAttackSlot{-1};
} g_item;

// WeaponSound_t::WEAPON_SOUND_SINGLE in the stock weapon VData sound map.
constexpr std::uint32_t kSingleSound = 2;
using ShootSounds = CUtlOrderedMap<std::uint32_t, CBufferString>;
SchemaField g_subclass{};
SchemaField g_shootSounds{};
std::int16_t g_fireBulletsMessage{};
bool g_impactHooked{};
INetworkStringTableContainer* g_effectTables{};
IGameEventSystem* g_effectEvents{};
INetworkSerializerPB* g_effectMessage{};

struct ShotBroadcast {
    std::uint32_t weapon;
    std::uint32_t sound{kSingleSound};
    std::int32_t slot{-1};
    plg::vec3 muzzle{};
    std::uint64_t recipients{};
    bool intercepted{};
    std::vector<Vector> points;
};
thread_local ShotBroadcast* g_stockShot{};

std::uint32_t NetworkHandle(std::int32_t handle) noexcept {
    const auto full = ToNativeHandle(handle);
    constexpr auto serialMask = (1u << NUM_NETWORKED_EHANDLE_SERIAL_NUMBER_BITS) - 1;
    return (full & ((1u << NUM_SERIAL_NUM_SHIFT_BITS) - 1)) |
           (((full >> NUM_SERIAL_NUM_SHIFT_BITS) & serialMask) << MAX_EDICT_BITS);
}

s2sdk::ResultType RecordStockImpact(const plg::string&, void* event, bool) noexcept {
    if (!g_stockShot) return s2sdk::ResultType::Continue;
    try {
        if (s2sdk::GetEventPlayerSlot(event, "userid") == g_stockShot->slot) {
            const Vector point{s2sdk::GetEventFloat(event, "x"), s2sdk::GetEventFloat(event, "y"),
                               s2sdk::GetEventFloat(event, "z")};
            auto& points = g_stockShot->points;
            if (std::ranges::find(points, point) == points.end()) points.push_back(point);
        }
    } catch (...) {
        // Impact feedback never changes the result of a dispatched shot.
    }
    return s2sdk::ResultType::Continue;
}

void ResolveImpactFeedback() noexcept {
    if (g_impactHooked || !__s2sdk_GetEventPlayerSlot || !__s2sdk_GetEventFloat || !__s2sdk_HookEvent ||
        !__s2sdk_UnhookEvent || !__s2sdk_GetClientEyePosition || !__s2sdk_UserMessageGetRecipientMask) return;
    try {
        g_effectTables = static_cast<INetworkStringTableContainer*>(
            s2sdk::QueryInterface("engine2", INTERFACENAME_NETWORKSTRINGTABLESERVER));
        g_effectEvents = static_cast<IGameEventSystem*>(
            s2sdk::QueryInterface("engine2", GAMEEVENTSYSTEM_INTERFACE_VERSION));
        auto* messages = static_cast<INetworkMessages*>(
            s2sdk::QueryInterface("networksystem", NETWORKMESSAGES_INTERFACE_VERSION));
        g_effectMessage = messages ? messages->FindNetworkMessagePartial("CMsgTEEffectDispatch") : nullptr;
        if (!g_effectTables || !g_effectEvents || !g_effectMessage) return;
        g_impactHooked = s2sdk::HookEvent("bullet_impact", RecordStockImpact, s2sdk::HookMode::Post) ==
                         s2sdk::EventHookError::Okay;
    } catch (...) {
    }
}

// te.proto CMsgTEEffectDispatch/CMsgEffectData. Let the game parse the
// game-allocated message instead of populating its nested reflection data
// through the provider's statically linked protobuf.
bool FillImpactMessage(CNetMessage& msg, const WorldShotContact& contact, std::uint32_t effectName) {
    constexpr std::uint32_t varint = 0, bytes = 2, fixed32 = 5;
    const auto tag = [](std::uint32_t field, std::uint32_t wire) { return (field << 3) | wire; };
    alignas(4) std::array<std::byte, 80> effectData{};
    bf_write effect(effectData.data(), static_cast<int>(effectData.size()));
    const auto vector = [&](std::uint32_t field, const Vector& v) {
        effect.WriteVarInt32(tag(field, bytes));
        effect.WriteVarInt32(3 * (1 + sizeof(float))); // CMsgVector: three fixed32 fields.
        for (int i = 0; i < 3; ++i) {
            effect.WriteVarInt32(tag(static_cast<std::uint32_t>(i + 1), fixed32));
            effect.WriteUBitLong(std::bit_cast<std::uint32_t>(v[i]), 32);
        }
    };
    vector(1, contact.position);
    vector(2, contact.start);
    vector(3, contact.normal);
    effect.WriteVarInt32(tag(5, fixed32));   // entity
    effect.WriteUBitLong(NetworkHandle(contact.entity), 32);
    effect.WriteVarInt32(tag(10, fixed32));  // surfaceprop
    effect.WriteUBitLong(contact.surface, 32);
    effect.WriteVarInt32(tag(12, varint));   // damagetype
    effect.WriteVarInt32(1u << 1);           // DMG_BULLET: visual only
    effect.WriteVarInt32(tag(18, varint));   // effectname
    effect.WriteVarInt32(effectName);

    alignas(4) std::array<std::byte, 84> messageData{};
    bf_write message(messageData.data(), static_cast<int>(messageData.size()));
    message.WriteVarInt32(tag(1, bytes));   // effectdata
    message.WriteVarInt32(static_cast<std::uint32_t>(effect.GetNumBytesWritten()));
    message.WriteBytes(effectData.data(), effect.GetNumBytesWritten());
    auto* proto = const_cast<google::protobuf::MessageLite*>(msg.AsMessageLite());
    return proto && proto->ParseFromArray(messageData.data(), message.GetNumBytesWritten());
}

void ShowStockWallContacts(std::int32_t shooter, const ShotBroadcast& shot) noexcept {
    if (!shot.intercepted || shot.points.empty()) return;
    try {
        auto* table = g_effectTables->FindTable("EffectDispatch");
        const auto impact = table ? table->FindStringIndex("Impact") : -1;
        // The client resolves this field through its replicated string table.
        // Don't invent an index or send an effect before a stock entry exists.
        if (impact <= 0) return;
        const Vector muzzle{shot.muzzle.x, shot.muzzle.y, shot.muzzle.z};
        uint64 recipients = shot.recipients;
        // The real shooter does not predict the temporary weapon locally.
        if (shot.slot >= 0 && shot.slot < 64) recipients |= 1ull << shot.slot;
        for (const auto& point : shot.points) {
            WorldShotContact contact{};
            if (!TraceWorldShotContact(shooter, muzzle, point, contact)) continue;
                    std::unique_ptr<CNetMessage> msg(g_effectMessage->AllocateMessage());
            if (!msg || !FillImpactMessage(*msg, contact, static_cast<std::uint32_t>(impact))) return;
            g_effectEvents->PostEventAbstract(CSplitScreenSlot{-1}, false, std::popcount(recipients), &recipients,
                                              g_effectMessage, msg.get(), 0, BUF_UNRELIABLE);
        }
    } catch (...) {
        // No rendering failure can un-fire a round or retry the stock command.
    }
}

s2sdk::ResultType BlockTemporaryShotMessage(void* message) noexcept {
    if (!g_stockShot) return s2sdk::ResultType::Continue;
    std::uint32_t source{};
    if (!s2sdk::PbGetUInt32(message, "weapon_id", source)) {
        // A message without a readable weapon reference is unusable during this shot.
        return s2sdk::ResultType::Handled;
    }
    if (source != g_stockShot->weapon) return s2sdk::ResultType::Continue;
    if (g_impactHooked) {
        g_stockShot->intercepted = true;
        g_stockShot->recipients = s2sdk::UserMessageGetRecipientMask(message);
    }
    std::int32_t sound{};
    if (s2sdk::PbGetInt32(message, "sound_type", sound) && sound >= 0)
        g_stockShot->sound = static_cast<std::uint32_t>(sound);
    return s2sdk::ResultType::Handled;
}

bool ResolveShotBroadcast() {
    if (g_fireBulletsMessage) return true;
    if (!__s2sdk_HookUserMessage || !__s2sdk_UnhookUserMessage || !__s2sdk_UserMessageFindMessageIdByName ||
        !__s2sdk_PbGetUInt32 || !__s2sdk_PbGetInt32 || !__s2sdk_EmitSound ||
        !ResolveSchemaField("CBaseEntity", "m_nSubclassID", sizeof(std::uint32_t), g_subclass) ||
        !ResolveSchemaField("CBasePlayerWeaponVData", "m_aShootSounds", sizeof(ShootSounds), g_shootSounds)) return false;
    const auto message = s2sdk::UserMessageFindMessageIdByName("CMsgTEFireBullets");
    if (!message || !s2sdk::HookUserMessage(message, BlockTemporaryShotMessage, s2sdk::HookMode::Pre)) return false;
    g_fireBulletsMessage = message;
    return true;
}

const ShootSounds* ReadShootSounds(const void* weapon) noexcept {
    // CBaseEntity::GetVData in the paired SDK: the VData pointer follows the subclass token.
    const std::byte* vdata{};
    std::memcpy(&vdata, static_cast<const std::byte*>(weapon) + g_subclass.offset + g_subclass.size, sizeof(vdata));
    return vdata ? reinterpret_cast<const ShootSounds*>(vdata + g_shootSounds.offset) : nullptr;
}

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

bool ResolveNativeWeaponShot() {
    if (!__s2sdk_GetGameConfigOffset || !__s2sdk_GetEntityAbsOrigin || !__s2sdk_GetEntityEyeAngles ||
        !__s2sdk_GetWeaponType || !__s2sdk_SetEntityOwner ||
        !__s2sdk_GetEntSchema2 || !ResolveWorldItem(false)) return false;
    if (g_item.primaryAttackSlot < 0)
        g_item.primaryAttackSlot = ProbeOffset(OffsetKey{"FunRounds::WeaponPrimaryAttackSlot"});
    if (g_item.primaryAttackSlot < 0 || !ResolveShotBroadcast()) return false;
    ResolveImpactFeedback();
    return true;
}

bool IsHitscanWeapon(s2sdk::CSWeaponType type) noexcept {
    using enum s2sdk::CSWeaponType;
    switch (type) {
        case Pistol:
        case SubmachineGun:
        case Rifle:
        case Shotgun:
        case SniperRifle:
        case MachineGun:
            return true;
        default:
            return false;
    }
}

bool FireStockShot(std::int32_t shooter, WeaponDefIndex definition) {
    const auto slot = NativeWeaponCommandSlot(shooter);
    if (slot < 0 || g_stockShot || !ResolveNativeWeaponShot() || !IsPlayerPawn(shooter)) return false;
    if (definition == WeaponDefIndex::Invalid) return false;
    auto* pawn = EntityFromHandle(shooter);
    if (!pawn || s2sdk::GetEntSchema2(pawn, "CBaseEntity", "m_lifeState", 0) != 0 ||
        s2sdk::GetEntSchema2(pawn, "CBaseEntity", "m_iHealth", 0) <= 0) return false;

    const auto position = s2sdk::GetEntityAbsOrigin(shooter);
    const auto angles = s2sdk::GetEntityEyeAngles(shooter);
    auto* weapon = g_item.create(g_item.economy, static_cast<std::uint16_t>(definition),
        Vector{position.x, position.y, position.z}, QAngle{angles.x, angles.y, angles.z});
    const auto handle = HandleFromEntity(weapon);
    if (handle == kInvalid) return false;
    struct RetireShotSource {
        std::int32_t handle;
        ~RetireShotSource() { if (EntityFromHandle(handle)) s2sdk::RemoveEntity(handle); }
    } retire{handle};

    const auto classname = s2sdk::GetEntityClassname(handle);
    if (!std::string_view{classname.data(), classname.size()}.starts_with("weapon_") ||
        s2sdk::GetWeaponItemDefinition(handle) != definition || !IsHitscanWeapon(s2sdk::GetWeaponType(handle))) return false;
    const auto clip = [weapon] { return s2sdk::GetEntSchema2(weapon, "CBasePlayerWeapon", "m_iClip1", 0); };
    const auto before = clip();
    if (before <= 0) return false;
    const auto* sounds = ReadShootSounds(weapon);
    const auto* single = sounds ? sounds->FindGetPtr(kSingleSound) : nullptr;
    if (!single || !single->Length()) return false;
    s2sdk::SetEntityOwner(handle, shooter);
    if (s2sdk::GetEntityOwner(handle) != shooter) return false;

    // Native PrimaryAttack builds the firing context and takes one cartridge from
    // this temporary weapon. It is owned for attribution, never equipped.
    using PrimaryAttack = void (*)(void*);
    const auto fire = reinterpret_cast<PrimaryAttack>((*static_cast<void***>(weapon))[g_item.primaryAttackSlot]);
    ShotBroadcast broadcast{};
    broadcast.weapon = NetworkHandle(handle);
    broadcast.slot = slot;
    if (g_impactHooked) broadcast.muzzle = s2sdk::GetClientEyePosition(slot);
    {
        struct ScopedBroadcast {
            ShotBroadcast* previous{g_stockShot};
            ~ScopedBroadcast() { g_stockShot = previous; }
        } scope;
        g_stockShot = &broadcast;
        fire(weapon);
    }
    if (EntityFromHandle(handle) != weapon || clip() != before - 1) return false;
    if (EntityFromHandle(shooter) == pawn) {
        const auto* sound = sounds->FindGetPtr(broadcast.sound);
        s2sdk::EmitSound(shooter, sound && sound->Length() ? sound->Get() : single->Get(), PITCH_NORM, 1.0f, 0.0f);
        ShowStockWallContacts(shooter, broadcast);
    }
    return true;
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

bool ShutdownWorldItem() noexcept {
    try {
        if (g_impactHooked) {
            if (s2sdk::UnhookEvent("bullet_impact", RecordStockImpact, s2sdk::HookMode::Post) != s2sdk::EventHookError::Okay)
                return false;
        }
        g_impactHooked = false;
        g_effectTables = nullptr;
        g_effectEvents = nullptr;
        g_effectMessage = nullptr;
        if (g_fireBulletsMessage && !s2sdk::UnhookUserMessage(g_fireBulletsMessage, BlockTemporaryShotMessage, s2sdk::HookMode::Pre))
            return false;
        g_fireBulletsMessage = 0;
        g_item = {};
        return true;
    } catch (...) {
        return false;
    }
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

extern "C" PLUGIN_API bool HasNativeWeaponShotSupport() noexcept {
    try {
        return funrounds::native::detail::HasNativeWeaponCommand() && funrounds::native::detail::ResolveNativeWeaponShot();
    } catch (...) {
        return false;
    }
}

extern "C" PLUGIN_API bool FireNativeWeaponShot(std::int32_t shooter, std::uint16_t definition) noexcept {
    try {
        return funrounds::native::detail::FireStockShot(shooter, static_cast<s2sdk::WeaponDefIndex>(definition));
    } catch (...) {
        return false;
    }
}
