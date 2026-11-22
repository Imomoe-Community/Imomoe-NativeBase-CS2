// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <source_location>
#include <vector>

#include "ghost_radar_policy.hpp"
#include "ghost_passive_sound_policy.hpp"
#include "ghost_passive_noise_policy.hpp"

namespace {

void require(bool condition, const std::source_location& location = std::source_location::current()) {
    if (!condition) {
        std::cerr << "ghost radar policy check failed at line " << location.line() << '\n';
        std::abort();
    }
}

struct Update {
    std::int32_t entityIndex{-1};
    std::int32_t classId{};
    std::array<std::int32_t, 3> origin{};

    std::int32_t entity_idx() const noexcept { return entityIndex; }
};

struct SendFrame {
    std::int32_t id{};
    bool ownsReplacement{};
    const void* client{};
    const void* originalMessage{};
    const void* replacementMessage{};
};

class RepeatedUpdates final {
public:
    int size() const noexcept { return static_cast<int>(updates_.size()); }
    const Update& Get(int index) const { return updates_[static_cast<std::size_t>(index)]; }
    Update& Get(int index) { return updates_[static_cast<std::size_t>(index)]; }
    void Swap(int left, int right) { std::swap(updates_[static_cast<std::size_t>(left)], updates_[static_cast<std::size_t>(right)]); }
    void RemoveLast() { updates_.pop_back(); }
    void Add(Update update) { updates_.push_back(update); }

private:
    std::vector<Update> updates_;
};

void target_set_preserves_full_identity_and_merges_only_duplicate_handles() {
    using funrounds::native::ghost_radar::BuildTargetSet;
    using funrounds::native::ghost_radar::Target;

    std::vector<Target> input{
        {UINT32_C(0x80001022), 42, UINT64_C(1) << 2},
        {UINT32_C(0x80001022), 42, UINT64_C(1) << 63},
        {UINT32_C(0x80001023), 43, UINT64_C(1) << 31},
    };
    std::vector<Target> targets;
    require(BuildTargetSet(input, targets));
    require(targets.size() == 2);
    require(targets[0].entityIndex == 42);
    require(targets[0].fullHandle == UINT32_C(0x80001022));
    require(targets[0].enemyRecipients == ((UINT64_C(1) << 2) | (UINT64_C(1) << 63)));
    require(targets[1].entityIndex == 43);

    const std::vector<Target> conflicting{
        {UINT32_C(0x80001022), 42, UINT64_C(1) << 2},
        {UINT32_C(0x80001024), 42, UINT64_C(1) << 3},
    };
    require(!BuildTargetSet(conflicting, targets));

    const std::vector<Target> invalidHandle{{UINT32_MAX, 42, 1}};
    require(!BuildTargetSet(invalidHandle, targets));
    const std::vector<Target> invalidIndex{{UINT32_C(0x80001022), -1, 1}};
    require(!BuildTargetSet(invalidIndex, targets));
}

void recipient_identity_and_64_bit_masks_are_exact() {
    using funrounds::native::ghost_radar::IsCurrentRecipient;
    using funrounds::native::ghost_radar::HasTargetForRecipient;
    using funrounds::native::ghost_radar::kInvalidUserId;
    using funrounds::native::ghost_radar::kMaxClientSlots;
    using funrounds::native::ghost_radar::ShouldFilterEntityUpdate;
    using funrounds::native::ghost_radar::Target;

    std::array<std::int32_t, kMaxClientSlots> userIds;
    userIds.fill(kInvalidUserId);
    userIds[2] = 501;
    userIds[63] = 702;
    require(IsCurrentRecipient(2, 501, userIds));
    require(!IsCurrentRecipient(2, 502, userIds));
    require(!IsCurrentRecipient(2, kInvalidUserId, userIds));
    require(!IsCurrentRecipient(64, 702, userIds));

    const std::vector<Target> targets{
        {UINT32_C(0x80001022), 42, (UINT64_C(1) << 2) | (UINT64_C(1) << 63)},
    };
    require(ShouldFilterEntityUpdate(42, 2, targets));
    require(ShouldFilterEntityUpdate(42, 63, targets));
    require(HasTargetForRecipient(2, targets));
    require(HasTargetForRecipient(63, targets));
    require(!HasTargetForRecipient(1, targets));
    require(!HasTargetForRecipient(64, targets));
    require(!ShouldFilterEntityUpdate(42, 1, targets));
    require(!ShouldFilterEntityUpdate(42, 64, targets));
    require(!ShouldFilterEntityUpdate(41, 2, targets));
    require(!ShouldFilterEntityUpdate(-1, 2, targets));
}

void passive_sound_filter_requires_known_event_enemy_mask_and_exact_hidden_handle() {
    using funrounds::native::ghost_passive_sound::FindSoundEventName;
    using funrounds::native::ghost_passive_sound::IsKnownPassiveSoundEvent;
    using funrounds::native::ghost_passive_sound::IsPassiveSoundEventName;
    using funrounds::native::ghost_passive_sound::ShouldFilterStartSoundEvent;
    using funrounds::native::ghost_radar::Target;

    constexpr std::uint32_t footstepHashLeft = UINT32_C(0x53b25cbe);
    constexpr std::uint32_t footstepHashCompanion = UINT32_C(0x5bfa1f98);
    constexpr std::uint32_t footstepHashRight = UINT32_C(0x45587a24);
    constexpr std::uint32_t ghostIdleHash = UINT32_C(0xd7892642);
    constexpr std::uint32_t breathHash = UINT32_C(0x8660857e);
    constexpr std::uint32_t unclassifiedHash = UINT32_C(0x24681357);
    constexpr std::array passiveHashes{footstepHashRight, footstepHashLeft, footstepHashCompanion, ghostIdleHash};
    static_assert(funrounds::native::ghost_passive_sound::kSoundEventNames.size() == 158);
    static_assert(funrounds::native::ghost_passive_sound::kPassiveSoundEventCount == 157);
    static_assert(IsPassiveSoundEventName("Land_Flesh.StepLeft"));
    static_assert(IsPassiveSoundEventName("Gear.JumpLand.T"));
    static_assert(IsPassiveSoundEventName("WetTile.Splash"));
    static_assert(IsPassiveSoundEventName("Player.GhostIdle"));
    static_assert(!IsPassiveSoundEventName("Hostage.Breath"));
    static_assert(!IsPassiveSoundEventName("weapon_fire"));
    static_assert(FindSoundEventName("Hostage.Breath") != nullptr);
    constexpr std::uint32_t hiddenHandle = UINT32_C(0x80001022);
    constexpr std::uint32_t visibleHandle = UINT32_C(0x80002022);
    const std::vector<Target> ghosts{
        {hiddenHandle, 42, (UINT64_C(1) << 2) | (UINT64_C(1) << 63)},
        {visibleHandle, 43, UINT64_C(1) << 2},
    };
    const std::vector<std::uint32_t> hidden{hiddenHandle};

    require(ShouldFilterStartSoundEvent(42, footstepHashLeft, 2, passiveHashes, ghosts, hidden));
    require(ShouldFilterStartSoundEvent(42, footstepHashCompanion, 2, passiveHashes, ghosts, hidden));
    require(ShouldFilterStartSoundEvent(42, footstepHashRight, 63, passiveHashes, ghosts, hidden));
    require(ShouldFilterStartSoundEvent(42, ghostIdleHash, 2, passiveHashes, ghosts, hidden));
    require(IsKnownPassiveSoundEvent(footstepHashLeft, passiveHashes));
    require(!IsKnownPassiveSoundEvent(breathHash, passiveHashes));
    require(!IsKnownPassiveSoundEvent(unclassifiedHash, passiveHashes));
    require(!ShouldFilterStartSoundEvent(42, footstepHashLeft, 1, passiveHashes, ghosts, hidden));
    require(!ShouldFilterStartSoundEvent(42, breathHash, 2, passiveHashes, ghosts, hidden));
    require(!ShouldFilterStartSoundEvent(42, unclassifiedHash, 2, passiveHashes, ghosts, hidden));
    require(!ShouldFilterStartSoundEvent(43, footstepHashLeft, 2, passiveHashes, ghosts, hidden));
    require(!ShouldFilterStartSoundEvent(-1, footstepHashLeft, 2, passiveHashes, ghosts, hidden));
    require(!ShouldFilterStartSoundEvent(42, footstepHashLeft, 64, passiveHashes, ghosts, hidden));

    // A recycled entity index is not enough: the active Ghost handle must still
    // match the exact hidden-handle snapshot owned by the visibility policy.
    const std::vector<std::uint32_t> staleHandle{UINT32_C(0x80001023)};
    require(!ShouldFilterStartSoundEvent(42, footstepHashLeft, 2, passiveHashes, ghosts, staleHandle));
}

void bot_noise_filter_only_suppresses_exact_hidden_enemy_footsteps() {
    using funrounds::native::ghost_passive_noise::PlayerFootstepScope;
    using funrounds::native::ghost_passive_noise::ShouldSuppressPlayerFootstep;
    using funrounds::native::ghost_radar::Target;

    constexpr std::uint32_t hiddenHandle = UINT32_C(0x80001022);
    constexpr std::uint32_t visibleHandle = UINT32_C(0x80002022);
    constexpr std::uint64_t enemyMask = (UINT64_C(1) << 2) | (UINT64_C(1) << 63);
    const std::vector<Target> targets{
        {hiddenHandle, 42, enemyMask},
        {visibleHandle, 43, UINT64_C(1) << 2},
    };
    const std::vector<std::uint32_t> hidden{hiddenHandle};
    std::array<std::uint32_t, 64> recipientControllerHandles{};
    recipientControllerHandles.fill(UINT32_MAX);
    std::array<std::int32_t, 64> recipientUserIds{};
    recipientUserIds.fill(-1);
    constexpr std::uint32_t ctBotController = UINT32_C(0x80003022);
    constexpr std::uint32_t otherBotController = UINT32_C(0x80003122);
    recipientControllerHandles[2] = ctBotController;
    recipientControllerHandles[63] = otherBotController;
    recipientUserIds[2] = 501;
    recipientUserIds[63] = 702;
    constexpr std::uint64_t connectedRecipients = (UINT64_C(1) << 2) | (UINT64_C(1) << 63);
    constexpr std::uintptr_t footstepEvent = 0x101;
    constexpr std::uintptr_t nestedFootstepEvent = 0x202;
    constexpr std::uintptr_t activeEvent = 0x303;

    const auto suppress = [&](std::uintptr_t scopeEvent,
                              std::uintptr_t helperEvent,
                              std::uint32_t sourceHandle,
                              std::int32_t sourceEntityIndex,
                              std::uint32_t controllerHandle,
                              std::int32_t recipientSlot,
                              std::uint64_t connectedMask,
                              const std::vector<std::uint32_t>& hiddenHandles) {
        return ShouldSuppressPlayerFootstep(scopeEvent,
                                            helperEvent,
                                            sourceHandle,
                                            sourceEntityIndex,
                                            controllerHandle,
                                            recipientSlot,
                                            recipientControllerHandles,
                                            recipientUserIds,
                                            connectedMask,
                                            targets,
                                            hiddenHandles);
    };

    PlayerFootstepScope<2> scope;
    require(scope.Empty() && !scope.Active());
    scope.Begin(true, footstepEvent);
    require(scope.Active() && scope.ActiveEventIdentity() == footstepEvent);
    require(suppress(scope.ActiveEventIdentity(), footstepEvent, hiddenHandle, 42, ctBotController, 2,
                     connectedRecipients, hidden));
    require(suppress(scope.ActiveEventIdentity(), footstepEvent, hiddenHandle, 42, otherBotController, 63,
                     connectedRecipients, hidden));
    // A synchronously re-entered weapon_fire/hurt-like event shares the outer
    // manager scope, but its distinct event identity must remain unfiltered.
    require(!suppress(scope.ActiveEventIdentity(), activeEvent, hiddenHandle, 42, ctBotController, 2,
                      connectedRecipients, hidden));

    // A genuinely nested footstep uses its own top-of-stack event identity,
    // then restores the outer footstep identity when its Post callback unwinds.
    scope.Begin(scope.Active(), nestedFootstepEvent);
    require(scope.ActiveEventIdentity() == nestedFootstepEvent);
    require(!suppress(scope.ActiveEventIdentity(), footstepEvent, hiddenHandle, 42, ctBotController, 2,
                      connectedRecipients, hidden));
    require(suppress(scope.ActiveEventIdentity(), nestedFootstepEvent, hiddenHandle, 42, ctBotController, 2,
                     connectedRecipients, hidden));
    require(scope.End(nestedFootstepEvent));
    require(scope.ActiveEventIdentity() == footstepEvent);
    require(scope.End(footstepEvent) && scope.Empty() && !scope.Active());

    // Unsupported/mismatched identities fail open; a mismatched Post still
    // closes the LIFO frame so the TLS scope cannot leak into a later event.
    require(!suppress(0, footstepEvent, hiddenHandle, 42, ctBotController, 2, connectedRecipients, hidden));
    scope.Begin(true, footstepEvent);
    require(!scope.End(activeEvent) && scope.Empty());
    // The native callback maps a failed GetArgument read to identity zero on
    // both Pre and Post; the fail-open frame must still unwind.
    scope.Begin(true, 0);
    require(!scope.Active() && !scope.Empty());
    require(!scope.End(0) && scope.Empty());

    // Overflow fails open and remains balanced across every nested Post.
    PlayerFootstepScope<2> overflowScope;
    overflowScope.Begin(true, footstepEvent);
    overflowScope.Begin(overflowScope.Active(), nestedFootstepEvent);
    overflowScope.Begin(overflowScope.Active(), activeEvent);
    require(!overflowScope.Active() && overflowScope.ActiveEventIdentity() == 0);
    overflowScope.Begin(overflowScope.Active(), static_cast<std::uintptr_t>(0x404));
    require(!overflowScope.Active());
    require(!overflowScope.End(static_cast<std::uintptr_t>(0x404)) && !overflowScope.Active());
    require(!overflowScope.End(activeEvent) && overflowScope.ActiveEventIdentity() == nestedFootstepEvent);
    require(overflowScope.End(nestedFootstepEvent) && overflowScope.ActiveEventIdentity() == footstepEvent);
    require(overflowScope.End(footstepEvent) && overflowScope.Empty());

    require(!suppress(footstepEvent, activeEvent, hiddenHandle, 42, ctBotController, 2, connectedRecipients, hidden));
    require(!suppress(footstepEvent, footstepEvent, hiddenHandle, 42, ctBotController, 1,
                      connectedRecipients, hidden));
    require(!suppress(footstepEvent, footstepEvent, hiddenHandle, 42, UINT32_C(0x80003023), 2,
                      connectedRecipients, hidden));
    require(!suppress(footstepEvent, footstepEvent, hiddenHandle, 42, ctBotController, 2,
                      UINT64_C(1) << 63, hidden));
    require(!suppress(footstepEvent, footstepEvent, hiddenHandle, 42, ctBotController, 2,
                      connectedRecipients, std::vector<std::uint32_t>{UINT32_C(0x80001023)}));
    require(!suppress(footstepEvent, footstepEvent, visibleHandle, 43, ctBotController, 2,
                      connectedRecipients, hidden));
    require(!suppress(footstepEvent, footstepEvent, hiddenHandle, -1, ctBotController, 2,
                      connectedRecipients, hidden));
}

void filtering_removes_only_selected_target_entries_and_keeps_order_and_payload() {
    using funrounds::native::ghost_radar::FilterEntityUpdates;
    using funrounds::native::ghost_radar::HasEntityUpdateToFilter;
    using funrounds::native::ghost_radar::Target;

    const std::vector<Target> targets{
        {UINT32_C(0x80001022), 42, (UINT64_C(1) << 2) | (UINT64_C(1) << 40)},
        {UINT32_C(0x80001023), 43, UINT64_C(1) << 63},
    };

    RepeatedUpdates updates;
    updates.Add({42, 8, {100, 200, 300}});
    updates.Add({88, 9, {400, 500, 600}});
    updates.Add({43, 10, {700, 800, 900}});
    updates.Add({42, 11, {1000, 1100, 1200}});
    updates.Add({-1, 12, {1300, 1400, 1500}});

    require(HasEntityUpdateToFilter(updates, 2, targets));
    require(!HasEntityUpdateToFilter(updates, 1, targets));
    require(FilterEntityUpdates(updates, 2, targets) == 2);
    require(updates.size() == 3);
    require(updates.Get(0).entityIndex == 88 && updates.Get(0).classId == 9);
    require(updates.Get(0).origin == (std::array<std::int32_t, 3>{400, 500, 600}));
    require(updates.Get(1).entityIndex == 43 && updates.Get(1).classId == 10);
    require(updates.Get(2).entityIndex == -1 && updates.Get(2).classId == 12);

    RepeatedUpdates teammateUpdates;
    teammateUpdates.Add({42, 13, {1, 2, 3}});
    require(FilterEntityUpdates(teammateUpdates, 1, targets) == 0);
    require(teammateUpdates.size() == 1 && teammateUpdates.Get(0).classId == 13);

    RepeatedUpdates observerUpdates;
    observerUpdates.Add({43, 14, {4, 5, 6}});
    require(FilterEntityUpdates(observerUpdates, 60, targets) == 0);
    require(observerUpdates.size() == 1 && observerUpdates.Get(0).entityIndex == 43);

    RepeatedUpdates highSlotUpdates;
    highSlotUpdates.Add({43, 15, {7, 8, 9}});
    require(FilterEntityUpdates(highSlotUpdates, 63, targets) == 1);
    require(highSlotUpdates.size() == 0);

    RepeatedUpdates reset;
    require(!HasEntityUpdateToFilter(reset, 2, targets));
    require(FilterEntityUpdates(reset, 2, targets) == 0);
    require(reset.size() == 0);
}

void global_spotted_is_converted_to_allowed_recipients_and_enemy_bits_are_removed() {
    using funrounds::native::ghost_radar::FilterSpottedState;

    constexpr std::uint64_t connected = (UINT64_C(1) << 1) | (UINT64_C(1) << 2) | (UINT64_C(1) << 3) |
                                        (UINT64_C(1) << 40) | (UINT64_C(1) << 63);
    constexpr std::uint64_t enemies = (UINT64_C(1) << 2) | (UINT64_C(1) << 40);
    const auto globallySpotted = FilterSpottedState(true, 0, enemies, connected);
    require(!globallySpotted.spotted);
    require(globallySpotted.spottedByMask ==
            ((UINT64_C(1) << 1) | (UINT64_C(1) << 3) | (UINT64_C(1) << 63)));

    const auto explicitlySpotted = FilterSpottedState(false, (UINT64_C(1) << 2) | (UINT64_C(1) << 3), enemies, connected);
    require(!explicitlySpotted.spotted);
    require(explicitlySpotted.spottedByMask == (UINT64_C(1) << 3));

    const auto oldConnectionBit = FilterSpottedState(true, UINT64_C(1) << 55, enemies, connected);
    require(oldConnectionBit.spottedByMask ==
            ((UINT64_C(1) << 1) | (UINT64_C(1) << 3) | (UINT64_C(1) << 55) | (UINT64_C(1) << 63)));
}

void spotted_by_mask_changes_identify_each_fixed_array_word() {
    using funrounds::native::ghost_radar::ChangedSpottedByMaskWords;
    using funrounds::native::ghost_radar::kSpottedByMaskHighWordChanged;
    using funrounds::native::ghost_radar::kSpottedByMaskLowWordChanged;

    require(ChangedSpottedByMaskWords(0, 0) == 0);
    require(ChangedSpottedByMaskWords(0, UINT64_C(1) << 31) == kSpottedByMaskLowWordChanged);
    require(ChangedSpottedByMaskWords(0, UINT64_C(1) << 32) == kSpottedByMaskHighWordChanged);
    require(ChangedSpottedByMaskWords(0, (UINT64_C(1) << 31) | (UINT64_C(1) << 63)) ==
            (kSpottedByMaskLowWordChanged | kSpottedByMaskHighWordChanged));
    require(ChangedSpottedByMaskWords(UINT64_C(1) << 63, 0) == kSpottedByMaskHighWordChanged);
}

void nested_unfiltered_send_does_not_release_an_outer_filtered_message() {
    using Stack = funrounds::native::ghost_radar::NestedInvocationStack<SendFrame, 4>;

    std::array<int, 3> identities{};
    Stack invocations;
    const auto outerIndex = invocations.Begin();
    require(outerIndex.has_value());
    auto* const outer = invocations.Get(*outerIndex);
    require(outer != nullptr);
    *outer = {1, true, &identities[0], &identities[1], &identities[2]};

    require(invocations.Begin().has_value()); // nested SendNetMessage is not filtered
    SendFrame completed;
    require(invocations.End(completed));
    require(!completed.ownsReplacement);

    require(invocations.End(completed));
    require(completed.ownsReplacement);
    require(funrounds::native::ghost_radar::MatchesSendInvocation(
        completed.client, completed.replacementMessage, &identities[0], &identities[2]));
}

void nested_sends_with_identical_client_and_message_addresses_pop_lifo() {
    using Stack = funrounds::native::ghost_radar::NestedInvocationStack<SendFrame, 4>;

    std::array<int, 2> identities{};
    Stack invocations;
    const auto outerIndex = invocations.Begin();
    require(outerIndex.has_value());
    auto* const outer = invocations.Get(*outerIndex);
    require(outer != nullptr);
    *outer = {1, true, &identities[0], &identities[1], &identities[1]};
    const auto innerIndex = invocations.Begin();
    require(innerIndex.has_value());
    auto* const inner = invocations.Get(*innerIndex);
    require(inner != nullptr);
    *inner = {2, true, &identities[0], &identities[1], &identities[1]};

    SendFrame completed;
    require(invocations.End(completed));
    require(completed.id == 2 && completed.ownsReplacement && completed.originalMessage == &identities[1]);
    require(funrounds::native::ghost_radar::MatchesSendInvocation(
        completed.client, completed.replacementMessage, &identities[0], &identities[1]));

    require(invocations.End(completed));
    require(completed.id == 1 && completed.ownsReplacement && completed.originalMessage == &identities[1]);
}

void mismatched_post_arguments_do_not_select_or_release_an_outer_frame() {
    using Stack = funrounds::native::ghost_radar::NestedInvocationStack<SendFrame, 4>;

    std::array<int, 4> identities{};
    Stack invocations;
    const auto outerIndex = invocations.Begin();
    require(outerIndex.has_value());
    auto* const outer = invocations.Get(*outerIndex);
    require(outer != nullptr);
    *outer = {1, true, &identities[0], &identities[1], &identities[2]};
    const auto innerIndex = invocations.Begin();
    require(innerIndex.has_value());
    auto* const inner = invocations.Get(*innerIndex);
    require(inner != nullptr);
    *inner = {2, true, &identities[1], &identities[2], &identities[3]};

    SendFrame completed;
    require(invocations.End(completed));
    require(completed.client == &identities[1]);
    require(!funrounds::native::ghost_radar::MatchesSendInvocation(
        completed.client, completed.replacementMessage, &identities[0], &identities[2]));

    require(invocations.End(completed));
    require(completed.client == &identities[0]);
    require(funrounds::native::ghost_radar::MatchesSendInvocation(
        completed.client, completed.replacementMessage, &identities[0], &identities[2]));
}

void invocation_overflow_skips_nested_frames_without_losing_outer_pairing() {
    using Stack = funrounds::native::ghost_radar::NestedInvocationStack<SendFrame, 1>;

    std::array<int, 3> identities{};
    Stack invocations;
    const auto outerIndex = invocations.Begin();
    require(outerIndex.has_value());
    auto* const outer = invocations.Get(*outerIndex);
    require(outer != nullptr);
    *outer = {1, true, &identities[0], &identities[1], &identities[2]};
    require(!invocations.Begin().has_value());
    require(!invocations.Begin().has_value());

    SendFrame completed;
    require(!invocations.End(completed));
    require(!invocations.End(completed));
    require(invocations.End(completed));
    require(completed.ownsReplacement && completed.client == &identities[0]);
}

void shutdown_cannot_close_pre_admission_while_a_nested_send_frame_is_active() {
    using Stack = funrounds::native::ghost_radar::NestedInvocationStack<SendFrame, 4>;
    using Gate = funrounds::native::ghost_radar::SendInvocationGate;

    std::array<int, 3> identities{};
    Gate gate;
    Stack invocations;
    require(gate.Open());
    require(gate.TryEnterRoot());

    const auto outerIndex = invocations.Begin();
    require(outerIndex.has_value());
    auto* const outer = invocations.Get(*outerIndex);
    require(outer != nullptr);
    *outer = {1, true, &identities[0], &identities[1], &identities[2]};

    require(!gate.TryCloseWhenIdle()); // shutdown must keep Pre installed while this send is active
    require(invocations.Begin().has_value()); // nested call still gets a balancing sentinel

    SendFrame completed;
    require(invocations.End(completed));
    require(!completed.ownsReplacement);
    require(invocations.End(completed));
    require(completed.id == 1 && completed.ownsReplacement);

    gate.LeaveRoot();
    require(gate.ActiveRoots() == 0);
    require(gate.TryCloseWhenIdle());
    require(gate.IsClosed());
    require(!gate.TryEnterRoot()); // post-only calls after close cannot consume an old frame
    require(!invocations.End(completed));
}

} // namespace

int main() {
    target_set_preserves_full_identity_and_merges_only_duplicate_handles();
    recipient_identity_and_64_bit_masks_are_exact();
    passive_sound_filter_requires_known_event_enemy_mask_and_exact_hidden_handle();
    bot_noise_filter_only_suppresses_exact_hidden_enemy_footsteps();
    filtering_removes_only_selected_target_entries_and_keeps_order_and_payload();
    global_spotted_is_converted_to_allowed_recipients_and_enemy_bits_are_removed();
    spotted_by_mask_changes_identify_each_fixed_array_word();
    nested_unfiltered_send_does_not_release_an_outer_filtered_message();
    nested_sends_with_identical_client_and_message_addresses_pop_lifo();
    mismatched_post_arguments_do_not_select_or_release_an_outer_frame();
    invocation_overflow_skips_nested_frames_without_losing_outer_pairing();
    shutdown_cannot_close_pre_admission_while_a_nested_send_frame_is_active();
    return 0;
}
