# Ghost product acceptance

Use this runbook only with a paired Windows or Linux candidate on the isolated
acceptance server: Windows `27046`, Linux `27045`. Do not use `27015`, mix
provider binaries across platforms, or treat this diagnostic as the product.
The R8 diagnostic is passive for this run. **Do not run `start`, `target`,
`clear`, or `stop` while the production `imomoe_native_base` Ghost hook is
resident.**

## Use the synthetic fixture, not preset 53

Install `tests/fixtures/configs/native-live/ghost-product-acceptance.toml` as
the isolated server's `funrounds.toml`. Its ID is a descriptive string, not 53.
The fixture intentionally keeps a global `[chicken]` configuration so the
per-mode `ordinary_chickens = false` path is exercised. For this mode it also
disables E-eating, the bomb/chicken-bomb path, C4 count and purchase, weapon
drops, and grenade drops.

The configured weapon list contains only primary-slot candidates. With
`random_loadout = false`, the existing per-slot selector chooses one primary
for each spawn. `give_weapons = "counter_terrorists"` limits that grant to CT;
`give_knife = "all"` keeps the requested T knife and the ordinary CT knife.
The fixture does not ban a CT knife. `ghost-restore-control` is a second
synthetic mode with no Ghost effect, so the same server can exercise a normal
mode transition and restoration.

This fixture does not become formal preset 53 or add to its pool. The formal
candidate remains separate until BOT and Mai's client acceptance are complete.

## Preflight: establish the paired product, without a visibility detour

1. Start the test candidate on `de_dust2` with one human on T and at least one
   CT BOT. Leave `sv_cheats = 0`.
2. Read `status`, `plg plugins`, `fr_status`, and `sv_cheats`. Require `s2sdk`,
   `imomoe_native_base`, and `funrounds` to be `Running`; `phase=Running
   current=ghost-capability-acceptance`; `effects_ready=true`;
   `recovery=Ready cleanup_pending=false`; and an empty `faults=` field.
3. If `ghost_visibility_minimal` is loaded for its read-only queries, run
   `fr_ghost_visibility_minimal status` and require `ready=0 detached=1
   gameconfig_open=0`.
4. If a BOT pose aid would reduce the number of human clients, query these
   names with no arguments and record the literal result on this server build:
   `bot_crouch`, `bot_walk`, `bot_mimic`, and `bot_ignore_enemies`. Use none of
   them to drive a target until both its support and its behavior have been
   verified. Existing `bot_stop`/`bot_dont_shoot` uses in other acceptance
   drivers are not proof of target-pose control.

Offline repository/source inspection found no `bot_crouch`, `bot_walk`, or
`bot_mimic` declaration. Existing harnesses set `bot_ignore_enemies` to quiet
BOT behavior, but do not establish that it can pose a target. These names are
runtime probes only, not an offline-supported BOT-control interface.

On 2026-10-26, the isolated Windows 27046 server on CS2
`1.41.9.2/14192` recognized all four names as ConVars: `bot_crouch=false`,
`bot_walk=false`, `bot_mimic=0`, and `bot_ignore_enemies=false`. This was a
read-only no-argument query against the paired R3 package with zero humans and
zero bots. It proves command availability only; no control value was changed
and no effect on an individual BOT's pose or behavior was verified. The raw
result is `local/sessions/ghost-bot-controls-windows-r1/ghost-bot-controls-results.json`.

A later zero-human pose probe used the R23 Rust product binary with the paired
S2SDK 3.0.4 provider/native package on the same Windows server. The passive
test-only diagnostic reported a live T BOT in slot 0 and CT BOT in slot 1;
`ghost-capability-acceptance` was Running with `effects_ready=true` and no
faults. The T BOT started grounded at body alpha 127. After `bot_crouch=1`, its
sampled buttons were `0x208` and flags `0x10091`: neither `IN_DUCK` (`0x4`) nor
`FL_DUCKING` (`0x2`) was set, and body alpha remained 127. This run did not
produce or prove a crouched target pose. With `bot_walk=1`, buttons were
`0x10008` (`IN_SPEED` plus forward) and body alpha became 13. After the
`bot_walk` readback returned to false, the next sample 800 ms later still had
`IN_SPEED` and alpha 13, so this run did not establish a standing transition.
The initial standing sample is the only standing-pose evidence from this probe.
The driver disabled Ghost cleanly (`recovery=Ready cleanup_pending=false`) and
restored its settings; the acceptance runner restored the server package and
configuration (`process.json`: `exitCode=0`, `configRestored=true`,
`restoreStatus=restored`). Results and raw readbacks are in
`local/sessions/ghost-bot-pose-windows-r4/ghost-bot-pose-results.json` and
`local/sessions/ghost-bot-pose-windows-r4/ghost-bot-pose-rcon.txt`; the
test-only diagnostic overlay records `restoreStatus=restored-absent` in
`local/sessions/ghost-bot-pose-windows-r4/ghost-diagnostic-overlay.json`.

These ConVar results are limited to the sampled BOT state; do not use them to
stand in for client-driven crouch/release or for actual BOT perception. The T
client must still drive and accept crouch, slow walk, and standing with the CT
BOT observing the product Ghost behavior.

After preflight, choose the live T-human and CT-BOT slots from `status`.
Record the following read-only queries in the session evidence directory:

```text
fr_ghost_visibility_minimal snapshot <slot>
fr_ghost_visibility_minimal reload_schema <T-slot>
```

To enter the Ghost fixture, restart the round and check the applied mode:

```text
fr_next ghost-capability-acceptance
mp_restartgame 1
fr_status
```

For a fresh CT observer, `bot_kick` followed by `bot_add_ct` replaces the BOT
roster; re-read `status` and use the new live CT slot. To compare restoration,
select the no-Ghost mode, restart the round, and read back the applied mode;
repeat with the Ghost mode to resume its trials:

```text
fr_next ghost-restore-control
mp_restartgame 1
fr_status
fr_next ghost-capability-acceptance
mp_restartgame 1
fr_status
```

The snapshot reports current team, player/pawn/controller handles, life state,
health/armor, buttons/flags, active weapon and owned inventory, and server
`m_nRenderMode`/`m_clrRender` readbacks for the body and owned weapons. Its
`world_chicken_entities` field lists every map entity with classname `chicken`;
it cannot distinguish map chickens from plugin-owned ordinary chickens.
Server alpha/readback does not establish what another client rendered.

## Confirm equipment and the no-chicken/drop composition

Read one T human snapshot and one CT BOT snapshot after the mode reaches
`Running`:

- CT has 100 armor, one of the four configured primary classnames, and its
  ordinary knife; no configured secondary, extra grenade, or C4 is granted.
- T has 100 armor and only the knife from this mode; there is no C4.
- `drop_weapons` and `drop_grenades` are explicitly false in the mode defaults.
- The fixture sets `ordinary_chickens=false` while global chicken spawning is
  configured. No plugin-owned ordinary chicken should be generated or bought
  in this mode. Map chickens and separate C4-chicken mechanics are not covered
  by that field.

Use the full live inventory in `snapshot`, not just the random-loadout request
log. Treat any class outside the stated inventory as a composition failure.

## BOT visual discovery and remembered tracking are separate trials

The product runtime owns the full Ghost target set. Never issue
`fr_ghost_visibility_native_minimal targets` in this product trial; that
consumer belongs only to the earlier provider-boundary check.

Use the `ghost-visual-perception` fixture for these windows: its package
configuration disables breathing with `breath_interval = 0.0`. This does not
disable footsteps or establish general silence. Geometry
preflight and ordinary lifecycle checks do not require visual quiet-scene
evidence. Full visual acceptance must measure each window independently: the
target stays grounded, within 2 units of position drift and 1 unit/second of
velocity, with stable speed scale and only idle or duck buttons; target attack,
footstep, or damage-to-observer events invalidate quietness. Hidden visual
windows also exclude CT fire and CT-to-target damage. The visible control still
requires a natural CT response at alpha 127. Run breathing, other sounds, and
incoming damage only in their own trials.

1. **Positive control:** let a CT BOT acquire the T human while the target is
   standing at alpha 127. Confirm ordinary visual attention before testing a
   hidden target.
2. **Fresh discovery:** verify an actual obstruction before the fresh CT's first
   sightline, then read back the same T pawn at alpha 0. Establish and verify a
   clear sightline while it remains hidden; compare alpha 0 and alpha 127 at the
   same CT/T geometry. It must not acquire the hidden pawn visually, then must
   respond to the same pawn once visible. A no-fire window without LOS evidence
   is inconclusive, not a pass.
3. **Remembered tracking:** in a separate visual-only window, let a CT acquire
   the visible T pawn, record the last visible location, then hide and move the
   same T pawn far enough to distinguish that location from its current one.
   Record whether the BOT pursues the old location or tracks the hidden pawn's
   current position; these are separate outcomes. A window that cannot
   distinguish the two positions does not close tracking acceptance.
4. **Sound:** repeat with a fresh BOT. First observe a quiet hidden baseline;
   then separately allow actual player footsteps, the product breathing pulse,
   and weapon fire. Record which sound makes the BOT turn or search. The
   diagnostic `sound` command verifies a controlled client-audio request only;
   it does not prove BOT hearing.
5. **Damage:** in a fresh trial, keep the human hidden, damage a BOT, and record
   the damage plus any BOT response. Do not combine this with a sound trial.
6. **Restore:** with Ghost active, record the T pawn and owned-weapon render
   values. Transition to `ghost-restore-control` (or disable the plugin through
   the normal test procedure), require no runtime faults and `recovery=Ready`,
   then take snapshots again. Confirm the prior render values and normal BOT
   visual acquisition are restored. API setter success alone is not a restore
   observation.

BOT spawn/reset and target posture controls must be supported by the running
server, not presumed from a command name. If verified BOT pose controls cannot
drive the target, Mai can test the movement/actions from a T client while CT
BOTs exercise perception. The test diagnostic has no native observer for
client-side rendering; Mai's client must judge the visible body, carried
weapons/attachments, shadow, and first-person hands. A second client on the
local LAN can view the body/attachments/shadow while the T client checks
first-person hands. Record actual cross-machine latency if it affected an
observation; do not add a public-network latency threshold to acceptance. Do
not use the diagnostic `sound` command during the quiet or product-breath
trial; it emits a separate test pulse and cannot establish BOT hearing.

## Posture, action reveal, and client-visible assets

With the T client in Ghost mode, accept each state separately:

- all non-bunny-hop poses (standing, crouch, Shift slow walk, ordinary jumping,
  and airborne uncrouch) at horizontal speed <=100: alpha 0;
- those same poses at speed >100: interpolate from alpha0 toward alpha127,
  reaching it at >=200, regardless of vertical speed or ground contact;
- moving with crouch+jump held retains the existing bunny-hop exemption and
  crouch alpha, on the ground or in the air;
- fire, secondary attack, and use: no action-triggered reveal or delay; the
  current posture/speed still determines alpha;
- reload: immediately alpha127, returning to the requested pose/speed only
  after reload ends and the 0.25-second delay passes.
- For reload specifically, begin a real reload, release R while the animation
  is still active, and query `reload_schema <T-slot>`. Require
  `reload_button_down=0` with the live `m_bInReload` value still true; Ghost
  must remain visible until the engine reload state ends and its reveal delay
  elapses.

The server snapshot supports identity, inventory, and schema readback only.
Mai must inspect the actual rendered body, carried weapon/backpack attachments,
shadow, and normal first-person arms in the client. Do not infer any of those
visual outcomes from alpha values or a BOT visibility result.

## Acceptance record

Retain the paired package/source identity with each platform result. Record
fresh discovery, remembered pursuit, sound, damage, restoration, posture,
reload-key release, loadout, client render, and exit/restore as distinct
observations. A passing native API boundary or a server alpha readback does not
close these product and client-acceptance gates.
