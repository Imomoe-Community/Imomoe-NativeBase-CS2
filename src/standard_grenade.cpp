// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "standard_grenade.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <entity2/entityinstance.h>
#include <plugin_export.h>
#include <plg/plugin.hpp>
#include <polyhook/polyhook.hpp>
#include <s2sdk/s2sdk.hpp>

#include "detour_slot.hpp"
#include "engine.hpp"
#include "fire_rain.hpp"
#include "standard_grenade_policy.hpp"

namespace funrounds::native::detail {
namespace {

struct PendingDetonation {
    std::int32_t handle{-1};
    void* entity{};
};

using grenade::Kind;

struct HeldDetonation {
    PendingDetonation identity;
    Kind kind;
    bool deferred{};
};

struct DetonateHook {
    void* address{};
    polyhook::HookHandle handle{};
    bool attempted{};
    bool shared{};
};

std::vector<PendingDetonation> g_pending;
std::vector<HeldDetonation> g_held;
std::array<DetonateHook, 6> g_hooks;
std::array<bool, 6> g_failureLogged{};
bool g_mapEndRegistered{};

std::size_t HookIndex(Kind kind) noexcept {
    return static_cast<std::size_t>(kind == Kind::Incendiary ? Kind::Molotov : kind) - 1;
}

polyhook::ResultType DetonateEntry(polyhook::HookHandle,
                                   polyhook::ParametersHandle parameters,
                                   std::int32_t,
                                   polyhook::ReturnHandle,
                                   polyhook::CallbackType type) noexcept;

polyhook::ResultType RainDetonateExit(polyhook::HookHandle,
                                     polyhook::ParametersHandle parameters,
                                     std::int32_t,
                                     polyhook::ReturnHandle,
                                     polyhook::CallbackType type) noexcept {
    if (type == polyhook::CallbackType::Post && parameters) {
        try {
            EndRainMolotovDetonation(polyhook::GetArgumentPointer(parameters, 0));
        } catch (...) {
        }
    }
    return polyhook::ResultType::Ignored;
}

const char* MissingRequiredImport() noexcept {
    if (!__s2sdk_GetGameConfigVTable) return "missing S2SDK GetGameConfigVTable import";
    if (!__s2sdk_GetGameConfigOffset) return "missing S2SDK GetGameConfigOffset import";
    if (!__s2sdk_IsValidEntHandle) return "missing S2SDK IsValidEntHandle import";
    if (!__s2sdk_EntHandleToEntPointer) return "missing S2SDK EntHandleToEntPointer import";
    if (!__s2sdk_EntPointerToEntHandle) return "missing S2SDK EntPointerToEntHandle import";
    if (!__s2sdk_GetEntityClassname) return "missing S2SDK GetEntityClassname import";
    if (!__s2sdk_OnMapEnd_Register) return "missing S2SDK OnMapEnd_Register import";
    if (!__s2sdk_OnMapEnd_Unregister) return "missing S2SDK OnMapEnd_Unregister import";
    if (!__polyhook_HookDetour) return "missing PolyHook HookDetour import";
    if (!__polyhook_FindDetour) return "missing PolyHook FindDetour import";
    if (!__polyhook_AddCallback) return "missing PolyHook AddCallback import";
    if (!__polyhook_IsCallbackRegistered) return "missing PolyHook IsCallbackRegistered import";
    if (!__polyhook_RemoveCallback) return "missing PolyHook RemoveCallback import";
    if (!__polyhook_AreCallbacksRegistered) return "missing PolyHook AreCallbacksRegistered import";
    if (!__polyhook_UnhookDetour) return "missing PolyHook UnhookDetour import";
    if (!__polyhook_GetArgumentPointer) return "missing PolyHook GetArgumentPointer import";
    return nullptr;
}

void LogSupportFailure(funrounds::native::grenade::Kind kind, const char* reason) noexcept {
    bool& logged = g_failureLogged[HookIndex(kind)];
    if (logged) return;
    logged = true;
    try {
        if (plg::Log != nullptr) {
            std::string message{grenade::ProjectileClass(kind)};
            message += " native terminal entry unavailable: ";
            message += reason;
            plg::Log(plg::string(message), plg::Severity::Error, plg::source_location::current());
        }
    } catch (...) {
    }
}

void ForgetPendingOnMapEnd() noexcept {
    g_pending.clear();
    g_held.clear();
}

bool RegisterMapEndCallback() noexcept {
    if (g_mapEndRegistered) return true;
    try {
        s2sdk::OnMapEnd_Register(ForgetPendingOnMapEnd);
        g_mapEndRegistered = true;
        return true;
    } catch (...) {
        return false;
    }
}

bool UnregisterMapEndCallback() noexcept {
    if (!g_mapEndRegistered) return true;
    try {
        s2sdk::OnMapEnd_Unregister(ForgetPendingOnMapEnd);
        g_mapEndRegistered = false;
        return true;
    } catch (...) {
        return false;
    }
}

std::int32_t DetonateSlot() noexcept {
    try {
        constexpr auto detonateSlot = OffsetKey{"CSGrenadeProjectileBase::Detonate"};
        const auto slot = ProbeOffset(detonateSlot);
        if (slot < 4096) return slot;
        InvalidGamedata(detonateSlot.name());
        return -1;
    } catch (...) {
        return -1;
    }
}

void* ProjectileDetonate(funrounds::native::grenade::Kind kind) noexcept {
    try {
        if (kind == Kind::Decoy) {
            // Decoy's virtual Detonate is a no-op. Its first Think starts the
            // native gunfire/explosion lifecycle once motion has settled.
            return __s2sdk_GetGameConfigSignature
                       ? ProbeSignature(SignatureKey{"FunRounds::DecoyDetonateThink"})
                       : nullptr;
        }
        const auto name = grenade::VTableName(kind);
        if (!name) return nullptr;
        auto* const table = static_cast<void**>(ProbeVTable(*name));
        const auto slot = DetonateSlot();
        if (!table || slot < 0) return nullptr;
        return table[slot];
    } catch (...) {
        return nullptr;
    }
}

DetonateHook& HookFor(funrounds::native::grenade::Kind kind) noexcept {
    return g_hooks[HookIndex(kind)];
}

bool CurrentProjectile(std::int32_t handle,
                       funrounds::native::grenade::Kind kind,
                       void*& entity) noexcept {
    entity = nullptr;
    try {
        if (handle == -1 || !s2sdk::IsValidEntHandle(handle)) return false;
        entity = EntityFromHandle(handle);
        if (!entity || HandleFromEntity(entity) != handle ||
            reinterpret_cast<CEntityInstance*>(entity)->GetRefEHandle().ToInt() != static_cast<std::uint32_t>(handle)) {
            entity = nullptr;
            return false;
        }
        const auto classname = s2sdk::GetEntityClassname(handle);
        if (std::string_view(classname.c_str()) != funrounds::native::grenade::ProjectileClass(kind)) {
            entity = nullptr;
            return false;
        }
        return true;
    } catch (...) {
        entity = nullptr;
        return false;
    }
}

std::optional<funrounds::native::grenade::Kind> LiveProjectileKind(std::int32_t handle, void* entity) {
    using funrounds::native::grenade::Kind;
    if (handle == -1 || !entity || !s2sdk::IsValidEntHandle(handle) || EntityFromHandle(handle) != entity ||
        HandleFromEntity(entity) != handle ||
        reinterpret_cast<CEntityInstance*>(entity)->GetRefEHandle().ToInt() != static_cast<std::uint32_t>(handle)) {
        return std::nullopt;
    }
    const auto classname = s2sdk::GetEntityClassname(handle);
    const std::string_view value(classname.c_str());
    for (const auto kind : {Kind::Smoke, Kind::Molotov, Kind::He, Kind::Flash, Kind::Decoy}) {
        if (value == funrounds::native::grenade::ProjectileClass(kind)) return kind;
    }
    return std::nullopt;
}

bool IsStale(const PendingDetonation& pending) noexcept {
    try {
        return pending.handle == -1 || !s2sdk::IsValidEntHandle(pending.handle) ||
               EntityFromHandle(pending.handle) != pending.entity || HandleFromEntity(pending.entity) != pending.handle ||
               reinterpret_cast<CEntityInstance*>(pending.entity)->GetRefEHandle().ToInt() != static_cast<std::uint32_t>(pending.handle);
    } catch (...) {
        return true;
    }
}

void RetireStalePending() noexcept {
    std::erase_if(g_pending, IsStale);
    std::erase_if(g_held, [](const HeldDetonation& hold) { return IsStale(hold.identity); });
}

auto FindHold(std::int32_t handle, void* entity) noexcept {
    return std::ranges::find_if(g_held, [handle, entity](const HeldDetonation& item) {
        return item.identity.handle == handle && item.identity.entity == entity;
    });
}

bool DecoySettled(std::int32_t handle) {
    // Both pinned engine builds test Length(absVelocity) <= 0.2 before
    // switching from the settle Think to the gunfire Think.
    const auto v = s2sdk::GetEntityAbsVelocity(handle);
    return v.x * v.x + v.y * v.y + v.z * v.z <= 0.2f * 0.2f;
}

using Detonate = void (*)(void*);

bool IsPending(std::int32_t handle) noexcept {
    return std::any_of(g_pending.begin(), g_pending.end(), [handle](const PendingDetonation& item) {
        return item.handle == handle;
    });
}

bool IsPending(std::int32_t handle, void* entity) noexcept {
    return std::any_of(g_pending.begin(), g_pending.end(), [handle, entity](const PendingDetonation& item) {
        return item.handle == handle && item.entity == entity;
    });
}

bool RemoveHook(DetonateHook& state) noexcept;

bool ClaimDetonation(std::int32_t handle, void* entity) noexcept {
    RetireStalePending();
    if (IsPending(handle)) return false;
    try {
        g_pending.push_back({handle, entity});
        return true;
    } catch (...) {
        return false;
    }
}

bool InstallDetonateHook(funrounds::native::grenade::Kind kind, void* address) noexcept {
    auto& state = HookFor(kind);
    if (!address) return false;
    try {
        DetourSlot slot(state.address, state.handle, &state.attempted);
        if (state.address && !slot.Refresh()) return false;
        if (state.handle) return slot.Add(polyhook::CallbackType::Pre, DetonateEntry);
        // Distinct projectile vtables can resolve to a single implementation.
        // Keep one callback owner; typed requests still use the resolved entry.
        for (auto& other : g_hooks) {
            if (&other == &state || other.shared || other.address != address) continue;
            const auto current = polyhook::FindDetour(address);
            if (!other.handle && other.attempted) other.handle = current;
            if (!current || current != other.handle ||
                !polyhook::IsCallbackRegistered(current, polyhook::CallbackType::Pre, DetonateEntry)) return false;
            state = {address, current, false, true};
            return true;
        }
        const plg::vector<polyhook::DataType> arguments{polyhook::DataType::Pointer};
        if (!slot.Acquire(address, [&] {
                return polyhook::HookDetour(address, polyhook::DataType::Void, arguments, -1,
                                            plg::string("FunRounds::StandardGrenade::Detonate"));
            }) || !slot.Add(polyhook::CallbackType::Pre, DetonateEntry)) {
            (void)RemoveHook(state);
            return false;
        }
        return true;
    } catch (...) {
        (void)RemoveHook(state);
        return false;
    }
}

bool RemoveHook(DetonateHook& state) noexcept {
    try {
        if (state.shared) {
            state = {};
            return true;
        }
        DetourSlot slot(state.address, state.handle, &state.attempted);
        if (!slot.Remove(polyhook::CallbackType::Pre, DetonateEntry) ||
            !slot.Remove(polyhook::CallbackType::Post, RainDetonateExit) || !slot.Release()) return false;
        if (!state.handle) state = {};
        return true;
    } catch (...) {
        return false;
    }
}

polyhook::ResultType DetonateEntry(polyhook::HookHandle,
                                   polyhook::ParametersHandle parameters,
                                   std::int32_t,
                                   polyhook::ReturnHandle,
                                   polyhook::CallbackType type) noexcept {
    if (type != polyhook::CallbackType::Pre || !parameters) return polyhook::ResultType::Ignored;
    try {
        auto* const entity = polyhook::GetArgument<void*>(parameters, 0);
        if (!entity) return polyhook::ResultType::Ignored;
        const auto handle = HandleFromEntity(entity);
        const auto kind = LiveProjectileKind(handle, entity);
        if (!kind) return polyhook::ResultType::Ignored;

        // Moving decoys are still in their rescheduling Think, not starting.
        if (*kind == Kind::Decoy && !DecoySettled(handle)) return polyhook::ResultType::Ignored;
        if (const auto hold = FindHold(handle, entity); hold != g_held.end()) {
            hold->deferred = true;
            return polyhook::ResultType::Supercede;
        }

        // This pre-entry claim catches natural fuse/collision paths as well as our
        // direct request. For smoke it is set before the asynchronous volume build
        // can emit smokegrenade_detonate or set m_bDidSmokeEffect.
        (void)ClaimDetonation(handle, entity);
        // Only the exact Molotov implementation has an inferno-producing scope.
        if (*kind == funrounds::native::grenade::Kind::Molotov) {
            BeginRainMolotovDetonation(handle, entity);
        }
    } catch (...) {
    }
    return polyhook::ResultType::Ignored;
}

} // namespace

bool HasGrenadeDetonateSupport(std::int32_t value) noexcept {
    if (!funrounds::native::grenade::ValidKind(value)) return false;
    const auto kind = static_cast<funrounds::native::grenade::Kind>(value);
    const auto fail = [kind](const char* reason) noexcept {
        LogSupportFailure(kind, reason);
        return false;
    };

    try {
        if (const auto* missing = MissingRequiredImport()) return fail(missing);
        if (!EnsureEngine()) return fail("engine schema or native gamedata is not ready");
        if (!RegisterMapEndCallback()) return fail("map-end claim cleanup callback could not be registered");
        if (kind == Kind::Decoy && (!__s2sdk_GetEntityAbsVelocity || !__s2sdk_SetEntityAbsVelocity)) {
            return fail("missing S2SDK decoy velocity imports");
        }
        auto& state = HookFor(kind);
        void* const detonate = state.address ? state.address : ProjectileDetonate(kind);
        if (!detonate) return fail("configured native entry did not resolve");
        if (!InstallDetonateHook(kind, detonate)) return fail("Detonate entry hook is unavailable or not owned");
        return true;
    } catch (...) {
        return fail("support probe threw while resolving the configured native entry");
    }
}

bool EnableRainMolotovLineage() noexcept {
    using funrounds::native::grenade::Kind;
    try {
        if (!HasGrenadeDetonateSupport(static_cast<std::int32_t>(Kind::Molotov))) return false;
        void* const address = HookFor(Kind::Molotov).address;
        const auto hook = address ? polyhook::FindDetour(address) : polyhook::HookHandle{};
        if (!hook || !polyhook::IsCallbackRegistered(hook, polyhook::CallbackType::Pre, DetonateEntry)) return false;
        auto& state = HookFor(Kind::Molotov);
        DetourSlot slot(state.address, state.handle, &state.attempted);
        return slot.Add(polyhook::CallbackType::Post, RainDetonateExit);
    } catch (...) {
        return false;
    }
}

std::int32_t GetGrenadeDetonationState(std::int32_t projectileHandle, std::int32_t value) noexcept {
    using funrounds::native::grenade::StateResult;
    using funrounds::native::grenade::StateValue;
    if (!funrounds::native::grenade::ValidKind(value) || projectileHandle == -1) {
        return StateValue(StateResult::Failed);
    }
    const auto kind = static_cast<funrounds::native::grenade::Kind>(value);
    try {
        if (!HookFor(kind).handle && !HasGrenadeDetonateSupport(value)) return StateValue(StateResult::Failed);
        if (!s2sdk::IsValidEntHandle(projectileHandle)) return StateValue(StateResult::StartedOrGone);

        // Validate the live full handle and its concrete projectile class before
        // consulting the shared natural/direct Detonate-entry claim set.
        void* entity{};
        if (!CurrentProjectile(projectileHandle, kind, entity)) {
            return StateValue(s2sdk::IsValidEntHandle(projectileHandle)
                                  ? StateResult::Failed
                                  : StateResult::StartedOrGone);
        }
        return StateValue(IsPending(projectileHandle, entity) ? StateResult::StartedOrGone
                                                              : StateResult::NotStarted);
    } catch (...) {
        return StateValue(StateResult::Failed);
    }
}

std::int32_t DetonateGrenade(std::int32_t projectileHandle, std::int32_t value) noexcept {
    using funrounds::native::grenade::RequestResult;
    using funrounds::native::grenade::ResultValue;
    if (!funrounds::native::grenade::ValidKind(value)) return ResultValue(RequestResult::Failed);
    const auto kind = static_cast<funrounds::native::grenade::Kind>(value);
    try {
        if (!HasGrenadeDetonateSupport(value)) return ResultValue(RequestResult::Failed);
        if (projectileHandle == -1) return ResultValue(RequestResult::Failed);
        // A non-sentinel full handle can become invalid between the adapter's
        // last read and this request when a natural fuse/collision wins the race.
        if (!s2sdk::IsValidEntHandle(projectileHandle)) return ResultValue(RequestResult::AlreadyStartedOrGone);
        RetireStalePending();
        void* const detonate = HookFor(kind).address;
        void* entity{};
        if (!detonate) return ResultValue(RequestResult::Failed);
        if (IsPending(projectileHandle)) return ResultValue(RequestResult::AlreadyStartedOrGone);
        if (!CurrentProjectile(projectileHandle, kind, entity)) {
            return ResultValue(s2sdk::IsValidEntHandle(projectileHandle)
                                   ? RequestResult::Failed
                                   : RequestResult::AlreadyStartedOrGone);
        }

        if (kind == Kind::Decoy && !DecoySettled(projectileHandle)) {
            s2sdk::SetEntityAbsVelocity(projectileHandle, plg::vec3{});
            if (!DecoySettled(projectileHandle)) return ResultValue(RequestResult::Failed);
        }
        if (const auto hold = FindHold(projectileHandle, entity); hold != g_held.end()) g_held.erase(hold);

        // Claim before dispatch so a second request in this frame cannot race the
        // entry callback. The hook also claims natural engine entry points.
        if (!ClaimDetonation(projectileHandle, entity)) {
            return ResultValue(IsPending(projectileHandle) ? RequestResult::AlreadyStartedOrGone
                                                           : RequestResult::Failed);
        }
        reinterpret_cast<Detonate>(detonate)(entity);
        return ResultValue(RequestResult::Submitted);
    } catch (...) {
        // Preserve the claim if dispatch began: retrying an ambiguous native call
        // could run the engine's effects twice.
        return ResultValue(RequestResult::Failed);
    }
}

bool SetGrenadeDetonationHold(std::int32_t projectileHandle, std::int32_t value, std::int32_t operation) noexcept {
    if (!grenade::ValidKind(value) || projectileHandle == -1 || operation < 0 || operation > 2) return false;
    const bool hold = operation == 1;
    const auto kind = static_cast<Kind>(value);
    if (!grenade::UsesDetonationHold(kind)) return false;
    try {
        if (hold && !HasGrenadeDetonateSupport(value)) return false;
        RetireStalePending();
        if (!s2sdk::IsValidEntHandle(projectileHandle)) return !hold;
        void* entity{};
        if (!CurrentProjectile(projectileHandle, kind, entity)) return false;
        const auto it = FindHold(projectileHandle, entity);
        if (hold) {
            if (IsPending(projectileHandle)) return false;
            if (it == g_held.end()) g_held.push_back({{projectileHandle, entity}, kind, false});
        } else if (it != g_held.end()) {
            const bool deferred = it->deferred;
            g_held.erase(it);
            // Replay only an actual intercepted native request. An untouched
            // flight/Think keeps the engine's original scheduling.
            if (deferred && operation == 0) reinterpret_cast<Detonate>(HookFor(kind).address)(entity);
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool ShutdownStandardGrenades() noexcept {
    while (!g_held.empty()) {
        const auto hold = g_held.back();
        if (!SetGrenadeDetonationHold(hold.identity.handle, static_cast<std::int32_t>(hold.kind), 0)) return false;
    }
    // A failed detach retains hook identity and pending handles for cleanup retry.
    bool clean = true;
    for (auto& hook : g_hooks) clean = RemoveHook(hook) && clean;
    if (!clean || !UnregisterMapEndCallback()) return false;
    g_pending.clear();
    g_failureLogged = {};
    return true;
}

} // namespace funrounds::native::detail

extern "C" PLUGIN_API bool HasGrenadeDetonateSupport(std::int32_t kind) {
    return funrounds::native::detail::HasGrenadeDetonateSupport(kind);
}

extern "C" PLUGIN_API std::int32_t GetGrenadeDetonationState(std::int32_t projectileHandle, std::int32_t kind) {
    return funrounds::native::detail::GetGrenadeDetonationState(projectileHandle, kind);
}

extern "C" PLUGIN_API std::int32_t DetonateGrenade(std::int32_t projectileHandle, std::int32_t kind) {
    return funrounds::native::detail::DetonateGrenade(projectileHandle, kind);
}


extern "C" PLUGIN_API bool SetGrenadeDetonationHold(std::int32_t projectileHandle, std::int32_t kind, std::int32_t operation) noexcept {
    return funrounds::native::detail::SetGrenadeDetonationHold(projectileHandle, kind, operation);
}
