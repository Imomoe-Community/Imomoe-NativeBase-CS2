# Actual-CS2 ground-reverse boundary check

This test-only consumer imports the exact production native provider. It must not
be included in a gameplay package. Never activate its ground-reverse API beside
an active gameplay consumer of that API. Unlike the standalone loader fixture, it requires a
real CS2 server and real S2SDK.

Generate native C++ bindings from `native/imomoe-native-base/imomoe_native_base.pplugin`
using the pinned generator. Configure this directory with
`FUNROUNDS_PLUGIFY_INCLUDE_DIR`, `FUNROUNDS_S2SDK_GENERATED_INCLUDE_DIR` and
`FUNROUNDS_NATIVE_GENERATED_INCLUDE_DIR`; these are the same include roots used
by the native build. Build Release and stage the resulting binary and
`driver.pplugin` as a separate Plugify environment in the isolated server only.
Back up any existing environment before staging; remove/restore it after the
server exits, in addition to the normal acceptance-runner recovery.

From server console/RCON, with `sv_cheats=0`:

1. Confirm `plg plugins` shows `configs`, `imomoe_native_base`,
   `ground_reverse_minimal`, `polyhook` and `s2sdk` in `Running`. If the Main
   `funrounds` plugin is loaded, run `fr_enable 0` and confirm `fr_status`
   reports `enabled=false`, `recovery=Ready` and `cleanup_pending=false`. In a
   Main-free minimal session, confirm `funrounds` is absent from `plg plugins`
   (or pass `-MainFree` to `scripts/validate-ground-reverse-minimal.ps1`).
2. `fr_ground_reverse_minimal support` must report `support=true`.
3. `fr_ground_reverse_minimal start` must report active with global acceleration
   zero and local coefficient 5.5. The command captures the previous ConVar.
4. Take repeated `fr_ground_reverse_minimal snapshot` observations with autonomous
   BOTs. Compare exact pawn handles and actual ground flags before comparing
   positions in the RCON reply log; a ConVar readback alone does not establish
   movement. `scripts/validate-ground-reverse-minimal.ps1` requires four BOTs,
   disables empty-server hibernation temporarily, and validates this movement.
5. `fr_ground_reverse_minimal stop` must confirm restoration. Repeat `start`/`stop`
   to verify reactivation and captured acceleration recovery, then confirm
   `support=true` and `plg plugins` remain healthy without running `plg reload`
   in the live CS2 process (the native Ghost visibility worker hook stays
   process-resident until server exit). The PowerShell driver records
   `mainFree` and `gameHotReload = false`.
6. Stop the server normally and restore the entire paired overlay and this
   diagnostic environment. Preserve logs for failures and incomplete recovery.

Snapshots are passive and include health, armor and weapon inventory for the
warmup lifecycle check. The explicit server-only `killbot` command refuses any
human presence, applies ordinary damage attributed to the validated world entity
at index zero to one live BOT, and reports its classname, actual health loss and
death readback. It does not bypass gameplay damage policy.
When observing active gameplay, use only snapshot/killbot and leave the
diagnostic's own start command unused; its unowned stop/unload is a no-op.

The driver sends no player input, performs no teleportation and cannot establish
human response latency. External ConVar changes are preserved on stop. A failed
native detach retains recovery state for retry rather than claiming success.
