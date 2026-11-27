# Entity damage minimal gate

This test-only Plugify consumer checks the optional non-player
`TakeDamageOld` callback in `imomoe_native_base` 0.6.0. It does not change the
product plugin or gameplay. The native provider exposes exact victim, attacker,
inflictor and ability handles, damage and damage type; the old player callback
and its contract are unchanged.

The diagnostic owns only the chickens it creates. It blocks burn (`0x8`) and
blast (`0x40`) damage to those exact current chicken handles, including calls
with `skipPluginRules=1`. It observes without blocking the six unowned grenade
weapon entities `weapon_hegrenade`, `weapon_flashbang`, `weapon_smokegrenade`,
`weapon_molotov`, `weapon_incgrenade`, and `weapon_decoy`. A ground-grenade
observation includes the exact current victim and owner handles; player-owned
grenades and stale handles are ignored. Other entities and all unrelated
damage pass through.

The callback records `OnPluginUpdate`'s thread ID and compares each native
callback against it. Every SDK access is refused off that thread. `status`,
`trace`, and each callback report exact handles and source classnames. Before
unloading or reloading, run `remove-chickens` until it reports
`cleanup=complete`, then run `stop` and require
`detached=1 damage_hooked=0 entity_hook_cleanup_pending=0`; `status` must also
report `chicken_cleanup_pending=0` and `owned_chickens=0`. `OnPluginEnd` attempts
command-listener removal independently of hook or chicken cleanup. Plugify may
still unload the DLL after an `OnPluginEnd` error, so if cleanup is incomplete,
stop the isolated server before unloading or reloading the plugin.

## Read the engine warmup flag

Use the server console to read the current `CCSGameRules` warmup state without
changing it or installing a hook:

```text
fr_entity_damage_minimal rules
```

The command reports whether the game-rules pointer is non-null, the schema
offset for `m_bWarmupPeriod`, and its raw `GetEntSchema2` value using element
`0`. A missing import, null rules object, or missing schema is reported as
unavailable rather than interpreted as `false`. Run it before and after
`mp_warmup_end` and `mp_restartgame 1` to distinguish an active engine warmup
from an unavailable warmup field.

## Build both platform binaries

Use generated imports paired with the same S2SDK 3.0.2 and native 0.6.0
manifests as the diagnostic server. Keep these outputs outside the product
package.

Windows x64 with MSVC:

```powershell
cmake -S tests/entity_damage_minimal `
  -B .work/entity-damage-minimal/build-windows-r1 `
  -G "Visual Studio 18 2026" -A x64 `
  -DFUNROUNDS_PLUGIFY_INCLUDE_DIR="$PWD/.work/install-layout-basic-modes/sdk-source/external/plugify/include" `
  -DFUNROUNDS_S2SDK_GENERATED_INCLUDE_DIR="$PWD/.work/install-layout-basic-modes/build-windows-r2/generated-s2sdk/external/plugify/include" `
  -DFUNROUNDS_NATIVE_GENERATED_INCLUDE_DIR="$PWD/.work/entity-damage-minimal/generated-native/external/plugify/include"
cmake --build .work/entity-damage-minimal/build-windows-r1 --config Release --target entity_damage_minimal
```

Linux x64 must use the fixed Steam Runtime Sniper GCC 14 image, not the host
compiler. From Ubuntu WSL, with the repository mounted at `/workspace`:

```bash
docker run --rm --network none --user "$(id -u):$(id -g)" --env HOME=/tmp \
  --volume "$PWD":/workspace --workdir /workspace \
  funrounds-steamrt-gcc14:sniper-20260928-gcc14.2.0 \
  cmake -S tests/entity_damage_minimal \
    -B .work/entity-damage-minimal/build-linux-sniper-r1 -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=/usr/bin/g++-14 \
    -DFUNROUNDS_PLUGIFY_INCLUDE_DIR=/workspace/.work/install-layout-basic-modes/sdk-source/external/plugify/include \
    -DFUNROUNDS_S2SDK_GENERATED_INCLUDE_DIR=/workspace/.work/install-layout-basic-modes/build-linux/generated-s2sdk/external/plugify/include \
    -DFUNROUNDS_NATIVE_GENERATED_INCLUDE_DIR=/workspace/.work/entity-damage-minimal/generated-native/external/plugify/include
docker run --rm --network none --user "$(id -u):$(id -g)" --env HOME=/tmp \
  --volume "$PWD":/workspace --workdir /workspace \
  funrounds-steamrt-gcc14:sniper-20260928-gcc14.2.0 \
  cmake --build .work/entity-damage-minimal/build-linux-sniper-r1 \
    --target entity_damage_minimal --parallel 4
```

Expected binaries are `bin/Release/entity-damage-minimal.dll` and
`bin/libentity-damage-minimal.so`; the copied `driver.pplugin` sits beside each.
Load the matching binary, manifest and 0.6.0 native provider only on the
isolated diagnostic server. This gate establishes compile-time/loader behavior;
human damage and ground-drop observations remain runtime evidence.

## Exercise on the diagnostic server

Use the server console:

```text
fr_entity_damage_minimal selftest <live-client-slot>
```

This single no-real-damage command creates one owned chicken and installs the
entity hook. A live client slot (a BOT is fine) supplies the attacker and spawn
origin; no client needs to shoot or otherwise act. It calls the existing
`TakeEntityDamageWithWeapon` seam for burn and blast first, requiring both to
be suppressed with `skipPluginRules=1` while the exact chicken handle stays
live at unchanged health. It then sends generic damage last and requires the
unblocked call to kill that test chicken. Each case logs
`source=synthetic_native_call`, `actualhealthlost`, before/after health,
`live`, `dead`, and the exact `fullhandle`. The summary reports
`damage_cases=pass` separately from cleanup: `RemoveEntity` can return before
the entity handle disappears, so an immediate `cleanup=retry_required` with
`chicken_cleanup_pending=1` is an outstanding recovery step, not a failed
damage case or permission to unload. The command detaches the damage hook;
require `damage_hook_detached=1` and `entity_hook_cleanup_pending=0`, then wait
for a server update and run `remove-chickens` again until it says
`cleanup=complete`. Finish with `status` reporting
`chicken_cleanup_pending=0`, `entity_hook_cleanup_pending=0`, and
`owned_chickens=0` before unloading or reloading. Require
`burn_blocked_skip1=1`, `blast_blocked_skip1=1`, and `generic_allowed=1` in the
case summary. This proves the observer's synthetic callback path; it does not
establish engine-originated fire/blast behavior or grounded grenade impacts.

## Real six-class ground-drop acceptance

The existing `entity_interaction_minimal` observer owns the native grenade
event trace and `snapshot-drops` inventory; this observer adds the exact
damage callback handles and attacker. Start both on the isolated server, use
the existing mode/config path to apply `shoot_dropped_grenades=true`, and keep
the `mp_shoot_dropped_grenades` previous value for the normal owned-ConVar
restore check. This live gate must be separate from `selftest`: synthetic
`TakeEntityDamageWithWeapon` calls and BOT evidence do not count as a human
G-drop hit.

Start and clear both observers before the six trials:

```text
fr_entity_interaction_minimal start
fr_entity_damage_minimal start
fr_entity_interaction_minimal trace clear
fr_entity_damage_minimal trace clear
```

For each of the six classes, run one isolated item trial:

1. One human can run the six effect/consumption trials: drop one grenade with
   the normal G-key action, let it settle, then shoot it. Start/clear both
   observers' traces and record the shooter's live pawn handle with
   `fr_entity_interaction_minimal snapshot-player <shooter-slot>`.
   If a second human is available, have A drop and B shoot to distinguish
   original dropper from shooter at runtime; otherwise retain the source-based
   attribution finding below separately from the same-human observation.
2. Run `fr_entity_interaction_minimal snapshot-drops`. Continue only when the
   intended `weapon_*` classname appears as a single `unowned_candidate=1`
   with its full handle recorded; if `player_owned=1`, this fixture does not
   classify it as a dropped candidate. Keep only one unowned item of that
   class in the scene.
3. The shooter fires one ordinary gun shot at that exact visible dropped item. Do not
   use a grenade projectile or synthetic damage. Immediately capture both
   `fr_entity_damage_minimal trace` and `fr_entity_interaction_minimal trace`,
   wait one server update for deferred entity removal, then run
   `snapshot-drops` again and
   `fr_entity_damage_minimal inspect-handle <recorded-full-handle>`.
4. The Native18 observation must report `target=ground_grenade`, the exact
   snapshotted `victim` full handle and matching `victim_class`, its pre-hit
   `victim_owner`, `blocked=0`, and `attacker` equal to the shooter's recorded live pawn
   handle. Its callback thread ID must equal `main_thread_id`.
5. The event trace must contain the class's native effect and identify the shooter via
   `userid_slot`. Require one matching detonation event for the exact hit
   trial; `decoy_firing` may pulse after the single `decoy_detonate`. The
   exact-handle check must report `exact_full_handle=0` for the original
   dropped weapon, and its full handle must be absent from the after-shot
   snapshot. This proves the original weapon was consumed instead of merely
   changing to a player-owned candidate; it pairs a unique pre-hit handle with
   the native effect without pretending the later projectile/event ID is the
   dropped weapon's ID.

Use these class/effect pairs, with a fresh G-drop for each row:

| Dropped classname | Required native result |
| --- | --- |
| `weapon_hegrenade` | `hegrenade_detonate` and visible HE blast |
| `weapon_flashbang` | `flashbang_detonate` and visible flash effect |
| `weapon_smokegrenade` | `smokegrenade_detonate` and visible smoke cloud |
| `weapon_molotov` | `molotov_detonate` plus the native inferno/fire path |
| `weapon_incgrenade` | `molotov_detonate` plus the native inferno/fire path |
| `weapon_decoy` | `decoy_detonate`; subsequent `decoy_firing` pulses are allowed |

Molotov and incendiary share the public detonation event, so that event by
itself cannot distinguish the pair. Keep their one-class-per-trial dropped
classname and capture the visible fire result; if a projectile snapshot is
available before it disappears, record `molotov_projectile` versus
`incendiarygrenade_projectile`. Never count the same event string as proof of
both classes in one trial.

Ownership is the thrower/shooter attribution used by the old SourceMod
G-drop path: its `OnGrenadeTakeDamage` passes `attacker` to
`CreateGrenadeExplosion`, which sets the emitted projectile's `m_hThrower` and
team from that client (`funrounds.sp` damage hook and `funrounds/frarms.inc`
`CreateGrenadeExplosion`). The native ConVar branch similarly resolves
`m_hAttacker` and passes that pawn into the class-specific grenade emitter.
For a distinct-player trial, B—not A—is the expected projectile thrower. The callback's exact
attacker handle plus the event's `userid_slot` should both identify the shooter; the
original dropped item is consumed once.

The six user shots and visible effect readbacks are the real gameplay gate.
After capturing all six rows, if this session also created owned chickens,
run `fr_entity_damage_minimal remove-chickens` until `cleanup=complete`. Then
run `fr_entity_interaction_minimal stop` and
`fr_entity_damage_minimal stop`; require `detached=1 damage_hooked=0
entity_hook_cleanup_pending=0` before any plugin unload/reload. Once both
observers are detached, close the controlled mode and verify the owned-ConVar
restore behavior. If cleanup is incomplete, stop the isolated server first;
an `OnPluginEnd` error is not an unload veto.
