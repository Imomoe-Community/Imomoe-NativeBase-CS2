// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <cstdlib>
#include <iostream>
#include <source_location>

#include "reload_sync_policy.hpp"

namespace {

using namespace funrounds::native::reload_sync;

void Require(bool condition, const std::source_location& location = std::source_location::current()) {
    if (!condition) {
        std::cerr << "Reload sync policy check failed at line " << location.line() << '\n';
        std::abort();
    }
}

void ButtonStatesMatchEngine() {
    constexpr bool expected[]{false, true, false, true, true, true, true, true};
    for (unsigned int code = 0; code < std::size(expected); ++code) {
        const ButtonStates states{(code & 1U) ? kReloadBit : 1, (code & 2U) ? kReloadBit : 2,
                                  (code & 4U) ? kReloadBit : 4};
        Require(ReloadButtonDown(states) == expected[code]);
    }
}

void RepeatedReloadsOnSameGun(const PlayerIdentity& player) {
    PolicyState policy;
    DispatchStack dispatch;
    int gun{};
    policy.Queue(player, 10, true);
    Require(!policy.Begin(player, 10, false));

    for (const int tick : {20, 200, 400}) {
        policy.Queue(player, tick, false);
        Require(policy.Begin(player, tick, false));
        auto& frame = dispatch.Push(&gun);
        frame.player = player;
        frame.synthetic = true;
        Require(dispatch.ObserveReloadEvent(player));
        policy.Finish(player, tick, dispatch.Pop().weaponReloadFired, false);
        Require(!policy.Begin(player, tick, false));
    }
    policy.Queue(player, 500, false);
    Require(!policy.Begin(player, 501, true));
    Require(!policy.Begin(player, 502, false));
}

void RequestsFollowLifeAcrossWeapons(const PlayerIdentity& gunUser) {
    PolicyState policy;
    const PlayerLife& life = gunUser;
    const PlayerIdentity knifeUser{life, 31}, grenadeUser{life, 32};
    const auto slot = static_cast<std::size_t>(life.slot);
    policy.Queue(life, 500, false);
    policy.Queue(life, 520, false);
    policy.fullAttempts[slot] = gunUser;
    policy.SetReceipt(slot, gunUser);
    policy.OnPlayerEvent(PlayerEvent::Equip, life.slot);
    Require(!policy.fullAttempts[slot] && !policy.fullReceipts[slot] && policy.fullReceiptCount == 0);

    Require(policy.Begin(knifeUser, 628, false)); // Original deadline passed; refresh kept the request.
    policy.Finish(knifeUser, 628, false, ReadyToReject(-1, 600, 628));
    Require(policy.Begin(grenadeUser, 630, false));
    policy.Finish(grenadeUser, 630, false, ReadyToReject(-1, 600, 630));
    // No base hook runs during the subsequent C4 interval. Recheck when a gun enters.
    Require(policy.Begin(gunUser, 639, false));
    policy.Finish(gunUser, 639, false, ReadyToReject(15, 640, 639));
    Require(policy.Begin(gunUser, 640, false));
    policy.Finish(gunUser, 640, false, ReadyToReject(15, 640, 640));
    Require(policy.Begin(gunUser, 641, false));
    policy.Finish(gunUser, 641, true, false);
    Require(!policy.Begin(gunUser, 642, false));

    policy.Queue(life, 700, false);
    Require(!policy.Begin(gunUser, 700 + kRequestTicks, false)); // First frame after C4.
    policy.Queue(life, 900, false);
    Require(policy.Begin(gunUser, 900 + kRequestTicks - 1, false));
    policy.Finish(gunUser, 900 + kRequestTicks, false, false);
    Require(!policy.Begin(gunUser, 900 + kRequestTicks, false));

    const PlayerLife nextLife{life.slot, life.controller, life.pawnHandle + 1, life.pawn};
    policy.Queue(life, 1100, false);
    Require(!policy.Begin(nextLife, 1101, false));
    Require(!policy.Begin(life, 1101, false));
    policy.Queue(life, 1102, false);
    policy.Queue(nextLife, 1102, false);
    policy.Finish(life, 1102, true, false); // An old frame cannot consume the new life.
    Require(policy.Begin(nextLife, 1102, false));
}

void ReadyRefusalIsFinal(const PlayerLife& life) {
    PolicyState policy;
    // The engine refuses a full clip with the extension off, or a weapon with
    // zero reserve. The policy uses the observed refusal, not guessed ammo rules.
    for (const int clip : {30, 0}) {
        policy.Queue(life, 1000, false);
        Require(policy.Begin(life, 1000, false));
        policy.Finish(life, 1000, false, ReadyToReject(clip, 1000, 1000));
        Require(policy.Begin(life, 1001, false));
        policy.Finish(life, 1001, false, ReadyToReject(clip, 1000, 1001));
        Require(!policy.Begin(life, 1002, false));
    }
}

void ReloadTransitionBroadcastsOnce(const PlayerIdentity& player) {
    int gun{};
    DispatchStack dispatch;
    auto& automatic = dispatch.Push(&gun);
    automatic.player = player;
    Require(!automatic.ShouldBroadcast(false));
    Require(dispatch.Pop().ShouldBroadcast(true)); // No event, false -> true.

    auto& ongoing = dispatch.Push(&gun);
    ongoing.player = player;
    ongoing.wasReloading = true;
    Require(!dispatch.Pop().ShouldBroadcast(true));

    auto& nativeInput = dispatch.Push(&gun);
    nativeInput.player = player;
    Require(!dispatch.ObserveReloadEvent(player)); // Broadcast the real event.
    const auto accepted = dispatch.Pop();
    Require(accepted.weaponReloadFired && !accepted.ShouldBroadcast(true));

    auto& synthetic = dispatch.Push(&gun);
    synthetic.player = player;
    synthetic.synthetic = true;
    Require(!dispatch.Pop().ShouldBroadcast(true)); // Silent synthetic success.
}

void FramesSuppressOnlyTheirOwnInjection(const PlayerIdentity& a, const PlayerIdentity& b) {
    int gunA{};
    int gunB{};
    std::byte buttonsA{};
    DispatchStack dispatch;
    auto& outer = dispatch.Push(&gunA);
    outer.player = a;
    outer.synthetic = true;
    outer.editedButtons = &buttonsA;
    Require(!dispatch.ObserveReloadEvent(b));
    Require(!outer.weaponReloadFired);

    auto& inner = dispatch.Push(&gunB);
    inner.player = b;
    Require(!dispatch.IsInjected(b, &buttonsA));
    Require(!dispatch.ObserveReloadEvent(b)); // Different player, real event.
    Require(dispatch.Pop().weaponReloadFired);
    Require(!dispatch.Current()->weaponReloadFired && dispatch.Current()->editedButtons == &buttonsA);

    auto& recursive = dispatch.Push(&gunA);
    recursive.player = a;
    recursive.synthetic = dispatch.IsInjected(a, &buttonsA);
    Require(!dispatch.ObserveReloadEvent(PlayerIdentity{a, a.weapon + 1}));
    Require(dispatch.ObserveReloadEvent(a));
    const auto nested = dispatch.Pop();
    Require(nested.weaponReloadFired && nested.editedButtons == nullptr);
    const auto final = dispatch.Pop();
    Require(final.weaponReloadFired && final.editedButtons == &buttonsA && !dispatch.Current());

    dispatch.Push(&gunA).player = a;
    dispatch.Push(&gunA).player = a;
    Require(dispatch.Pop().ShouldBroadcast(true));
    dispatch.ObserveNestedReload(a);
    Require(!dispatch.Pop().ShouldBroadcast(true));
}

void LifecycleClearsRequests(const PlayerIdentity& a, const PlayerIdentity& b) {
    PolicyState policy;
    const auto slot = static_cast<std::size_t>(a.slot);
    for (const auto event : {PlayerEvent::Death, PlayerEvent::Spawn, PlayerEvent::Disconnect, PlayerEvent::BotTakeover}) {
        policy.Queue(a, 1200, false);
        policy.Queue(b, 1200, false);
        policy.fullAttempts[slot] = a;
        policy.SetReceipt(slot, a);
        policy.OnPlayerEvent(event, a.slot);
        Require(!policy.Begin(a, 1200, false) && policy.Begin(b, 1200, false));
        Require(!policy.fullAttempts[slot] && !policy.fullReceipts[slot] && policy.fullReceiptCount == 0);
    }
    policy.Queue(a, 1200, false);
    policy.fullAttempts[slot] = a;
    policy.OnRoundEnd();
    Require(!policy.Begin(a, 1200, false) && !policy.Begin(b, 1200, false) && !policy.fullAttempts[slot]);
    policy.Queue(a, 1200, false);
    policy.SetReceipt(slot, a);
    policy.ClearPending(); // Synchronization off; an admitted full reload may still drain.
    Require(!policy.Begin(a, 1200, false) && policy.fullReceipts[slot] == a);
}

} // namespace

int main() {
    int pawnA{};
    int pawnB{};
    const PlayerIdentity a{{0, 10, 20, &pawnA}, 30};
    const PlayerIdentity b{{1, 11, 21, &pawnB}, 40};
    ButtonStatesMatchEngine();
    RepeatedReloadsOnSameGun(a);
    RequestsFollowLifeAcrossWeapons(a);
    ReadyRefusalIsFinal(a);
    ReloadTransitionBroadcastsOnce(a);
    FramesSuppressOnlyTheirOwnInjection(a, b);
    LifecycleClearsRequests(a, b);
}
