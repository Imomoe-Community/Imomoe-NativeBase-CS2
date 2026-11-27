// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "reload_sync.hpp"
#include "reload_sync_policy.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <plugin_export.h>
#include <entity2/entityinstance.h>
#include <plg/plugin.hpp>
#include <polyhook/polyhook.hpp>
#include <s2sdk/s2sdk.hpp>

#include "detour_slot.hpp"
#include "engine.hpp"
#include "entity_handle.hpp"

namespace funrounds::native::detail {
namespace {

using namespace reload_sync;

constexpr auto kInputSignature = SignatureKey{"FunRounds::WeaponInputReloadSync"};
constexpr auto kFullAdmissionSignature = SignatureKey{"FunRounds::ReloadFullAdmission"};
#if defined(_WIN32)
constexpr std::array<SignatureKey, 4> kFullSignatures{
    kFullAdmissionSignature, SignatureKey{"FunRounds::ReloadFullStart"}, SignatureKey{"FunRounds::ReloadFullTransfer"},
    SignatureKey{"FunRounds::ReloadFullAlternateTransfer"}};
#else
constexpr std::array<SignatureKey, 1> kFullSignatures{kFullAdmissionSignature};
constexpr std::array<SignatureKey, 4> kFullCallerSignatures{
    SignatureKey{"FunRounds::ReloadFullDispatcherCall"}, SignatureKey{"FunRounds::ReloadFullStartCall"},
    SignatureKey{"FunRounds::ReloadFullTransferCall"}, SignatureKey{"FunRounds::ReloadFullAlternateTransferCall"}};
#endif


struct InputHook {
    void* address{};
    polyhook::HookHandle handle{};
    bool attempted{};
};

struct MidHook {
    std::string_view signature;
    void* address{};
    polyhook::HookHandle handle{};
};

bool SameTarget(const PlayerIdentity& player) noexcept;

PolicyState g_policy;
InputHook g_inputHook;
std::array<MidHook, kFullSignatures.size()> g_fullHooks{};
#if defined(__linux__)
std::array<std::uintptr_t, kFullCallerSignatures.size()> g_fullCallerReturns{};
#endif
std::int32_t g_subclassOffset{-1};
std::int32_t g_reserveClipsOffset{-1};
struct SyncSchema {
    SchemaField movementServices{};
    SchemaField buttonState{};
    SchemaField buttonMask{};
    SchemaField reloading{};
    SchemaField reserve{};
    SchemaField clip{};
    SchemaField nextPrimaryAttackTick{};
    bool ready{};
    bool tickReady{};
} g_syncSchema;
thread_local DispatchStack g_dispatch;
thread_local std::size_t g_untrackedDispatchDepth{};
bool g_enabled{};
bool g_fullMagazineEnabled{};
bool g_reloadEventRegistered{};
bool g_roundEndEventRegistered{};
std::array<bool, 5> g_lifecycleEventsRegistered{};
constexpr std::array<std::pair<const char*, PlayerEvent>, 5> kLifecycleEvents{{
    {"player_death", PlayerEvent::Death},
    {"player_spawn", PlayerEvent::Spawn},
    {"player_disconnect", PlayerEvent::Disconnect},
    {"bot_takeover", PlayerEvent::BotTakeover},
    {"item_equip", PlayerEvent::Equip},
}};

polyhook::ResultType Input(polyhook::HookHandle,
                           polyhook::ParametersHandle,
                           std::int32_t,
                           polyhook::ReturnHandle,
                           polyhook::CallbackType) noexcept;
s2sdk::ResultType Event(const plg::string&, void*, bool) noexcept;
polyhook::ResultType FullCapacity(polyhook::HookHandle,
                                  polyhook::ParametersHandle,
                                  std::int32_t,
                                  polyhook::ReturnHandle,
                                  polyhook::CallbackType) noexcept;

void Log(std::string message) noexcept {
    try {
        s2sdk::PrintToServer(plg::string("[FUNROUNDS_RELOAD_SYNC] " + std::move(message)));
    } catch (...) {
    }
}

bool RequiredImportsAvailable(bool synchronize) noexcept {
    return (!synchronize || __s2sdk_GetGameTickCount) &&
           __s2sdk_GetGameConfigSignature && __s2sdk_GetSchemaOffset && __s2sdk_GetEntSchemaEnt2 &&
           __s2sdk_IsClientInGame && __s2sdk_IsClientAlive &&
           __s2sdk_GetMaxClients && __s2sdk_PlayerSlotToEntHandle && __s2sdk_GetClientPawn &&
           __s2sdk_EntPointerToEntHandle && __s2sdk_GetClientActiveWeapon && __s2sdk_GetEntityOwner &&
           __s2sdk_IsValidEntHandle && __s2sdk_EntHandleToEntPointer && __s2sdk_IsEntityPlayerController &&
           __s2sdk_GetEventPlayerSlot &&
           __s2sdk_HookEvent && __s2sdk_UnhookEvent && __polyhook_HookDetour && __polyhook_FindDetour &&
           __polyhook_AddCallback && __polyhook_IsCallbackRegistered && __polyhook_RemoveCallback &&
           __polyhook_AreCallbacksRegistered && __polyhook_UnhookDetour && __polyhook_GetArgumentPointer;
}

bool FullReloadImportsAvailable() noexcept {
    const bool available = __polyhook_HookDetour2 && __polyhook_GetRegisterUInt32 && __polyhook_SetRegisterUInt32 &&
           __polyhook_GetRegisterUInt64 && __polyhook_SetRegisterUInt64 && __polyhook_GetRegisterPointer &&
           __s2sdk_GetEntityOwner && __s2sdk_EntHandleToEntPointer &&
           __s2sdk_GetEntSchemaEnt2 &&
           __s2sdk_GetMaxClients && __s2sdk_PlayerSlotToEntHandle && __s2sdk_IsClientInGame && __s2sdk_GetClientPawn;
#if defined(__linux__)
    return available && __s2sdk_GetGameConfigOffset;
#else
    return available;
#endif
}

#if defined(__linux__)
bool ResolveFullCallerReturns() noexcept {
    try {
        auto* const frame = static_cast<std::uint8_t*>(ProbeSignature(SignatureKey{"FunRounds::ReloadFullCapacityFrame"}));
        if (!frame) return false;
        for (std::size_t i = 0; i < kFullCallerSignatures.size(); ++i) {
            auto* const call = static_cast<std::uint8_t*>(ProbeSignature(kFullCallerSignatures[i]));
            if (!call) return false;
            if (call[0] != 0xE8) {
                InvalidGamedata(kFullCallerSignatures[i].name());
                return false;
            }
            std::int32_t displacement{};
            std::memcpy(&displacement, call + 1, sizeof(displacement));
            const auto target = reinterpret_cast<std::uintptr_t>(call + 5) + displacement;
            if (target != reinterpret_cast<std::uintptr_t>(frame)) {
                InvalidGamedata(kFullCallerSignatures[i].name());
                return false;
            }
            g_fullCallerReturns[i] = reinterpret_cast<std::uintptr_t>(call + 5);
        }
        return true;
    } catch (...) {
        return false;
    }
}
#endif

void* ResolveFullAddress(SignatureKey signature) noexcept {
    try {
        if (!FullReloadImportsAvailable() || !EnsureEngine()) return nullptr;
        auto* const address = ProbeSignature(signature);
        if (!address) return nullptr;
        const auto installed = std::find_if(g_fullHooks.begin(), g_fullHooks.end(), [address](const MidHook& hook) {
            return hook.address == address && hook.handle && polyhook::FindDetour(address) == hook.handle;
        });
        if (installed != g_fullHooks.end()) return address;
#if defined(__linux__)
        auto* const frame = static_cast<std::uint8_t*>(ProbeSignature(SignatureKey{"FunRounds::ReloadFullCapacityFrame"}));
        constexpr auto compare = OffsetKey{"FunRounds::ReloadFullCompareOffset"};
        const int compareOffset = ProbeOffset(compare);
        if (!frame || compareOffset < 0) return nullptr;
        if (reinterpret_cast<std::uintptr_t>(frame) + static_cast<std::uintptr_t>(compareOffset) !=
            reinterpret_cast<std::uintptr_t>(address)) {
            InvalidGamedata(compare.name());
            return nullptr;
        }
        if (!ResolveFullCallerReturns()) return nullptr;
#endif
        return address;
    } catch (...) {
        return nullptr;
    }
}

bool RequiredSyncSchemaAvailable() noexcept {
    if (g_syncSchema.ready) return true;
    try {
        if (!ResolveSchemaField("CBasePlayerPawn", "m_pMovementServices", sizeof(void*), g_syncSchema.movementServices) ||
            !ResolveSchemaField("CPlayer_MovementServices", "m_nButtons", 0, g_syncSchema.buttonState) ||
            !ResolveSchemaField("CInButtonState", "m_pButtonStates", sizeof(ButtonStates), g_syncSchema.buttonMask) ||
            !ResolveSchemaField("CCSWeaponBase", "m_bInReload", sizeof(bool), g_syncSchema.reloading) ||
            !ResolveSchemaField("CBasePlayerWeapon", "m_pReserveAmmo", 0, g_syncSchema.reserve) ||
            !ResolveSchemaField("CBasePlayerWeapon", "m_iClip1", sizeof(std::int32_t), g_syncSchema.clip) ||
            ProbeSchemaOffset("CBasePlayerPawn", "m_hController") < 0) return false;
        if (g_syncSchema.buttonState.offset < 0 || g_syncSchema.buttonMask.offset < 0 ||
            static_cast<std::size_t>(g_syncSchema.buttonMask.offset) + sizeof(ButtonStates) > g_syncSchema.buttonState.size) {
            MissingSchema("CPlayer_MovementServices", "m_nButtons");
            return false;
        }
        g_syncSchema.ready = true;
        return true;
    } catch (...) {
        return false;
    }
}

bool RequiredFullSchemaAvailable() noexcept {
    if (g_subclassOffset >= 0 && g_reserveClipsOffset >= 0) return true;
    if (!RequiredSyncSchemaAvailable()) return false;
    g_subclassOffset = ProbeSchemaOffset("CBaseEntity", "m_nSubclassID");
    g_reserveClipsOffset = ProbeSchemaOffset("CCSWeaponBaseVData", "m_bReserveAmmoAsClips");
    return g_subclassOffset >= 0 && g_reserveClipsOffset >= 0;
}

template <typename T>
T ReadAt(const void* object, std::int32_t offset) noexcept {
    T value{};
    if (object && offset >= 0) std::memcpy(&value, static_cast<const std::uint8_t*>(object) + offset, sizeof(T));
    return value;
}

bool IsFullMagazine(const PlayerIdentity& player) noexcept {
    try {
        if (!SameTarget(player)) return false;
        auto* const weapon = s2sdk::EntHandleToEntPointer(player.weapon);
        if (!weapon || g_subclassOffset < 0 || g_reserveClipsOffset < 0) return false;
        const auto subclass = ReadAt<std::uint32_t>(weapon, g_subclassOffset);
        void* data{};
        std::memcpy(&data, static_cast<std::uint8_t*>(weapon) + g_subclassOffset + sizeof(subclass), sizeof(data));
        if (!data || ReadAt<std::uint8_t>(data, g_reserveClipsOffset) == 0) return false;
        const int reserve = ReadAt<std::int32_t>(weapon, g_syncSchema.reserve.offset);
        const int clip = ReadAt<std::int32_t>(weapon, g_syncSchema.clip.offset);
        return reserve > 0 && clip > 0;
    } catch (...) {
        return false;
    }
}

void* ResolveInputAddress(bool synchronize) noexcept {
    try {
        if (!RequiredImportsAvailable(synchronize) || !EnsureEngine() || !RequiredSyncSchemaAvailable()) return nullptr;
        if (synchronize && !g_syncSchema.tickReady) {
            // GameTick_t boxes one int32; the native comparison also has a subtick fraction.
            g_syncSchema.tickReady = ResolveSchemaField("CBasePlayerWeapon", "m_nNextPrimaryAttackTick",
                                                        sizeof(std::int32_t), g_syncSchema.nextPrimaryAttackTick);
            if (!g_syncSchema.tickReady) return nullptr;
        }
        auto* const address = ProbeSignature(kInputSignature);
        if (!address) return nullptr;
        // Our installed detour has already validated this exact gamedata address.
        // Do not mistake our patched entry bytes for ABI drift during a support query.
        if (g_inputHook.address == address && g_inputHook.handle &&
            polyhook::FindDetour(address) == g_inputHook.handle) return address;
        return address;
    } catch (...) {
        return nullptr;
    }
}

bool ValidHandle(int handle) noexcept {
    try {
        return handle != -1 && s2sdk::IsValidEntHandle(handle) && s2sdk::EntHandleToEntPointer(handle);
    } catch (...) {
        return false;
    }
}

bool ReadLife(int slot, PlayerLife& life) noexcept {
    try {
        if (slot < 0 || slot >= kMaxPlayers || slot >= s2sdk::GetMaxClients() ||
            !s2sdk::IsClientInGame(slot) || !s2sdk::IsClientAlive(slot)) {
            return false;
        }
        auto* const pawn = s2sdk::GetClientPawn(slot);
        const int controller = s2sdk::PlayerSlotToEntHandle(slot);
        if (!pawn || !ValidHandle(controller)) return false;
        const int pawnHandle = s2sdk::EntPointerToEntHandle(pawn);
        if (!ValidHandle(pawnHandle) || s2sdk::EntHandleToEntPointer(pawnHandle) != pawn) {
            return false;
        }
        life = {slot, controller, pawnHandle, pawn};
        return true;
    } catch (...) {
        return false;
    }
}

int ActiveWeapon(const PlayerLife& life) {
    const int weapon = s2sdk::GetClientActiveWeapon(life.slot);
    return ValidHandle(weapon) && s2sdk::GetEntityOwner(weapon) == life.pawnHandle ? weapon : -1;
}

bool ReadPlayer(int slot, PlayerIdentity& player) noexcept {
    try {
        PlayerLife life;
        if (!ReadLife(slot, life)) return false;
        const int weapon = ActiveWeapon(life);
        if (weapon == -1) return false;
        player = {life, weapon};
        return true;
    } catch (...) {
        return false;
    }
}

std::optional<PlayerIdentity> PlayerForWeapon(int weapon) noexcept {
    try {
        auto* const weaponEntity = s2sdk::EntHandleToEntPointer(weapon);
        if (!weaponEntity) return std::nullopt;
        const int ownerHandle = s2sdk::GetEntityOwner(weapon);
        auto* const pawn = s2sdk::EntHandleToEntPointer(ownerHandle);
        if (!pawn) return std::nullopt;
        const int controllerHandle = s2sdk::GetEntSchemaEnt2(
            pawn, plg::string("CBasePlayerPawn"), plg::string("m_hController"), 0);
        auto* const controller = static_cast<CEntityInstance*>(EntityFromHandle(controllerHandle));
        if (!controller || !IsPlayerController(controllerHandle) || HandleFromEntity(controller) != controllerHandle)
            return std::nullopt;
        const auto controllerRef = controller->GetRefEHandle();
        const int slot = controllerRef.GetEntryIndex() - 1;
        PlayerLife life;
        if (controllerRef.ToInt() != ToNativeHandle(controllerHandle) || !ReadLife(slot, life) ||
            life.controller != controllerHandle || life.pawn != pawn || life.pawnHandle != ownerHandle ||
            s2sdk::GetClientActiveWeapon(slot) != weapon) {
            return std::nullopt;
        }
        return PlayerIdentity{life, weapon};
    } catch (...) {
    }
    return std::nullopt;
}

bool SameLife(const PlayerLife& player) noexcept {
    PlayerLife current;
    return ReadLife(player.slot, current) && current == player;
}

bool SameTarget(const PlayerIdentity& player) noexcept {
    PlayerIdentity current;
    return ReadPlayer(player.slot, current) && current == player;
}

std::byte* CurrentButtonMask(const PlayerLife& player) noexcept {
    auto* const movement = ReadAt<std::byte*>(player.pawn, g_syncSchema.movementServices.offset);
    return movement ? movement + g_syncSchema.buttonState.offset + g_syncSchema.buttonMask.offset : nullptr;
}

std::optional<bool> ReloadState(const PlayerIdentity& player) noexcept {
    try {
        if (!SameTarget(player)) return std::nullopt;
        if (!RequiredSyncSchemaAvailable()) return std::nullopt;
        return ReadAt<bool>(s2sdk::EntHandleToEntPointer(player.weapon), g_syncSchema.reloading.offset);
    } catch (...) {
        return std::nullopt;
    }
}

bool ReloadButtonDown(const PlayerIdentity& player) noexcept {
    const auto* const buttons = CurrentButtonMask(player);
    if (!buttons) return false;
    ButtonStates states;
    std::memcpy(states, buttons, sizeof(states));
    return reload_sync::ReloadButtonDown(states);
}

polyhook::ResultType FullCapacity(polyhook::HookHandle handle,
                                  polyhook::ParametersHandle parameters,
                                  std::int32_t,
                                  polyhook::ReturnHandle,
                                  polyhook::CallbackType phase) noexcept {
    using Result = polyhook::ResultType;
    if (phase != polyhook::CallbackType::Pre || !parameters ||
        (!g_fullMagazineEnabled && g_policy.fullReceiptCount == 0)) return Result::Ignored;
    const auto site = std::find_if(g_fullHooks.begin(), g_fullHooks.end(), [handle](const MidHook& hook) {
        return hook.handle == handle;
    });
    if (site == g_fullHooks.end()) return Result::Ignored;

    try {
        auto registers = static_cast<polyhook::RegistersHandle>(parameters);
#if defined(_WIN32)
        auto* const weapon = polyhook::GetRegister<void*>(registers, polyhook::RegisterType::RSI);
        auto gap = static_cast<std::int32_t>(polyhook::GetRegisterUInt64(registers, polyhook::RegisterType::RAX));
        const auto reserve = static_cast<std::int32_t>(polyhook::GetRegisterUInt64(registers, polyhook::RegisterType::RBP));
#else
        auto* const weapon = polyhook::GetRegister<void*>(registers, polyhook::RegisterType::RBX);
        auto gap = static_cast<std::int32_t>(polyhook::GetRegisterUInt32(registers, polyhook::RegisterType::R12));
        const auto reserve = static_cast<std::int32_t>(polyhook::GetRegisterUInt32(registers, polyhook::RegisterType::R13));
#endif
        if (!weapon || gap != 0 || reserve <= 0) return Result::Ignored;
        const int weaponHandle = s2sdk::EntPointerToEntHandle(weapon);
        bool transferSite = site->signature == "FunRounds::ReloadFullTransfer" ||
                            site->signature == "FunRounds::ReloadFullAlternateTransfer";
        bool admissionSite = site->signature == kFullAdmissionSignature.name() ||
                             site->signature == "FunRounds::ReloadFullStart";
#if defined(__linux__)
        auto* const nativeFrame = polyhook::GetRegister<void*>(registers, polyhook::RegisterType::RBP);
        std::uintptr_t caller{};
        if (!nativeFrame) return Result::Ignored;
        std::memcpy(&caller, static_cast<const std::uint8_t*>(nativeFrame) + sizeof(std::uintptr_t), sizeof(caller));
        transferSite = caller == g_fullCallerReturns[2] || caller == g_fullCallerReturns[3];
        admissionSite = caller == g_fullCallerReturns[0] || caller == g_fullCallerReturns[1];
        if (!transferSite && !admissionSite) return Result::Ignored;
#endif
        std::optional<PlayerIdentity>* authorization = nullptr;
        const bool receiptAuthorization = transferSite;
        auto* const frame = g_untrackedDispatchDepth == 0 ? g_dispatch.Current() : nullptr;
        if (transferSite) {
            const auto current = PlayerForWeapon(weaponHandle);
            if (current) {
                auto& receipt = g_policy.fullReceipts[static_cast<std::size_t>(current->slot)];
                if (receipt && receipt->weapon == weaponHandle) authorization = &receipt;
            }
        } else if (admissionSite && g_fullMagazineEnabled && frame && frame->weapon == weapon) {
            if (!frame->player) frame->player = PlayerForWeapon(weaponHandle);
            if (frame->player) {
                const auto& candidate = *frame->player;
                auto& attempt = g_policy.fullAttempts[static_cast<std::size_t>(candidate.slot)];
                if (attempt == candidate) {
                    authorization = &attempt;
                } else if (IsFullMagazine(candidate) && ReloadButtonDown(candidate)) {
                    attempt = candidate;
                    frame->ownsFullAttempt = true;
                    authorization = &attempt;
                }
            }
        }
        if ((!transferSite && !admissionSite) || (admissionSite && !g_fullMagazineEnabled))
            return Result::Ignored;
        if (!authorization || !*authorization || (*authorization)->weapon != weaponHandle) return Result::Ignored;
        if (!SameTarget(**authorization)) {
            if (receiptAuthorization) g_policy.ClearReceipt(static_cast<std::size_t>((*authorization)->slot));
            else authorization->reset();
            return Result::Ignored;
        }
        if (!IsFullMagazine(**authorization)) {
            if (receiptAuthorization) g_policy.ClearReceipt(static_cast<std::size_t>((*authorization)->slot));
            return Result::Ignored;
        }

        const auto reloading = ReloadState(**authorization);
        if (transferSite) {
            if (!reloading || !*reloading) {
                g_policy.ClearReceipt(static_cast<std::size_t>((*authorization)->slot));
                return Result::Ignored;
            }
        } else if (reloading && *reloading) {
            return Result::Ignored;
        } else if (!ReloadButtonDown(**authorization)) {
            authorization->reset();
            return Result::Ignored;
        }

#if !defined(_WIN32)
        if (gap == 0 && reserve > 0) {
            polyhook::SetRegisterUInt32(registers, polyhook::RegisterType::R12, 1);
            if (receiptAuthorization) g_policy.ClearReceipt(static_cast<std::size_t>((*authorization)->slot));
            return Result::Ignored;
        }
#else
        if (gap == 0 && reserve > 0) {
            polyhook::SetRegisterUInt64(registers, polyhook::RegisterType::RAX, 1);
            if (transferSite) g_policy.ClearReceipt(static_cast<std::size_t>((*authorization)->slot));
            return Result::Ignored;
        }
#endif
    } catch (...) {
        Log("full-magazine comparison inspection failed");
    }
    return Result::Ignored;
}

bool RemoveInputHook() noexcept {
    try {
        DetourSlot slot(g_inputHook.address, g_inputHook.handle, &g_inputHook.attempted);
        return slot.Remove(polyhook::CallbackType::Pre, Input) &&
               slot.Remove(polyhook::CallbackType::Post, Input) && slot.Release();
    } catch (...) {
        return false;
    }
}

bool RemoveFullHooks() noexcept {
    bool success = true;
    for (auto& hook : g_fullHooks) {
        try {
            DetourSlot slot(hook.address, hook.handle);
            if (!slot.Remove(polyhook::CallbackType::Pre, FullCapacity) || !slot.Release()) {
                success = false;
            } else if (!hook.handle) {
                hook = {};
            }
        } catch (...) {
            success = false;
        }
    }
    return success;
}

bool InstallFullHooks() noexcept {
    try {
        for (std::size_t i = 0; i < g_fullHooks.size(); ++i) {
            auto& hook = g_fullHooks[i];
            void* const address = hook.address ? hook.address : ResolveFullAddress(kFullSignatures[i]);
            DetourSlot slot(hook.address, hook.handle);
            if (!slot.Acquire(address, [&] {
                    return polyhook::HookDetour2(address, kFullSignatures[i].name().data());
                }) || !slot.Add(polyhook::CallbackType::Pre, FullCapacity)) return false;
            hook.signature = kFullSignatures[i].name();
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool RemoveEvents() noexcept {
    bool success = true;
    std::array<std::pair<const char*, bool*>, 2> baseEvents{{
        {"weapon_reload", &g_reloadEventRegistered}, {"round_end", &g_roundEndEventRegistered}}};
    for (const auto& [name, registered] : baseEvents) {
        if (!*registered) continue;
        try {
            if (s2sdk::UnhookEvent(plg::string(name), Event, s2sdk::HookMode::Post) == s2sdk::EventHookError::Okay) {
                *registered = false;
                continue;
            }
        } catch (...) {
        }
        success = false;
    }
    for (std::size_t i = 0; i < kLifecycleEvents.size(); ++i) {
        if (!g_lifecycleEventsRegistered[i]) continue;
        try {
            if (s2sdk::UnhookEvent(plg::string(kLifecycleEvents[i].first), Event, s2sdk::HookMode::Post) ==
                s2sdk::EventHookError::Okay) {
                g_lifecycleEventsRegistered[i] = false;
                continue;
            }
        } catch (...) {
        }
        success = false;
    }
    return success;
}

void RestoreEdit(const DispatchFrame& frame) noexcept {
    if (!frame.editedButtons || !SameLife(*frame.player) ||
        CurrentButtonMask(*frame.player) != frame.editedButtons) return;
    ButtonStates states;
    std::memcpy(states, frame.editedButtons, sizeof(states));
    // An edit only sets a previously clear bit; preserve all other input.
    states[0] &= ~kReloadBit;
    std::memcpy(frame.editedButtons, states, sizeof(states[0]));
}

void QueueOtherPlayers(const PlayerLife& source, int tick) {
    const int count = std::min(s2sdk::GetMaxClients(), kMaxPlayers);
    for (int slot = 0; slot < count; ++slot) {
        if (slot == source.slot) continue;
        PlayerLife life;
        if (!ReadLife(slot, life)) continue;
        const int active = ActiveWeapon(life);
        const auto* weapon = active != -1 ? s2sdk::EntHandleToEntPointer(active) : nullptr;
        g_policy.Queue(life, tick, ReadAt<bool>(weapon, g_syncSchema.reloading.offset));
    }
}

bool InstallInputHook() noexcept {
    try {
        void* const address = ResolveInputAddress(false);
        const plg::vector<polyhook::DataType> arguments{polyhook::DataType::Pointer};
        DetourSlot slot(g_inputHook.address, g_inputHook.handle, &g_inputHook.attempted);
        if (!slot.Acquire(address, [&] {
                return polyhook::HookDetour(address, polyhook::DataType::Void, arguments, -1,
                                            plg::string("FunRounds::WeaponInputReloadSync"));
            }) || !slot.Add(polyhook::CallbackType::Pre, Input) ||
            !slot.Add(polyhook::CallbackType::Post, Input)) {
            (void)RemoveInputHook();
            return false;
        }
        return true;
    } catch (...) {
        (void)RemoveInputHook();
        return false;
    }
}

bool InstallEvents() noexcept {
    for (const auto& [name, registered] : std::array<std::pair<const char*, bool*>, 2>{{
             {"weapon_reload", &g_reloadEventRegistered}, {"round_end", &g_roundEndEventRegistered}}}) {
        if (*registered) continue;
        try {
            if (s2sdk::HookEvent(plg::string(name), Event, s2sdk::HookMode::Post) != s2sdk::EventHookError::Okay) {
                return false;
            }
            *registered = true;
        } catch (...) {
            return false;
        }
    }
    for (std::size_t i = 0; i < kLifecycleEvents.size(); ++i) {
        if (g_lifecycleEventsRegistered[i]) continue;
        try {
            if (s2sdk::HookEvent(plg::string(kLifecycleEvents[i].first), Event, s2sdk::HookMode::Post) !=
                s2sdk::EventHookError::Okay) {
                return false;
            }
            g_lifecycleEventsRegistered[i] = true;
        } catch (...) {
            return false;
        }
    }
    return true;
}

bool HasFullReceipts() noexcept {
    return g_policy.fullReceiptCount != 0;
}

void PruneFinishedReceipts() noexcept {
    for (auto& receipt : g_policy.fullReceipts) {
        if (!receipt) continue;
        const auto reloading = ReloadState(*receipt);
        if (!SameTarget(*receipt) || !reloading || !*reloading)
            g_policy.ClearReceipt(static_cast<std::size_t>(receipt->slot));
    }
}

polyhook::ResultType Input(polyhook::HookHandle,
                           polyhook::ParametersHandle parameters,
                           std::int32_t,
                           polyhook::ReturnHandle,
                           polyhook::CallbackType phase) noexcept {
    using Result = polyhook::ResultType;
    try {
        if (phase == polyhook::CallbackType::Post) {
            if (g_untrackedDispatchDepth != 0) {
                --g_untrackedDispatchDepth;
                return Result::Ignored;
            }
            if (!g_dispatch.Current()) return Result::Ignored;
            const auto frame = g_dispatch.Pop();
            RestoreEdit(frame);
            if (!frame.player) return Result::Ignored;

            const auto& player = *frame.player;
            auto& attempt = g_policy.fullAttempts[static_cast<std::size_t>(player.slot)];
            if (frame.ownsFullAttempt && attempt == player) attempt.reset();
            if (!g_enabled || (!frame.attempted && !frame.ShouldBroadcast(true))) return Result::Ignored;

            const bool sameWeapon = SameTarget(player);
            const bool nowReloading = sameWeapon && ReadAt<bool>(frame.weapon, g_syncSchema.reloading.offset);
            if (!frame.wasReloading && nowReloading) g_dispatch.ObserveNestedReload(player);
            const int tick = s2sdk::GetGameTickCount();
            if (frame.attempted) {
                const bool accepted = frame.weaponReloadFired || nowReloading;
                const bool rejected = !accepted && sameWeapon &&
                    ReadyToReject(ReadAt<std::int32_t>(frame.weapon, g_syncSchema.clip.offset),
                                  ReadAt<std::int32_t>(frame.weapon, g_syncSchema.nextPrimaryAttackTick.offset), tick);
                g_policy.Finish(player, tick, accepted, rejected);
            }
            if (frame.ShouldBroadcast(nowReloading)) QueueOtherPlayers(player, tick);
            return Result::Ignored;
        }
        if (g_untrackedDispatchDepth != 0) {
            ++g_untrackedDispatchDepth;
            return Result::Ignored;
        }
        DispatchFrame* frame{};
        try {
            frame = &g_dispatch.Push(polyhook::GetArgumentPointer(parameters, 0));
        } catch (...) {
            // Do not let an untracked invocation's Post pop its enclosing frame.
            ++g_untrackedDispatchDepth;
            return Result::Ignored;
        }
        if (!frame->weapon || !g_enabled) return Result::Ignored;
        frame->player = PlayerForWeapon(s2sdk::EntPointerToEntHandle(frame->weapon));
        if (!frame->player) return Result::Ignored;

        const auto& player = *frame->player;
        frame->wasReloading = ReadAt<bool>(frame->weapon, g_syncSchema.reloading.offset);
        const bool pending = g_policy.Begin(player, s2sdk::GetGameTickCount(), frame->wasReloading);
        if (!pending && !g_dispatch.IsNested()) return Result::Ignored;
        auto* const buttons = CurrentButtonMask(player);
        if (!buttons) return Result::Ignored;
        frame->attempted = pending;
        ButtonStates states;
        std::memcpy(states, buttons, sizeof(states));
        if (reload_sync::ReloadButtonDown(states)) {
            frame->synthetic = g_dispatch.IsNested() && (states[2] & kReloadBit) == 0 &&
                g_dispatch.IsInjected(player, buttons);
        } else if (pending) {
            frame->synthetic = true;
            frame->editedButtons = buttons;
            states[0] |= kReloadBit;
            std::memcpy(buttons, states, sizeof(states[0]));
        }
    } catch (...) {
        Log("reload input inspection failed");
    }
    return Result::Ignored;
}

s2sdk::ResultType Event(const plg::string& name, void* event, bool) noexcept {
    try {
        const std::string_view eventName(name.c_str());
        if (eventName == "round_end") {
            g_policy.OnRoundEnd();
            return s2sdk::ResultType::Continue;
        }
        if (!event) return s2sdk::ResultType::Continue;
        const auto lifecycle = std::find_if(kLifecycleEvents.begin(), kLifecycleEvents.end(), [&](const auto& entry) {
            return entry.first == eventName;
        });
        if (lifecycle != kLifecycleEvents.end()) {
            const int slot = s2sdk::GetEventPlayerSlot(event, plg::string("userid"));
            g_policy.OnPlayerEvent(lifecycle->second, slot);
            return s2sdk::ResultType::Continue;
        }
        if (eventName != "weapon_reload") return s2sdk::ResultType::Continue;
        const int slot = s2sdk::GetEventPlayerSlot(event, plg::string("userid"));
        PlayerLife life;
        if (!ReadLife(slot, life)) return s2sdk::ResultType::Continue;
        const PlayerIdentity source{life, ActiveWeapon(life)};
        if (g_fullMagazineEnabled && source.weapon != -1) {
            const auto index = static_cast<std::size_t>(slot);
            auto& attempt = g_policy.fullAttempts[index];
            if (attempt == source && IsFullMagazine(source) && ReloadButtonDown(source)) {
                g_policy.SetReceipt(index, source);
                attempt.reset();
            }
        }
        const bool synthetic = g_untrackedDispatchDepth == 0 && g_dispatch.ObserveReloadEvent(source);
        if (g_enabled) {
            const int tick = s2sdk::GetGameTickCount();
            g_policy.Finish(life, tick, true, false);
            if (!synthetic) QueueOtherPlayers(life, tick);
        }
    } catch (...) {
        Log("weapon_reload event inspection failed");
    }
    return s2sdk::ResultType::Continue;
}

} // namespace

bool HasReloadSyncSupport() noexcept {
    void* const address = ResolveInputAddress(true);
    if (!address) return false;
    try {
        const auto existing = polyhook::FindDetour(address);
        return !existing || (g_inputHook.handle == existing && g_inputHook.address == address);
    } catch (...) {
        return false;
    }
}

bool HasFullMagazineReloadSupport() noexcept {
    if (!ResolveInputAddress(false) || !FullReloadImportsAvailable() || !RequiredFullSchemaAvailable()) return false;
    for (std::size_t i = 0; i < kFullSignatures.size(); ++i) {
        void* const address = ResolveFullAddress(kFullSignatures[i]);
        if (!address) return false;
        const auto existing = polyhook::FindDetour(address);
        if (existing && (g_fullHooks[i].address != address || g_fullHooks[i].handle != existing)) return false;
    }
    return true;
}

enum class ReloadPolicyResult : std::int32_t { Failed, Settled, Draining };

ReloadPolicyResult StopReloadPolicy(bool immediate) noexcept {
    g_enabled = false;
    g_fullMagazineEnabled = false;
    g_policy.OnRoundEnd();
    while (g_dispatch.Current()) RestoreEdit(g_dispatch.Pop());
    g_untrackedDispatchDepth = 0;
    if (immediate) {
        g_policy.fullReceipts.fill(std::nullopt);
        g_policy.fullReceiptCount = 0;
        g_subclassOffset = -1;
        g_reserveClipsOffset = -1;
#if defined(__linux__)
        g_fullCallerReturns.fill(0);
#endif
    } else if (HasFullReceipts()) {
        PruneFinishedReceipts();
    }
    const bool inputClean = RemoveInputHook();
    if (HasFullReceipts()) {
        return inputClean ? ReloadPolicyResult::Draining : ReloadPolicyResult::Failed;
    }
    const bool fullClean = RemoveFullHooks();
    const bool eventsClean = RemoveEvents();
    return inputClean && fullClean && eventsClean ? ReloadPolicyResult::Settled : ReloadPolicyResult::Failed;
}

ReloadPolicyResult UpdateReloadPolicy(bool synchronize, bool allowFullMagazine, bool immediate) noexcept {
    if (!synchronize && !allowFullMagazine) return StopReloadPolicy(immediate);
    if (immediate || (synchronize && !HasReloadSyncSupport()) ||
        (allowFullMagazine && !HasFullMagazineReloadSupport())) return ReloadPolicyResult::Failed;
    if (!InstallInputHook() || !InstallEvents() ||
        (allowFullMagazine && !InstallFullHooks())) {
        (void)StopReloadPolicy(false);
        return ReloadPolicyResult::Failed;
    }
    g_enabled = synchronize;
    g_fullMagazineEnabled = allowFullMagazine;
    if (!synchronize) g_policy.ClearPending();
    if (!allowFullMagazine) {
        g_policy.fullAttempts.fill(std::nullopt);
        if (HasFullReceipts()) {
            PruneFinishedReceipts();
            if (HasFullReceipts()) return ReloadPolicyResult::Draining;
        }
        if (!RemoveFullHooks()) return ReloadPolicyResult::Failed;
    }
    return ReloadPolicyResult::Settled;
}

bool ShutdownReloadSync() noexcept {
    const bool clean = StopReloadPolicy(true) == ReloadPolicyResult::Settled;
    if (clean) g_syncSchema = {};
    return clean;
}

} // namespace funrounds::native::detail

extern "C" PLUGIN_API std::int32_t UpdateReloadPolicy(bool synchronize, bool allowFullMagazine, bool immediate) {
    return static_cast<std::int32_t>(funrounds::native::detail::UpdateReloadPolicy(synchronize, allowFullMagazine, immediate));
}

extern "C" PLUGIN_API bool HasReloadSyncSupport() {
    return funrounds::native::detail::HasReloadSyncSupport();
}

extern "C" PLUGIN_API bool HasFullMagazineReloadSupport() {
    return funrounds::native::detail::HasFullMagazineReloadSupport();
}
