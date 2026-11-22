// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "reload_sync.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

constexpr std::uint64_t kReloadBit = 1ULL << 13;
constexpr int kMaxPlayers = 64;
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


struct PlayerIdentity {
    int slot{-1};
    int controller{-1};
    int pawnHandle{-1};
    int weapon{-1};
    void* pawn{};
};

struct PendingRequest {
    PlayerIdentity target;
};

struct ButtonEdit {
    bool active{};
    PlayerIdentity target{};
    void* weapon{};
    void* buttonState{};
};

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

std::vector<PendingRequest> g_pending;
InputHook g_inputHook;
std::array<MidHook, kFullSignatures.size()> g_fullHooks{};
#if defined(__linux__)
std::array<std::uintptr_t, kFullCallerSignatures.size()> g_fullCallerReturns{};
#endif
std::array<std::optional<PlayerIdentity>, kMaxPlayers> g_fullAttempts{};
std::array<std::optional<PlayerIdentity>, kMaxPlayers> g_fullReceipts{};
std::size_t g_fullReceiptCount{};
std::int32_t g_subclassOffset{-1};
std::int32_t g_reserveClipsOffset{-1};
struct SyncSchema {
    SchemaField movementServices{};
    SchemaField buttonState{};
    SchemaField buttonMask{};
    SchemaField reloading{};
    SchemaField reserve{};
    SchemaField clip{};
    bool ready{};
} g_syncSchema;
thread_local ButtonEdit g_edit;
thread_local int g_dispatchWeapon{-1};
thread_local int g_dispatchSlot{-1};
bool g_enabled{};
bool g_fullMagazineEnabled{};
bool g_reloadEventRegistered{};
bool g_roundEndEventRegistered{};
std::array<bool, 5> g_lifecycleEventsRegistered{};
constexpr std::array<const char*, 5> kLifecycleEvents{
    "player_death", "player_spawn", "player_disconnect", "bot_takeover", "item_equip"};



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

bool RequiredImportsAvailable() noexcept {
    return __s2sdk_GetGameConfigSignature && __s2sdk_GetSchemaOffset && __s2sdk_GetEntSchema2 &&
           __s2sdk_SetEntSchema2 && __s2sdk_IsClientInGame && __s2sdk_IsClientAlive &&
           __s2sdk_GetMaxClients && __s2sdk_PlayerSlotToEntHandle && __s2sdk_GetClientPawn &&
           __s2sdk_EntPointerToEntHandle && __s2sdk_GetClientActiveWeapon && __s2sdk_GetEntityOwner &&
           __s2sdk_IsValidEntHandle && __s2sdk_EntHandleToEntPointer && __s2sdk_GetEventPlayerSlot &&
           __s2sdk_HookEvent && __s2sdk_UnhookEvent && __polyhook_HookDetour && __polyhook_FindDetour &&
           __polyhook_AddCallback && __polyhook_IsCallbackRegistered && __polyhook_RemoveCallback &&
           __polyhook_AreCallbacksRegistered && __polyhook_UnhookDetour && __polyhook_GetArgumentPointer;
}

bool FullReloadImportsAvailable() noexcept {
    const bool available = __polyhook_HookDetour2 && __polyhook_GetRegisterUInt32 && __polyhook_SetRegisterUInt32 &&
           __polyhook_GetRegisterUInt64 && __polyhook_SetRegisterUInt64 && __polyhook_GetRegisterPointer &&
           __s2sdk_GetEntityOwner && __s2sdk_EntHandleToEntPointer && __s2sdk_GetEntSchema2 &&
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
            !ResolveSchemaField("CInButtonState", "m_pButtonStates", 0, g_syncSchema.buttonMask) ||
            !ResolveSchemaField("CCSWeaponBase", "m_bInReload", sizeof(bool), g_syncSchema.reloading) ||
            !ResolveSchemaField("CBasePlayerWeapon", "m_pReserveAmmo", 0, g_syncSchema.reserve) ||
            !ResolveSchemaField("CBasePlayerWeapon", "m_iClip1", sizeof(std::int32_t), g_syncSchema.clip)) return false;
        g_syncSchema.ready = true;
        return true;
    } catch (...) {
        return false;
    }
}

bool RequiredFullSchemaAvailable() noexcept {
    if (g_subclassOffset >= 0 && g_reserveClipsOffset >= 0) return true;
    if (!RequiredSyncSchemaAvailable() || ProbeSchemaOffset("CBasePlayerPawn", "m_hController") < 0) return false;
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

void* ResolveInputAddress() noexcept {
    try {
        if (!RequiredImportsAvailable() || !EnsureEngine() || !RequiredSyncSchemaAvailable()) return nullptr;
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

bool ReadPlayer(int slot, PlayerIdentity& player) noexcept {
    try {
        if (slot < 0 || slot >= kMaxPlayers || slot >= s2sdk::GetMaxClients() ||
            !s2sdk::IsClientInGame(slot) || !s2sdk::IsClientAlive(slot)) {
            return false;
        }
        auto* const pawn = s2sdk::GetClientPawn(slot);
        const int controller = s2sdk::PlayerSlotToEntHandle(slot);
        const int pawnHandle = s2sdk::EntPointerToEntHandle(pawn);
        const int weapon = s2sdk::GetClientActiveWeapon(slot);
        if (!pawn || controller == -1 || pawnHandle == -1 || !ValidHandle(controller) || !ValidHandle(pawnHandle) ||
            !ValidHandle(weapon) || s2sdk::EntHandleToEntPointer(pawnHandle) != pawn ||
            s2sdk::GetEntityOwner(weapon) != pawnHandle) {
            return false;
        }
        player = {slot, controller, pawnHandle, weapon, pawn};
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
        if (controllerRef.ToInt() != ToNativeHandle(controllerHandle) || slot < 0 ||
            slot >= s2sdk::GetMaxClients() || !s2sdk::IsClientInGame(slot) ||
            s2sdk::PlayerSlotToEntHandle(slot) != controllerHandle || s2sdk::GetClientPawn(slot) != pawn)
            return std::nullopt;
        PlayerIdentity player;
        if (ReadPlayer(slot, player) && player.weapon == weapon && player.pawnHandle == ownerHandle) return player;
    } catch (...) {
    }
    return std::nullopt;
}

void SetReceipt(std::size_t slot, const PlayerIdentity& player) noexcept {
    if (slot >= g_fullReceipts.size()) return;
    auto& receipt = g_fullReceipts[slot];
    if (!receipt) ++g_fullReceiptCount;
    receipt = player;
}

void ClearReceipt(std::size_t slot) noexcept {
    if (slot >= g_fullReceipts.size()) return;
    auto& receipt = g_fullReceipts[slot];
    if (receipt) {
        receipt.reset();
        --g_fullReceiptCount;
    }
}

bool SameLife(const PlayerIdentity& player) noexcept {
    PlayerIdentity current;
    return ReadPlayer(player.slot, current) && current.controller == player.controller &&
           current.pawnHandle == player.pawnHandle && current.pawn == player.pawn;
}

bool SameTarget(const PlayerIdentity& player) noexcept {
    PlayerIdentity current;
    return ReadPlayer(player.slot, current) && current.controller == player.controller &&
           current.pawnHandle == player.pawnHandle && current.pawn == player.pawn &&
           current.weapon == player.weapon;
}

void* CurrentButtonState(const PlayerIdentity& player) noexcept {
    try {
        if (!SameLife(player)) return nullptr;
        if (!RequiredSyncSchemaAvailable()) return nullptr;
        auto* const movement = ReadAt<void*>(player.pawn, g_syncSchema.movementServices.offset);
        if (!movement) return nullptr;
        return static_cast<std::byte*>(movement) + g_syncSchema.buttonState.offset;
    } catch (...) {
        return nullptr;
    }
}

std::uint64_t ButtonMask(void* state) {
    if (!state) return 0;
    return ReadAt<std::uint64_t>(state, g_syncSchema.buttonMask.offset);
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
    try {
        const auto mask = ButtonMask(CurrentButtonState(player));
        return (mask & kReloadBit) != 0;
    } catch (...) {
        return false;
    }
}

polyhook::ResultType FullCapacity(polyhook::HookHandle handle,
                                  polyhook::ParametersHandle parameters,
                                  std::int32_t,
                                  polyhook::ReturnHandle,
                                  polyhook::CallbackType phase) noexcept {
    using Result = polyhook::ResultType;
    if (phase != polyhook::CallbackType::Pre || !parameters ||
        (!g_fullMagazineEnabled && g_fullReceiptCount == 0)) return Result::Ignored;
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
        if (!g_fullMagazineEnabled && g_fullReceiptCount == 0) return Result::Ignored;

        bool transferSite = site->signature == "FunRounds::ReloadFullTransfer" ||
                            site->signature == "FunRounds::ReloadFullAlternateTransfer";
        bool admissionSite = site->signature == kFullAdmissionSignature.name() ||
                             site->signature == "FunRounds::ReloadFullStart";
#if defined(__linux__)
        auto* const frame = polyhook::GetRegister<void*>(registers, polyhook::RegisterType::RBP);
        std::uintptr_t caller{};
        if (!frame) return Result::Ignored;
        std::memcpy(&caller, static_cast<const std::uint8_t*>(frame) + sizeof(std::uintptr_t), sizeof(caller));
        transferSite = caller == g_fullCallerReturns[2] || caller == g_fullCallerReturns[3];
        admissionSite = caller == g_fullCallerReturns[0] || caller == g_fullCallerReturns[1];
        if (!transferSite && !admissionSite) return Result::Ignored;
#endif
        std::optional<PlayerIdentity>* authorization = nullptr;
        const bool receiptAuthorization = transferSite;
        if (transferSite) {
            const auto current = PlayerForWeapon(weaponHandle);
            if (current) {
                auto& receipt = g_fullReceipts[static_cast<std::size_t>(current->slot)];
                if (receipt && receipt->weapon == weaponHandle) authorization = &receipt;
            }
        } else if (admissionSite && g_fullMagazineEnabled && g_dispatchWeapon == weaponHandle) {
            if (g_dispatchSlot >= 0 && g_dispatchSlot < kMaxPlayers) {
                auto& attempt = g_fullAttempts[static_cast<std::size_t>(g_dispatchSlot)];
                if (attempt && attempt->weapon == weaponHandle) authorization = &attempt;
            }
        }
        if (!authorization && admissionSite && g_fullMagazineEnabled && g_dispatchWeapon == weaponHandle) {
            auto candidate = PlayerForWeapon(weaponHandle);
            if (candidate && IsFullMagazine(*candidate) && ReloadButtonDown(*candidate)) {
                g_fullAttempts[static_cast<std::size_t>(candidate->slot)] = *candidate;
                g_dispatchSlot = candidate->slot;
                authorization = &g_fullAttempts[static_cast<std::size_t>(candidate->slot)];
            }
        }
        if ((!transferSite && !admissionSite) || (admissionSite && !g_fullMagazineEnabled))
            return Result::Ignored;
        if (!authorization || !*authorization || (*authorization)->weapon != weaponHandle) return Result::Ignored;
        if (!SameTarget(**authorization)) {
            if (receiptAuthorization) ClearReceipt(static_cast<std::size_t>((*authorization)->slot));
            else authorization->reset();
            return Result::Ignored;
        }
        if (!IsFullMagazine(**authorization)) {
            if (receiptAuthorization) ClearReceipt(static_cast<std::size_t>((*authorization)->slot));
            return Result::Ignored;
        }

        const auto reloading = ReloadState(**authorization);
        if (transferSite) {
            if (!reloading || !*reloading) {
                ClearReceipt(static_cast<std::size_t>((*authorization)->slot));
                return Result::Ignored;
            }
        } else if (reloading && *reloading) {
            return Result::Ignored;
        } else if (!ReloadButtonDown(**authorization)) {
            if (receiptAuthorization) ClearReceipt(static_cast<std::size_t>((*authorization)->slot));
            else authorization->reset();
            return Result::Ignored;
        }

#if !defined(_WIN32)
        if (gap == 0 && reserve > 0) {
            polyhook::SetRegisterUInt32(registers, polyhook::RegisterType::R12, 1);
            if (receiptAuthorization) ClearReceipt(static_cast<std::size_t>((*authorization)->slot));
            return Result::Ignored;
        }
#else
        if (gap == 0 && reserve > 0) {
            polyhook::SetRegisterUInt64(registers, polyhook::RegisterType::RAX, 1);
            if (transferSite) ClearReceipt(static_cast<std::size_t>((*authorization)->slot));
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
            if (s2sdk::UnhookEvent(plg::string(kLifecycleEvents[i]), Event, s2sdk::HookMode::Post) ==
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

void RestoreEdit(void* callbackWeapon) noexcept {
    if (!g_edit.active || callbackWeapon != g_edit.weapon) return;
    const ButtonEdit edit = g_edit;
    g_edit = {};
    try {
        if (!SameLife(edit.target) || CurrentButtonState(edit.target) != edit.buttonState) return;
        const auto mask = ButtonMask(edit.buttonState);
        if ((mask & kReloadBit) != 0) {
            (void)s2sdk::SetEntSchema2(edit.buttonState, plg::string("CInButtonState"), plg::string("m_pButtonStates"),
                                       static_cast<std::int64_t>(mask & ~kReloadBit), false, 0);
        }
    } catch (...) {
        Log("temporary IN_RELOAD restoration failed");
    }
}

void PrunePending() {
    std::erase_if(g_pending, [](const PendingRequest& request) { return !SameTarget(request.target); });
}

void QueueOtherPlayers(const PlayerIdentity& source) {
    const int count = std::min(s2sdk::GetMaxClients(), kMaxPlayers);
    for (int slot = 0; slot < count; ++slot) {
        if (slot == source.slot) continue;
        PlayerIdentity target;
        if (!ReadPlayer(slot, target)) continue;
        const auto reloading = ReloadState(target);
        if (!reloading || *reloading) continue;
        const bool duplicate = std::any_of(g_pending.begin(), g_pending.end(), [&](const PendingRequest& pending) {
            return pending.target.slot == target.slot && pending.target.controller == target.controller &&
                   pending.target.pawnHandle == target.pawnHandle && pending.target.weapon == target.weapon;
        });
        if (!duplicate) {
            g_pending.push_back({target});
        }
    }
}

bool InstallInputHook() noexcept {
    try {
        void* const address = ResolveInputAddress();
        const plg::vector<polyhook::DataType> arguments{polyhook::DataType::Pointer};
        DetourSlot slot(g_inputHook.address, g_inputHook.handle, &g_inputHook.attempted);
        if (!slot.Acquire(address, [&] {
                return polyhook::HookDetour(address, polyhook::DataType::Int64, arguments, -1,
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
            if (s2sdk::HookEvent(plg::string(kLifecycleEvents[i]), Event, s2sdk::HookMode::Post) !=
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
    return g_fullReceiptCount != 0;
}

void PruneFinishedReceipts() noexcept {
    for (auto& receipt : g_fullReceipts) {
        if (!receipt) continue;
        const auto reloading = ReloadState(*receipt);
        if (!SameTarget(*receipt) || !reloading || !*reloading)
            ClearReceipt(static_cast<std::size_t>(receipt->slot));
    }
}

polyhook::ResultType Input(polyhook::HookHandle,
                           polyhook::ParametersHandle parameters,
                           std::int32_t,
                           polyhook::ReturnHandle,
                           polyhook::CallbackType phase) noexcept {
    using Result = polyhook::ResultType;
    void* callbackWeapon{};
    try {
        callbackWeapon = polyhook::GetArgumentPointer(parameters, 0);
        if (phase == polyhook::CallbackType::Post) {
            RestoreEdit(callbackWeapon);
            const int postWeapon = callbackWeapon ? s2sdk::EntPointerToEntHandle(callbackWeapon) : -1;
            if (g_dispatchSlot >= 0 && g_dispatchSlot < kMaxPlayers) {
                auto& attempt = g_fullAttempts[static_cast<std::size_t>(g_dispatchSlot)];
                if (attempt && attempt->weapon == postWeapon) attempt.reset();
            }
            g_dispatchWeapon = -1;
            g_dispatchSlot = -1;
            return Result::Ignored;
        }
        if (!callbackWeapon) return Result::Ignored;

        const int weaponHandle = s2sdk::EntPointerToEntHandle(callbackWeapon);
        g_dispatchWeapon = weaponHandle;
        g_dispatchSlot = -1;
        if (!g_enabled || g_edit.active) return Result::Ignored;
        const auto found = std::find_if(g_pending.begin(), g_pending.end(), [weaponHandle](const PendingRequest& request) {
            return request.target.weapon == weaponHandle;
        });
        if (found == g_pending.end()) return Result::Ignored;
        const PlayerIdentity target = found->target;
        g_pending.erase(found); // Consume at most one native attempt for this recipient.
        const auto reloading = ReloadState(target);
        if (!SameTarget(target) || target.weapon != weaponHandle || !reloading || *reloading) return Result::Ignored;
        void* const buttonState = CurrentButtonState(target);
        if (!buttonState) return Result::Ignored;
        const auto before = ButtonMask(buttonState);
        if ((before & kReloadBit) != 0) return Result::Ignored;

        g_edit = {true, target, callbackWeapon, buttonState};
        (void)s2sdk::SetEntSchema2(buttonState, plg::string("CInButtonState"), plg::string("m_pButtonStates"),
                                   static_cast<std::int64_t>(before | kReloadBit), false, 0);
        if ((ButtonMask(buttonState) & kReloadBit) == 0) {
            g_edit = {};
            return Result::Ignored;
        }
    } catch (...) {
        RestoreEdit(callbackWeapon);
        g_dispatchWeapon = -1;
        g_dispatchSlot = -1;
    }
    return Result::Ignored;
}

s2sdk::ResultType Event(const plg::string& name, void* event, bool) noexcept {
    try {
        const std::string_view eventName(name.c_str());
        if (eventName == "round_end") {
            g_pending.clear();
            g_fullAttempts.fill(std::nullopt);
            return s2sdk::ResultType::Continue;
        }
        if (!event) return s2sdk::ResultType::Continue;
        if (std::find(kLifecycleEvents.begin(), kLifecycleEvents.end(), eventName) != kLifecycleEvents.end()) {
            const int slot = s2sdk::GetEventPlayerSlot(event, plg::string("userid"));
            std::erase_if(g_pending, [slot](const PendingRequest& request) { return request.target.slot == slot; });
            if (slot >= 0 && slot < kMaxPlayers) {
                g_fullAttempts[static_cast<std::size_t>(slot)].reset();
                ClearReceipt(static_cast<std::size_t>(slot));
            }
            return s2sdk::ResultType::Continue;
        }
        if (eventName != "weapon_reload" || !event) return s2sdk::ResultType::Continue;
        const int slot = s2sdk::GetEventPlayerSlot(event, plg::string("userid"));
        if (g_fullMagazineEnabled) {
            PlayerIdentity source;
            if (ReadPlayer(slot, source) && source.slot >= 0 && source.slot < kMaxPlayers &&
                g_fullAttempts[static_cast<std::size_t>(source.slot)] &&
                SameTarget(*g_fullAttempts[static_cast<std::size_t>(source.slot)]) &&
                IsFullMagazine(source) && ReloadButtonDown(source)) {
                const auto index = static_cast<std::size_t>(source.slot);
                SetReceipt(index, source);
                g_fullAttempts[index].reset();
            }
        }
        if (g_edit.active && g_edit.target.slot == slot) return s2sdk::ResultType::Continue;
        if (!g_enabled) return s2sdk::ResultType::Continue;
        PlayerIdentity source;
        if (!ReadPlayer(slot, source)) return s2sdk::ResultType::Continue;
        void* const buttonState = CurrentButtonState(source);
        if (!buttonState || (ButtonMask(buttonState) & kReloadBit) == 0) return s2sdk::ResultType::Continue;
        PrunePending();
        QueueOtherPlayers(source);
    } catch (...) {
        Log("weapon_reload event inspection failed");
    }
    return s2sdk::ResultType::Continue;
}

} // namespace

bool HasReloadSyncSupport() noexcept {
    void* const address = ResolveInputAddress();
    if (!address) return false;
    try {
        const auto existing = polyhook::FindDetour(address);
        return !existing || (g_inputHook.handle == existing && g_inputHook.address == address);
    } catch (...) {
        return false;
    }
}

bool HasFullMagazineReloadSupport() noexcept {
    if (!ResolveInputAddress() || !FullReloadImportsAvailable() || !RequiredFullSchemaAvailable()) return false;
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
    g_pending.clear();
    g_fullAttempts.fill(std::nullopt);
    RestoreEdit(g_edit.weapon);
    g_dispatchWeapon = -1;
    g_dispatchSlot = -1;
    if (immediate) {
        g_fullReceipts.fill(std::nullopt);
        g_fullReceiptCount = 0;
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
    if (!synchronize) g_pending.clear();
    if (!allowFullMagazine) {
        g_fullAttempts.fill(std::nullopt);
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
