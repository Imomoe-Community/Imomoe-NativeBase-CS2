// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <algorithm>
#include <charconv>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include <plg/plugin.hpp>
#include <plugin_export.h>
#include <polyhook/polyhook.hpp>
#include <s2sdk/s2sdk.hpp>

namespace {

constexpr char kCommand[] = "fr_ghost_visibility_minimal";
constexpr char kHookName[] = "FunRounds::GhostVisibilityMinimal";
constexpr char kBreathSound[] = "Hostage.Breath";
constexpr char kBaseEntityClass[] = "CBaseEntity";
constexpr char kEntityFlameClass[] = "CEntityFlame";
constexpr char kEntityFlameName[] = "entityflame";
constexpr char kEffectEntityField[] = "m_hEffectEntity";
constexpr char kFlameAttachedField[] = "m_hEntAttached";
constexpr char kModelEntityClass[] = "CBaseModelEntity";
constexpr char kRenderColorField[] = "m_clrRender";
constexpr char kRenderModeField[] = "m_nRenderMode";
constexpr float kBreathPulseSeconds = 1.5F;
constexpr double kDefaultBreathDistance = 1000.0;
constexpr std::uint64_t kInReloadButton = 1ull << 13;
constexpr std::int32_t kInvalidHandle = -1;
constexpr std::size_t kInvalidOffset = static_cast<std::size_t>(-1);

// Source SDK ConVarRef::IsValidRef() treats access index 0xFFFF as invalid.
// Index zero is a valid ConVar handle, so do not use a truthiness check here.
constexpr bool IsValidConVarHandle(std::uint64_t handle) noexcept {
    return static_cast<std::uint16_t>(handle & 0xFFFFu) != 0xFFFFu;
}
static_assert(IsValidConVarHandle(0));
static_assert(!IsValidConVarHandle(0x00000000FFFFFFFFull));

polyhook::HookHandle g_hook{};
void* g_address{};
s2sdk::ConfigId g_config{};
std::atomic<std::size_t> g_botPlayerOffset{kInvalidOffset};
std::mutex g_hookControlMutex;
std::atomic<bool> g_commandRegistered{};
std::atomic<bool> g_ready{};
bool g_hookAttempted{};
std::atomic<std::int32_t> g_targetSlot{-1};
std::atomic<std::int32_t> g_targetPawnHandle{kInvalidHandle};
std::atomic<std::int32_t> g_targetControllerHandle{kInvalidHandle};
std::atomic<bool> g_targetFakeClient{};
std::atomic<std::uint64_t> g_calls{};
std::atomic<std::uint64_t> g_botCalls{};
std::atomic<std::uint64_t> g_targetCalls{};
std::atomic<std::uint64_t> g_blockedCalls{};
std::atomic<std::uint64_t> g_maskCleared{};
std::atomic<std::uint64_t> g_offThreadCalls{};
std::atomic<std::uint64_t> g_commandCalls{};
std::atomic<std::uint64_t> g_updateCalls{};
std::atomic<std::uint64_t> g_frameCalls{};
std::atomic<std::uint64_t> g_commandThreadId{};
std::atomic<std::uint64_t> g_updateThreadId{};
std::atomic<std::uint64_t> g_frameThreadId{};
std::atomic<std::uint64_t> g_visibleThreadId{};
std::atomic<bool> g_gameFrameRegistered{};
std::atomic<bool> g_hookStatePublished{};
std::atomic<bool> g_gameConfigOpenPublished{};
std::atomic<std::int32_t> g_lastReceiverSlot{-1};
std::atomic<std::int32_t> g_lastTargetSlot{-1};
std::atomic<std::int32_t> g_lastTargetPawn{kInvalidHandle};
std::atomic<std::int8_t> g_lastTestFov{};
std::atomic<bool> g_breathActive{};
std::atomic<std::int32_t> g_breathSourceSlot{-1};
std::atomic<std::int32_t> g_breathSourcePawn{kInvalidHandle};
std::atomic<std::int32_t> g_breathSourceController{kInvalidHandle};
std::atomic<bool> g_breathSourceFakeClient{};
std::atomic<float> g_breathStopTime{};
std::atomic<std::uint64_t> g_breathRequests{};
std::atomic<std::uint64_t> g_breathStopRequests{};
std::atomic<std::uint64_t> g_breathInvalidSourceStops{};

std::uint64_t CurrentNativeThreadId() noexcept {
#if defined(_WIN32)
    return static_cast<std::uint64_t>(::GetCurrentThreadId());
#else
    const auto threadId = ::syscall(SYS_gettid);
    return threadId > 0 ? static_cast<std::uint64_t>(threadId) : 0;
#endif
}

bool CurrentPlayer(std::int32_t slot, bool requireBot, void* expectedPawn,
                   std::int32_t expectedPawnHandle, std::int32_t expectedControllerHandle) noexcept;

void StopBreath(const char* reason) noexcept {
    if (!g_breathActive.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    const auto pawnHandle = g_breathSourcePawn.exchange(kInvalidHandle, std::memory_order_relaxed);
    g_breathSourceSlot.store(-1, std::memory_order_relaxed);
    g_breathSourceController.store(kInvalidHandle, std::memory_order_relaxed);
    g_breathSourceFakeClient.store(false, std::memory_order_relaxed);
    if (pawnHandle != kInvalidHandle && __s2sdk_StopSound && s2sdk::IsValidEntHandle(pawnHandle)) {
        try {
            s2sdk::StopSound(pawnHandle, plg::string(kBreathSound));
            g_breathStopRequests.fetch_add(1, std::memory_order_relaxed);
        } catch (...) {
        }
    }
    if (reason && std::strcmp(reason, "source_invalid") == 0) {
        g_breathInvalidSourceStops.fetch_add(1, std::memory_order_relaxed);
    }
    try {
        s2sdk::PrintToServer(plg::string(std::string("[ghost-visibility-minimal] sound_stop event=Hostage.Breath reason=") +
                                         (reason ? reason : "unspecified")));
    } catch (...) {
    }
}

void GameFrame(bool, bool, bool) noexcept {
    g_frameThreadId.store(CurrentNativeThreadId(), std::memory_order_relaxed);
    g_frameCalls.fetch_add(1, std::memory_order_relaxed);
    if (!g_breathActive.load(std::memory_order_acquire)) {
        return;
    }
    const auto slot = g_breathSourceSlot.load(std::memory_order_relaxed);
    const auto pawnHandle = g_breathSourcePawn.load(std::memory_order_relaxed);
    const auto controllerHandle = g_breathSourceController.load(std::memory_order_relaxed);
    if (!CurrentPlayer(slot, g_breathSourceFakeClient.load(std::memory_order_relaxed), nullptr,
                       pawnHandle, controllerHandle)) {
        StopBreath("source_invalid");
        return;
    }
    if (s2sdk::GetGameTime() >= g_breathStopTime.load(std::memory_order_relaxed)) {
        StopBreath("deadline");
    }
}

template <typename T>
T ReadField(const void* object, std::size_t offset) noexcept {
    T value{};
    if (object && offset != kInvalidOffset) {
        std::memcpy(&value, static_cast<const std::byte*>(object) + offset, sizeof(T));
    }
    return value;
}

bool CurrentPlayer(std::int32_t slot, bool requireBot, void* expectedPawn,
                   std::int32_t expectedPawnHandle, std::int32_t expectedControllerHandle) noexcept {
    if (slot < 0 || slot >= s2sdk::GetMaxClients() || !s2sdk::IsClientInGame(slot) ||
        !s2sdk::IsClientAlive(slot) || s2sdk::IsFakeClient(slot) != requireBot) {
        return false;
    }
    void* const pawn = s2sdk::GetClientPawn(slot);
    if (!pawn || (expectedPawn && pawn != expectedPawn)) {
        return false;
    }
    const std::int32_t pawnHandle = s2sdk::EntPointerToEntHandle(pawn);
    const std::int32_t controllerHandle = s2sdk::PlayerSlotToEntHandle(slot);
    return pawnHandle != kInvalidHandle && s2sdk::IsValidEntHandle(pawnHandle) &&
           s2sdk::IsEntityPlayerPawn(pawnHandle) && s2sdk::EntHandleToEntPointer(pawnHandle) == pawn &&
           controllerHandle != kInvalidHandle && s2sdk::IsValidEntHandle(controllerHandle) &&
           s2sdk::IsEntityPlayerController(controllerHandle) &&
           s2sdk::GetEntSchemaEnt2(pawn, plg::string("CBasePlayerPawn"), plg::string("m_hController"), 0) == controllerHandle &&
           (expectedPawnHandle == kInvalidHandle || pawnHandle == expectedPawnHandle) &&
           (expectedControllerHandle == kInvalidHandle || controllerHandle == expectedControllerHandle);
}

bool ParseFiniteFloat(std::string_view text, float& value) noexcept {
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size() && std::isfinite(value);
}

void AppendRenderReadback(std::ostringstream& output, const char* prefix, void* entity,
                          std::int32_t colorOffset, std::int32_t modeOffset) {
    if (!entity) {
        output << ' ' << prefix << "_render=unavailable";
        return;
    }
    output << ' ' << prefix << "_render_mode=";
    if (modeOffset < 0) {
        output << "unavailable";
    } else {
        output << static_cast<unsigned int>(static_cast<std::uint8_t>(s2sdk::GetEntSchema2(
            entity, plg::string(kModelEntityClass), plg::string(kRenderModeField), 0)));
    }
    output << ' ' << prefix << "_alpha=";
    if (colorOffset < 0) {
        output << "unavailable";
    } else {
        const auto color = static_cast<std::uint32_t>(s2sdk::GetEntSchema2(
            entity, plg::string(kModelEntityClass), plg::string(kRenderColorField), 0));
        output << static_cast<unsigned int>((color >> 24) & 0xFFu);
    }
}

void AppendFlameReadback(std::ostringstream& output, std::int32_t slot, bool fakeClient,
                         void* pawn, std::int32_t pawnHandle, std::int32_t controllerHandle) {
    const auto effectOffset = s2sdk::GetSchemaOffset(plg::string(kBaseEntityClass), plg::string(kEffectEntityField));
    const auto attachedOffset = s2sdk::GetSchemaOffset(plg::string(kEntityFlameClass), plg::string(kFlameAttachedField));
    output << " flame_effect_schema_offset=" << effectOffset
           << " flame_attached_schema_offset=" << attachedOffset;
    if (effectOffset < 0) {
        output << " flame_effect_handle=unavailable flame_effect_class=unavailable"
               << " flame_attached_handle=unavailable flame_attached_matches_pawn=unavailable";
        return;
    }

    if (!CurrentPlayer(slot, fakeClient, pawn, pawnHandle, controllerHandle)) {
        output << " flame_effect_handle=unavailable flame_effect_class=unavailable"
               << " flame_attached_handle=unavailable flame_attached_matches_pawn=unavailable"
               << " flame_readback=player_identity_changed";
        return;
    }

    // The pinned native burn schema check verifies these fields are uint32 CHandle atomics.
    // Read element zero through the SDK's typed handle accessor, never through a guessed offset.
    const auto effectHandle = s2sdk::GetEntSchemaEnt2(
        pawn, plg::string(kBaseEntityClass), plg::string(kEffectEntityField), 0);
    output << " flame_effect_handle=" << effectHandle;
    if (effectHandle == kInvalidHandle) {
        output << " flame_effect_class=none flame_attached_handle=not_applicable"
               << " flame_attached_matches_pawn=not_applicable";
        return;
    }
    if (!s2sdk::IsValidEntHandle(effectHandle)) {
        output << " flame_effect_class=unavailable flame_attached_handle=unavailable"
               << " flame_attached_matches_pawn=unavailable flame_readback=invalid_effect_handle";
        return;
    }

    void* const effect = s2sdk::EntHandleToEntPointer(effectHandle);
    if (!effect || s2sdk::EntPointerToEntHandle(effect) != effectHandle) {
        output << " flame_effect_class=unavailable flame_attached_handle=unavailable"
               << " flame_attached_matches_pawn=unavailable flame_readback=stale_effect_handle";
        return;
    }
    const auto classname = s2sdk::GetEntityClassname(effectHandle);
    const char* const classnameText = classname.c_str();
    if (!classnameText || classnameText[0] == '\0') {
        output << " flame_effect_class=unavailable flame_attached_handle=unavailable"
               << " flame_attached_matches_pawn=unavailable flame_readback=classname_unavailable";
        return;
    }
    output << " flame_effect_class=" << classnameText;
    if (std::string_view(classnameText) != kEntityFlameName) {
        output << " flame_attached_handle=not_applicable"
               << " flame_attached_matches_pawn=not_applicable";
        return;
    }
    if (attachedOffset < 0) {
        output << " flame_attached_handle=unavailable flame_attached_matches_pawn=unavailable";
        return;
    }
    if (!CurrentPlayer(slot, fakeClient, pawn, pawnHandle, controllerHandle) ||
        s2sdk::EntHandleToEntPointer(effectHandle) != effect ||
        s2sdk::EntPointerToEntHandle(effect) != effectHandle) {
        output << " flame_attached_handle=unavailable flame_attached_matches_pawn=unavailable"
               << " flame_readback=identity_changed";
        return;
    }

    const auto attachedHandle = s2sdk::GetEntSchemaEnt2(
        effect, plg::string(kEntityFlameClass), plg::string(kFlameAttachedField), 0);
    const bool identityStillCurrent = CurrentPlayer(slot, fakeClient, pawn, pawnHandle, controllerHandle) &&
        s2sdk::EntHandleToEntPointer(effectHandle) == effect &&
        s2sdk::EntPointerToEntHandle(effect) == effectHandle;
    output << " flame_attached_handle=" << attachedHandle
           << " flame_attached_matches_pawn=";
    if (identityStillCurrent) {
        output << (attachedHandle == pawnHandle ? "true" : "false");
    } else {
        output << "unavailable flame_readback=identity_changed";
    }
}

bool FiniteOrigin(const plg::vec3& origin) noexcept {
    return std::isfinite(origin.x) && std::isfinite(origin.y) && std::isfinite(origin.z);
}

double DistanceSquared(const plg::vec3& first, const plg::vec3& second) noexcept {
    const auto dx = static_cast<double>(first.x) - static_cast<double>(second.x);
    const auto dy = static_cast<double>(first.y) - static_cast<double>(second.y);
    const auto dz = static_cast<double>(first.z) - static_cast<double>(second.z);
    return dx * dx + dy * dy + dz * dz;
}

bool HasHookState() noexcept {
    return g_address != nullptr || static_cast<bool>(g_hook);
}

void ClearTarget() noexcept {
    g_targetSlot.store(-1, std::memory_order_relaxed);
    g_targetPawnHandle.store(kInvalidHandle, std::memory_order_relaxed);
    g_targetControllerHandle.store(kInvalidHandle, std::memory_order_relaxed);
    g_targetFakeClient.store(false, std::memory_order_relaxed);
}

std::int32_t FindBotReceiver(void* receiver) noexcept {
    const auto botPlayerOffset = g_botPlayerOffset.load(std::memory_order_relaxed);
    if (!receiver || botPlayerOffset == kInvalidOffset) {
        return -1;
    }
    // CCSBot::IsVisible's receiver is CCSBot/CBot; CS2-Bot-Vision resolves CBot::m_pPlayer
    // then validates its pawn controller handle to recover the exact server BOT slot.
    void* const pawn = ReadField<void*>(receiver, botPlayerOffset);
    if (!pawn) {
        return -1;
    }
    const std::int32_t pawnHandle = s2sdk::EntPointerToEntHandle(pawn);
    if (pawnHandle == kInvalidHandle || !s2sdk::IsValidEntHandle(pawnHandle) ||
        !s2sdk::IsEntityPlayerPawn(pawnHandle) || s2sdk::EntHandleToEntPointer(pawnHandle) != pawn) {
        return -1;
    }
    const std::int32_t controllerHandle = static_cast<std::int32_t>(s2sdk::GetEntSchemaEnt2(
        pawn, plg::string("CBasePlayerPawn"), plg::string("m_hController"), 0));
    if (controllerHandle == kInvalidHandle || !s2sdk::IsValidEntHandle(controllerHandle) ||
        !s2sdk::IsEntityPlayerController(controllerHandle)) {
        return -1;
    }
    for (std::int32_t slot = 0; slot < s2sdk::GetMaxClients(); ++slot) {
        if (CurrentPlayer(slot, true, pawn, pawnHandle, controllerHandle)) {
            return slot;
        }
    }
    return -1;
}

std::int32_t FindSelectedTarget(void* player, std::int32_t pawnHandle) noexcept {
    const auto targetSlot = g_targetSlot.load(std::memory_order_relaxed);
    const auto targetPawnHandle = g_targetPawnHandle.load(std::memory_order_relaxed);
    const auto targetControllerHandle = g_targetControllerHandle.load(std::memory_order_relaxed);
    if (!player || pawnHandle != targetPawnHandle || targetSlot < 0 ||
        !CurrentPlayer(targetSlot, g_targetFakeClient.load(std::memory_order_relaxed), player,
                       targetPawnHandle, targetControllerHandle)) {
        return -1;
    }
    return targetSlot;
}

polyhook::ResultType GhostVisibilityPre(polyhook::HookHandle,
                                        polyhook::ParametersHandle parameters,
                                        std::int32_t,
                                        polyhook::ReturnHandle result,
                                        polyhook::CallbackType type) noexcept {
    g_calls.fetch_add(1, std::memory_order_relaxed);
    const auto callbackThreadId = CurrentNativeThreadId();
    g_visibleThreadId.store(callbackThreadId, std::memory_order_relaxed);
    const auto frameThreadId = g_frameThreadId.load(std::memory_order_relaxed);
    if (frameThreadId == 0 || callbackThreadId != frameThreadId) {
        g_offThreadCalls.fetch_add(1, std::memory_order_relaxed);
        return polyhook::ResultType::Ignored;
    }
    if (type != polyhook::CallbackType::Pre || !g_ready.load(std::memory_order_acquire) || !parameters || !result) {
        return polyhook::ResultType::Ignored;
    }
    try {
        auto* const receiver = polyhook::GetArgument<void*>(parameters, 0);
        auto* const player = polyhook::GetArgument<void*>(parameters, 1);
        const auto testFov = polyhook::GetArgument<std::int8_t>(parameters, 2);
        auto* const visibleParts = static_cast<std::uint8_t*>(polyhook::GetArgument<void*>(parameters, 3));
        const auto botSlot = FindBotReceiver(receiver);
        if (botSlot < 0) {
            return polyhook::ResultType::Ignored;
        }
        g_botCalls.fetch_add(1, std::memory_order_relaxed);
        const auto playerHandle = player ? s2sdk::EntPointerToEntHandle(player) : kInvalidHandle;
        if (playerHandle == kInvalidHandle || !s2sdk::IsValidEntHandle(playerHandle) ||
            !s2sdk::IsEntityPlayerPawn(playerHandle) || s2sdk::EntHandleToEntPointer(playerHandle) != player) {
            return polyhook::ResultType::Ignored;
        }
        const auto targetSlot = FindSelectedTarget(player, playerHandle);
        if (targetSlot < 0) {
            return polyhook::ResultType::Ignored;
        }
        g_targetCalls.fetch_add(1, std::memory_order_relaxed);
        g_lastReceiverSlot.store(botSlot, std::memory_order_relaxed);
        g_lastTargetSlot.store(targetSlot, std::memory_order_relaxed);
        g_lastTargetPawn.store(playerHandle, std::memory_order_relaxed);
        g_lastTestFov.store(testFov, std::memory_order_relaxed);

        // One target-only visual decision: clear the engine's body-part output, then skip
        // this player's scan. Sound, weapon use, damage and generic point visibility remain native.
        if (visibleParts) {
            *visibleParts = 0;
            g_maskCleared.fetch_add(1, std::memory_order_relaxed);
        }
        g_blockedCalls.fetch_add(1, std::memory_order_relaxed);
        polyhook::SetReturn<bool>(result, false);
        return polyhook::ResultType::Supercede;
    } catch (...) {
        return polyhook::ResultType::Ignored;
    }
}

bool RemoveHook() noexcept {
    try {
        g_ready.store(false, std::memory_order_release);
        if (!g_address) {
            return !g_hook;
        }
        const auto current = polyhook::FindDetour(g_address);
        if (!current) {
            g_hook = nullptr;
            g_address = nullptr;
            g_hookAttempted = false;
            g_hookStatePublished.store(false, std::memory_order_relaxed);
            return true;
        }
        // HookDetour may have installed the detour before returning an empty
        // handle or throwing. The address was checked for an existing detour
        // immediately before this attempt, so retry cleanup can adopt it.
        if (!g_hook && g_hookAttempted) {
            g_hook = current;
        }
        if (!g_hook || current != g_hook) {
            return false;
        }
        if (polyhook::IsCallbackRegistered(current, polyhook::CallbackType::Pre, GhostVisibilityPre)) {
            (void)polyhook::RemoveCallback(current, polyhook::CallbackType::Pre, GhostVisibilityPre);
            if (polyhook::IsCallbackRegistered(current, polyhook::CallbackType::Pre, GhostVisibilityPre)) {
                return false;
            }
        }
        if (!polyhook::UnhookDetour(g_address) && polyhook::FindDetour(g_address)) {
            return false;
        }
        if (polyhook::FindDetour(g_address)) {
            return false;
        }
        g_hook = nullptr;
        g_address = nullptr;
        g_hookAttempted = false;
        g_hookStatePublished.store(false, std::memory_order_relaxed);
        return true;
    } catch (...) {
        return false;
    }
}

void CloseGameConfig() noexcept {
    if (g_config != 0) {
        s2sdk::CloseGameConfigFile(g_config);
        g_config = 0;
    }
    g_gameConfigOpenPublished.store(false, std::memory_order_relaxed);
}

bool StopHook() noexcept {
    ClearTarget();
    if (!RemoveHook()) {
        return false;
    }
    CloseGameConfig();
    g_botPlayerOffset.store(kInvalidOffset, std::memory_order_relaxed);
    return true;
}

bool StartHook() noexcept {
    if (g_ready.load(std::memory_order_acquire)) {
        return true;
    }
    if (HasHookState()) {
        return false;
    }

    try {
        CloseGameConfig();
        const auto botPlayerOffset = s2sdk::GetSchemaOffset(plg::string("CBot"), plg::string("m_pPlayer"));
        if (botPlayerOffset < 0) {
            return false;
        }
        g_botPlayerOffset.store(static_cast<std::size_t>(botPlayerOffset), std::memory_order_relaxed);

        plg::string gamedataPath = plg::GetPlugin()->GetLocation();
        gamedataPath += "/gamedata/ghost-visibility-minimal.jsonc";
        g_config = s2sdk::LoadGameConfigFile({gamedataPath});
        if (g_config == 0) {
            g_botPlayerOffset.store(kInvalidOffset, std::memory_order_relaxed);
            return false;
        }
        g_gameConfigOpenPublished.store(true, std::memory_order_relaxed);
        const auto signature = s2sdk::GetGameConfigSignature(g_config, plg::string("CCSBot::IsVisiblePlayer"));
        if (!signature) {
            CloseGameConfig();
            g_botPlayerOffset.store(kInvalidOffset, std::memory_order_relaxed);
            return false;
        }
        if (polyhook::FindDetour(signature)) {
            CloseGameConfig();
            g_botPlayerOffset.store(kInvalidOffset, std::memory_order_relaxed);
            return false;
        }

        const plg::vector<polyhook::DataType> arguments{
            polyhook::DataType::Pointer, polyhook::DataType::Pointer,
            polyhook::DataType::Int8, polyhook::DataType::Pointer};
        g_address = signature;
        g_hookStatePublished.store(true, std::memory_order_relaxed);
        g_hookAttempted = true;
        g_hook = polyhook::HookDetour(signature, polyhook::DataType::Bool, arguments, -1, plg::string(kHookName));
        if (!g_hook) {
            (void)StopHook();
            return false;
        }
        (void)polyhook::AddCallback(g_hook, polyhook::CallbackType::Pre, GhostVisibilityPre);
        if (!polyhook::IsCallbackRegistered(g_hook, polyhook::CallbackType::Pre, GhostVisibilityPre)) {
            (void)StopHook();
            return false;
        }
        g_ready.store(true, std::memory_order_release);
        return true;
    } catch (...) {
        (void)StopHook();
        return false;
    }
}

void Reply(s2sdk::ConCommandContext context, std::int32_t caller, const std::string& message) {
    s2sdk::ReplyToCommand(context, caller, plg::string(message));
}

s2sdk::ResultType Command(std::int32_t caller,
                          s2sdk::ConCommandContext context,
                          const plg::vector<plg::string>& arguments) noexcept {
    g_commandThreadId.store(CurrentNativeThreadId(), std::memory_order_relaxed);
    g_commandCalls.fetch_add(1, std::memory_order_relaxed);
    try {
        if (caller != -1 || context != s2sdk::ConCommandContext::Console || arguments.size() < 2) {
            Reply(context, caller, "usage: fr_ghost_visibility_minimal status|cvar|reload_schema <slot>|sound <slot> [distance]|sound_stop|start|stop|target <slot>|off|clear (server console only)");
            return s2sdk::ResultType::Handled;
        }
        const std::string_view action(arguments[1].c_str());
        if (action == "status") {
            std::ostringstream out;
            out << "[ghost-visibility-minimal] ready=" << g_ready.load(std::memory_order_relaxed)
                << " detached=" << !g_hookStatePublished.load(std::memory_order_relaxed)
                << " gameconfig_open=" << g_gameConfigOpenPublished.load(std::memory_order_relaxed)
                << " command_registered=" << g_commandRegistered.load(std::memory_order_relaxed)
                << " gameframe_registered=" << g_gameFrameRegistered.load(std::memory_order_relaxed)
                << " target_slot=" << g_targetSlot.load(std::memory_order_relaxed)
                << " target_pawn=" << g_targetPawnHandle.load(std::memory_order_relaxed)
                << " target_controller=" << g_targetControllerHandle.load(std::memory_order_relaxed)
                << " calls=" << g_calls.load(std::memory_order_relaxed)
                << " bot_calls=" << g_botCalls.load(std::memory_order_relaxed)
                << " target_calls=" << g_targetCalls.load(std::memory_order_relaxed)
                << " blocked=" << g_blockedCalls.load(std::memory_order_relaxed)
                << " mask_cleared=" << g_maskCleared.load(std::memory_order_relaxed)
                << " off_thread_calls=" << g_offThreadCalls.load(std::memory_order_relaxed)
                << " command_calls=" << g_commandCalls.load(std::memory_order_relaxed)
                << " update_calls=" << g_updateCalls.load(std::memory_order_relaxed)
                << " gameframe_calls=" << g_frameCalls.load(std::memory_order_relaxed)
                << " command_tid=" << g_commandThreadId.load(std::memory_order_relaxed)
                << " update_tid=" << g_updateThreadId.load(std::memory_order_relaxed)
                << " gameframe_tid=" << g_frameThreadId.load(std::memory_order_relaxed)
                << " isvisible_tid=" << g_visibleThreadId.load(std::memory_order_relaxed)
                << " last_receiver_slot=" << g_lastReceiverSlot.load(std::memory_order_relaxed)
                << " last_target_slot=" << g_lastTargetSlot.load(std::memory_order_relaxed)
                << " last_target_pawn=" << g_lastTargetPawn.load(std::memory_order_relaxed)
                << " test_fov=" << static_cast<int>(g_lastTestFov.load(std::memory_order_relaxed))
                << " breath_active=" << g_breathActive.load(std::memory_order_relaxed)
                << " breath_source_slot=" << g_breathSourceSlot.load(std::memory_order_relaxed)
                << " breath_source_pawn=" << g_breathSourcePawn.load(std::memory_order_relaxed)
                << " breath_source_controller=" << g_breathSourceController.load(std::memory_order_relaxed)
                << " breath_requests=" << g_breathRequests.load(std::memory_order_relaxed)
                << " breath_stop_requests=" << g_breathStopRequests.load(std::memory_order_relaxed)
                << " breath_invalid_source_stops=" << g_breathInvalidSourceStops.load(std::memory_order_relaxed);
            Reply(context, caller, out.str());
            return s2sdk::ResultType::Handled;
        }
        const auto frameThreadId = g_frameThreadId.load(std::memory_order_relaxed);
        if (frameThreadId == 0 || CurrentNativeThreadId() != frameThreadId) {
            Reply(context, caller,
                  "[ghost-visibility-minimal] control command refused; command must run on the recorded GameFrame thread");
            return s2sdk::ResultType::Handled;
        }
        if (action == "cvar") {
            constexpr char kThreatDetectionConVar[] = "sv_bot_parallel_threat_detection";
            if (!__s2sdk_FindConVar || !__s2sdk_GetConVarValue) {
                Reply(context, caller,
                      "[ghost-visibility-minimal] public SDK FindConVar/GetConVarValue imports unavailable");
                return s2sdk::ResultType::Handled;
            }
            const auto handle = s2sdk::FindConVar(kThreatDetectionConVar);
            if (!IsValidConVarHandle(handle)) {
                Reply(context, caller,
                      "[ghost-visibility-minimal] public SDK unavailable: FindConVar returned an invalid handle for sv_bot_parallel_threat_detection");
                return s2sdk::ResultType::Handled;
            }
            const auto value = s2sdk::GetConVarValue(handle);
            std::ostringstream out;
            out << "[ghost-visibility-minimal] public_sdk_convar=sv_bot_parallel_threat_detection value="
                << value.c_str();
            Reply(context, caller, out.str());
            return s2sdk::ResultType::Handled;
        }
        if (action == "reload_schema" && arguments.size() == 3) {
            std::int32_t slot = -1;
            const std::string_view slotText(arguments[2].c_str());
            const auto [end, error] = std::from_chars(slotText.data(), slotText.data() + slotText.size(), slot);
            const auto baseOffset = s2sdk::GetSchemaOffset(plg::string("CBasePlayerWeapon"), plg::string("m_bInReload"));
            const auto ccsOffset = s2sdk::GetSchemaOffset(plg::string("CCSWeaponBase"), plg::string("m_bInReload"));
            std::ostringstream out;
            out << "[ghost-visibility-minimal] reload_schema field=m_bInReload"
                << " CBasePlayerWeapon.offset=" << baseOffset
                << " CCSWeaponBase.offset=" << ccsOffset;
            if (error != std::errc{} || end != slotText.data() + slotText.size() || slot < 0 || slot >= s2sdk::GetMaxClients()) {
                out << " active_weapon=not_queried reason=invalid_slot";
                Reply(context, caller, out.str());
                return s2sdk::ResultType::Handled;
            }
            const bool isFakeClient = s2sdk::IsFakeClient(slot);
            if (!CurrentPlayer(slot, isFakeClient, nullptr, kInvalidHandle, kInvalidHandle)) {
                out << " active_weapon=not_queried reason=player_not_live";
                Reply(context, caller, out.str());
                return s2sdk::ResultType::Handled;
            }
            void* const pawn = s2sdk::GetClientPawn(slot);
            const auto pawnHandle = s2sdk::EntPointerToEntHandle(pawn);
            const auto weaponHandle = s2sdk::GetClientActiveWeapon(slot);
            void* const weapon = s2sdk::IsValidEntHandle(weaponHandle) ? s2sdk::EntHandleToEntPointer(weaponHandle) : nullptr;
            const bool currentWeapon = pawnHandle != kInvalidHandle && s2sdk::IsValidEntHandle(pawnHandle) &&
                s2sdk::EntHandleToEntPointer(pawnHandle) == pawn && weapon != nullptr &&
                s2sdk::IsValidEntHandle(weaponHandle) && s2sdk::EntPointerToEntHandle(weapon) == weaponHandle &&
                s2sdk::GetEntityOwner(weaponHandle) == pawnHandle;
            if (!currentWeapon) {
                out << " active_weapon=unavailable slot=" << slot;
                Reply(context, caller, out.str());
                return s2sdk::ResultType::Handled;
            }
            out << " slot=" << slot << " weapon_handle=" << weaponHandle
                << " weapon_classname=" << s2sdk::GetEntityClassname(weaponHandle).c_str()
                << " reload_button_down=" << ((s2sdk::GetClientButtons(slot, 0) & kInReloadButton) != 0);
            if (baseOffset >= 0) {
                out << " CBasePlayerWeapon.value="
                    << s2sdk::GetEntSchema2(weapon, plg::string("CBasePlayerWeapon"), plg::string("m_bInReload"), 0);
            }
            if (ccsOffset >= 0) {
                out << " CCSWeaponBase.value="
                    << s2sdk::GetEntSchema2(weapon, plg::string("CCSWeaponBase"), plg::string("m_bInReload"), 0);
            }
            Reply(context, caller, out.str());
            return s2sdk::ResultType::Handled;
        }
        if (action == "snapshot" && arguments.size() == 3) {
            std::int32_t slot = -1;
            const std::string_view slotText(arguments[2].c_str());
            const auto [end, error] = std::from_chars(slotText.data(), slotText.data() + slotText.size(), slot);
            if (error != std::errc{} || end != slotText.data() + slotText.size() || slot < 0 ||
                slot >= s2sdk::GetMaxClients()) {
                Reply(context, caller, "[ghost-visibility-minimal] snapshot requires a player slot");
                return s2sdk::ResultType::Handled;
            }
            const bool fakeClient = s2sdk::IsFakeClient(slot);
            if (!CurrentPlayer(slot, fakeClient, nullptr, kInvalidHandle, kInvalidHandle)) {
                Reply(context, caller, "[ghost-visibility-minimal] snapshot requires a live player pawn");
                return s2sdk::ResultType::Handled;
            }

            void* const pawn = s2sdk::GetClientPawn(slot);
            const auto pawnHandle = s2sdk::EntPointerToEntHandle(pawn);
            const auto controllerHandle = s2sdk::PlayerSlotToEntHandle(slot);
            if (pawnHandle == kInvalidHandle || controllerHandle == kInvalidHandle ||
                !CurrentPlayer(slot, fakeClient, pawn, pawnHandle, controllerHandle)) {
                Reply(context, caller, "[ghost-visibility-minimal] snapshot player identity changed during read");
                return s2sdk::ResultType::Handled;
            }

            const auto colorOffset = s2sdk::GetSchemaOffset(plg::string(kModelEntityClass), plg::string(kRenderColorField));
            const auto modeOffset = s2sdk::GetSchemaOffset(plg::string(kModelEntityClass), plg::string(kRenderModeField));
            const auto activeHandle = s2sdk::GetClientActiveWeapon(slot);
            const auto inventory = s2sdk::GetClientWeapons(slot);
            std::ostringstream out;
            out << "[ghost-visibility-minimal] snapshot slot=" << slot
                << " team=" << static_cast<std::int32_t>(s2sdk::GetClientTeam(slot))
                << " bot=" << fakeClient
                << " alive=" << s2sdk::IsClientAlive(slot)
                << " health=" << s2sdk::GetClientHealth(slot)
                << " armor=" << s2sdk::GetClientArmor(slot)
                << " pawn=" << pawnHandle
                << " controller=" << controllerHandle
                << " flags=0x" << std::hex << static_cast<std::uint32_t>(s2sdk::GetClientFlags(slot))
                << " buttons=0x" << s2sdk::GetClientButtons(slot, 0) << std::dec;
            AppendRenderReadback(out, "body", pawn, colorOffset, modeOffset);
            AppendFlameReadback(out, slot, fakeClient, pawn, pawnHandle, controllerHandle);
            out << " active_weapon=" << activeHandle;
            if (activeHandle != kInvalidHandle && s2sdk::IsValidEntHandle(activeHandle)) {
                out << ':' << s2sdk::GetEntityClassname(activeHandle).c_str();
            }
            out << " inventory=[";
            bool firstWeapon = true;
            for (const auto handle : inventory) {
                if (handle == kInvalidHandle || !s2sdk::IsValidEntHandle(handle)) {
                    continue;
                }
                void* const weapon = s2sdk::EntHandleToEntPointer(handle);
                if (!weapon || s2sdk::EntPointerToEntHandle(weapon) != handle || s2sdk::GetEntityOwner(handle) != pawnHandle) {
                    continue;
                }
                if (!firstWeapon) {
                    out << ',';
                }
                firstWeapon = false;
                out << handle << ':' << s2sdk::GetEntityClassname(handle).c_str();
                AppendRenderReadback(out, "weapon", weapon, colorOffset, modeOffset);
            }
            out << "] schema_offsets={render_color:" << colorOffset << ",render_mode:" << modeOffset << '}';

            std::vector<std::int32_t> chickenHandles;
            std::int32_t startFrom = kInvalidHandle;
            while (true) {
                const auto handle = s2sdk::FindEntityByClassname(startFrom, plg::string("chicken"));
                if (handle == kInvalidHandle || !s2sdk::IsValidEntHandle(handle) ||
                    std::find(chickenHandles.begin(), chickenHandles.end(), handle) != chickenHandles.end()) {
                    break;
                }
                chickenHandles.push_back(handle);
                startFrom = handle;
            }
            out << " world_chicken_entities=[";
            for (std::size_t index = 0; index < chickenHandles.size(); ++index) {
                if (index != 0) {
                    out << ',';
                }
                out << chickenHandles[index];
            }
            out << ']';
            Reply(context, caller, out.str());
            return s2sdk::ResultType::Handled;
        }
        if (action == "sound" && (arguments.size() == 3 || arguments.size() == 4)) {
            std::int32_t slot = -1;
            const std::string_view slotText(arguments[2].c_str());
            const auto [slotEnd, slotError] = std::from_chars(slotText.data(), slotText.data() + slotText.size(), slot);
            if (slotError != std::errc{} || slotEnd != slotText.data() + slotText.size() || slot < 0 || slot >= s2sdk::GetMaxClients()) {
                Reply(context, caller, "[ghost-visibility-minimal] sound requires a live player slot");
                return s2sdk::ResultType::Handled;
            }
            const double distanceCap = [&] {
                if (arguments.size() == 3) {
                    return kDefaultBreathDistance;
                }
                float parsed{};
                const std::string_view text(arguments[3].c_str());
                if (!ParseFiniteFloat(text, parsed) || parsed < 0.0F) {
                    return -1.0;
                }
                return static_cast<double>(parsed);
            }();
            const bool sourceIsFakeClient = s2sdk::IsFakeClient(slot);
            if (distanceCap < 0.0 || !CurrentPlayer(slot, sourceIsFakeClient, nullptr, kInvalidHandle, kInvalidHandle)) {
                Reply(context, caller, "[ghost-visibility-minimal] sound requires a live player slot and a finite non-negative distance");
                return s2sdk::ResultType::Handled;
            }
            void* const pawn = s2sdk::GetClientPawn(slot);
            const auto pawnHandle = s2sdk::EntPointerToEntHandle(pawn);
            const auto controllerHandle = s2sdk::PlayerSlotToEntHandle(slot);
            if (pawnHandle == kInvalidHandle || controllerHandle == kInvalidHandle ||
                !CurrentPlayer(slot, sourceIsFakeClient, pawn, pawnHandle, controllerHandle)) {
                Reply(context, caller, "[ghost-visibility-minimal] sound source full handles changed before emission");
                return s2sdk::ResultType::Handled;
            }
            const auto sourceOrigin = s2sdk::GetClientAbsOrigin(slot);
            if (!FiniteOrigin(sourceOrigin)) {
                Reply(context, caller, "[ghost-visibility-minimal] sound source position is not finite");
                return s2sdk::ResultType::Handled;
            }
            plg::vector<std::int32_t> recipients;
            const auto maxClients = s2sdk::GetMaxClients();
            const double distanceSquaredCap = distanceCap * distanceCap;
            for (std::int32_t recipient = 0; recipient < maxClients; ++recipient) {
                if (!s2sdk::IsClientInGame(recipient)) {
                    continue;
                }
                const auto recipientOrigin = s2sdk::GetClientAbsOrigin(recipient);
                if (FiniteOrigin(recipientOrigin) && DistanceSquared(sourceOrigin, recipientOrigin) <= distanceSquaredCap) {
                    recipients.push_back(recipient);
                }
            }
            if (recipients.empty()) {
                Reply(context, caller, "[ghost-visibility-minimal] sound not emitted; no in-game recipient is within the explicit distance cap");
                return s2sdk::ResultType::Handled;
            }

            StopBreath("replaced");
            s2sdk::EmitSoundToClient(pawnHandle, recipients, plg::string(kBreathSound), 0.9F, 100.0F);
            g_breathSourceSlot.store(slot, std::memory_order_relaxed);
            g_breathSourcePawn.store(pawnHandle, std::memory_order_relaxed);
            g_breathSourceController.store(controllerHandle, std::memory_order_relaxed);
            g_breathSourceFakeClient.store(sourceIsFakeClient, std::memory_order_relaxed);
            g_breathStopTime.store(s2sdk::GetGameTime() + kBreathPulseSeconds, std::memory_order_relaxed);
            g_breathRequests.fetch_add(1, std::memory_order_relaxed);
            g_breathActive.store(true, std::memory_order_release);

            std::ostringstream out;
            out << "[ghost-visibility-minimal] sound_emit_requested event=" << kBreathSound
                << " source_slot=" << slot << " source_pawn=" << pawnHandle << " source_controller=" << controllerHandle
                << " distance_cap=" << distanceCap << " volume=0.9 pitch=100 pulse_seconds=" << kBreathPulseSeconds
                << " recipients=[";
            for (std::size_t index = 0; index < recipients.size(); ++index) {
                if (index != 0) {
                    out << ',';
                }
                out << recipients[index];
            }
            out << ']';
            Reply(context, caller, out.str());
            return s2sdk::ResultType::Handled;
        }
        if (action == "sound_stop") {
            StopBreath("command");
            Reply(context, caller, "[ghost-visibility-minimal] sound stop requested; inspect status and server logs");
            return s2sdk::ResultType::Handled;
        }
        if (action == "start") {
            bool started = false;
            bool hookState = false;
            {
                const std::lock_guard<std::mutex> lock(g_hookControlMutex);
                started = StartHook();
                hookState = HasHookState();
            }
            if (started) {
                Reply(context, caller, "[ghost-visibility-minimal] hook ready; set target with target <slot>");
            } else if (hookState) {
                Reply(context, caller, "[ghost-visibility-minimal] start failed; hook cleanup is unconfirmed; retry stop and do not reload");
            } else {
                Reply(context, caller, "[ghost-visibility-minimal] start failed; hook is detached; inspect status before any reload");
            }
            return s2sdk::ResultType::Handled;
        }
        if (action == "stop") {
            StopBreath("stop");
            bool stopped = false;
            {
                const std::lock_guard<std::mutex> lock(g_hookControlMutex);
                stopped = StopHook();
            }
            if (stopped) {
                Reply(context, caller, "[ghost-visibility-minimal] detached=true; run status and confirm before manager reload");
            } else {
                Reply(context, caller, "[ghost-visibility-minimal] detach unconfirmed; retry stop; if still failing, stop the isolated server without reloading");
            }
            return s2sdk::ResultType::Handled;
        }
        if (action == "off" || action == "clear") {
            StopBreath("clear");
            ClearTarget();
            Reply(context, caller, "[ghost-visibility-minimal] target cleared; native visual acquisition restored");
            return s2sdk::ResultType::Handled;
        }
        if (action == "target" && arguments.size() == 3) {
            if (!g_ready.load(std::memory_order_acquire)) {
                Reply(context, caller, "[ghost-visibility-minimal] run start before setting a target");
                return s2sdk::ResultType::Handled;
            }
            std::int32_t slot = -1;
            const std::string_view slotText(arguments[2].c_str());
            const auto [end, error] = std::from_chars(slotText.data(), slotText.data() + slotText.size(), slot);
            if (error != std::errc{} || end != slotText.data() + slotText.size() || slot < 0 ||
                !CurrentPlayer(slot, s2sdk::IsFakeClient(slot), nullptr, kInvalidHandle, kInvalidHandle)) {
                Reply(context, caller, "[ghost-visibility-minimal] target requires an in-game live player pawn");
                return s2sdk::ResultType::Handled;
            }
            void* const pawn = s2sdk::GetClientPawn(slot);
            const auto pawnHandle = s2sdk::EntPointerToEntHandle(pawn);
            if (pawnHandle == kInvalidHandle || !s2sdk::IsValidEntHandle(pawnHandle) ||
                !s2sdk::IsEntityPlayerPawn(pawnHandle) || s2sdk::EntHandleToEntPointer(pawnHandle) != pawn) {
                Reply(context, caller, "[ghost-visibility-minimal] target pawn handle is not current");
                return s2sdk::ResultType::Handled;
            }
            const auto controllerHandle = s2sdk::PlayerSlotToEntHandle(slot);
            const auto targetIsFakeClient = s2sdk::IsFakeClient(slot);
            g_targetSlot.store(slot, std::memory_order_relaxed);
            g_targetPawnHandle.store(pawnHandle, std::memory_order_relaxed);
            g_targetControllerHandle.store(controllerHandle, std::memory_order_relaxed);
            g_targetFakeClient.store(targetIsFakeClient, std::memory_order_relaxed);
            std::ostringstream out;
            out << "[ghost-visibility-minimal] target_slot=" << slot << " pawn=" << pawnHandle
                << " controller=" << controllerHandle << " invisible_to=server_BOTs";
            Reply(context, caller, out.str());
            return s2sdk::ResultType::Handled;
        }
            Reply(context, caller, "usage: fr_ghost_visibility_minimal status|cvar|reload_schema <slot>|snapshot <slot>|sound <slot> [distance]|sound_stop|start|stop|target <slot>|off|clear (server console only)");
    } catch (...) {
        try { Reply(context, caller, "[ghost-visibility-minimal] command failed"); } catch (...) {}
    }
    return s2sdk::ResultType::Handled;
}

class GhostVisibilityMinimal final : public plg::Plugin {
public:
    plg::PluginResult OnPluginStart() override {
        try {
            if (!__s2sdk_LoadGameConfigFile || !__s2sdk_GetGameConfigSignature || !__s2sdk_CloseGameConfigFile ||
                !__s2sdk_GetSchemaOffset || !__s2sdk_GetMaxClients || !__s2sdk_IsClientInGame ||
                !__s2sdk_IsClientAlive || !__s2sdk_IsFakeClient || !__s2sdk_GetClientPawn || !__s2sdk_GetClientButtons ||
                !__s2sdk_PlayerSlotToEntHandle || !__s2sdk_EntPointerToEntHandle || !__s2sdk_EntHandleToEntPointer ||
                !__s2sdk_IsValidEntHandle || !__s2sdk_IsEntityPlayerPawn || !__s2sdk_IsEntityPlayerController ||
                !__s2sdk_GetClientActiveWeapon || !__s2sdk_GetClientWeapons || !__s2sdk_GetClientTeam ||
                !__s2sdk_GetClientHealth || !__s2sdk_GetClientArmor || !__s2sdk_GetClientFlags ||
                !__s2sdk_GetEntityOwner || !__s2sdk_GetEntityClassname || !__s2sdk_GetEntSchema2 || !__s2sdk_FindEntityByClassname ||
                !__s2sdk_GetEntSchemaEnt2 || !__s2sdk_AddConsoleCommand || !__s2sdk_RemoveCommandListener ||
                !__s2sdk_ReplyToCommand || !__s2sdk_PrintToServer || !__s2sdk_HasConsoleCommandCallbacks ||
                !__s2sdk_GetClientAbsOrigin || !__s2sdk_GetGameTime || !__s2sdk_EmitSoundToClient || !__s2sdk_StopSound ||
                !__s2sdk_OnGameFrame_Register || !__s2sdk_OnGameFrame_Unregister ||
                !__polyhook_HookDetour || !__polyhook_FindDetour ||
                !__polyhook_AddCallback || !__polyhook_IsCallbackRegistered || !__polyhook_RemoveCallback ||
                !__polyhook_UnhookDetour || !__polyhook_GetArgumentPointer || !__polyhook_GetArgumentInt8 ||
                !__polyhook_SetReturnBool) {
                return plg::PluginResult{"Required S2SDK/PolyHook imports unavailable"};
            }
            if (s2sdk::HasConsoleCommandCallbacks(kCommand)) {
                return plg::PluginResult{"Diagnostic command already has callbacks"};
            }

            const bool commandAdded = s2sdk::AddConsoleCommand(
                kCommand, "Isolated BOT ghost-visibility gate probe",
                s2sdk::ConVarFlag::None, Command, s2sdk::HookMode::Pre);
            const bool callbackPresent = s2sdk::HasConsoleCommandCallbacks(kCommand);
            if (callbackPresent) {
                g_commandRegistered = true;
                g_gameFrameRegistered.store(true, std::memory_order_relaxed);
                s2sdk::OnGameFrame_Register(GameFrame);
                s2sdk::PrintToServer(plg::string(
                    "[ghost-visibility-minimal] loaded passive; detached=true; GameFrame thread recording active; sound probe uses Hostage.Breath only on explicit command"));
                if (!commandAdded) {
                    s2sdk::PrintToServer(plg::string(
                        "[ghost-visibility-minimal] command registration reported failure but callback is present; plugin remains passive for recovery"));
                }
                return {};
            }
            return plg::PluginResult{"Diagnostic console command callback was not registered; no hook was attempted"};
        } catch (...) {
            try {
                if (g_gameFrameRegistered.load(std::memory_order_relaxed)) {
                    s2sdk::OnGameFrame_Unregister(GameFrame);
                    g_gameFrameRegistered.store(false, std::memory_order_relaxed);
                }
                if (s2sdk::HasConsoleCommandCallbacks(kCommand)) {
                    g_commandRegistered = true;
                    s2sdk::PrintToServer(plg::string(
                        "[ghost-visibility-minimal] startup command may be partially registered; plugin remains passive for recovery"));
                    return {};
                }
            } catch (...) {
            }
            return plg::PluginResult{"Ghost visibility passive command initialization failed; no hook was attempted"};
        }
    }

    plg::PluginResult OnPluginUpdate(std::chrono::milliseconds) override {
        g_updateThreadId.store(CurrentNativeThreadId(), std::memory_order_relaxed);
        g_updateCalls.fetch_add(1, std::memory_order_relaxed);
        return {};
    }

    plg::PluginResult OnPluginEnd() override {
        try {
            StopBreath("plugin_end");
            bool hookClean = false;
            {
                const std::lock_guard<std::mutex> lock(g_hookControlMutex);
                hookClean = StopHook();
            }
            if (g_gameFrameRegistered.load(std::memory_order_relaxed)) {
                s2sdk::OnGameFrame_Unregister(GameFrame);
                g_gameFrameRegistered.store(false, std::memory_order_relaxed);
            }
            bool commandClean = true;
            if (g_commandRegistered.load(std::memory_order_relaxed)) {
                (void)s2sdk::RemoveCommandListener(plg::string(kCommand), Command, s2sdk::HookMode::Pre);
                if (s2sdk::HasConsoleCommandCallbacks(plg::string(kCommand))) {
                    commandClean = false;
                } else {
                    g_commandRegistered.store(false, std::memory_order_relaxed);
                }
            }
            if (!hookClean || !commandClean) {
                return plg::PluginResult{
                    "Visibility hook, GameFrame listener, or command cleanup was not confirmed before unload"};
            }
            s2sdk::PrintToServer(plg::string("[ghost-visibility-minimal] unloaded after cleanup was confirmed"));
            return {};
        } catch (...) {
            return plg::PluginResult{"Ghost visibility cleanup could not be confirmed before unload"};
        }
    }
};

GhostVisibilityMinimal g_plugin;

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_plugin)
