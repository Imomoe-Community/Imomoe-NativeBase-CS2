// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <cmath>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>

#include <plg/plugin.hpp>
#include <plugin_export.h>
#include <imomoe_native_base/imomoe_native_base.hpp>
#include <s2sdk/s2sdk.hpp>

namespace {

constexpr char kCommand[] = "fr_ground_reverse_minimal";
std::optional<plg::string> g_previousAcceleration;
bool g_cleanupPending{};
bool g_registered{};
std::uint64_t g_snapshot{};

float Acceleration(const plg::string& value) {
    return std::stof(std::string(value.c_str()));
}

bool Stop() {
    if (!g_cleanupPending && !g_previousAcceleration) {
        return true;
    }
    // A failed detach retains both ownership and the recovery input for retry.
    if (!imomoe_native_base::SetGroundReverseActive(false, 5.5F)) {
        return false;
    }
    g_cleanupPending = false;
    if (g_previousAcceleration) {
        const auto variable = s2sdk::FindConVar("sv_accelerate");
        if (!variable) {
            return false;
        }
        if (Acceleration(s2sdk::GetConVarValue(variable)) == 0.0F) {
            s2sdk::SetConVarValue(variable, *g_previousAcceleration, true, false);
            if (std::abs(Acceleration(s2sdk::GetConVarValue(variable)) - Acceleration(*g_previousAcceleration)) > 0.001F) {
                return false;
            }
        }
        g_previousAcceleration.reset();
    }
    return true;
}

void Snapshot() {
    const auto id = ++g_snapshot;
    const auto flags = s2sdk::GetSchemaOffset("CBaseEntity", "m_fFlags");
    for (std::int32_t slot = 0; slot < s2sdk::GetMaxClients(); ++slot) {
        if (!s2sdk::IsClientInGame(slot) || !s2sdk::IsClientAlive(slot)) {
            continue;
        }
        auto* pawn = s2sdk::GetClientPawn(slot);
        const auto handle = s2sdk::EntPointerToEntHandle(pawn);
        if (!pawn || !s2sdk::IsValidEntHandle(handle)) {
            continue;
        }
        const auto position = s2sdk::GetEntityAbsOrigin(handle);
        const auto velocity = s2sdk::GetEntityAbsVelocity(handle);
        std::ostringstream line;
        line << "[ground-reverse-minimal] snapshot=" << id << " tick=" << s2sdk::GetGameTickCount()
             << " slot=" << slot << " controller=" << s2sdk::PlayerSlotToEntHandle(slot)
             << " pawn=" << handle << " bot=" << s2sdk::IsFakeClient(slot)
             << " flags=" << (flags >= 0 ? s2sdk::GetEntSchema2(pawn, "CBaseEntity", "m_fFlags", 0) : -1)
             << " position=" << position.x << ',' << position.y << ',' << position.z
             << " velocity=" << velocity.x << ',' << velocity.y << ',' << velocity.z
             << " health=" << s2sdk::GetClientHealth(slot) << " armor=" << s2sdk::GetClientArmor(slot);
        const auto weapon = s2sdk::GetClientActiveWeapon(slot);
        if (s2sdk::IsValidEntHandle(weapon)) {
            line << " active=" << s2sdk::GetEntityClassname(weapon).c_str();
        }
        line << " inventory=";
        for (const auto item : s2sdk::GetClientWeapons(slot)) {
            if (s2sdk::IsValidEntHandle(item)) {
                line << s2sdk::GetEntityClassname(item).c_str() << ',';
            }
        }
        s2sdk::PrintToServer(plg::string(line.str()));
    }
}

plg::string KillOneBot() {
    for (std::int32_t slot = 0; slot < s2sdk::GetMaxClients(); ++slot) {
        if (s2sdk::IsClientInGame(slot) && !s2sdk::IsFakeClient(slot)) {
            return "[ground-reverse-minimal] killbot refused: human present";
        }
    }
    const auto world = s2sdk::EntIndexToEntHandle(0);
    if (!s2sdk::IsValidEntHandle(world)) {
        return "[ground-reverse-minimal] killbot refused: world entity unavailable";
    }
    for (std::int32_t slot = 0; slot < s2sdk::GetMaxClients(); ++slot) {
        if (!s2sdk::IsClientInGame(slot) || !s2sdk::IsFakeClient(slot) || !s2sdk::IsClientAlive(slot)) {
            continue;
        }
        auto* pawn = s2sdk::GetClientPawn(slot);
        const auto handle = s2sdk::EntPointerToEntHandle(pawn);
        if (!pawn || !s2sdk::IsValidEntHandle(handle)) {
            continue;
        }
        const plg::vec3 zero{};
        const auto position = s2sdk::GetEntityAbsOrigin(handle);
        const auto lost = imomoe_native_base::TakeEntityDamageWithWeapon(
            handle, world, world, zero, position, static_cast<float>(s2sdk::GetClientHealth(slot)) * 10.0F,
            0, -1, false);
        std::ostringstream message;
        message << "[ground-reverse-minimal] killbot slot=" << slot << " pawn=" << handle
                << " lost=" << lost << " dead=" << !s2sdk::IsClientAlive(slot)
                << " world=" << world << " world_class=" << s2sdk::GetEntityClassname(world).c_str();
        return plg::string(message.str());
    }
    return "[ground-reverse-minimal] killbot refused: no live BOT";
}

s2sdk::ResultType Command(std::int32_t caller, s2sdk::ConCommandContext context,
                          const plg::vector<plg::string>& arguments) noexcept {
    const auto reply = [&](const char* text) { s2sdk::ReplyToCommand(context, caller, plg::string(text)); };
    try {
        if (caller != -1 || context != s2sdk::ConCommandContext::Console || arguments.size() != 2) {
            reply("usage: fr_ground_reverse_minimal support|start|stop|snapshot|killbot (server console only)");
            return s2sdk::ResultType::Handled;
        }
        if (arguments[1] == "support") {
            reply(imomoe_native_base::HasGroundReverseSupport() ? "[ground-reverse-minimal] support=true" : "[ground-reverse-minimal] support=false");
        } else if (arguments[1] == "start") {
            if (g_cleanupPending || g_previousAcceleration || !imomoe_native_base::HasGroundReverseSupport()) {
                reply("[ground-reverse-minimal] start refused: active, recovery pending, or unsupported");
                return s2sdk::ResultType::Handled;
            }
            const auto variable = s2sdk::FindConVar("sv_accelerate");
            if (!variable) {
                reply("[ground-reverse-minimal] sv_accelerate unavailable");
                return s2sdk::ResultType::Handled;
            }
            g_previousAcceleration = s2sdk::GetConVarValue(variable);
            s2sdk::SetConVarValue(variable, "0", true, false);
            // Retain potential partial native registration even when activation fails.
            g_cleanupPending = true;
            const bool started = Acceleration(s2sdk::GetConVarValue(variable)) == 0.0F
                && imomoe_native_base::SetGroundReverseActive(true, 5.5F);
            if (!started) {
                const bool restored = Stop();
                reply(restored ? "[ground-reverse-minimal] start failed; restored=true" : "[ground-reverse-minimal] start failed; restored=false");
            } else {
                reply("[ground-reverse-minimal] active=true accelerate=0 coefficient=5.5");
            }
        } else if (arguments[1] == "stop") {
            reply(Stop() ? "[ground-reverse-minimal] active=false restored=true" : "[ground-reverse-minimal] active=false restored=false");
        } else if (arguments[1] == "snapshot") {
            Snapshot();
            reply("[ground-reverse-minimal] snapshot complete");
        } else if (arguments[1] == "killbot") {
            s2sdk::ReplyToCommand(context, caller, KillOneBot());
        } else {
            reply("usage: fr_ground_reverse_minimal support|start|stop|snapshot|killbot");
        }
    } catch (...) {
        // Do not discard state: stop/unload can retry restoration after a failed command.
        try { reply("[ground-reverse-minimal] command failed; recovery retained"); } catch (...) {}
    }
    return s2sdk::ResultType::Handled;
}

class Driver final : public plg::Plugin {
public:
    plg::PluginResult OnPluginStart() override {
        try {
            // S2SDK keeps the engine command name after its last callback is removed.
            if (s2sdk::HasConsoleCommandCallbacks(kCommand)) {
                return plg::PluginResult{"Minimal diagnostic command already exists"};
            }
            if (!s2sdk::AddConsoleCommand(kCommand, "Isolated ground-reverse boundary diagnostic",
                                          s2sdk::ConVarFlag::None, Command, s2sdk::HookMode::Pre)) {
                (void)s2sdk::RemoveCommandListener(kCommand, Command, s2sdk::HookMode::Pre);
                return plg::PluginResult{"Minimal diagnostic command registration failed"};
            }
            g_registered = true;
            return {};
        } catch (...) {
            return plg::PluginResult{"Minimal diagnostic initialization threw"};
        }
    }

    plg::PluginResult OnPluginEnd() override {
        try {
            if (!Stop()) {
                return plg::PluginResult{"Minimal diagnostic native/ConVar recovery failed"};
            }
            if (g_registered) {
                (void)s2sdk::RemoveCommand(kCommand, Command);
                if (s2sdk::HasConsoleCommandCallbacks(kCommand)) {
                    return plg::PluginResult{"Minimal diagnostic command cleanup failed"};
                }
            }
            g_registered = false;
            return {};
        } catch (...) {
            return plg::PluginResult{"Minimal diagnostic cleanup threw"};
        }
    }
};

Driver g_driver;
} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_driver)
