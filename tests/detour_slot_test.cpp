/* Copyright (C) 2026 Mai Ooizumi
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include "detour_slot.hpp"

#include <cstdlib>
#include <iostream>
#include <set>
#include <utility>

namespace {
using Callback = polyhook::Callback;
using Phase = polyhook::CallbackType;
void* const address = reinterpret_cast<void*>(0x1000);
void* const handle = reinterpret_cast<void*>(0x2000);
void* const replacement = reinterpret_cast<void*>(0x3000);
void* g_current{};
std::set<std::pair<Phase, Callback>> callbacks;
bool failAdd{}, failRemove{}, failPostRemove{}, failUnhook{};
int creates{}, unhooks{}, foreignCalls{};
polyhook::ResultType Own(polyhook::HookHandle, polyhook::ParametersHandle, int,
                         polyhook::ReturnHandle, Phase) { return polyhook::ResultType::Ignored; }
polyhook::ResultType Foreign(polyhook::HookHandle, polyhook::ParametersHandle, int,
                             polyhook::ReturnHandle, Phase) { ++foreignCalls; return polyhook::ResultType::Ignored; }
void Check(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
polyhook::HookHandle Create() { ++creates; g_current = handle; return handle; }
}

namespace polyhook {
HookHandle FindDetour(void* target) { return target == address ? g_current : nullptr; }
bool UnhookDetour(void*) { ++unhooks; if (failUnhook) return false; g_current = nullptr; return true; }
bool AddCallback(HookHandle, CallbackType type, Callback callback) {
    return !failAdd && callbacks.emplace(type, callback).second;
}
bool RemoveCallback(HookHandle, CallbackType type, Callback callback) {
    return !failRemove && !(failPostRemove && type == CallbackType::Post) && callbacks.erase({type, callback}) != 0;
}
bool IsCallbackRegistered(HookHandle, CallbackType type, Callback callback) {
    return callbacks.contains({type, callback});
}
bool AreCallbacksRegistered(HookHandle) { return !callbacks.empty(); }
}

int main() {
    using funrounds::native::detail::DetourSlot;
    void* knownAddress{};
    polyhook::HookHandle knownHook{};
    DetourSlot slot(knownAddress, knownHook);
    const auto enable = [&] {
        return slot.Acquire(address, Create) && slot.Add(Phase::Post, Own) && slot.Add(Phase::Pre, Own);
    };
    const auto disable = [&] {
        return slot.Remove(Phase::Pre, Own) && slot.Remove(Phase::Post, Own) && slot.Release();
    };

    Check(enable() && enable() && creates == 1 && callbacks.size() == 2, "idempotent enable");
    Check(polyhook::AddCallback(handle, Phase::Pre, Foreign), "foreign attach");
    Check(disable() && knownAddress == address && knownHook == handle && g_current == handle,
          "retain shared detour");
    Check(disable() && knownHook == handle && callbacks.size() == 1, "idempotent shared disable");
    Check(enable() && creates == 1 && callbacks.size() == 3, "reattach known shared detour");
    Check(polyhook::RemoveCallback(handle, Phase::Pre, Foreign), "foreign removed first");
    Check(disable() && !knownAddress && !knownHook && !g_current && callbacks.empty(),
          "final owner unhooks");
    Check(disable(), "idempotent empty disable");

    Check(enable() && polyhook::AddCallback(handle, Phase::Pre, Foreign) && disable(),
          "foreign last setup");
    Check(polyhook::RemoveCallback(handle, Phase::Pre, Foreign), "foreign removed last callback");
    Check(disable() && !g_current && !knownHook, "shutdown reclaims foreign-last detour");

    Check(enable() && polyhook::AddCallback(handle, Phase::Pre, Foreign) && disable(),
          "external detour removal setup");
    callbacks.clear(); g_current = nullptr;
    Check(enable() && creates == 4 && disable(), "recreate after external unhook");

    Check(enable(), "replacement setup");
    g_current = replacement;
    Check(!disable() && knownHook == handle, "reject unknown replacement");
    g_current = handle;
    failRemove = true;
    Check(!disable() && knownHook == handle, "retain failed callback cleanup");
    failRemove = false;
    failPostRemove = true;
    Check(!disable() && !polyhook::IsCallbackRegistered(handle, Phase::Pre, Own) &&
          polyhook::IsCallbackRegistered(handle, Phase::Post, Own), "partial callback cleanup retained");
    failPostRemove = false;
    failUnhook = true;
    Check(!disable() && knownHook == handle, "retain failed detour cleanup");
    failUnhook = false;
    Check(disable(), "retry detour cleanup");

    // A foreign consumer can tear down the old shared detour and immediately
    // create a different one at the same address before our next cleanup.
    Check(enable() && polyhook::AddCallback(handle, Phase::Pre, Foreign) && disable(),
          "replacement after shared disable setup");
    callbacks.clear();
    g_current = replacement;
    Check(polyhook::AddCallback(replacement, Phase::Pre, Foreign), "replacement foreign attach");
    const int unhooksBeforeReplacement = unhooks;
    Check(!slot.Acquire(address, Create), "unknown replacement is not implicitly joined");
    Check(disable() && !knownAddress && !knownHook && g_current == replacement &&
          polyhook::IsCallbackRegistered(replacement, Phase::Pre, Foreign) && unhooks == unhooksBeforeReplacement,
          "forget stale record without touching replacement");
    Check(!slot.Acquire(address, Create), "forgotten replacement remains foreign");
    Check(slot.Acquire(address, Create, true) && slot.Add(Phase::Post, Own) &&
          slot.Remove(Phase::Post, Own) && slot.Release() && g_current == replacement,
          "explicitly join replacement after cleanup");
    Check(polyhook::RemoveCallback(replacement, Phase::Pre, Foreign) && slot.Release() && !g_current,
          "reclaim explicitly joined replacement when final owner leaves");

    Check(enable() && polyhook::AddCallback(handle, Phase::Pre, Foreign) && disable(),
          "direct compatible replacement reactivation setup");
    callbacks.clear();
    g_current = replacement;
    Check(polyhook::AddCallback(replacement, Phase::Pre, Foreign) &&
          slot.Acquire(address, Create, true) && slot.Add(Phase::Post, Own),
          "compatible callers can rejoin a replacement directly");
    Check(disable() && g_current == replacement &&
          polyhook::RemoveCallback(replacement, Phase::Pre, Foreign) && slot.Release() && !g_current,
          "release directly rejoined replacement");

    Check(enable(), "replacement callback safety setup");
    callbacks.clear();
    g_current = replacement;
    Check(polyhook::AddCallback(replacement, Phase::Post, Own), "replacement contains our post callback");
    Check(!disable() && knownHook == handle &&
          polyhook::IsCallbackRegistered(replacement, Phase::Post, Own),
          "replacement with our callback blocks cleanup without mutation");
    Check(polyhook::RemoveCallback(replacement, Phase::Post, Own) && disable() &&
          !knownHook && g_current == replacement, "callback owner removal permits forgetting stale record");
    g_current = nullptr;

    failAdd = true;
    Check(slot.Acquire(address, Create) && !slot.Add(Phase::Pre, Own), "failed callback registration");
    failAdd = false;
    Check(disable(), "rollback failed callback registration");
    g_current = replacement;
    Check(!slot.Acquire(address, Create), "reject unknown existing detour");
    Check(slot.Acquire(address, Create, true) && slot.Add(Phase::Pre, Own), "explicitly join existing");
    Check(slot.Remove(Phase::Pre, Own) && slot.Release() && !g_current, "release joined detour");
    bool attempted{};
    DetourSlot attempt(knownAddress, knownHook, &attempted);
    Check(!attempt.Acquire(address, [&] { g_current = handle; return polyhook::HookHandle{}; }) &&
          attempted && g_current == handle, "record partial detour creation");
    Check(attempt.Release() && !g_current && !attempted, "reclaim partial detour creation");
}
