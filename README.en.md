<div align="center">

# Imomoe NativeBase (`imomoe_native_base`)

[![License: AGPL v3](https://img.shields.io/badge/License-AGPL_v3-blue.svg)](LICENSE)
[![C++: C++23](https://img.shields.io/badge/C%2B%2B-C%2B%2B23-orange.svg)](CMakeLists.txt)
[![CS2: Compatible](https://img.shields.io/badge/CS2-Dedicated%20Server-black.svg)](https://www.counter-strike.net/)
[![Platform: Plugify](https://img.shields.io/badge/Plugify-v3.1.0%2B-purple.svg)](https://github.com/untrustedmodders/plugify)
[![CI](https://github.com/Imomoe-Community/Imomoe-NativeBase-CS2/actions/workflows/ci.yml/badge.svg)](https://github.com/Imomoe-Community/Imomoe-NativeBase-CS2/actions/workflows/ci.yml)

**The shared Plugify C++ provider for FunRounds and Imomoe Admin on Counter-Strike 2 dedicated servers.**

[双语 / Bilingual](README.md) · **English**

</div>

---

### Overview

`imomoe_native_base` is the shared native C++23 [Plugify](https://github.com/untrustedmodders/plugify) provider used by FunRounds and Imomoe Admin. It owns the existing engine hooks and gamedata ([imomoe-native-base.jsonc](gamedata/imomoe-native-base.jsonc)). FunRounds keeps its gameplay and configuration in Rust; Imomoe Admin can call `GetClientUserID(slot)` without FunRounds. The return value is the current engine userid, or `-1` for an invalid or disconnected slot.

---

### Exported Capabilities

[imomoe_native_base.pplugin](imomoe_native_base.pplugin) exports **96 methods** across 30 capability groups and **14 callback prototypes**:

| Groups | Capabilities & Scope |
| :--- | :--- |
| `Clients` | `GetClientUserID` resolves engine user IDs; `SnapPlayerViewAngles` updates view angles for one complete pawn handle; `ForceRespawnClient` performs a full engine respawn by slot. |
| `Damage`, `Entities` | Player and non-player `TakeDamage` hooks, direct knife/firearm hitbox and source-bearing inspection, `TakeEntityDamageWithWeapon`, `Touch` pre-hooks, and collision group reset. |
| `RoundScore`, `C4Access`, `MolotovRain`, `Grenades`, `ReloadSync`, `WorldItems`, `RandomSpawn`, `Afterburn`, `Suicide`, `Healthshot`, `TeamFlash` | Native team round completion, C4 plant/defuse gating, native HE/smoke/Molotov projectile creation, rain self-damage filtering, grenade detonation state/holds/duds, reload sync & full-magazine reload, ownerless world items, random deathmatch spawns, player ignition, suicide pickup gating, healthshot caps, and team-flash judging veto. |
| `Weapons` | Register `SetNativeWeaponCommandCallback` and issue `FireNativeWeaponShot` within that pawn's command callback. It fires one native hitscan shot along the pawn's aim using the selected stock firearm profile without equipping the temporary source. |
| `GhostVisibility`, `GhostRadar`, `PoseGraph`, `PoseMove`, `PlayerFreeze`, `AirMove`, `GroundReverse`, `WallHang`, `ThirdPerson`, `BigHead`, `ChickenEating`, `ChickenFollow`, `TargetAttack`, `Chrono`, `RecoilImpulse` | BOT visibility/radar/footstep filtering, AnimGraph2 pose graph actions, movement freeze/auto-jump/airmove/ground-reverse/hull tracing, third-person camera eye targets, big-head scaling, chicken use/follow gating, and weapon attack/recoil hooks. |

---

### Building & CI Outputs

Use Python 3.11+, `plugify-gen 1.2.15`, and Visual Studio x64/C++23 (with MASM) on Windows. Linux uses Docker and the Steam Runtime Sniper SDK pinned below, with GCC 14.2. The builder uses S2SDK `3.5.2@86efb99` with SourceSDK `e4e7260`, as pinned in [`runtime/s2sdk.json`](runtime/s2sdk.json). It builds the SDK, compiles the provider, runs both CTest suites, and produces separate NativeBase and fork S2SDK ZIPs.

```powershell
# Windows (PowerShell)
python scripts/build-sdk.py --generator PATH/plugify-gen.exe --work-dir .work/win --resume-source PATH/pinned-sdk --dist dist
```

```bash
# Linux (Steam Runtime Sniper Docker)
python3 scripts/build-sdk.py --generator PATH/plugify-gen --work-dir .work/linux \
  --container-image registry.gitlab.steamos.cloud/steamrt/sniper/sdk@sha256:bb30053d3cd19d35c76ae69b2790dbaa1430448006f8240cb6d7e723f2d1940e \
  --resume-source .work/pinned-sdk --dist dist
```

Pass `--resume-source` to reuse a clean checkout at both pins. Work directories can be under this checkout's `.work/` or `IMOMOE_WORK_ROOT`. Check changes to hook keys with `python scripts/validate-native-gamedata.py`. [`imomoe_native_base.pplugin`](imomoe_native_base.pplugin) is the method and callback ABI source.

#### CI Outputs

[`ci.yml`](.github/workflows/ci.yml) runs the Windows and Linux Release builds and both CTest suites on pushes to `main`, PRs targeting `main`, or a manual run. Each platform uploads two Actions artifacts:

| Actions artifact | Install ZIP inside |
| :--- | :--- |
| `imomoe-native-base-<platform>` | `imomoe-native-base-0.4.0-<platform>.zip` |
| `s2sdk-fork-<platform>` | `s2sdk-fork-3.5.2-86efb99-<platform>.zip` |

> [!NOTE]
> The fork exposes the official S2SDK 3.5.2 ABI. Use `--resume-source` locally until hosted CI can fetch the pinned fork commit.

---

### Installation & Rollback

1. Use the two files for your server's platform:
   - `imomoe-native-base-0.4.0-<platform>.zip` installs `envs/imomoe_native_base/` only.
   - `s2sdk-fork-3.5.2-86efb99-<platform>.zip` installs `envs/plugify-plugin-s2sdk/` only. Its `s2sdk-fork.json` identifies the fork and SourceSDK commits.

2. For a first installation, prepare:
   - [S2-Launcher 2.5.8](https://github.com/untrustedmodders/plugify-source2-launcher/releases/tag/v2.5.8) / [Plugify](https://github.com/untrustedmodders/plugify)
   - [Rust module 1.3.0](https://github.com/untrustedmodders/plugify-module-rust/releases/tag/v1.3.0) & [C++ module 2.4.5](https://github.com/untrustedmodders/plugify-module-cpp/releases/tag/v2.4.5)
   - [PolyHook 2.5.9](https://github.com/untrustedmodders/plugify-plugin-polyhook/releases/tag/v2.5.9) & [Configs 2.1.2](https://github.com/untrustedmodders/plugify-plugin-configs/releases/tag/v2.1.2)
   - Then install the fork, NativeBase, and the chosen consumer, copying only the `game/` tree from each ZIP. When adding the second consumer, keep both shared packages in place and install only that consumer and any missing dependencies.

> [!WARNING]
> The pinned fork replaces the official provider with the same name; **never load both**. Stop the server and back up the old set before switching. Move official `s2sdk` and legacy `funrounds_native` out of discovery if installed. Keep the existing package-management setup; overwriting the official files and then running `mamba update --all` is not supported.

3. **Rollback**: To undo an add-on installation, remove that consumer after a normal stop and keep dependencies still in use. To return to the old provider layout, stop and restore the complete previous SDK, provider, consumers, and configuration.

---

### Maintenance Boundary

- FunRounds and Admin release only their own plugins. NativeBase owns Imomoe's native capabilities.
- Keep the official S2SDK export ABI and carry only necessary internal fixes in the fork.
- Send generic SDK fixes upstream. When adopting an official release that contains an accepted fix, remove its local patch. If upstream declines a change that Imomoe still needs, decide whether it belongs in NativeBase or in the consumer based on why it is needed.

---

### Documentation & License

- [Contributing](CONTRIBUTING.md) · [Security](SECURITY.md) · [Changelog](CHANGELOG.md)
- **Author**: Mai Ooizumi ([@Mai-Ooizumi](https://github.com/Mai-Ooizumi))
- **License**: GNU Affero General Public License v3.0 or later ([AGPL-3.0-or-later](LICENSE))
