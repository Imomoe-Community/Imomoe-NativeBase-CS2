// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <source_location>

#include "button_state.hpp"
#include "chicken_use_abi.hpp"
#include "chicken_follow_policy.hpp"
#include "damage_call_scope.hpp"
#include "damage_frame_stack.hpp"
#include "entity_handle.hpp"
#include "ground_reverse_policy.hpp"
#include "linux_image.hpp"

namespace {

int first_callback() { return 1; }
int second_callback() { return 2; }
void entity_callback() {}

enum class SimdArgument : std::uint8_t {
    Xmm0,
    Xmm1,
    Xmm2,
};

void require(bool condition, const std::source_location& location = std::source_location::current());

void linux_image_requires_a_readable_load_segment() {
    using funrounds::native::detail::ContainsReadableImageRange;

    struct ProgramHeader {
        std::uint32_t p_type;
        std::uint32_t p_flags;
        std::uint64_t p_vaddr;
        std::uint64_t p_memsz;
    };

    constexpr ProgramHeader headers[]{
        {4, 4, 0x0000, 0x0080},
        {1, 5, 0x1000, 0x1000},
        {1, 6, 0x3000, 0x1000},
        {1, 1, 0x4000, 0x1000},
    };
    constexpr auto count = sizeof(headers) / sizeof(headers[0]);
    require(ContainsReadableImageRange(headers, count, 0x1000, 8));
    require(ContainsReadableImageRange(headers, count, 0x3ff8, 8));
    require(!ContainsReadableImageRange(headers, count, 0x0, 8));
    require(!ContainsReadableImageRange(headers, count, 0x2000, 8));
    require(!ContainsReadableImageRange(headers, count, 0x3ff9, 8));
    require(!ContainsReadableImageRange(headers, count, 0x4000, 8));
}

void ground_control_requires_current_controller_identity() {
    using funrounds::native::detail::ClassifyGroundControlSource;
    using funrounds::native::detail::GroundControlIdentity;
    using funrounds::native::detail::GroundControlSource;

    int pawn = 0;
    int otherPawn = 0;
    const GroundControlIdentity human{
        .pawnHandle = 17,
        .currentControllerHandle = 21,
        .slotControllerHandle = 21,
        .pawn = &pawn,
        .slotPawn = &pawn,
        .exactSlotMatches = 1,
        .clientInGame = true,
        .fakeClient = false,
    };
    require(ClassifyGroundControlSource(human) == GroundControlSource::Human);

    auto bot = human;
    bot.currentControllerHandle = 22;
    bot.slotControllerHandle = 22;
    bot.fakeClient = true;
    require(ClassifyGroundControlSource(bot) == GroundControlSource::Bot);

    auto staleTakeover = bot;
    staleTakeover.currentControllerHandle = 22;
    staleTakeover.slotControllerHandle = 21;
    require(ClassifyGroundControlSource(staleTakeover) == GroundControlSource::Unresolved);

    auto reusedController = human;
    reusedController.slotControllerHandle = 23;
    require(ClassifyGroundControlSource(reusedController) == GroundControlSource::Unresolved);

    auto reusedPawn = human;
    reusedPawn.slotPawn = &otherPawn;
    require(ClassifyGroundControlSource(reusedPawn) == GroundControlSource::Unresolved);

    auto duplicate = human;
    duplicate.exactSlotMatches = 2;
    require(ClassifyGroundControlSource(duplicate) == GroundControlSource::Unresolved);

    auto disconnected = human;
    disconnected.clientInGame = false;
    require(ClassifyGroundControlSource(disconnected) == GroundControlSource::Unresolved);
}

void ground_reverse_changes_only_horizontal_human_wish() {
    float direction[3]{1.0F, -2.0F, 3.0F};
    funrounds::native::detail::ReverseHorizontalWish(direction);
    require(direction[0] == -1.0F);
    require(direction[1] == 2.0F);
    require(direction[2] == 3.0F);
}

void ground_reverse_sets_the_acceleration_argument_for_the_platform_abi() {
    float arguments[3]{1.0F, 320.0F, 0.0F};
    funrounds::native::detail::SetGroundReverseAccelerationRegister(
        SimdArgument::Xmm0,
        SimdArgument::Xmm1,
        SimdArgument::Xmm2,
        [&arguments](SimdArgument argument, float value) {
            switch (argument) {
            case SimdArgument::Xmm0:
                arguments[0] = value;
                break;
            case SimdArgument::Xmm1:
                arguments[1] = value;
                break;
            case SimdArgument::Xmm2:
                arguments[2] = value;
                break;
            }
        },
        5.5F);

#if defined(__linux__)
    require(arguments[0] == 1.0F);
    require(arguments[1] == 320.0F);
    require(arguments[2] == 5.5F);
#else
    require(arguments[0] == 5.5F);
    require(arguments[1] == 320.0F);
    require(arguments[2] == 0.0F);
#endif
}

void ground_reverse_requires_positive_finite_acceleration() {
    using funrounds::native::detail::IsValidGroundReverseAcceleration;
    require(IsValidGroundReverseAcceleration(5.5F));
    require(IsValidGroundReverseAcceleration(0.001F));
    require(!IsValidGroundReverseAcceleration(0.0F));
    require(!IsValidGroundReverseAcceleration(-5.5F));
    require(!IsValidGroundReverseAcceleration(std::numeric_limits<float>::infinity()));
    require(!IsValidGroundReverseAcceleration(std::numeric_limits<float>::quiet_NaN()));
}

void use_button_release_transition_is_not_currently_down() {
    using funrounds::native::detail::ButtonStateGroup;
    using funrounds::native::detail::IsButtonCurrentlyDown;

    constexpr std::uint64_t kInUse = UINT64_C(1) << 5;
    constexpr std::int32_t currentDown = static_cast<std::int32_t>(ButtonStateGroup::CurrentDown);
    constexpr std::uint64_t held[3]{kInUse, 0, 0};
    constexpr std::uint64_t released[3]{0, kInUse, 0};
    constexpr std::uint64_t pressed[3]{kInUse, kInUse, 0};
    constexpr std::uint64_t scrollOnly[3]{0, 0, kInUse};

    static_assert(currentDown == 0);
    static_assert(static_cast<std::int32_t>(ButtonStateGroup::Changed) == 1);
    require(IsButtonCurrentlyDown(held[currentDown], kInUse));
    require(!IsButtonCurrentlyDown(released[currentDown], kInUse));
    require(IsButtonCurrentlyDown(pressed[currentDown], kInUse));
    require(!IsButtonCurrentlyDown(scrollOnly[currentDown], kInUse));
}

void chicken_follow_block_requires_active_exact_chicken() {
    using funrounds::native::detail::ShouldBlockChickenFollow;
    require(!ShouldBlockChickenFollow(false, false));
    require(!ShouldBlockChickenFollow(false, true));
    require(!ShouldBlockChickenFollow(true, false));
    require(ShouldBlockChickenFollow(true, true));
}

void* g_chickenUseEntity{};
const funrounds::native::detail::abi::EntityUseInput_t* g_chickenUseInput{};
std::uint32_t g_chickenUseCalls{};

void CaptureChickenUse(void* chicken,
                       const funrounds::native::detail::abi::EntityUseInput_t* input) {
    g_chickenUseEntity = chicken;
    g_chickenUseInput = input;
    ++g_chickenUseCalls;
}

void chicken_use_abi_preserves_both_arguments_when_unblocked_and_after_disable() {
    using funrounds::native::detail::abi::CChickenChickenUse;
    using funrounds::native::detail::abi::EntityUseInput_t;
    using funrounds::native::detail::abi::UseType;
    using funrounds::native::detail::ShouldBlockChickenFollow;
    static_assert(std::is_same_v<CChickenChickenUse, void (*)(void*, const EntityUseInput_t*)>);

    int chicken{};
    int activator{};
    int caller{};
    const EntityUseInput_t input{&activator, &caller, UseType::Toggle, 0.0F};
    const CChickenChickenUse use = CaptureChickenUse;
    const auto invoke = [&](bool blockingActive) {
        if (!ShouldBlockChickenFollow(blockingActive, true)) {
            use(&chicken, &input);
        }
    };

    invoke(false);
    require(g_chickenUseCalls == 1);
    require(g_chickenUseEntity == &chicken);
    require(g_chickenUseInput == &input);
    require(g_chickenUseInput->m_pActivator == &activator);
    require(g_chickenUseInput->m_pCaller == &caller);
    require(g_chickenUseInput->m_nType == UseType::Toggle);

    // An active native follow blocker must suppress the callback without
    // rewriting either argument or invoking the original use function.
    invoke(true);
    require(g_chickenUseCalls == 1);

    // Disabling the blocker restores the same original two-argument call.
    invoke(false);
    require(g_chickenUseCalls == 2);
    require(g_chickenUseEntity == &chicken);
    require(g_chickenUseInput == &input);
    require(g_chickenUseInput->m_pActivator == &activator);
    require(g_chickenUseInput->m_pCaller == &caller);
    require(g_chickenUseInput->m_nType == UseType::Toggle);
}

void require(bool condition, const std::source_location& location) {
    if (!condition) {
        std::cerr << "native boundary check failed at line " << location.line() << '\n';
        std::abort();
    }
}

void handle_bits_round_trip() {
    constexpr std::uint32_t raw = 0xf1234567U;
    constexpr std::int32_t api = funrounds::native::ToApiHandle(raw);
    static_assert(funrounds::native::ToNativeHandle(api) == raw);
    require(funrounds::native::MatchesHandle(raw, api));
    require(funrounds::native::ToNativeHandle(-1) == 0xffffffffU);
    require(funrounds::native::ToApiHandle(0xffffffffU) == -1);
}

void scope_is_exact_and_nested() {
    const int outerInfo = 0;
    const int innerInfo = 0;
    using Callback = int (*)();
    using Scope = funrounds::native::DamageCallScope<Callback>;

    require(!Scope::SkipRules(&outerInfo, first_callback));
    {
        Scope outer(&outerInfo, first_callback, true);
        require(Scope::SkipRules(&outerInfo, first_callback));
        require(!Scope::SkipRules(&outerInfo, second_callback));
        {
            Scope inner(&innerInfo, first_callback, false);
            require(!Scope::SkipRules(&innerInfo, first_callback));
            require(Scope::SkipRules(&outerInfo, first_callback));
        }
        require(Scope::SkipRules(&outerInfo, first_callback));
    }
    require(!Scope::SkipRules(&outerInfo, first_callback));
}

void player_and_entity_scopes_are_independent() {
    const int info = 0;
    using PlayerCallback = int (*)();
    using EntityCallback = void (*)();
    using PlayerScope = funrounds::native::DamageCallScope<PlayerCallback>;
    using EntityScope = funrounds::native::DamageCallScope<EntityCallback>;

    PlayerScope player(&info, first_callback, true);
    EntityScope entity(&info, entity_callback, true);
    require(PlayerScope::SkipRules(&info, first_callback));
    require(EntityScope::SkipRules(&info, entity_callback));
    {
        EntityScope nestedEntity(&info, entity_callback, false);
        require(PlayerScope::SkipRules(&info, first_callback));
        require(!EntityScope::SkipRules(&info, entity_callback));
    }
    require(PlayerScope::SkipRules(&info, first_callback));
    require(EntityScope::SkipRules(&info, entity_callback));
}

void frame_tokens_nest_and_unwind() {
    using funrounds::native::DamageFrameScope;
    using funrounds::native::DamageFrameStack;

    DamageFrameStack::SetEnabled(true);
    require(DamageFrameStack::ActiveCalls() == 0);
    std::uint64_t outerToken = 0;
    {
        DamageFrameScope outer;
        outerToken = outer.Token();
        require(outerToken != 0);
        require(DamageFrameStack::Current() == outerToken);
        require(DamageFrameStack::DirectFirearmContactGroup() == -1);
        require(!DamageFrameStack::HasDirectKnifeContact());
        DamageFrameStack::MarkDirectKnifeContact();
        require(DamageFrameStack::HasDirectKnifeContact());
        DamageFrameStack::SetDirectFirearmContactGroup(0);
        DamageFrameStack::SetDirectFirearmRay(129.0f, -1.0f);
        require(DamageFrameStack::DirectFirearmContactGroup() == 0);
        {
            DamageFrameScope inner;
            require(inner.Token() != 0 && inner.Token() != outerToken);
            require(DamageFrameStack::Current() == inner.Token());
            require(DamageFrameStack::DirectFirearmContactGroup() == -1);
            require(!DamageFrameStack::HasDirectKnifeContact());
            require((DamageFrameStack::DirectFirearmRay() == std::array<float, 2>{}));
            DamageFrameStack::SetDirectFirearmContactGroup(2);
            require(DamageFrameStack::DirectFirearmContactGroup() == 2);
        }
        require(DamageFrameStack::Current() == outerToken);
        require(DamageFrameStack::DirectFirearmContactGroup() == 0);
        require((DamageFrameStack::DirectFirearmRay() == std::array<float, 2>{129.0f, -1.0f}));
    }
    require(DamageFrameStack::DirectFirearmContactGroup() == -1);
    require(!DamageFrameStack::HasDirectKnifeContact());
    require((DamageFrameStack::DirectFirearmRay() == std::array<float, 2>{}));
    require(DamageFrameStack::Current() == 0);
    require(DamageFrameStack::ActiveCalls() == 0);

    DamageFrameStack::SetEnabled(false);
    {
        DamageFrameScope untracked;
        require(untracked.Token() == 0);
        require(DamageFrameStack::Current() == 0);
    }
    require(DamageFrameStack::ActiveCalls() == 0);
}

} // namespace

int main() {
    handle_bits_round_trip();
    linux_image_requires_a_readable_load_segment();
    ground_control_requires_current_controller_identity();
    ground_reverse_changes_only_horizontal_human_wish();
    ground_reverse_sets_the_acceleration_argument_for_the_platform_abi();
    ground_reverse_requires_positive_finite_acceleration();
    use_button_release_transition_is_not_currently_down();
    chicken_follow_block_requires_active_exact_chicken();
    chicken_use_abi_preserves_both_arguments_when_unblocked_and_after_disable();
    scope_is_exact_and_nested();
    player_and_entity_scopes_are_independent();
    frame_tokens_nest_and_unwind();
}
