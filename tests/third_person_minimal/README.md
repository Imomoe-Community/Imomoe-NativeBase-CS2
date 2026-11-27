# Third-person camera feasibility consumer

This isolated S2SDK/native consumer creates a centered `custom_player_camera`
for one connected live player or BOT, registering the exact pawn/camera pair
before assigning its view. It restores only its captured pawn while that field
still points to an owned diagnostic camera. Distance is 15–150 Source units;
follow-eyes, clipping and return strength 1.0 use the engine implementation.

The `abi` command tests the raw mid-hook's branch relocation, nested call and
platform register/return contract with a synthetic target. `eyecheck <slot>`
then calls the real pawn eye function: the native no-camera baseline, registered
own camera, different external camera while the Hook remains installed, clean
detach and reattachment. It compares all three components and requires distinct
camera/pawn origins. Both platform raw ABI and two-BOT 64/96-distance eye checks have passed;
input identity, retained driver failures and current scope are in [STATUS](../../../docs/STATUS.md). Actual gun/knife/grenade/Use paths and client visuals follow the
minimal boundary gate, as recorded in the current plan and status.
## Build without changing the product

Configure against the paired Plugify include root and generated S2SDK 3.5.2
include root, then build Release:

```powershell
cmake -S tests/third_person_minimal -B .work/third-person-minimal/build `
  -DFUNROUNDS_PLUGIFY_INCLUDE_DIR=<plugify-include> `
  -DFUNROUNDS_S2SDK_GENERATED_INCLUDE_DIR=<generated-s2sdk-include> `
  -DFUNROUNDS_NATIVE_GENERATED_INCLUDE_DIR=<generated-native-include>
cmake --build .work/third-person-minimal/build --config Release
```

The Windows output is `bin/Release/third-person-minimal.dll`; the Linux output
is `bin/libthird-person-minimal.so`. Keep the binary and copied `driver.pplugin`
together when staging.

Stage only `bin/third-person-minimal` and the generated `driver.pplugin` in an
isolated Plugify environment on the already-running acceptance server. This
consumer is not part of `funrounds.pplugin`, the native product manifest, a
candidate package, or a production deployment. The test does not start or stop
CS2. Use the server console or RCON, not a player client.

## Console sequence and cleanup gate

1. `fr_third_person_minimal schema` must report `schema=available`; run `fr_third_person_minimal abi` and require its ABI checks to pass.
2. Use a disposable human slot with no external camera owner:
   `fr_third_person_minimal start <slot> 64`. Confirm it returns `active=true`.
3. After at least one engine frame, `fr_third_person_minimal eyecheck <slot>` must report `eyecheck=1`, `owned_32=1`, `external=1`, `distinct_branches=1`, `detached_native=1`, and `reattached=1`. Then `fr_third_person_minimal inspect <slot>` must report the same pawn and camera
   handles, mode `3`, offset `-64,0,0`, `follow_eyes=1`, `clipped=1`, and
   `return_strength=1`.
4. Set a different center distance, e.g.
   `fr_third_person_minimal distance <slot> 96`, then inspect again. Verify in
   the actual client that the camera follows position/angles, remains centered,
   pulls in against nearby geometry, and returns smoothly after the obstruction
   clears. Schema readback alone does not prove those engine effects.
5. Run `fr_third_person_minimal stop <slot>`. Do not continue until it reports
   `active=false restored=1 camera_removed=true`. Stop resolves the captured
   full pawn handle, even if the original slot now maps to a replacement pawn;
   it restores only the captured pawn and only while that pawn still views this
   camera. If the captured pawn is invalid, it never writes a replacement pawn:
   it removes the camera and clears the record only after connected-player view
   fields confirm no reference and entity removal completes. An unavailable
   view field, a remaining reference, or deferred removal keeps the recovery
   record for retry. `stopall` provides the same gate for every record, including
   records whose slots are now outside the current client range.
6. Only after `stopall` reports `remaining=0` may the manager unload/reload this
   diagnostic. Repeat start/stop once, then confirm unload/load. `OnPluginEnd`
   makes a best-effort stop attempt, but it cannot guarantee recovery if the
   manager proceeds with unload after an error; an active camera is not safe to
   carry through manager unload.
7. On a separate controlled trial, record death, takeover, reconnect, map change,
   and external camera-owner scenarios. For each one, use `stop` / `stopall` to
   exercise captured-pawn restoration, stale-pawn reference checks, and retry
   after deferred removal; record unresolved references or unavailable state as
   failures. These lifecycle paths are implemented in the recovery driver but
   still need actual engine trials before being treated as runtime-accepted.

The minimal checks do not automate player input. Actual attack/Use origins and near-wall obstruction belong to the subsequent combined gameplay runtime scene.

## Retained failure: the aggregate return-replacement route

IDA inspected the task-local database snapshots under `.work/third-person-abi-resume-r1`.
The raw `server-win64.dll` and `server-linux.so` inputs are retained there;
the consumed IDA caches can be regenerated from those modules.
The Windows snapshot's input was `D:\dev\imomoe-funrounds-cs2svr\game\csgo\bin\win64\server.dll`;
the Linux input is the task-local copy of
`/home/mai/funrounds-acceptance/cs2-update-20261023/server/game/csgo/bin/linuxsteamrt64/libserver.so`.
Windows `server.dll` `sub_180B55060` takes `(pawn=RCX, output=RDX)`; its active-camera
call at `0x180B55093` returns to `0x180B5509A` with pawn in `RDI` and output buffer
in `RBX`, a narrow post-camera output site. The DB input and raw Windows module agree
at file offsets `0xB55093` (`41 FF 90 E8 05 00 00`), `0xB5509A` (`48 8B C3`), and
`0xB550A7` (`C3`).

Linux `libserver.so` `CBasePlayerPawn` and `CCSPlayerPawn` vtable slot `+0x5E0`
points to `sub_1652250`. Its camera-services path resolves the view entity;
`0x16522D8` restores the caller's `RBX`, and `0x16522E4` tail-jumps to that
entity's `GetEyePosition`. This bypasses the pawn's only local return at
`0x1652429`, reached by the non-camera calculation. At the tail-call `RDI` is the
camera entity, not the pawn, so this vfunc has no shared pawn-validated
post-camera result point. The `sub_B71350` caller's vcall at `0xB71420`, followed
by stores from `XMM0` and `XMM1` at `0xB7142D` and `0xB71432`, proves the real
SysV return layout: `X/Y` in `XMM0`, `Z` in `XMM1` for a 12-byte `Vector`. IDA's
file offsets match the task-local raw ELF bytes: the camera path is `0x16522DC`
(`48 8B 80 E0 05 00 00`) then `0x16522E4` (`FF E0`); the local non-camera return
is `0x1652429` (`C3`); the caller's vcall and result stores are `0xB71420`
(`FF 90 E0 05 00 00`), `0xB7142D` (`66 0F D6 45 B4`), and `0xB71432`
(`F3 0F 11 4D BC`).

PolyHook 2.5.7's typed `ReturnSlot` is limited to 8 bytes; its raw `HookDetour2`
callback runs before the trampoline and offers no post-return result override.
That cannot replace Linux's 12-byte return, and a hook at the pawn's local return
would miss the active-camera tail-call. That earlier return-replacement route failed. The current implementation instead changes only the temporary camera-services register before the original TEST/JZ, preserving the Windows output-buffer and Linux `XMM0`/`XMM1` contracts; its minimal boundary evidence is recorded in [STATUS](../../../docs/STATUS.md). See the [CS2Fixes fix](https://github.com/Source2ZE/CS2Fixes/commit/cd5c31ba5e8546e620dad179ae666796ee1d8e90)
and the [PolyHook 2.5.7 callback header](https://github.com/untrustedmodders/plugify-plugin-polyhook/blob/v2.5.7/src/callback.hpp),
[implementation](https://github.com/untrustedmodders/plugify-plugin-polyhook/blob/v2.5.7/src/callback.cpp),
and [entry points](https://github.com/untrustedmodders/plugify-plugin-polyhook/blob/v2.5.7/src/plugin.cpp).
