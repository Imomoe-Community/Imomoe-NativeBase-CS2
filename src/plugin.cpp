// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <plg/plugin.hpp>

#include <plugin_export.h>

#include "airmove.hpp"
#include "burn.hpp"
#include "c4_access.hpp"
#include "chrono_attack.hpp"
#include "target_attack.hpp"
#include "collision.hpp"
#include "damage.hpp"
#include "engine.hpp"
#include "fire_rain.hpp"
#include "grenade_factory.hpp"
#include "bighead.hpp"
#include "ghost_radar.hpp"
#include "ghost_visibility.hpp"
#include "ground_reverse.hpp"
#include "interactions.hpp"
#include "movement_trace.hpp"
#include "pose_move.hpp"
#include "player_movement_policy.hpp"
#include "recoil_impulse.hpp"
#include "reload_sync.hpp"
#include "suicide.hpp"
#include "standard_grenade.hpp"
#include "touch.hpp"
#include "third_person.hpp"
#include "world_item.hpp"

#if defined(FUNROUNDS_NATIVE_LIFECYCLE_PROBE)
#include "lifecycle_probe.hpp"
#endif

namespace {

class NativeBasePlugin final : public plg::Plugin {
public:
    plg::PluginResult OnPluginStart() override {
#if defined(FUNROUNDS_NATIVE_LIFECYCLE_PROBE)
        try {
            const auto fail = [](const char* message) {
                (void)funrounds::native::detail::ShutdownDamage();
                (void)funrounds::native::detail::ShutdownSuicide();
                (void)funrounds::native::detail::ShutdownTouch();
                (void)funrounds::native::detail::ShutdownInteractions();
                (void)funrounds::native::detail::ShutdownThirdPersonEye();
                (void)funrounds::native::detail::StopLifecycleProbe();
                return plg::PluginResult{message};
            };
            if (!funrounds::native::detail::StartLifecycleProbe()) {
                return fail(funrounds::native::detail::LifecycleProbeFailure());
            }
            if (!funrounds::native::detail::ProbeChickenFollowSharedDetour()) {
                return fail("Native shared ChickenUse detour probe failed");
            }
            if (!funrounds::native::detail::ProbeThirdPersonKnownHookRecord()) {
                return fail("Native third-person known detour record probe failed");
            }
            if (!funrounds::native::detail::ProbeNativeDamagePaths(17, 21, 18, 19)) {
                return fail("Native damage path probe failed");
            }
            if (!funrounds::native::detail::ProbeDamageRegistrationRollback()) {
                return fail("Native damage registration rollback probe failed");
            }
            if (!funrounds::native::detail::ProbeDamageDetachFailureRetry()) {
                return fail("Native damage detach retry probe failed");
            }
            if (!funrounds::native::detail::ProbeTouchUnhookContract(17)) {
                return fail(funrounds::native::detail::TouchUnhookProbeFailure());
            }
            return {};
        } catch (...) {
            (void)funrounds::native::detail::ShutdownDamage();
            (void)funrounds::native::detail::ShutdownSuicide();
            (void)funrounds::native::detail::ShutdownTouch();
            (void)funrounds::native::detail::ShutdownInteractions();
            (void)funrounds::native::detail::ShutdownThirdPersonEye();
            (void)funrounds::native::detail::StopLifecycleProbe();
            return plg::PluginResult{"Native lifecycle probe threw"};
        }
#else
        return {};
#endif
    }

    plg::PluginResult OnPluginEnd() override {
        try {
            const bool damageClean = funrounds::native::detail::ShutdownDamage();
            const bool attackClean = funrounds::native::detail::ShutdownTargetAttack();
            const bool commandClean = funrounds::native::detail::ShutdownNativeWeaponCommand();
            const bool chronoClean = funrounds::native::detail::ShutdownChronoAttack();
            const bool c4Clean = funrounds::native::detail::ShutdownC4Access();
            const bool bigHeadClean = funrounds::native::detail::ShutdownBigHead();
            const bool suicideClean = funrounds::native::detail::ShutdownSuicide();
            const bool recoilImpulseClean = funrounds::native::detail::ShutdownRecoilImpulse();
            const bool reloadSyncClean = funrounds::native::detail::ShutdownReloadSync();
            funrounds::native::detail::ResetGrenadeFactories();
            const bool fireRainClean = funrounds::native::detail::ShutdownMolotovRain();
            const bool standardGrenadesClean = funrounds::native::detail::ShutdownStandardGrenades();
            const bool touchClean = funrounds::native::detail::ShutdownTouch();
            const bool airMoveClean = funrounds::native::detail::ShutdownAirMove();
            const bool poseMoveClean = funrounds::native::detail::ShutdownPoseMove();
            const bool playerPoliciesClean = funrounds::native::detail::ShutdownPlayerMovementPolicies();
            const bool groundReverseClean = funrounds::native::detail::ShutdownGroundReverse();
            const bool ghostRadarClean = funrounds::native::detail::ShutdownGhostRadar();
            const bool ghostVisibilityClean = funrounds::native::detail::ShutdownGhostVisibility();
            const bool interactionsClean = funrounds::native::detail::ShutdownInteractions();
            const bool thirdPersonClean = funrounds::native::detail::ShutdownThirdPersonEye();
            const bool worldItemClean = funrounds::native::detail::ShutdownWorldItem();
#if defined(FUNROUNDS_NATIVE_LIFECYCLE_PROBE)
            const bool probeClean = funrounds::native::detail::StopLifecycleProbe();
#else
            constexpr bool probeClean = true;
#endif
            if (!damageClean || !attackClean || !commandClean || !chronoClean || !c4Clean || !bigHeadClean || !suicideClean || !recoilImpulseClean || !reloadSyncClean || !fireRainClean || !standardGrenadesClean || !touchClean || !airMoveClean || !groundReverseClean || !ghostRadarClean ||
                !ghostVisibilityClean || !poseMoveClean || !playerPoliciesClean ||
                !interactionsClean || !thirdPersonClean || !worldItemClean || !probeClean) {
                return plg::PluginResult{"Native hook cleanup could not be verified"};
            }
            funrounds::native::detail::ResetBurnMetadata();
            funrounds::native::detail::ResetRecoilImpulseMetadata();
            funrounds::native::detail::ResetCollisionSchema();
            funrounds::native::detail::ResetMovementTraceMetadata();
            funrounds::native::detail::ShutdownEngine();
            return {};
        } catch (...) {
            return plg::PluginResult{"Native hook cleanup threw"};
        }
    }
};

NativeBasePlugin g_plugin;

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_plugin)
