# BOT ghost-visibility minimal

This isolated, test-only Plugify plugin gates one explicitly selected live
player pawn at the actual `CCSBot::IsVisible(player)` entry point. It resolves
the receiver's `CBot::m_pPlayer` to a current pawn and controller, then requires
that exact current controller to be a fake client. The target is likewise
revalidated by slot, controller handle, full pawn handle, and pointer on every
call. The plugin clears `visibleParts` when supplied and supersedes only the
selected player scan with `false`.

It never hooks `CCSBot::IsVisible(pos)`, never writes `FL_NOTARGET`, render
alpha, shadow, or player state, and never changes hearing, damage, attacks, or
last-known tracking. Those are separate behavior boundaries to inspect in a
live BOT scenario; hook counters alone do not prove BOT perception.

## Build and stage

Build against the paired selected SDK and PolyHook headers from the acceptance
tree. The resulting DLL, manifest, and gamedata must be staged together under
one temporary Plugify environment named `ghost_visibility_minimal`:

- `bin/ghost-visibility-minimal.dll` on Windows;
- `bin/libghost-visibility-minimal.so` on Linux;
- `driver.pplugin`;
- `gamedata/ghost-visibility-minimal.jsonc`.

Add this test environment only to the isolated test server's existing dependency
overlay. Do not package it with FunRounds or load the production gameplay plugin
as its substitute. On Windows use isolated server 27046; on Linux use 27045.
Do not use 27015. Preserve the preexisting test server install and restore the
overlay after the server has exited.

## r3 thread diagnostic

r3 adds a test-only paired S2SDK manifest containing the existing
`OnGameFrame_Register` and `OnGameFrame_Unregister` exports. The plugin records
native thread IDs for the console-command callback, Plugify `OnPluginUpdate`,
S2SDK `GameFrame`, and `CCSBot::IsVisible`; `status` reports them as
`command_tid`, `update_tid`, `gameframe_tid`, and `isvisible_tid`, alongside
call counts and `off_thread_calls`. The status fields and counters are atomic.
The full test-only provider manifest is staged at
`.work/ghost-visibility-minimal/r3/stage-r3/provider/s2sdk.pplugin` for use with
the unchanged paired provider binary; it does not replace formal bindings or
the product package.

`status` is read-only and may run on any thread. Hook control commands are
accepted only when the command callback is on the recorded GameFrame thread.
`cvar` is also main-thread-only; it reads `sv_bot_parallel_threat_detection`
through the public S2SDK `FindConVar`/`GetConVarValue` APIs. A missing public
API import or a handle whose access index is `0xFFFF` is reported as
unavailable. Access index `0` is valid. It never changes the ConVar.
`reload_schema <slot>` is main-thread-only and read-only. It reports the live
`GetSchemaOffset` results for `CBasePlayerWeapon.m_bInReload` and
`CCSWeaponBase.m_bInReload`, then reports the value read from the selected
player's current owned weapon through `GetEntSchema2` when either class exposes
the field, along with whether `IN_RELOAD` is currently down. Run it after
releasing R while the reload animation is still active to establish both the
schema scope and live value; it does not write weapon or player state.
`snapshot <slot>` is also main-thread-only and read-only. It reports the live
player identity, team, health/armor, buttons/flags, active weapon and owned
inventory, plus server render-mode/alpha readbacks for the pawn and owned
weapons. When the schema fields are available it also reports the pawn's
`CBaseEntity.m_hEffectEntity` full handle and classname; for an `entityflame`,
it reports the typed `CEntityFlame.m_hEntAttached` handle and whether it matches
that pawn. A missing schema field is `unavailable`; `-1` is reported only when
the readable effect field actually contains the empty-handle sentinel. The
fields are read only after validating the current pawn/effect handles, and no
pointer is printed. This observes attachment, not who owns or created the
flame; ownership needs before/after and Runtime evidence. It also lists world
entities with classname `chicken`; those handles are not tagged by owner.
Server readback does not show what a client actually renders.
The visibility callback records its TID and returns `Ignored` before any SDK or
entity access if no GameFrame thread has been recorded or if the IDs differ.
Atomic instrumentation does not make engine or entity access safe from another
thread. If the RCON callback is off-thread, record the refusal rather than
bypassing the guard with direct engine calls.

## Hostage.Breath sound minimal

The isolated `sound` command tests the pinned `Hostage.Breath` event on the
current map without installing the visibility detour. On `de_dust2`, use
`fr_ghost_visibility_minimal sound <live-slot> [distance]` from the server
console. The source must be a live player pawn. The default recipient cap is
1000 units; the optional distance selects the exact in-game recipient slots
using 3D distance from the source. The sound event itself fades to zero at
about 1100 units, so a larger cap does not extend audible range. The reply
records the source's full entity handle, cap, and recipient slots.

The diagnostic calls `EmitSoundToClient` with `Hostage.Breath`, volume 0.9,
and Source pitch 100, then calls `StopSound` on that same full-handle source
after 1.5 seconds. `sound_stop`, `stop`, `clear`, source death/disconnect, and
plugin shutdown also request a stop. `status` reports the source handle and
emit/stop counters. These counters prove only that the SDK calls were
requested; confirm that the event loads on `de_dust2` from server logs and
check the actual volume and direction in Mai's client window. Do not use
`Precache`'s S2SDK bookkeeping as evidence that the engine loaded this event.
The zero-human RCON validator requires normal playback to start with
`sv_cheats=0`, captures and restores `sv_hibernate_when_empty`, and temporarily
enables `sv_cheats` only for `bot_kill` so source-death cleanup can be observed.
It immediately verifies the original cheat value is restored.

Use the S2SDK descriptor pinned by `runtime/s2sdk.json`, including the official
GameFrame listener imports. While the production visibility hook is loaded,
use only the read-only `status`, `cvar`, `reload_schema`, and `snapshot`
queries.

## Historical standalone diagnostic BOT procedure

This earlier procedure describes the old independently installed visibility
detour only. It is not a product Ghost acceptance step and must not run in the
same process as the production native visibility hook.

Keep one non-target BOT as a native-vision control. Use an unobstructed map
position so ordinary LOS can be observed. From the server console:

1. `fr_ghost_visibility_minimal status` — confirm the plugin is loaded
   passively, `detached=true`, and the target is unset.
2. `fr_ghost_visibility_minimal cvar` — record the public SDK result and
   current value of `sv_bot_parallel_threat_detection` without changing it.
3. After Plugify reports the plugin `Running`, run
   `fr_ghost_visibility_minimal start`, then `status`; require
   `ready=true` and `detached=false` before setting a target.
4. With the target hidden from the BOT's prior sight and sound, run
   `fr_ghost_visibility_minimal target <slot>` before exposing the target.
   Observe a fresh BOT acquire the visible control but not the selected pawn.
   Confirm `target_calls` and `blocked` increment and `mask_cleared` increments
   whenever the engine supplied `visibleParts`.
5. To test existing tracking separately, first clear the gate, let the BOT
   visibly acquire and face the target, then set the target again. Observe
   whether it keeps pursuing its last-known location and for how long.
6. While the target remains gated, trigger footstep/breathing or firing sounds;
   separately damage the BOT from the target. Confirm hearing and actual damage
   still happen. These observations do not by themselves establish the hidden
   target was not also revealed by another path.
7. Run `fr_ghost_visibility_minimal clear` and confirm native visual
   acquisition resumes. Then run `fr_ghost_visibility_minimal stop`, followed
   by `status`; require `detached=true` and `gameconfig_open=false` before
   ending the diagnostic. The r3 GameFrame listener manager retains snapshots
   already taken by an in-flight dispatch; unregistering does not drain them.
   A stopped hook therefore does **not** authorize live plugin unload or
   `plg reload`. Keep the plugin loaded and stop the isolated server process
   normally after collecting the r3 observations. The runner may restore its
   saved overlay only after that process exits. This diagnostic does not test
   live plugin unload/reload.

`OnPluginStart` registers only the passive recovery command and GameFrame
thread recorder; it does not load gamedata or install a visibility hook. `start`
performs hook installation only after the plugin is already `Running`. If
installation cleanup cannot be confirmed, the plugin stays in its passive
`Running` state with the recovery command and owned hook/gamedata state
available to `stop`; never close gamedata while a detour may still be active.
Unregistering the r3 GameFrame listener does not prove that previously copied
dispatch snapshots are drained, and Plugify 3.1.0 unloads the plugin even when
`OnPluginEnd` reports cleanup failure. Keep r3 process-lifetime: do not live
unload or reload this diagnostic plugin.

Reset or replace a BOT between first-acquisition and remembered-target trials
so retained BOT memory does not contaminate the no-prior-contact case. A RCON
counter is call-site evidence only; Mai must visually accept the real BOT
behavior on the live client. This test has no client-rendering/alpha component.
