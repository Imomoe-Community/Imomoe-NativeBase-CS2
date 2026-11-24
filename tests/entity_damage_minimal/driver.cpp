// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <plg/plugin.hpp>
#include <plugin_export.h>
#include <imomoe_native_base/imomoe_native_base.hpp>
#include <s2sdk/s2sdk.hpp>

namespace {

constexpr char kCommand[] = "fr_entity_damage_minimal";
constexpr std::size_t kObservationLimit = 128;
constexpr std::size_t kOwnedChickenLimit = 6;
constexpr std::int32_t kInvalidHandle = -1;
constexpr std::uint32_t kDamageBurn = 0x8;
constexpr std::uint32_t kDamageBlast = 0x40;
constexpr char kEntitySchema[] = "CBaseEntity";
constexpr char kGameRulesSchema[] = "CCSGameRules";

constexpr std::array<std::string_view, 6> kGroundGrenadeClasses{
    "weapon_hegrenade",
    "weapon_flashbang",
    "weapon_smokegrenade",
    "weapon_molotov",
    "weapon_incgrenade",
    "weapon_decoy",
};

struct OwnedChicken {
    std::int32_t handle = kInvalidHandle;
    bool cleanupRequested = false;
};

struct DamageObservation {
    std::int32_t victim = kInvalidHandle;
    std::int32_t victimOwner = kInvalidHandle;
    std::int32_t attacker = kInvalidHandle;
    std::int32_t inflictor = kInvalidHandle;
    std::int32_t ability = kInvalidHandle;
    float damage = 0.0f;
    std::uint32_t damageType = 0;
    bool skipPluginRules = false;
    bool synthetic = false;
    std::uint64_t frameToken = 0;
    bool ownedChicken = false;
    bool blocked = false;
    std::string victimClass;
    std::string attackerClass;
    std::string inflictorClass;
    std::string abilityClass;
};

struct ChickenState {
    std::int64_t health = -1;
    std::int64_t lifeState = -1;
    bool exactFullHandle = false;
    bool isChicken = false;
    bool schemaReadable = false;
    bool live = false;
    bool dead = false;
};

struct SyntheticCaseResult {
    std::int32_t actualHealthLost = -1;
    ChickenState before;
    ChickenState after;
    bool callbackObserved = false;
    bool callbackBlocked = false;
};

std::vector<OwnedChicken> g_ownedChickens;
std::array<DamageObservation, kObservationLimit> g_observations{};
std::size_t g_observationCount = 0;
std::vector<std::string> g_trace;
std::thread::id g_mainThreadId;
std::atomic<bool> g_callbackThreadMismatch{false};
bool g_mainThreadReady = false;
bool g_updateThreadMismatch = false;
bool g_commandRegistered = false;
bool g_damageHooked = false;
bool g_damageHookAttempted = false;
bool g_cleanupInProgress = false;
bool g_cleanupPending = false;
bool g_syntheticCallInProgress = false;

class SyntheticCallScope final {
public:
    explicit SyntheticCallScope(bool& active) : active_(active) { active_ = true; }
    ~SyntheticCallScope() { active_ = false; }

    SyntheticCallScope(const SyntheticCallScope&) = delete;
    SyntheticCallScope& operator=(const SyntheticCallScope&) = delete;

private:
    bool& active_;
};

std::string ThreadIdText(std::thread::id id) {
    std::ostringstream output;
    output << id;
    return output.str();
}

bool IsMainThread() noexcept {
    return g_mainThreadReady && std::this_thread::get_id() == g_mainThreadId;
}

bool ExactHandle(std::int32_t handle) {
    if (handle == kInvalidHandle || !s2sdk::IsValidEntHandle(handle)) {
        return false;
    }
    auto* const entity = s2sdk::EntHandleToEntPointer(handle);
    return entity && s2sdk::EntPointerToEntHandle(entity) == handle;
}

bool CurrentChicken(std::int32_t handle) {
    return ExactHandle(handle) && s2sdk::GetEntityClassname(handle) == "chicken";
}

// CreateEntityByName returns a valid, not-yet-spawned entity handle. Match the
// established entity-interaction probe's pre-DispatchSpawn checks here; require
// a pointer-to-full-handle round trip only after DispatchSpawn has registered
// the entity in the live entity system.
bool CreatedChicken(std::int32_t handle) {
    return handle != kInvalidHandle && s2sdk::IsValidEntHandle(handle) &&
           s2sdk::GetEntityClassname(handle) == "chicken" && s2sdk::EntHandleToEntPointer(handle) != nullptr;
}

std::string ChickenHandleStatus(std::int32_t handle) {
    const bool valid = handle != kInvalidHandle && s2sdk::IsValidEntHandle(handle);
    void* const entity = valid ? s2sdk::EntHandleToEntPointer(handle) : nullptr;
    const auto roundTrip = entity ? s2sdk::EntPointerToEntHandle(entity) : kInvalidHandle;
    const auto classname = entity ? s2sdk::GetEntityClassname(handle) : plg::string("unavailable");
    std::ostringstream output;
    output << "chicken_handle=" << handle << " sdk_valid=" << valid << " pointer=" << (entity != nullptr)
           << " pointer_handle=" << roundTrip << " classname=" << classname;
    return output.str();
}

ChickenState ReadChickenState(std::int32_t handle) {
    ChickenState state;
    state.exactFullHandle = ExactHandle(handle);
    if (!state.exactFullHandle || !CurrentChicken(handle)) {
        state.dead = true;
        return state;
    }
    state.isChicken = true;
    if (s2sdk::GetSchemaOffset(kEntitySchema, "m_iHealth") < 0 ||
        s2sdk::GetSchemaOffset(kEntitySchema, "m_lifeState") < 0) {
        return state;
    }
    auto* const entity = s2sdk::EntHandleToEntPointer(handle);
    if (!entity) {
        state.exactFullHandle = false;
        state.dead = true;
        return state;
    }
    state.health = s2sdk::GetEntSchema2(entity, kEntitySchema, "m_iHealth", 0);
    state.lifeState = s2sdk::GetEntSchema2(entity, kEntitySchema, "m_lifeState", 0);
    state.schemaReadable = true;
    state.live = state.health > 0 && state.lifeState == 0;
    state.dead = !state.live;
    return state;
}

bool OwnsChicken(std::int32_t handle) {
    return std::find_if(g_ownedChickens.begin(), g_ownedChickens.end(), [handle](const OwnedChicken& chicken) {
               return chicken.handle == handle;
           }) != g_ownedChickens.end();
}

std::string SourceDescription(std::int32_t handle) {
    if (!ExactHandle(handle)) {
        return handle == kInvalidHandle ? "none" : "stale";
    }

    const std::string classname(s2sdk::GetEntityClassname(handle).c_str());
    if (std::find(kGroundGrenadeClasses.begin(), kGroundGrenadeClasses.end(), classname) ==
        kGroundGrenadeClasses.end()) {
        return classname;
    }

    const auto owner = s2sdk::GetEntityOwner(handle);
    const bool exactOwner = owner != kInvalidHandle && ExactHandle(owner);
    const bool playerOwned = exactOwner &&
                             (s2sdk::IsEntityPlayerPawn(owner) || s2sdk::IsEntityPlayerController(owner));
    return classname + (playerOwned ? ":player_owned" : ":not_player_owned") + ":owner=" +
           std::to_string(owner);
}

bool IsGroundGrenadeClass(std::string_view classname) {
    return std::find(kGroundGrenadeClasses.begin(), kGroundGrenadeClasses.end(), classname) !=
           kGroundGrenadeClasses.end();
}

void AppendTrace(std::string line) {
    constexpr std::size_t kTraceLimit = 128;
    if (g_trace.size() == kTraceLimit) {
        g_trace.erase(g_trace.begin());
    }
    g_trace.push_back(std::move(line));
    s2sdk::PrintToServer(plg::string(g_trace.back()));
}

std::int32_t EntityDamageCallback(std::int32_t victimHandle,
                                  std::int32_t attackerHandle,
                                  std::int32_t inflictorHandle,
                                  std::int32_t abilityHandle,
                                  float damage,
                                  std::uint32_t damageType,
                                  bool skipPluginRules) noexcept {
    if (!IsMainThread()) {
        g_callbackThreadMismatch.store(true, std::memory_order_relaxed);
        return 0;
    }
    if (g_cleanupInProgress) {
        return 0;
    }

    const bool ownedChicken = OwnsChicken(victimHandle) && CurrentChicken(victimHandle);
    std::string victimClass;
    std::int32_t victimOwner = kInvalidHandle;
    if (ownedChicken) {
        victimClass = "chicken";
    } else {
        if (!ExactHandle(victimHandle)) {
            return 0;
        }
        victimClass = s2sdk::GetEntityClassname(victimHandle).c_str();
        if (!IsGroundGrenadeClass(victimClass)) {
            return 0;
        }
        victimOwner = s2sdk::GetEntityOwner(victimHandle);
        const bool exactOwner = victimOwner != kInvalidHandle && ExactHandle(victimOwner);
        const bool playerOwned = exactOwner &&
                                 (s2sdk::IsEntityPlayerPawn(victimOwner) ||
                                  s2sdk::IsEntityPlayerController(victimOwner));
        if (playerOwned) {
            return 0;
        }
    }

    const bool blocked = ownedChicken && (damageType & (kDamageBurn | kDamageBlast)) != 0;
    try {
        const auto frameToken = imomoe_native_base::GetCurrentDamageFrameToken();
        const auto attackerClass = SourceDescription(attackerHandle);
        const auto inflictorClass = SourceDescription(inflictorHandle);
        const auto abilityClass = SourceDescription(abilityHandle);
        const auto index = g_observationCount;
        if (index < g_observations.size()) {
            g_observations[index] = {victimHandle,
                                     victimOwner,
                                     attackerHandle,
                                     inflictorHandle,
                                     abilityHandle,
                                     damage,
                                     damageType,
                                     skipPluginRules,
                                     g_syntheticCallInProgress,
                                     frameToken,
                                     ownedChicken,
                                     blocked,
                                     victimClass,
                                     attackerClass,
                                     inflictorClass,
                                     abilityClass};
            ++g_observationCount;
        }

        std::ostringstream line;
        line << "entity_damage=1 target=" << (ownedChicken ? "owned_chicken" : "ground_grenade")
             << " source=" << (g_syntheticCallInProgress ? "synthetic_native_call" : "engine_damage")
             << " victim=" << victimHandle << " victim_class=" << victimClass
             << " victim_owner=" << victimOwner << " attacker=" << attackerHandle
             << " attacker_class=" << attackerClass << " inflictor=" << inflictorHandle
             << " inflictor_class=" << inflictorClass << " ability=" << abilityHandle
             << " ability_class=" << abilityClass << " damage=" << damage
             << " damage_type=" << damageType << " damage_type_hex=0x" << std::hex << damageType << std::dec
             << " skip_plugin_rules=" << skipPluginRules << " blocked=" << blocked
             << " frame_token=" << frameToken << " main_thread_id=" << ThreadIdText(g_mainThreadId)
             << " callback_thread_id=" << ThreadIdText(std::this_thread::get_id())
             << " callback_thread_ok=1";
        AppendTrace(line.str());
    } catch (...) {
    }
    return blocked ? 1 : 0;
}

bool StopDamageHook();

bool StartDamageHook(std::string& reason) {
    if (!IsMainThread()) {
        reason = "OnPluginUpdate main-thread identity is not available on this thread";
        return false;
    }
    if (g_damageHooked) {
        return true;
    }
    if (g_damageHookAttempted && !StopDamageHook()) {
        reason = "a previous native entity callback still needs unhook retry";
        return false;
    }
    if (!imomoe_native_base::HasEntityTakeDamagePre()) {
        reason = "native entity damage prerequisites are unavailable";
        return false;
    }
    g_damageHookAttempted = true;
    if (!imomoe_native_base::HookEntityTakeDamagePre(EntityDamageCallback)) {
        if (StopDamageHook()) {
            reason = "native entity damage callback could not attach";
        } else {
            reason = "native entity damage callback could not attach and its ownership is still pending cleanup";
        }
        return false;
    }
    g_damageHooked = true;
    return true;
}

bool StopDamageHook() {
    if (!imomoe_native_base::UnhookEntityTakeDamagePre(EntityDamageCallback)) {
        g_damageHookAttempted = true;
        return false;
    }
    g_damageHooked = false;
    g_damageHookAttempted = false;
    return true;
}

std::string SpawnOwnedChicken(std::int32_t slot) {
    if (!IsMainThread()) {
        return "spawn=refused reason=not_main_thread";
    }
    if (g_ownedChickens.size() >= kOwnedChickenLimit) {
        return "spawn=refused reason=owned_chicken_limit_reached_cleanup_required";
    }
    if (g_cleanupPending) {
        return "spawn=refused reason=cleanup_retry_required";
    }
    if (slot < 0 || slot >= s2sdk::GetMaxClients() || !s2sdk::IsClientInGame(slot) ||
        !s2sdk::IsClientAlive(slot)) {
        return "spawn=refused reason=slot_must_be_a_live_client";
    }
    auto* const pawn = s2sdk::GetClientPawn(slot);
    const auto pawnHandle = pawn ? s2sdk::EntPointerToEntHandle(pawn) : kInvalidHandle;
    if (!pawn || !ExactHandle(pawnHandle) || s2sdk::EntHandleToEntPointer(pawnHandle) != pawn) {
        return "spawn=refused reason=player_pawn_full_handle_invalid";
    }

    const auto pawnOrigin = s2sdk::GetEntityAbsOrigin(pawnHandle);
    const auto chicken = s2sdk::CreateEntityByName(plg::string("chicken"));
    if (!CreatedChicken(chicken)) {
        if (chicken != kInvalidHandle && s2sdk::IsValidEntHandle(chicken) &&
            s2sdk::EntHandleToEntPointer(chicken) != nullptr) {
            s2sdk::RemoveEntity(chicken);
        }
        return "spawn=refused reason=chicken_create_failed " + ChickenHandleStatus(chicken);
    }
    s2sdk::DispatchSpawn(chicken);
    if (!CurrentChicken(chicken)) {
        if (CreatedChicken(chicken)) {
            s2sdk::RemoveEntity(chicken);
        }
        return "spawn=refused reason=chicken_invalid_after_spawn " + ChickenHandleStatus(chicken);
    }

    const plg::vec3 origin{pawnOrigin.x + 64.0f, pawnOrigin.y, pawnOrigin.z + 4.0f};
    s2sdk::SetEntityAbsOrigin(chicken, origin);
    const auto placed = s2sdk::GetEntityAbsOrigin(chicken);
    if (!CurrentChicken(chicken) || placed.x != origin.x || placed.y != origin.y || placed.z != origin.z) {
        s2sdk::RemoveEntity(chicken);
        return "spawn=refused reason=chicken_origin_readback_mismatch";
    }

    g_ownedChickens.push_back({chicken, false});
    return "spawn=complete chicken_handle=" + std::to_string(chicken) + " exact_full_handle=1 slot=" +
           std::to_string(slot) + " health_properties_written=0";
}

std::string RemoveOwnedChickens() {
    if (!IsMainThread()) {
        return "cleanup=refused reason=not_main_thread";
    }

    g_cleanupInProgress = true;
    std::vector<OwnedChicken> remaining;
    for (auto chicken : g_ownedChickens) {
        if (CurrentChicken(chicken.handle)) {
            chicken.cleanupRequested = true;
            s2sdk::RemoveEntity(chicken.handle);
        }
        if (CurrentChicken(chicken.handle)) {
            remaining.push_back(chicken);
        }
    }
    g_ownedChickens = std::move(remaining);
    g_cleanupInProgress = false;
    g_cleanupPending = !g_ownedChickens.empty();

    if (g_ownedChickens.empty()) {
        return "cleanup=complete";
    }
    std::ostringstream output;
    output << "cleanup=retry_required retained_handles=";
    for (std::size_t index = 0; index < g_ownedChickens.size(); ++index) {
        if (index != 0) {
            output << ',';
        }
        output << g_ownedChickens[index].handle;
    }
    return output.str();
}

SyntheticCaseResult ApplySyntheticCase(std::int32_t chickenHandle,
                                       std::int32_t pawnHandle,
                                       std::string_view caseName,
                                       float damage,
                                       std::uint32_t damageType,
                                       bool skipPluginRules) {
    SyntheticCaseResult result;
    result.before = ReadChickenState(chickenHandle);
    const auto observationStart = g_observationCount;
    if (result.before.live && result.before.schemaReadable && result.before.exactFullHandle &&
        OwnsChicken(chickenHandle)) {
        const plg::vec3 zero{};
        {
            SyntheticCallScope sourceScope(g_syntheticCallInProgress);
            result.actualHealthLost = imomoe_native_base::TakeEntityDamageWithWeapon(
                chickenHandle,
                pawnHandle,
                pawnHandle,
                zero,
                zero,
                damage,
                static_cast<std::int32_t>(damageType),
                kInvalidHandle,
                skipPluginRules);
        }
        if (g_observationCount == observationStart + 1 && observationStart < g_observations.size()) {
            const auto& observation = g_observations[observationStart];
            result.callbackObserved = observation.synthetic && observation.ownedChicken &&
                                      observation.victim == chickenHandle && observation.damageType == damageType &&
                                      observation.skipPluginRules == skipPluginRules;
            result.callbackBlocked = result.callbackObserved && observation.blocked;
        }
    }
    result.after = ReadChickenState(chickenHandle);

    std::ostringstream line;
    line << "synthetic_case=" << caseName << " source=synthetic_native_call damage_type=0x" << std::hex
         << damageType << std::dec << " skip_plugin_rules=" << skipPluginRules
         << " actualhealthlost=" << result.actualHealthLost << " fullhandle=" << chickenHandle
         << " exact_full_handle_before=" << result.before.exactFullHandle
         << " exact_full_handle_after=" << result.after.exactFullHandle
         << " chicken_health_before=" << result.before.health << " chicken_health_after=" << result.after.health
         << " live_before=" << result.before.live << " dead_before=" << result.before.dead
         << " live=" << result.after.live << " dead=" << result.after.dead
         << " callback_observed=" << result.callbackObserved << " callback_blocked=" << result.callbackBlocked;
    AppendTrace(line.str());
    return result;
}

std::string VerifyOwnedChickenDamage(std::int32_t slot) {
    if (!IsMainThread()) {
        return "selftest=fail reason=not_main_thread";
    }
    if (!g_ownedChickens.empty()) {
        return "selftest=refused reason=remove_existing_owned_chickens_first";
    }
    if (slot < 0 || slot >= s2sdk::GetMaxClients() || !s2sdk::IsClientInGame(slot) ||
        !s2sdk::IsClientAlive(slot)) {
        return "selftest=refused reason=slot_must_be_a_live_client";
    }
    auto* const pawn = s2sdk::GetClientPawn(slot);
    const auto pawnHandle = pawn ? s2sdk::EntPointerToEntHandle(pawn) : kInvalidHandle;
    if (!pawn || !ExactHandle(pawnHandle) || s2sdk::EntHandleToEntPointer(pawnHandle) != pawn) {
        return "selftest=refused reason=player_pawn_full_handle_invalid";
    }

    std::string hookReason;
    if (!StartDamageHook(hookReason)) {
        return "selftest=fail stage=hook_start reason=" + hookReason;
    }

    const auto spawn = SpawnOwnedChicken(slot);
    if (!spawn.starts_with("spawn=complete")) {
        const bool detached = StopDamageHook();
        return "selftest=fail stage=chicken_spawn " + spawn + " damage_hook_detached=" +
               (detached ? "1" : "0") + " entity_hook_cleanup_pending=" +
               (g_damageHookAttempted && !g_damageHooked ? "1" : "0");
    }

    const auto chickenHandle = g_ownedChickens.back().handle;
    g_observationCount = 0;
    g_trace.clear();
    g_syntheticCallInProgress = false;
    const auto initial = ReadChickenState(chickenHandle);
    const auto burn = ApplySyntheticCase(chickenHandle, pawnHandle, "burn", 1.0f, kDamageBurn, true);
    const auto blast = ApplySyntheticCase(chickenHandle, pawnHandle, "blast", 1.0f, kDamageBlast, true);
    const float genericDamage = std::max(1.0f, static_cast<float>(initial.health) + 1.0f);
    const auto generic = ApplySyntheticCase(chickenHandle, pawnHandle, "generic", genericDamage, 0, false);

    const auto blockedAndPreserved = [](const SyntheticCaseResult& check) {
        return check.actualHealthLost == 0 && check.before.live && check.before.exactFullHandle &&
               check.before.schemaReadable && check.after.live && check.after.exactFullHandle &&
               check.after.isChicken && check.after.schemaReadable && check.before.health == check.after.health &&
               check.callbackObserved && check.callbackBlocked;
    };
    const bool burnBlocked = blockedAndPreserved(burn);
    const bool blastBlocked = blockedAndPreserved(blast);
    const bool genericAllowed = generic.actualHealthLost > 0 && generic.before.live &&
                                generic.before.exactFullHandle && generic.callbackObserved &&
                                !generic.callbackBlocked && generic.after.dead;

    const auto cleanup = RemoveOwnedChickens();
    const bool detached = StopDamageHook();
    const bool casesPassed = initial.schemaReadable && initial.live && initial.exactFullHandle && burnBlocked &&
                             blastBlocked && genericAllowed && g_observationCount == 3;

    std::ostringstream output;
    output << "damage_cases=" << (casesPassed ? "pass" : "fail") << " source=synthetic_native_call chicken="
           << chickenHandle << " initial_health=" << initial.health << " burn_blocked_skip1=" << burnBlocked
           << " blast_blocked_skip1=" << blastBlocked << " generic_allowed=" << genericAllowed
           << " generic_lethal=" << genericAllowed
           << ' ' << cleanup << " damage_hook_detached=" << detached << " damage_hooked=" << g_damageHooked
           << " chicken_cleanup_pending=" << g_cleanupPending
           << " entity_hook_cleanup_pending=" << (g_damageHookAttempted && !g_damageHooked)
           << " owned_chickens=" << g_ownedChickens.size();
    if (g_cleanupPending) {
        output << "; retry remove-chickens after a server update before unloading";
    }
    if (!detached) {
        output << "; stop the isolated server before unloading or reloading this plugin";
    }
    return output.str();
}

bool ParseSlot(const plg::string& value, std::int32_t& slot) {
    const std::string text(value.c_str());
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), slot);
    return error == std::errc{} && end == text.data() + text.size();
}

std::string Status() {
    std::ostringstream output;
    output << "damage_hooked=" << g_damageHooked << " entity_hook_cleanup_pending="
           << (g_damageHookAttempted && !g_damageHooked) << " chicken_cleanup_pending=" << g_cleanupPending
           << " entity_prerequisites="
           << imomoe_native_base::HasEntityTakeDamagePre() << " owned_chickens=" << g_ownedChickens.size()
           << " observations=" << g_observationCount << " main_thread_ready=" << g_mainThreadReady
           << " main_thread_id=" << (g_mainThreadReady ? ThreadIdText(g_mainThreadId) : "unknown")
           << " callback_thread_mismatch=" << g_callbackThreadMismatch.load(std::memory_order_relaxed)
           << " update_thread_mismatch=" << g_updateThreadMismatch << " handles=";
    if (g_ownedChickens.empty()) {
        output << "none";
    } else {
        for (std::size_t index = 0; index < g_ownedChickens.size(); ++index) {
            if (index != 0) {
                output << ',';
            }
            output << g_ownedChickens[index].handle << ':' << CurrentChicken(g_ownedChickens[index].handle);
        }
    }
    return output.str();
}

std::string InspectHandle(std::int32_t handle) {
    if (!IsMainThread()) {
        return "inspect=refused reason=not_main_thread";
    }
    if (!ExactHandle(handle)) {
        return "inspect=complete fullhandle=" + std::to_string(handle) + " exact_full_handle=0 entity_class=stale";
    }
    const auto classname = s2sdk::GetEntityClassname(handle);
    const auto owner = s2sdk::GetEntityOwner(handle);
    std::ostringstream output;
    output << "inspect=complete fullhandle=" << handle << " exact_full_handle=1 entity_class="
           << classname << " owner=" << owner;
    if (classname == "chicken") {
        const auto state = ReadChickenState(handle);
        const auto scale = s2sdk::GetEntityModelScale(handle);
        output << " live=" << (state.live ? 1 : 0) << " health=";
        if (state.schemaReadable) {
            output << state.health;
        } else {
            output << "unavailable";
        }
        output << " scale=" << scale;
    }
    return output.str();
}

std::string ReadWarmupRules() {
    if (!IsMainThread()) {
        return "rules=refused reason=not_main_thread";
    }
    if (!__s2sdk_GetGameRules || !__s2sdk_GetSchemaOffset || !__s2sdk_GetEntSchema2) {
        return "rules=refused reason=required_import_unavailable";
    }

    void* const rules = s2sdk::GetGameRules();
    if (!rules) {
        return "rules=unavailable reason=game_rules_null rules_nonnull=0 warmup_schema_offset=unavailable warmup_raw=unavailable";
    }

    const auto offset = s2sdk::GetSchemaOffset(kGameRulesSchema, "m_bWarmupPeriod");
    if (offset < 0) {
        return "rules=unavailable rules_nonnull=1 warmup_schema_offset=" + std::to_string(offset) +
               " warmup_raw=unavailable";
    }

    const auto raw = s2sdk::GetEntSchema2(rules, kGameRulesSchema, "m_bWarmupPeriod", 0);
    return "rules=available rules_nonnull=1 warmup_schema_offset=" + std::to_string(offset) +
           " warmup_raw=" + std::to_string(raw) + " warmup_active=" + (raw != 0 ? "1" : "0");
}

std::string ObservationTrace() {
    std::ostringstream output;
    if (g_trace.empty()) {
        return "trace=empty";
    }
    for (const auto& line : g_trace) {
        output << line << '\n';
    }
    return output.str();
}

s2sdk::ResultType Command(std::int32_t caller,
                          s2sdk::ConCommandContext context,
                          const plg::vector<plg::string>& arguments) noexcept {
    const auto reply = [&](const std::string& text) {
        s2sdk::ReplyToCommand(context, caller, plg::string(text));
    };
    try {
        if (!IsMainThread()) {
            g_callbackThreadMismatch.store(true, std::memory_order_relaxed);
            return s2sdk::ResultType::Handled;
        }
        if (caller != -1 || context != s2sdk::ConCommandContext::Console || arguments.size() < 2) {
            reply("usage: fr_entity_damage_minimal start|stop|status|rules|selftest <live-slot>|spawn-chicken <slot>|inspect-handle <fullhandle>|trace [clear]|remove-chickens (server console only)");
            return s2sdk::ResultType::Handled;
        }
        if (arguments[1] == "status" && arguments.size() == 2) {
            reply(Status());
        } else if (arguments[1] == "rules" && arguments.size() == 2) {
            reply(ReadWarmupRules());
        } else if (arguments[1] == "start" && arguments.size() == 2) {
            std::string reason;
            const bool started = StartDamageHook(reason);
            reply(started ? "damage_hook=active " + Status() : "damage_hook=start_failed reason=" + reason);
        } else if (arguments[1] == "stop" && arguments.size() == 2) {
            const bool detached = StopDamageHook();
            reply(std::string("detached=") + (detached ? "1" : "0") + " damage_hooked=" +
                  (g_damageHooked ? "1" : "0") + " entity_hook_cleanup_pending=" +
                  (g_damageHookAttempted && !g_damageHooked ? "1" : "0"));
        } else if (arguments[1] == "selftest" && arguments.size() == 3) {
            std::int32_t slot = -1;
            reply(ParseSlot(arguments[2], slot) ? VerifyOwnedChickenDamage(slot)
                                                : "selftest=refused reason=decimal_slot_required");
        } else if (arguments[1] == "inspect-handle" && arguments.size() == 3) {
            std::int32_t handle = kInvalidHandle;
            reply(ParseSlot(arguments[2], handle) ? InspectHandle(handle)
                                                  : "inspect=refused reason=decimal_full_handle_required");
        } else if (arguments[1] == "spawn-chicken" && arguments.size() == 3) {
            std::int32_t slot = -1;
            reply(ParseSlot(arguments[2], slot) ? SpawnOwnedChicken(slot)
                                                : "spawn=refused reason=decimal_slot_required");
        } else if (arguments[1] == "remove-chickens" && arguments.size() == 2) {
            reply(RemoveOwnedChickens());
        } else if (arguments[1] == "trace" && arguments.size() == 2) {
            reply(ObservationTrace());
        } else if (arguments[1] == "trace" && arguments.size() == 3 && arguments[2] == "clear") {
            g_observationCount = 0;
            g_trace.clear();
            reply("trace_cleared=1 engine_unchanged=1");
        } else {
            reply("usage: fr_entity_damage_minimal start|stop|status|rules|selftest <live-slot>|spawn-chicken <slot>|inspect-handle <fullhandle>|trace [clear]|remove-chickens (server console only)");
        }
    } catch (...) {
        try {
            reply("command_failed=1 observer_state_retained=1");
        } catch (...) {
        }
    }
    return s2sdk::ResultType::Handled;
}

class Driver final : public plg::Plugin {
public:
    plg::PluginResult OnPluginStart() override {
        try {
            if (!s2sdk::AddConsoleCommand(plg::string(kCommand),
                                          plg::string("Test the native non-player damage callback on owned chickens"),
                                          s2sdk::ConVarFlag::None,
                                          Command,
                                          s2sdk::HookMode::Pre)) {
                return plg::PluginResult{"Entity-damage command registration failed"};
            }
            g_commandRegistered = true;
            return {};
        } catch (...) {
            return plg::PluginResult{"Entity-damage diagnostic startup threw"};
        }
    }

    plg::PluginResult OnPluginUpdate(std::chrono::milliseconds) override {
        try {
            const auto current = std::this_thread::get_id();
            if (!g_mainThreadReady) {
                g_mainThreadId = current;
                g_mainThreadReady = true;
            } else if (current != g_mainThreadId) {
                g_updateThreadMismatch = true;
            }

            if (IsMainThread() && g_cleanupPending) {
                (void)RemoveOwnedChickens();
            }
        } catch (...) {
        }
        return {};
    }

    plg::PluginResult OnPluginEnd() override {
        bool commandDetached = true;
        if (g_commandRegistered) {
            try {
                commandDetached = s2sdk::RemoveCommand(plg::string(kCommand), Command);
                if (commandDetached) {
                    g_commandRegistered = false;
                }
            } catch (...) {
                commandDetached = false;
            }
        }

        bool damageDetached = false;
        try {
            damageDetached = StopDamageHook();
        } catch (...) {
            damageDetached = false;
        }

        std::string chickenCleanup = "cleanup=refused reason=not_main_thread";
        if (IsMainThread()) {
            try {
                chickenCleanup = RemoveOwnedChickens();
            } catch (...) {
                chickenCleanup = "cleanup=failed reason=exception";
            }
        } else if (!g_ownedChickens.empty()) {
            g_cleanupPending = true;
        } else {
            g_cleanupPending = false;
            chickenCleanup = "cleanup=complete";
        }

        if (!commandDetached || !damageDetached || chickenCleanup != "cleanup=complete") {
            std::ostringstream failure;
            failure << "Cleanup incomplete command_listener_detached=" << commandDetached
                    << " entity_damage_detached=" << damageDetached << ' ' << chickenCleanup
                    << "; stop the isolated server before unloading or reloading this plugin";
            return plg::PluginResult{failure.str()};
        }
        return {};
    }
};

Driver g_plugin;

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_plugin)
