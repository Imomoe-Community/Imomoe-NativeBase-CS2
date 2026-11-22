// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <atomic>
#include <bit>
#include <cstdint>
#include <array>
#include <string>
#include <string_view>

#include <const.h>
#include <polyhook/polyhook.hpp>
#include <plg/plugin.hpp>
#include <plugin_export.h>
#include <imomoe_native_base/imomoe_native_base.hpp>
#include <s2sdk/s2sdk.hpp>

namespace {

constexpr std::int32_t kFixtureEntityHandle = 17;
constexpr std::int32_t kChickenHandle = std::bit_cast<std::int32_t>(0x80000016U);
constexpr std::int32_t kStaleChickenHandle = 22;
constexpr std::int32_t kProjectileHandle = std::bit_cast<std::int32_t>(0x90000017U);
constexpr std::int32_t kStaleProjectileHandle = 23;
constexpr std::int32_t kSerialWeaponHandle = std::bit_cast<std::int32_t>(0xA0000012U);
constexpr std::int32_t kIgniteFlameHandle = 24;
constexpr std::int32_t kSmokeGrenadeKind = 1;
constexpr std::int32_t kMolotovGrenadeKind = 2;
constexpr std::int32_t kIncendiaryGrenadeKind = 3;
constexpr std::int32_t kHeGrenadeKind = 4;
constexpr std::int32_t kSmokeGrenadeHandle = std::bit_cast<std::int32_t>(0x00018030U);
constexpr std::int32_t kReusedSmokeGrenadeHandle = std::bit_cast<std::int32_t>(0x00020030U);
constexpr std::int32_t kMolotovGrenadeHandle = std::bit_cast<std::int32_t>(0x00018031U);
constexpr std::int32_t kHeGrenadeHandle = std::bit_cast<std::int32_t>(0x00018033U);
constexpr std::int32_t kMissingGrenadeHandle = std::bit_cast<std::int32_t>(0x00038032U);
std::atomic<std::int32_t> g_touchCallbacks{0};
std::atomic<std::int32_t> g_secondaryTouchCallbacks{0};
std::atomic<std::int32_t> g_externalTouchCallbacks{0};
std::atomic<std::int32_t> g_callbackEntity{-1};
std::atomic<std::int32_t> g_callbackOther{-1};
bool g_hooked = false;
bool g_secondaryHooked = false;
bool g_externalTouchHooked = false;
polyhook::HookHandle g_externalTouchHook = nullptr;
void* g_externalTouchAddress = nullptr;
bool g_damageHooked = false;
bool g_entityDamageHooked = false;
std::int32_t g_damageDepth = 0;
std::int32_t g_damageCallbacks = 0;
std::int32_t g_nestedDamageResult = 0;

struct DamageObservation {
    std::int32_t victim = -1;
    std::int32_t attacker = -1;
    std::int32_t inflictor = -1;
    std::int32_t weaponKind = -1;
    std::int32_t hitGroup = -1;
    bool tracedHead = false;
    float damage = 0.0f;
    std::uint32_t damageType = 0;
    bool markHeadshot = false;
    std::int32_t weapon = -1;
    bool skipPluginRules = false;
    std::uint64_t frameToken = 0;
};

DamageObservation g_outerDamage;
DamageObservation g_nestedDamage;

struct EntityDamageObservation {
    std::int32_t victim = -1;
    std::int32_t attacker = -1;
    std::int32_t inflictor = -1;
    std::int32_t ability = -1;
    float damage = 0.0f;
    std::uint32_t damageType = 0;
    bool skipPluginRules = false;
    std::uint64_t frameToken = 0;
};

std::array<EntityDamageObservation, 8> g_entityDamageObservations{};
std::int32_t g_entityDamageCallbacks = 0;

std::int32_t TouchCallback(std::int32_t entityHandle, std::int32_t otherHandle) {
    g_callbackEntity.store(entityHandle, std::memory_order_relaxed);
    g_callbackOther.store(otherHandle, std::memory_order_relaxed);
    g_touchCallbacks.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

std::int32_t SecondaryTouchCallback(std::int32_t entityHandle, std::int32_t otherHandle) {
    if (entityHandle == kFixtureEntityHandle && otherHandle == kFixtureEntityHandle) {
        g_secondaryTouchCallbacks.fetch_add(1, std::memory_order_relaxed);
    }
    return 1;
}

polyhook::ResultType ExternalTouchCallback(polyhook::HookHandle,
                                           polyhook::ParametersHandle parameters,
                                           std::int32_t,
                                           polyhook::ReturnHandle,
                                           polyhook::CallbackType type) {
    if (type != polyhook::CallbackType::Pre || parameters == nullptr ||
        __polyhook_GetArgumentPointer == nullptr || __s2sdk_EntPointerToEntHandle == nullptr) {
        return polyhook::ResultType::Ignored;
    }
    void* const entity = polyhook::GetArgument<void*>(parameters, 0);
    void* const other = polyhook::GetArgument<void*>(parameters, 1);
    const auto entityHandle = entity ? s2sdk::EntPointerToEntHandle(entity) : -1;
    const auto otherHandle = other ? s2sdk::EntPointerToEntHandle(other) : -1;
    if (entityHandle == kFixtureEntityHandle && otherHandle == kFixtureEntityHandle) {
        g_externalTouchCallbacks.fetch_add(1, std::memory_order_relaxed);
    }
    return polyhook::ResultType::Supercede;
}

float FixtureBigHeadScale(void* pawn) {
    return static_cast<float>(s2sdk::GetEntSchemaFloat2(pawn, "CCSPlayerPawn", "m_flBigHeadScale", 0));
}

std::uint32_t FixtureMutators(void* rules) {
    return static_cast<std::uint32_t>(s2sdk::GetEntSchema2(rules, "CCSGameRules", "m_nActiveMutators", 0));
}

void SetFixtureMutators(void* rules, std::uint32_t value) {
    s2sdk::SetEntSchema2(rules, "CCSGameRules", "m_nActiveMutators", static_cast<std::int64_t>(value), true, 0);
}

void SetFixtureBigHeadScale(void* pawn, float value) {
    s2sdk::SetEntSchemaFloat2(pawn, "CCSPlayerPawn", "m_flBigHeadScale", static_cast<double>(value), true, 0);
}

std::string_view ExerciseBigHeadManager() {
    constexpr std::uint32_t kBigHeadBit = 1U;
    constexpr std::uint32_t kOtherMutatorBits = 0x28U;
    constexpr float kOriginalScale = 1.25f;
    auto* const pawn = s2sdk::EntHandleToEntPointer(kFixtureEntityHandle);
    auto* const rules = s2sdk::GetGameRules();
    auto* const resetAddress = s2sdk::GetGameConfigSignature(s2sdk::ConfigId{1}, "FunRounds::BigHeadScaleReset");
    if (pawn == nullptr || rules == nullptr || resetAddress == nullptr) {
        return "The fixture did not resolve the production Big Head Pawn, GameRules, or reset target";
    }
    const auto reset = reinterpret_cast<void (*)(void*)>(resetAddress);

    SetFixtureMutators(rules, kOtherMutatorBits);
    SetFixtureBigHeadScale(pawn, kOriginalScale);
    if (!imomoe_native_base::HasBigHeadScaleSupport()) {
        return "Production Big Head capability did not resolve the fixture schema and reset contract";
    }
    if (!imomoe_native_base::SetBigHeadScaleTarget(kFixtureEntityHandle, 2.0f) ||
        FixtureBigHeadScale(pawn) != 2.0f || FixtureMutators(rules) != (kOtherMutatorBits | kBigHeadBit)) {
        return "Production Big Head did not install its target and own only mutator bit 0";
    }

    const auto resetCount = s2sdk::NativeFixtureBigHeadResetCallCount();
    reset(pawn);
    if (s2sdk::NativeFixtureBigHeadResetCallCount() != resetCount + 1 || FixtureBigHeadScale(pawn) != 2.0f) {
        return "Production Big Head Post did not restore target scale after the original Pawn reset";
    }

    SetFixtureBigHeadScale(pawn, 3.25f);
    if (!imomoe_native_base::ClearBigHeadScaleTarget(kFixtureEntityHandle) || FixtureBigHeadScale(pawn) != 3.25f ||
        FixtureMutators(rules) != kOtherMutatorBits) {
        return "Big Head Clear overwrote an external scale or failed to restore its owned mutator bit";
    }
    SetFixtureBigHeadScale(pawn, kOriginalScale);
    reset(pawn);
    if (s2sdk::NativeFixtureBigHeadResetCallCount() != resetCount + 2 || FixtureBigHeadScale(pawn) != 1.0f) {
        return "The original Big Head reset did not resume after the production callback detached";
    }

    // Begin while another system owns mutator bit 0. Clearing its bit must
    // fault-latch support, and neither Has nor a later Set may reclaim it.
    SetFixtureMutators(rules, kOtherMutatorBits | kBigHeadBit);
    SetFixtureBigHeadScale(pawn, kOriginalScale);
    if (!imomoe_native_base::SetBigHeadScaleTarget(kFixtureEntityHandle, 2.5f) ||
        FixtureMutators(rules) != (kOtherMutatorBits | kBigHeadBit)) {
        return "Big Head did not preserve a mutator bit that was already owned externally";
    }
    reset(pawn);
    if (FixtureBigHeadScale(pawn) != 2.5f) {
        return "Big Head did not apply its scale through the production reset callback with a foreign bit lease";
    }
    SetFixtureMutators(rules, kOtherMutatorBits);
    if (imomoe_native_base::HasBigHeadScaleSupport() ||
        imomoe_native_base::SetBigHeadScaleTarget(kFixtureEntityHandle, 3.5f) ||
        FixtureMutators(rules) != kOtherMutatorBits) {
        return "Lost foreign mutator ownership did not disable Big Head without reclaiming the bit";
    }
    if (!imomoe_native_base::ClearBigHeadScaleTarget(kFixtureEntityHandle) || FixtureBigHeadScale(pawn) != kOriginalScale ||
        FixtureMutators(rules) != kOtherMutatorBits) {
        return "Clear did not restore the still-owned scale while leaving the foreign mutator bit untouched";
    }

    // A failed final clear must keep map-end recovery subscribed. A same-map
    // retry can still restore the original mutator bit before the map changes.
    SetFixtureMutators(rules, kOtherMutatorBits);
    SetFixtureBigHeadScale(pawn, kOriginalScale);
    if (!imomoe_native_base::SetBigHeadScaleTarget(kFixtureEntityHandle, 2.75f)) {
        return "Big Head could not start the mutator restore-retry fixture";
    }
    s2sdk::NativeFixtureFailMutatorWrites(true);
    const bool firstClear = imomoe_native_base::ClearBigHeadScaleTarget(kFixtureEntityHandle);
    s2sdk::NativeFixtureFailMutatorWrites(false);
    if (firstClear || FixtureMutators(rules) != (kOtherMutatorBits | kBigHeadBit) ||
        !imomoe_native_base::ClearBigHeadScaleTarget(kFixtureEntityHandle) || FixtureMutators(rules) != kOtherMutatorBits) {
        return "Big Head did not retain and retry a failed final mutator-bit restore";
    }

    // Map-end is the safe boundary for discarding the old GameRules lease.
    // The fixture reuses its GameRules address, so any old snapshot write at
    // this point would clear the new map's independently-owned bit.
    SetFixtureBigHeadScale(pawn, kOriginalScale);
    if (!imomoe_native_base::SetBigHeadScaleTarget(kFixtureEntityHandle, 2.75f)) {
        return "Big Head could not start the cross-map mutator restore fixture";
    }
    s2sdk::NativeFixtureFailMutatorWrites(true);
    const bool failedMapClear = imomoe_native_base::ClearBigHeadScaleTarget(kFixtureEntityHandle);
    s2sdk::NativeFixtureFailMutatorWrites(false);
    if (failedMapClear || FixtureMutators(rules) != (kOtherMutatorBits | kBigHeadBit)) {
        return "Big Head did not preserve the failed restore snapshot until the map boundary";
    }
    SetFixtureMutators(rules, 0x41U);
    s2sdk::NativeFixtureFireMapEnd();
    if (!imomoe_native_base::ClearBigHeadScaleTarget(kFixtureEntityHandle) || FixtureMutators(rules) != 0x41U) {
        return "A retained Big Head map-end recovery changed the new map's reused GameRules object";
    }
    return {};
}

std::string_view ExerciseStandardGrenadeDetonate() {
    if (imomoe_native_base::HasGrenadeDetonateSupport(0) ||
        !imomoe_native_base::HasGrenadeDetonateSupport(kSmokeGrenadeKind) ||
        !imomoe_native_base::HasGrenadeDetonateSupport(kMolotovGrenadeKind) ||
        !imomoe_native_base::HasGrenadeDetonateSupport(kIncendiaryGrenadeKind) ||
        !imomoe_native_base::HasGrenadeDetonateSupport(kHeGrenadeKind)) {
        return "Standard-grenade support did not isolate smoke, Molotov, incendiary, and HE kinds";
    }

    if (imomoe_native_base::GetGrenadeDetonationState(kSmokeGrenadeHandle, kSmokeGrenadeKind) != 0 ||
        imomoe_native_base::GetGrenadeDetonationState(kSmokeGrenadeHandle, kMolotovGrenadeKind) != -1 ||
        imomoe_native_base::GetGrenadeDetonationState(kMissingGrenadeHandle, kSmokeGrenadeKind) != 1 ||
        imomoe_native_base::GetGrenadeDetonationState(-1, kSmokeGrenadeKind) != -1 ||
        imomoe_native_base::GetGrenadeDetonationState(kSmokeGrenadeHandle, 0) != -1) {
        return "Grenade detonation state did not distinguish a live, missing, sentinel, or wrong-kind handle";
    }

    if (!s2sdk::NativeFixtureFireStandardGrenadeDetonate(kSmokeGrenadeHandle) ||
        s2sdk::NativeFixtureGrenadeDetonateCount(kSmokeGrenadeHandle) != 1 ||
        imomoe_native_base::GetGrenadeDetonationState(kSmokeGrenadeHandle, kSmokeGrenadeKind) != 1 ||
        imomoe_native_base::GetGrenadeDetonationState(kSmokeGrenadeHandle, kMolotovGrenadeKind) != -1 ||
        imomoe_native_base::DetonateGrenade(kSmokeGrenadeHandle, kSmokeGrenadeKind) != 0 ||
        s2sdk::NativeFixtureGrenadeDetonateCount(kSmokeGrenadeHandle) != 1) {
        return "A natural smoke Detonate entry was not claimed before a duplicate request";
    }

    auto* const smokeEntity = s2sdk::EntHandleToEntPointer(kSmokeGrenadeHandle);
    const auto reusedHandle = s2sdk::NativeFixtureReuseSmokeGrenadeHandle();
    if (smokeEntity == nullptr || reusedHandle != kReusedSmokeGrenadeHandle ||
        s2sdk::EntHandleToEntPointer(kSmokeGrenadeHandle) != nullptr ||
        s2sdk::EntHandleToEntPointer(reusedHandle) != smokeEntity ||
        imomoe_native_base::GetGrenadeDetonationState(kSmokeGrenadeHandle, kSmokeGrenadeKind) != 1 ||
        imomoe_native_base::GetGrenadeDetonationState(reusedHandle, kSmokeGrenadeKind) != 0 ||
        imomoe_native_base::DetonateGrenade(reusedHandle, kMolotovGrenadeKind) != -1) {
        return "A recycled smoke entity did not use its new full handle or reject a wrong type";
    }

    if (imomoe_native_base::DetonateGrenade(reusedHandle, kSmokeGrenadeKind) != 1 ||
        s2sdk::NativeFixtureGrenadeDetonateCount(reusedHandle) != 2 ||
        imomoe_native_base::GetGrenadeDetonationState(reusedHandle, kSmokeGrenadeKind) != 1 ||
        imomoe_native_base::DetonateGrenade(reusedHandle, kSmokeGrenadeKind) != 0 ||
        s2sdk::NativeFixtureGrenadeDetonateCount(reusedHandle) != 2 ||
        imomoe_native_base::DetonateGrenade(-1, kSmokeGrenadeKind) != -1) {
        return "An explicit smoke request was not submitted exactly once for the current full handle";
    }

    if (imomoe_native_base::GetGrenadeDetonationState(kMolotovGrenadeHandle, kMolotovGrenadeKind) != 0 ||
        !s2sdk::NativeFixtureFireStandardGrenadeDetonate(kMolotovGrenadeHandle) ||
        s2sdk::NativeFixtureGrenadeDetonateCount(kMolotovGrenadeHandle) != 2 ||
        imomoe_native_base::GetGrenadeDetonationState(kMolotovGrenadeHandle, kMolotovGrenadeKind) != 1 ||
        imomoe_native_base::GetGrenadeDetonationState(kMolotovGrenadeHandle, kIncendiaryGrenadeKind) != 1 ||
        imomoe_native_base::GetGrenadeDetonationState(kMolotovGrenadeHandle, kSmokeGrenadeKind) != -1 ||
        imomoe_native_base::DetonateGrenade(kMolotovGrenadeHandle, kIncendiaryGrenadeKind) != 0) {
        return "The shared Molotov/incendiary native entry did not claim its natural detonation";
    }
    if (imomoe_native_base::GetGrenadeDetonationState(kHeGrenadeHandle, kHeGrenadeKind) != 0 ||
        imomoe_native_base::GetGrenadeDetonationState(kHeGrenadeHandle, kSmokeGrenadeKind) != -1 ||
        imomoe_native_base::DetonateGrenade(kHeGrenadeHandle, kHeGrenadeKind) != 1 ||
        s2sdk::NativeFixtureGrenadeDetonateCount(kHeGrenadeHandle) != 1 ||
        imomoe_native_base::GetGrenadeDetonationState(kHeGrenadeHandle, kHeGrenadeKind) != 1 ||
        imomoe_native_base::DetonateGrenade(kHeGrenadeHandle, kHeGrenadeKind) != 0 ||
        s2sdk::NativeFixtureGrenadeDetonateCount(kHeGrenadeHandle) != 1) {
        return "The HE native entry did not claim and submit detonation exactly once";
    }
    return {};
}

bool IsFixtureFlame(std::int32_t handle) {
    if (!s2sdk::IsValidEntHandle(handle)) {
        return false;
    }
    const auto classname = s2sdk::GetEntityClassname(handle);
    return std::string_view(classname.c_str()) == "entityflame";
}

std::string_view ExerciseAfterburnManager() {
    constexpr std::int32_t kPawnHandle = kFixtureEntityHandle;
    constexpr float kDuration = 4.0f;
    auto* const pawn = s2sdk::EntHandleToEntPointer(kPawnHandle);
    if (pawn == nullptr) {
        return "The fixture did not resolve the current player pawn for Afterburn";
    }
    const auto schemaLookupsBefore = s2sdk::NativeFixtureSchemaClassLookupCount();
    auto schemaLookupsAfterColdCall = schemaLookupsBefore;
    for (const bool cold : {true, false}) {
        s2sdk::NativeFixtureEnableAfterburnClient(true);
        if (s2sdk::IsClientAlive(0) || s2sdk::GetClientPawn(0) != pawn ||
            imomoe_native_base::TryIgnitePlayer(kPawnHandle, kDuration) != kIgniteFlameHandle ||
            s2sdk::GetEntSchemaEnt2(pawn, "CBaseEntity", "m_hEffectEntity", 0) != kIgniteFlameHandle ||
            !IsFixtureFlame(kIgniteFlameHandle) ||
            s2sdk::GetEntSchemaEnt2(s2sdk::EntHandleToEntPointer(kIgniteFlameHandle),
                                    "CEntityFlame",
                                    "m_hEntAttached",
                                    0) != kPawnHandle) {
            s2sdk::NativeFixtureEnableAfterburnClient(false);
            return "Afterburn rejected a live current pawn only because its original player pawn is dead";
        }
        s2sdk::RemoveEntity(kIgniteFlameHandle);
        const bool flameCleanupVerified =
            !IsFixtureFlame(kIgniteFlameHandle) &&
            s2sdk::GetEntSchemaEnt2(pawn, "CBaseEntity", "m_hEffectEntity", 0) == -1;
        s2sdk::NativeFixtureEnableAfterburnClient(false);
        if (!flameCleanupVerified) {
            return "Afterburn fixture cleanup left the newly-created flame attached";
        }

        const auto schemaLookups = s2sdk::NativeFixtureSchemaClassLookupCount();
        if (cold) {
            if (schemaLookups <= schemaLookupsBefore) {
                return "Afterburn did not resolve schema on the first ignition of this load";
            }
            schemaLookupsAfterColdCall = schemaLookups;
        } else if (schemaLookups != schemaLookupsAfterColdCall) {
            return "Afterburn re-resolved schema on a second successful ignition";
        }
    }
    return {};
}

void* FixtureTouchImplementation() {
    if (!__s2sdk_GetGameConfigOffset || !__s2sdk_EntHandleToEntPointer) {
        return nullptr;
    }
    void* const entity = s2sdk::EntHandleToEntPointer(kFixtureEntityHandle);
    const auto offset = s2sdk::GetGameConfigOffset(
        s2sdk::ConfigId{1}, plg::string("CBaseEntity::Touch"));
    if (!entity || offset < 0) {
        return nullptr;
    }
    auto** const vtable = *reinterpret_cast<void***>(entity);
    return vtable && vtable[offset] ? vtable[offset] : nullptr;
}

bool UnhookExternalTouch();

bool HookExternalTouch() {
    if (!__polyhook_HookDetour || !__polyhook_FindDetour || !__polyhook_AddCallback ||
        !__polyhook_IsCallbackRegistered || !__polyhook_AreCallbacksRegistered ||
        !__polyhook_RemoveCallback || !__polyhook_UnhookDetour) {
        return false;
    }
    void* const address = FixtureTouchImplementation();
    if (!address) {
        return false;
    }
    auto hook = polyhook::FindDetour(address);
    if (hook && !polyhook::AreCallbacksRegistered(hook)) {
        return false;
    }
    if (!hook) {
        constexpr std::array arguments{polyhook::DataType::Pointer, polyhook::DataType::Pointer};
        hook = polyhook::HookDetour(address,
                                    polyhook::DataType::Void,
                                    plg::vector<polyhook::DataType>(arguments.begin(), arguments.end()),
                                    -1,
                                    "Fixture::ExternalTouch");
        if (!hook) {
            return false;
        }
    }
    g_externalTouchAddress = address;
    g_externalTouchHook = hook;
    g_externalTouchHooked = true;
    if (!polyhook::IsCallbackRegistered(hook, polyhook::CallbackType::Pre, ExternalTouchCallback)) {
        (void)polyhook::AddCallback(hook, polyhook::CallbackType::Pre, ExternalTouchCallback);
    }
    if (!polyhook::IsCallbackRegistered(hook, polyhook::CallbackType::Pre, ExternalTouchCallback)) {
        (void)UnhookExternalTouch();
        return false;
    }
    return true;
}

bool UnhookExternalTouch() {
    if (!g_externalTouchHooked) {
        return true;
    }
    try {
        const auto current = g_externalTouchAddress ? polyhook::FindDetour(g_externalTouchAddress) : nullptr;
        if (!current) {
            g_externalTouchAddress = nullptr;
            g_externalTouchHook = nullptr;
            g_externalTouchHooked = false;
            return true;
        }
        if (current != g_externalTouchHook) {
            return false;
        }
        if (polyhook::IsCallbackRegistered(current, polyhook::CallbackType::Pre, ExternalTouchCallback)) {
            (void)polyhook::RemoveCallback(current, polyhook::CallbackType::Pre, ExternalTouchCallback);
        }
        if (polyhook::IsCallbackRegistered(current, polyhook::CallbackType::Pre, ExternalTouchCallback)) {
            return false;
        }
        if (polyhook::AreCallbacksRegistered(current)) {
            g_externalTouchAddress = nullptr;
            g_externalTouchHook = nullptr;
            g_externalTouchHooked = false;
            return true;
        }
        (void)polyhook::UnhookDetour(g_externalTouchAddress);
        if (polyhook::FindDetour(g_externalTouchAddress)) {
            return false;
        }
        g_externalTouchAddress = nullptr;
        g_externalTouchHook = nullptr;
        g_externalTouchHooked = false;
        return true;
    } catch (...) {
        return false;
    }
}

std::int32_t DamageCallback(std::int32_t victimHandle,
                            std::int32_t attackerHandle,
                            std::int32_t inflictorHandle,
                            std::int32_t weaponKind,
                            std::int32_t hitGroup,
                            bool tracedHead,
                            float damage,
                            std::uint32_t damageType,
                            float& multiplier,
                            bool& markHeadshot,
                            std::int32_t weaponHandle,
                            bool skipPluginRules) {
    const auto depth = g_damageDepth++;
    auto& observed = depth == 0 ? g_outerDamage : g_nestedDamage;
    observed = {victimHandle,
                attackerHandle,
                inflictorHandle,
                weaponKind,
                hitGroup,
                tracedHead,
                damage,
                damageType,
                false,
                weaponHandle,
                skipPluginRules,
                imomoe_native_base::GetCurrentDamageFrameToken()};
    ++g_damageCallbacks;

    if (depth == 0) {
        multiplier = 2.0f;
        markHeadshot = true;
        const plg::vec3 force{0.0f, 0.0f, 0.0f};
        const plg::vec3 position{1.0f, 2.0f, 3.0f};
        g_nestedDamageResult = imomoe_native_base::TakeEntityDamageWithWeapon(
            21, 19, 17, force, position, 7.0f, 0x40, -1, false);
    }

    observed.markHeadshot = markHeadshot;
    --g_damageDepth;
    return 0;
}

std::int32_t EntityDamageCallback(std::int32_t victimHandle,
                                 std::int32_t attackerHandle,
                                 std::int32_t inflictorHandle,
                                 std::int32_t abilityHandle,
                                 float damage,
                                 std::uint32_t damageType,
                                 bool skipPluginRules) {
    const auto index = g_entityDamageCallbacks++;
    if (index < static_cast<std::int32_t>(g_entityDamageObservations.size())) {
        g_entityDamageObservations[static_cast<std::size_t>(index)] = {victimHandle,
                                                                       attackerHandle,
                                                                       inflictorHandle,
                                                                       abilityHandle,
                                                                       damage,
                                                                       damageType,
                                                                       skipPluginRules,
                                                                       imomoe_native_base::GetCurrentDamageFrameToken()};
    }
    return victimHandle == kChickenHandle && (damageType & (0x8U | 0x40U)) != 0 ? 1 : 0;
}

std::int32_t OtherEntityDamageCallback(std::int32_t,
                                      std::int32_t,
                                      std::int32_t,
                                      std::int32_t,
                                      float,
                                      std::uint32_t,
                                      bool) {
    return 0;
}

plg::PluginResult Fail(std::string_view message) {
    (void)imomoe_native_base::ClearBigHeadScaleTarget(kFixtureEntityHandle);
    (void)UnhookExternalTouch();
    if (imomoe_native_base::UnhookEntityTakeDamagePre(EntityDamageCallback)) {
        g_entityDamageHooked = false;
    } else {
        g_entityDamageHooked = true;
    }
    if (g_damageHooked) {
        (void)imomoe_native_base::UnhookPlayerTakeDamagePre(DamageCallback);
        g_damageHooked = false;
    }
    if (g_hooked) {
        (void)imomoe_native_base::UnhookEntityTouchPre(TouchCallback);
        g_hooked = false;
    }
    if (g_secondaryHooked) {
        (void)imomoe_native_base::UnhookEntityTouchPre(SecondaryTouchCallback);
        g_secondaryHooked = false;
    }
    return plg::PluginResult{message};
}

class NativeLoaderTestDriver final : public plg::Plugin {
public:
    plg::PluginResult OnPluginStart() override {
        g_touchCallbacks.store(0, std::memory_order_relaxed);
        g_secondaryTouchCallbacks.store(0, std::memory_order_relaxed);
        g_externalTouchCallbacks.store(0, std::memory_order_relaxed);
        g_callbackEntity.store(-1, std::memory_order_relaxed);
        g_callbackOther.store(-1, std::memory_order_relaxed);
        g_hooked = false;
        g_secondaryHooked = false;
        g_damageHooked = false;
        g_entityDamageHooked = false;
        g_damageDepth = 0;
        g_damageCallbacks = 0;
        g_nestedDamageResult = 0;
        g_outerDamage = {};
        g_nestedDamage = {};
        g_entityDamageCallbacks = 0;
        g_entityDamageObservations = {};
        try {
            if (!imomoe_native_base::ClearBigHeadScaleTarget(kFixtureEntityHandle)) {
                return Fail("A Big Head target from an earlier fixture run could not be cleared");
            }
            if (!UnhookExternalTouch()) {
                return Fail("An external Touch callback from an earlier fixture run could not be cleared");
            }
            if (imomoe_native_base::GetCurrentDamageFrameToken() != 0) {
                return Fail("Damage frame token was nonzero outside a damage call");
            }
            if (!imomoe_native_base::UnhookEntityTakeDamagePre(EntityDamageCallback)) {
                return Fail("An entity damage callback from an earlier fixture run could not be cleared");
            }
            if (imomoe_native_base::HasGroundReverseSupport()) {
                return Fail("Ground reverse unexpectedly reported support without its server callsite and player imports");
            }
            if (!imomoe_native_base::SetGroundReverseActive(false, 5.5f)) {
                return Fail("Ground reverse could not confirm a no-hook disable");
            }
            if (imomoe_native_base::SetGroundReverseActive(true, 0.0f)) {
                return Fail("Ground reverse accepted a non-positive acceleration");
            }
            if (!__imomoe_native_base_UpdateReloadPolicy || imomoe_native_base::UpdateReloadPolicy(true, false, false) != 0 ||
                imomoe_native_base::UpdateReloadPolicy(false, true, false) != 0 ||
                imomoe_native_base::UpdateReloadPolicy(false, false, true) != 1) {
                return Fail("Reload ABI failed closed without a dispatcher or failed to confirm forced OFF");
            }
            if (imomoe_native_base::HasGhostVisibilitySupport()) {
                return Fail("Ghost visibility unexpectedly reported support without its fixture signature");
            }
            if (imomoe_native_base::HasGhostRadarSupport()) {
                return Fail("Ghost radar unexpectedly reported support without its optional SDK imports and spotted-state schema");
            }
            if (const auto bigHeadFailure = ExerciseBigHeadManager(); !bigHeadFailure.empty()) {
                return Fail(bigHeadFailure);
            }
            if (const auto grenadeFailure = ExerciseStandardGrenadeDetonate(); !grenadeFailure.empty()) {
                return Fail(grenadeFailure);
            }
            if (imomoe_native_base::HasThirdPersonEyeSupport() ||
                imomoe_native_base::RegisterThirdPersonEyeTarget(17, 19)) {
                return Fail("Third-person eye Hook unexpectedly available without its server signature/schema");
            }
            if (!imomoe_native_base::ResetThirdPersonEyeTargets() ||
                !imomoe_native_base::UnregisterThirdPersonEyeTarget(17, 19)) {
                return Fail("Third-person empty target/detour cleanup failed");
            }
            const plg::vector<std::int32_t> noGhostTargets{};
            if (!imomoe_native_base::SetGhostVisibilityTargets(noGhostTargets)) {
                return Fail("Ghost visibility could not confirm an empty target set without a hook");
            }
            const plg::vector<std::int32_t> noGhostRadarPawns{};
            const plg::vector<std::uint64_t> noGhostRadarRecipients{};
            if (!imomoe_native_base::SetGhostRadarTargets(noGhostRadarPawns, noGhostRadarRecipients)) {
                return Fail("Ghost radar could not clear an empty target snapshot without engine support");
            }
            if (!imomoe_native_base::IsEntityTouchPreHookAvailable()) {
                return Fail("Native Touch capability did not initialize against the fixture engine");
            }
            const auto touchCallsBeforeHook = s2sdk::NativeFixtureTouchCallCount();
            const auto touchChecksumBeforeHook = s2sdk::NativeFixtureTouchChecksum();
            if (!imomoe_native_base::HookEntityTouchPre(kFixtureEntityHandle, TouchCallback)) {
                return Fail("Production Touch hook could not attach to the fixture vtable target");
            }
            g_hooked = true;

            s2sdk::NativeFixtureFireTouch();
            if (g_touchCallbacks.load(std::memory_order_relaxed) != 1 ||
                g_secondaryTouchCallbacks.load(std::memory_order_relaxed) != 0) {
                return Fail("Production Touch callback was not invoked");
            }
            if (g_callbackEntity.load(std::memory_order_relaxed) != kFixtureEntityHandle ||
                g_callbackOther.load(std::memory_order_relaxed) != kFixtureEntityHandle) {
                return Fail("Production Touch callback received incorrect entity handles");
            }
            if (s2sdk::NativeFixtureTouchCallCount() != touchCallsBeforeHook + 1) {
                return Fail("Allowing Touch callback did not reach the original target");
            }

            if (!imomoe_native_base::HookEntityTouchPre(kFixtureEntityHandle, SecondaryTouchCallback)) {
                return Fail("A second Touch consumer could not join the shared detour");
            }
            g_secondaryHooked = true;
            s2sdk::NativeFixtureFireTouch();
            if (g_touchCallbacks.load(std::memory_order_relaxed) != 2 ||
                g_secondaryTouchCallbacks.load(std::memory_order_relaxed) != 1 ||
                s2sdk::NativeFixtureTouchCallCount() != touchCallsBeforeHook + 1) {
                return Fail("One consumer did not suppress Touch while both callbacks remained active");
            }

            if (!imomoe_native_base::UnhookEntityTouchPre(TouchCallback)) {
                return Fail("First Touch consumer could not detach without removing the second consumer");
            }
            g_hooked = false;
            s2sdk::NativeFixtureFireTouch();
            if (g_touchCallbacks.load(std::memory_order_relaxed) != 2 ||
                g_secondaryTouchCallbacks.load(std::memory_order_relaxed) != 2 ||
                s2sdk::NativeFixtureTouchCallCount() != touchCallsBeforeHook + 1) {
                return Fail("The second Touch consumer did not keep its callback and veto active");
            }

            if (!imomoe_native_base::UnhookEntityTouchPre(SecondaryTouchCallback)) {
                return Fail("Final Touch consumer did not detach the shared detour");
            }
            g_secondaryHooked = false;

            s2sdk::NativeFixtureFireTouch();
            if (g_touchCallbacks.load(std::memory_order_relaxed) != 2 ||
                g_secondaryTouchCallbacks.load(std::memory_order_relaxed) != 2 ||
                s2sdk::NativeFixtureTouchCallCount() != touchCallsBeforeHook + 2 ||
                s2sdk::NativeFixtureTouchChecksum() != touchChecksumBeforeHook + 2) {
                return Fail("Original Touch target did not resume after production unhook");
            }

            // A separate plugin can add its own PolyHook callback to the
            // provider-owned raw handle. Removing the provider callback first
            // must leave that external detour and callback live.
            if (!imomoe_native_base::HookEntityTouchPre(kFixtureEntityHandle, TouchCallback)) {
                return Fail("Native Touch could not create the first half of a cross-plugin shared detour");
            }
            g_hooked = true;
            if (!HookExternalTouch()) {
                return Fail("Independent PolyHook consumer could not join an existing native Touch detour");
            }
            s2sdk::NativeFixtureFireTouch();
            if (g_touchCallbacks.load(std::memory_order_relaxed) != 3 ||
                g_externalTouchCallbacks.load(std::memory_order_relaxed) != 1 ||
                s2sdk::NativeFixtureTouchCallCount() != touchCallsBeforeHook + 2) {
                return Fail("Native and independent Touch callbacks did not share one vetoing detour");
            }
            if (!imomoe_native_base::UnhookEntityTouchPre(TouchCallback)) {
                return Fail("Native owner could not revoke its callback while an independent consumer remained");
            }
            g_hooked = false;
            s2sdk::NativeFixtureFireTouch();
            if (g_touchCallbacks.load(std::memory_order_relaxed) != 3 ||
                g_externalTouchCallbacks.load(std::memory_order_relaxed) != 2 ||
                s2sdk::NativeFixtureTouchCallCount() != touchCallsBeforeHook + 2) {
                return Fail("Removing the native callback detached an independent Touch owner");
            }
            if (!UnhookExternalTouch()) {
                return Fail("The final independent Touch callback could not detach its shared hook");
            }
            s2sdk::NativeFixtureFireTouch();
            if (g_touchCallbacks.load(std::memory_order_relaxed) != 3 ||
                g_externalTouchCallbacks.load(std::memory_order_relaxed) != 2 ||
                s2sdk::NativeFixtureTouchCallCount() != touchCallsBeforeHook + 3 ||
                s2sdk::NativeFixtureTouchChecksum() != touchChecksumBeforeHook + 3) {
                return Fail("Original Touch did not resume after native-first cross-plugin teardown");
            }

            // Exercise the reverse creator and teardown order as well: the
            // independent plugin owns the initial raw handle and is removed
            // while the native callback is still live.
            if (!HookExternalTouch()) {
                return Fail("Independent plugin could not create its own first Touch detour");
            }
            if (!imomoe_native_base::HookEntityTouchPre(kFixtureEntityHandle, TouchCallback)) {
                return Fail("Native provider could not join an independently created Touch detour");
            }
            g_hooked = true;
            s2sdk::NativeFixtureFireTouch();
            if (g_touchCallbacks.load(std::memory_order_relaxed) != 4 ||
                g_externalTouchCallbacks.load(std::memory_order_relaxed) != 3 ||
                s2sdk::NativeFixtureTouchCallCount() != touchCallsBeforeHook + 3) {
                return Fail("Reverse-order Touch owners did not share the existing detour");
            }
            if (!UnhookExternalTouch()) {
                return Fail("Independent plugin could not revoke its callback while native Touch remained");
            }
            s2sdk::NativeFixtureFireTouch();
            if (g_touchCallbacks.load(std::memory_order_relaxed) != 5 ||
                g_externalTouchCallbacks.load(std::memory_order_relaxed) != 3 ||
                s2sdk::NativeFixtureTouchCallCount() != touchCallsBeforeHook + 4) {
                return Fail("Removing the independent callback detached native Touch or failed to resume the original");
            }
            if (!imomoe_native_base::UnhookEntityTouchPre(TouchCallback)) {
                return Fail("Final native Touch callback could not detach an independently created hook");
            }
            g_hooked = false;
            s2sdk::NativeFixtureFireTouch();
            if (g_touchCallbacks.load(std::memory_order_relaxed) != 5 ||
                g_externalTouchCallbacks.load(std::memory_order_relaxed) != 3 ||
                s2sdk::NativeFixtureTouchCallCount() != touchCallsBeforeHook + 5 ||
                s2sdk::NativeFixtureTouchChecksum() != touchChecksumBeforeHook + 5) {
                return Fail("Original Touch did not resume after independent-first cross-plugin teardown");
            }

            const auto defaultCollisionGroup = static_cast<std::int32_t>(COLLISION_GROUP_DEFAULT);
            const auto schemaLookupsBefore = s2sdk::NativeFixtureSchemaClassLookupCount();
            const auto collisionRulesBefore = s2sdk::NativeFixtureCollisionRulesChangedCount();
            s2sdk::NativeFixtureSetCollisionState(false, 7, 11);
            imomoe_native_base::SetEntityDefaultCollisionGroup(kFixtureEntityHandle);
            const auto lookupsAfterMissingField = s2sdk::NativeFixtureSchemaClassLookupCount();
            if (s2sdk::NativeFixtureCollisionGroup() != 7 ||
                s2sdk::NativeFixtureCollisionAttributeGroup() != 11 ||
                s2sdk::NativeFixtureCollisionRulesChangedCount() != collisionRulesBefore ||
                lookupsAfterMissingField <= schemaLookupsBefore) {
                return Fail("Production collision reset cached an incomplete schema or wrote/notified before resolution succeeded");
            }

            s2sdk::NativeFixtureSetCollisionState(true, 7, 11);
            imomoe_native_base::SetEntityDefaultCollisionGroup(kFixtureEntityHandle);
            const auto lookupsAfterSchemaRecovery = s2sdk::NativeFixtureSchemaClassLookupCount();
            if (s2sdk::NativeFixtureCollisionGroup() != defaultCollisionGroup ||
                s2sdk::NativeFixtureCollisionAttributeGroup() != defaultCollisionGroup ||
                s2sdk::NativeFixtureCollisionRulesChangedCount() != collisionRulesBefore + 1 ||
                lookupsAfterSchemaRecovery <= lookupsAfterMissingField) {
                return Fail("Production collision reset did not retry missing schema metadata and apply both groups");
            }

            s2sdk::NativeFixtureSetCollisionState(true, 7, 11);
            imomoe_native_base::SetEntityDefaultCollisionGroup(kFixtureEntityHandle);
            if (s2sdk::NativeFixtureCollisionGroup() != defaultCollisionGroup ||
                s2sdk::NativeFixtureCollisionAttributeGroup() != defaultCollisionGroup ||
                s2sdk::NativeFixtureCollisionRulesChangedCount() != collisionRulesBefore + 2 ||
                s2sdk::NativeFixtureSchemaClassLookupCount() != lookupsAfterSchemaRecovery) {
                return Fail("Production collision reset did not use the cached schema fields on a subsequent call");
            }

            imomoe_native_base::SetEntityDefaultCollisionGroup(999);
            if (s2sdk::NativeFixtureCollisionGroup() != defaultCollisionGroup ||
                s2sdk::NativeFixtureCollisionAttributeGroup() != defaultCollisionGroup ||
                s2sdk::NativeFixtureCollisionRulesChangedCount() != collisionRulesBefore + 2 ||
                s2sdk::NativeFixtureSchemaClassLookupCount() != lookupsAfterSchemaRecovery) {
                return Fail("Invalid collision handle changed fields or notified collision rules");
            }

            const plg::vec3 force{0.0f, 0.0f, 0.0f};
            const plg::vec3 position{4.0f, 5.0f, 6.0f};

            const auto navCallsBeforeEntityOnly = s2sdk::NativeFixtureNavTraceCallCount();
            s2sdk::NativeFixtureSetNavAvailable(false);
            if (!imomoe_native_base::HasEntityTakeDamagePre()) {
                s2sdk::NativeFixtureSetNavAvailable(true);
                return Fail("Entity damage prerequisites incorrectly required player-only nav tracing");
            }
            if (imomoe_native_base::HasPlayerTakeDamagePre()) {
                s2sdk::NativeFixtureSetNavAvailable(true);
                return Fail("Player damage readiness unexpectedly succeeded without the nav-trace interface");
            }
            if (!imomoe_native_base::HookEntityTakeDamagePre(EntityDamageCallback)) {
                s2sdk::NativeFixtureSetNavAvailable(true);
                return Fail("Entity damage detour did not install without player-only nav tracing");
            }
            g_entityDamageHooked = true;
            s2sdk::NativeFixtureSetNavAvailable(true);
            if (!imomoe_native_base::UnhookEntityTakeDamagePre(OtherEntityDamageCallback)) {
                return Fail("Unsubscribed entity callback was not an idempotent no-op");
            }
            if (s2sdk::NativeFixtureNavTraceCallCount() != navCallsBeforeEntityOnly) {
                return Fail("Entity-only hook or readiness path invoked player nav tracing");
            }

            const auto genericEntityDamage = imomoe_native_base::TakeEntityDamageWithWeapon(
                kChickenHandle, kProjectileHandle, kProjectileHandle, force, position, 5.0f, 0x2,
                kSerialWeaponHandle, false);
            if (genericEntityDamage != 5 || g_entityDamageCallbacks != 1 ||
                s2sdk::NativeFixtureTakeDamageCallCount() != 1) {
                return Fail("Allowed non-player damage did not reach the original target");
            }
            const auto callsBeforeBurnBlock = s2sdk::NativeFixtureTakeDamageCallCount();
            if (imomoe_native_base::TakeEntityDamageWithWeapon(kChickenHandle,
                                                             kProjectileHandle,
                                                             kProjectileHandle,
                                                             force,
                                                             position,
                                                             6.0f,
                                                             0x8,
                                                             kSerialWeaponHandle,
                                                             true) != 0 ||
                g_entityDamageCallbacks != 2 || s2sdk::NativeFixtureTakeDamageCallCount() != callsBeforeBurnBlock) {
                return Fail("Burn damage with skipPluginRules=true was not suppressed for the exact owned entity");
            }
            if (imomoe_native_base::TakeEntityDamageWithWeapon(kChickenHandle,
                                                             kProjectileHandle,
                                                             kProjectileHandle,
                                                             force,
                                                             position,
                                                             7.0f,
                                                             0x40,
                                                             kSerialWeaponHandle,
                                                             false) != 0 ||
                g_entityDamageCallbacks != 3 || s2sdk::NativeFixtureTakeDamageCallCount() != callsBeforeBurnBlock) {
                return Fail("Blast damage was not suppressed for the exact owned entity");
            }
            const auto constructorsBeforeStale = s2sdk::NativeFixtureDamageConstructorCallCount();
            if (imomoe_native_base::TakeEntityDamageWithWeapon(kStaleChickenHandle,
                                                             kProjectileHandle,
                                                             kProjectileHandle,
                                                             force,
                                                             position,
                                                             8.0f,
                                                             0x8,
                                                             kSerialWeaponHandle,
                                                             false) != -1 ||
                imomoe_native_base::TakeEntityDamageWithWeapon(kChickenHandle,
                                                             kStaleProjectileHandle,
                                                             kProjectileHandle,
                                                             force,
                                                             position,
                                                             8.0f,
                                                             0x8,
                                                             kSerialWeaponHandle,
                                                             false) != -1 ||
                s2sdk::NativeFixtureDamageConstructorCallCount() != constructorsBeforeStale ||
                g_entityDamageCallbacks != 3) {
                return Fail("A stale full victim or source handle reached the native damage constructor");
            }
            const auto& genericObserved = g_entityDamageObservations[0];
            const auto& burnObserved = g_entityDamageObservations[1];
            const auto& blastObserved = g_entityDamageObservations[2];
            if (genericObserved.victim != kChickenHandle || genericObserved.attacker != kProjectileHandle ||
                genericObserved.inflictor != kProjectileHandle || genericObserved.ability != kSerialWeaponHandle ||
                genericObserved.damage != 5.0f || genericObserved.damageType != 0x2 ||
                genericObserved.skipPluginRules || genericObserved.frameToken == 0 ||
                burnObserved.victim != kChickenHandle || burnObserved.attacker != kProjectileHandle ||
                burnObserved.inflictor != kProjectileHandle || burnObserved.ability != kSerialWeaponHandle ||
                burnObserved.damage != 6.0f || burnObserved.damageType != 0x8 || !burnObserved.skipPluginRules ||
                burnObserved.frameToken == 0 || blastObserved.victim != kChickenHandle ||
                blastObserved.attacker != kProjectileHandle || blastObserved.inflictor != kProjectileHandle ||
                blastObserved.ability != kSerialWeaponHandle || blastObserved.damage != 7.0f ||
                blastObserved.damageType != 0x40 || blastObserved.skipPluginRules || blastObserved.frameToken == 0) {
                return Fail("Entity damage callback did not preserve the exact raw handles, damage, type or rule scope");
            }

            if (!imomoe_native_base::HasPlayerTakeDamagePre()) {
                return Fail("Production player damage prerequisites did not resolve after nav tracing was restored");
            }
            if (!imomoe_native_base::HookPlayerTakeDamagePre(DamageCallback)) {
                return Fail("Production player damage callback could not share the entity detour");
            }
            g_damageHooked = true;

            const auto outerResult = imomoe_native_base::TakeEntityDamageWithWeapon(
                21, 19, 20, force, position, 25.0f, 0x4, 18, true);
            if (outerResult != 17 || g_nestedDamageResult != 7 || g_damageCallbacks != 2 || g_damageDepth != 0 ||
                g_entityDamageCallbacks != 3) {
                return Fail("Production damage callback did not apply scaling and finish the nested damage call");
            }
            if (g_outerDamage.victim != 21 || g_outerDamage.attacker != 20 || g_outerDamage.inflictor != 19 ||
                g_outerDamage.weaponKind != 2 || g_outerDamage.hitGroup != 1 || g_outerDamage.tracedHead ||
                g_outerDamage.damage != 25.0f || g_outerDamage.damageType != 0x4 ||
                !g_outerDamage.markHeadshot || g_outerDamage.weapon != 18 || !g_outerDamage.skipPluginRules ||
                g_outerDamage.frameToken == 0) {
                return Fail("Production damage callback outer observation was incorrect");
            }
            if (g_nestedDamage.victim != 21 || g_nestedDamage.attacker != 17 || g_nestedDamage.inflictor != 19 ||
                g_nestedDamage.weaponKind != 0 || g_nestedDamage.hitGroup != -1 || g_nestedDamage.tracedHead ||
                g_nestedDamage.damage != 7.0f || g_nestedDamage.damageType != 0x40 ||
                g_nestedDamage.markHeadshot || g_nestedDamage.weapon != -1 || g_nestedDamage.skipPluginRules ||
                g_nestedDamage.frameToken == 0 || g_nestedDamage.frameToken == g_outerDamage.frameToken) {
                return Fail("Production damage callback nested observation was incorrect");
            }
            if (s2sdk::NativeFixtureOuterDamageType() != (0x4U | (1U << 30U)) ||
                s2sdk::NativeFixtureOuterDamageHitGroup() != 1 || s2sdk::NativeFixtureOuterDamageAbility() != 18 ||
                s2sdk::NativeFixtureOuterDamageAmount() != 50.0f ||
                s2sdk::NativeFixtureDamageConstructorCallCount() != 5 ||
                s2sdk::NativeFixtureTakeDamageCallCount() != 3 ||
                s2sdk::NativeFixtureNavTraceCallCount() != navCallsBeforeEntityOnly ||
                s2sdk::NativeFixtureLastEyePositionHandle() != 17 ||
                s2sdk::NativeFixtureLastEyeAngleHandle() != 17) {
                return Fail("Native ApplyDamage lost headshot/weapon/eye-trace data or returned the engine result instead of actual health lost");
            }

            const auto constructorsBeforeInvalidWeapon = s2sdk::NativeFixtureDamageConstructorCallCount();
            const auto callsBeforeInvalidWeapon = s2sdk::NativeFixtureTakeDamageCallCount();
            if (imomoe_native_base::TakeEntityDamageWithWeapon(21, 19, 20, force, position, 9.0f, 0x4, 19, false) != -1 ||
                s2sdk::NativeFixtureDamageConstructorCallCount() != constructorsBeforeInvalidWeapon ||
                s2sdk::NativeFixtureTakeDamageCallCount() != callsBeforeInvalidWeapon) {
                return Fail("A live nonweapon handle reached the native damage constructor or did not report failure");
            }

            if (!imomoe_native_base::UnhookEntityTakeDamagePre(EntityDamageCallback)) {
                return Fail("Entity damage callback did not unhook independently while the player callback remained");
            }
            g_entityDamageHooked = false;
            const auto beforeUnhookedEntity = s2sdk::NativeFixtureTakeDamageCallCount();
            if (imomoe_native_base::TakeEntityDamageWithWeapon(kChickenHandle,
                                                             kProjectileHandle,
                                                             kProjectileHandle,
                                                             force,
                                                             position,
                                                             8.0f,
                                                             0x8,
                                                             kSerialWeaponHandle,
                                                             true) != 8 ||
                s2sdk::NativeFixtureTakeDamageCallCount() != beforeUnhookedEntity + 1 ||
                g_entityDamageCallbacks != 3 || g_damageCallbacks != 2) {
                return Fail("Entity callback unhook detached the player callback or still intercepted entity damage");
            }
            const auto afterEntityUnsubscribe = imomoe_native_base::TakeEntityDamageWithWeapon(21, 19, 20, force, position, 5.0f, 0x4, 18, false);
            // The still-subscribed player callback doubles the requested 5 damage.
            if (afterEntityUnsubscribe != 10 ||
                g_damageCallbacks != 4 || g_damageDepth != 0 || g_entityDamageCallbacks != 3 ||
                s2sdk::NativeFixtureNavTraceCallCount() != navCallsBeforeEntityOnly) {
                return Fail("Player callback stopped working after entity-only unsubscribe: result=" + std::to_string(afterEntityUnsubscribe) +
                            " callbacks=" + std::to_string(g_damageCallbacks) + " depth=" + std::to_string(g_damageDepth) +
                            " entityCallbacks=" + std::to_string(g_entityDamageCallbacks) +
                            " navCalls=" + std::to_string(s2sdk::NativeFixtureNavTraceCallCount()) +
                            " expectedNavCalls=" + std::to_string(navCallsBeforeEntityOnly));
            }

            if (!imomoe_native_base::HookEntityTakeDamagePre(EntityDamageCallback)) {
                return Fail("Entity damage callback could not rejoin the existing player detour");
            }
            g_entityDamageHooked = true;
            const auto callsBeforeRehookedBlock = s2sdk::NativeFixtureTakeDamageCallCount();
            if (imomoe_native_base::TakeEntityDamageWithWeapon(kChickenHandle,
                                                             kProjectileHandle,
                                                             kProjectileHandle,
                                                             force,
                                                             position,
                                                             6.0f,
                                                             0x8,
                                                             kSerialWeaponHandle,
                                                             false) != 0 ||
                s2sdk::NativeFixtureTakeDamageCallCount() != callsBeforeRehookedBlock ||
                g_entityDamageCallbacks != 4 || g_damageCallbacks != 4) {
                return Fail("Entity callback did not remain independently functional after rejoining the player detour");
            }

            if (!imomoe_native_base::UnhookPlayerTakeDamagePre(DamageCallback)) {
                return Fail("Player damage callback did not unhook independently while the entity callback remained");
            }
            g_damageHooked = false;
            if (imomoe_native_base::GetCurrentDamageFrameToken() != 0) {
                return Fail("Damage frame remained active after unhook");
            }
            const auto callsBeforePlayerPassThrough = s2sdk::NativeFixtureTakeDamageCallCount();
            if (imomoe_native_base::TakeEntityDamageWithWeapon(21, 19, 17, force, position, 3.0f, 0x2, -1, false) != 3 ||
                s2sdk::NativeFixtureTakeDamageCallCount() != callsBeforePlayerPassThrough + 1 ||
                g_damageCallbacks != 4 || g_entityDamageCallbacks != 4) {
                return Fail("Player damage callback or detour remained after player-only unsubscribe");
            }
            const auto callsBeforeFinalBlock = s2sdk::NativeFixtureTakeDamageCallCount();
            if (imomoe_native_base::TakeEntityDamageWithWeapon(kChickenHandle,
                                                             kProjectileHandle,
                                                             kProjectileHandle,
                                                             force,
                                                             position,
                                                             7.0f,
                                                             0x40,
                                                             kSerialWeaponHandle,
                                                             true) != 0 ||
                s2sdk::NativeFixtureTakeDamageCallCount() != callsBeforeFinalBlock ||
                g_entityDamageCallbacks != 5 || g_damageCallbacks != 4) {
                return Fail("Entity callback stopped working after player-only unsubscribe");
            }
            if (!imomoe_native_base::UnhookEntityTakeDamagePre(EntityDamageCallback)) {
                return Fail("Final entity damage callback did not detach the shared detour");
            }
            g_entityDamageHooked = false;
            if (!imomoe_native_base::UnhookEntityTakeDamagePre(EntityDamageCallback)) {
                return Fail("Repeated entity callback unhook did not remain a safe no-op");
            }

            if (imomoe_native_base::TakeEntityDamageWithWeapon(21, 19, 17, force, position, 0.0f, 0x40, -1, false) != 0 ||
                s2sdk::NativeFixtureDamageConstructorCallCount() != 12 ||
                s2sdk::NativeFixtureTakeDamageCallCount() != 8) {
                return Fail("Successful zero-damage execution was reported as failure or skipped the native target");
            }
            if (imomoe_native_base::TakeEntityDamageWithWeapon(21, 19, 17, force, position, 9.0f, 0x40, -1, false) != 0 ||
                g_damageCallbacks != 4 || s2sdk::NativeFixtureDamageConstructorCallCount() != 13 ||
                s2sdk::NativeFixtureTakeDamageCallCount() != 9) {
                return Fail("Nonzero requested damage with zero actual health loss did not return success as zero");
            }
            if (imomoe_native_base::TakeEntityDamageWithWeapon(21, 19, 17, force, position, 11.0f, 0x40, -1, false) != 11 ||
                g_damageCallbacks != 4 || s2sdk::NativeFixtureDamageConstructorCallCount() != 14 ||
                s2sdk::NativeFixtureTakeDamageCallCount() != 10) {
                return Fail("Original native damage target did not resume after production unhook");
            }
            if (const auto afterburnFailure = ExerciseAfterburnManager(); !afterburnFailure.empty()) {
                return Fail(afterburnFailure);
            }
            // Leave a deferred hold with an expired full handle for native unload.
            const auto heldSmoke = s2sdk::NativeFixtureReuseSmokeGrenadeHandle();
            if (!imomoe_native_base::SetGrenadeDetonationHold(heldSmoke, kSmokeGrenadeKind, 1) ||
                !s2sdk::NativeFixtureFireStandardGrenadeDetonate(heldSmoke) ||
                s2sdk::NativeFixtureGrenadeDetonateCount(heldSmoke) != 2 ||
                s2sdk::NativeFixtureReuseSmokeGrenadeHandle() == heldSmoke ||
                s2sdk::IsValidEntHandle(heldSmoke)) {
                return Fail("Could not leave a deferred smoke hold with a recycled full handle for unload");
            }
            return {};
        } catch (...) {
            return Fail("Native loader test driver threw");
        }
    }

    plg::PluginResult OnPluginEnd() override {
        if (!imomoe_native_base::ClearBigHeadScaleTarget(kFixtureEntityHandle)) {
            return plg::PluginResult{"Test driver could not clear its production Big Head target before unload"};
        }
        if (!UnhookExternalTouch()) {
            return plg::PluginResult{"Test driver could not revoke its independent Touch callback before unload"};
        }
        if (!imomoe_native_base::UnhookEntityTakeDamagePre(EntityDamageCallback)) {
            return plg::PluginResult{"Test driver could not revoke its entity damage callback before unload"};
        }
        g_entityDamageHooked = false;
        if (g_damageHooked && !imomoe_native_base::UnhookPlayerTakeDamagePre(DamageCallback)) {
            return plg::PluginResult{"Test driver could not revoke its damage callback before unload"};
        }
        g_damageHooked = false;
        if (g_hooked && !imomoe_native_base::UnhookEntityTouchPre(TouchCallback)) {
            return plg::PluginResult{"Test driver could not revoke its Touch callback before unload"};
        }
        g_hooked = false;
        return {};
    }
};

NativeLoaderTestDriver g_plugin;

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_plugin)
