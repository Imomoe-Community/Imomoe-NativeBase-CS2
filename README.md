<div align="center">

# Imomoe NativeBase (`imomoe_native_base`)

[![License: AGPL v3](https://img.shields.io/badge/License-AGPL_v3-blue.svg)](LICENSE)
[![C++: C++23](https://img.shields.io/badge/C%2B%2B-C%2B%2B23-orange.svg)](CMakeLists.txt)
[![CS2: Compatible](https://img.shields.io/badge/CS2-Dedicated%20Server-black.svg)](https://www.counter-strike.net/)
[![Platform: Plugify](https://img.shields.io/badge/Plugify-v3.1.0%2B-purple.svg)](https://github.com/untrustedmodders/plugify)
[![CI](https://github.com/Imomoe-Community/Imomoe-NativeBase-CS2/actions/workflows/ci.yml/badge.svg)](https://github.com/Imomoe-Community/Imomoe-NativeBase-CS2/actions/workflows/ci.yml)

**面向 Counter-Strike 2 专用服务端的 FunRounds 与 Imomoe Admin 共享 C++ Plugify 底层提供者**<br>
<sub><sup>The shared Plugify C++ provider for FunRounds and Imomoe Admin on Counter-Strike 2 dedicated servers.</sup></sub>

**双语 / Bilingual** · [English](README.en.md)

</div>

---

### 项目简介 / Overview

**imomoe_native_base** 是由 `FunRounds` 与 `Imomoe-Admin` 共用的原生 C++23 [Plugify](https://github.com/untrustedmodders/plugify) 底层插件，统一维护全部引擎 Hook、内存补丁与 Gamedata（[imomoe-native-base.jsonc](gamedata/imomoe-native-base.jsonc)）。`FunRounds` 将玩法逻辑与配置保留在 Rust；`Imomoe-Admin` 无需依赖 `FunRounds` 即可直接调用 `GetClientUserID(slot)`（返回当前引擎 userid，无效或未连接槽位返回 `-1`）。
<sub><sup>**imomoe_native_base** is the shared native C++23 [Plugify](https://github.com/untrustedmodders/plugify) provider used by `FunRounds` and `Imomoe-Admin`. It owns the engine hooks and gamedata ([imomoe-native-base.jsonc](gamedata/imomoe-native-base.jsonc)). `FunRounds` keeps gameplay and configuration in Rust, while `Imomoe-Admin` can call `GetClientUserID(slot)` (returning the current engine userid, or `-1` for an invalid or disconnected slot) without `FunRounds`.</sup></sub>

---

### 导出能力概览 / Exported Capabilities

在 [imomoe_native_base.pplugin](imomoe_native_base.pplugin) 中共声明 **88 个导出方法**与 **13 个回调原型**，覆盖以下能力域：<br>
<sub><sup>Declares **88 exported methods** across 28 groups and **13 callback prototypes** in [imomoe_native_base.pplugin](imomoe_native_base.pplugin):</sup></sub>

| 能力域 / <sub><sup>Groups</sup></sub> | 涵盖接口与用途 / <sub><sup>Capabilities & Scope</sup></sub> |
| :--- | :--- |
| **客户端标识**<br><sub><sup>`Clients`</sup></sub> | `GetClientUserID`：查询在线玩家真实引擎 userid，供 `Imomoe-Admin` 等消费插件使用。<br><sub><sup>`GetClientUserID`: resolves the live engine userid for `Imomoe-Admin` and companion consumers.</sup></sub> |
| **伤害、碰撞与实体**<br><sub><sup>`Damage`, `Entities`</sup></sub> | 玩家与非玩家伤害拦截/改写、直接近战/枪械命中部位与来源方位、实体伤害注入、`Touch` 拦截及标准碰撞组重置。<br><sub><sup>Player/entity `TakeDamage` hooks, direct knife/firearm contact and bearing inspection, `Touch` pre-hooks, and collision group reset.</sup></sub> |
| **回合与武器投掷物**<br><sub><sup>`RoundScore`, `C4Access`, `MolotovRain`, `Grenades`, `ReloadSync`, `WorldItems`, `RandomSpawn`, `Afterburn`, `Suicide`, `Healthshot`</sup></sub> | 原生回合结算、C4 安放/拆弹控制、原生 HE／烟雾／燃烧瓶弹体工厂、火雨自伤隔离、全投掷物引爆状态与滞留控制、换弹同步与满弹夹换弹、无主道具生成、死亡竞赛随机出生点、点燃、自爆拾取拦截与治疗针上限控制。<br><sub><sup>Native round win completion, C4 plant/defuse gating, native HE/smoke/Molotov projectile creation, rain self-damage filtering, grenade detonation holds/duds, reload sync, world items, random spawns, ignite, suicide pickup gating, and healthshot caps.</sup></sub> |
| **移动、隐身与表现**<br><sub><sup>`GhostVisibility`, `GhostRadar`, `PoseGraph`, `PoseMove`, `PlayerFreeze`, `AirMove`, `GroundReverse`, `WallHang`, `ThirdPerson`, `BigHead`, `ChickenEating`, `ChickenFollow`, `TargetAttack`, `Chrono`, `RecoilImpulse`</sup></sub> | BOT 视野/雷达/被动脚步声隔离、AnimGraph2 姿态、冻结移动/自动连跳/空中辅助/反向地面移动、碰撞盒扫掠、第三人称相机眼位、大头缩放、小鸡交互与跟随屏蔽、武器攻击/后坐力干预。<br><sub><sup>BOT visibility/radar/footstep filtering, AnimGraph2 poses, freeze/auto-jump/airmove/ground-reverse/hull tracing, third-person eye targets, big-head scaling, chicken interactions, and weapon/recoil hooks.</sup></sub> |

---

### 构建与 CI 产物 / Building & CI Outputs

Windows 需要 Python 3.11+、`plugify-gen 1.2.15` 与 Visual Studio x64 / C++23（含 MASM）；Linux 使用 Docker 与下文固定的 Steam Runtime Sniper SDK（GCC 14.2）。构建脚本基于 [runtime/s2sdk.json](runtime/s2sdk.json) 中固定的 S2SDK `3.4.3@e2b88d3` 与 SourceSDK `92f0494` 编译 SDK 与提供者、运行两套 CTest，并分别打包 NativeBase 与 S2SDK fork 安装包。<br>
<sub><sup>Windows requires Python 3.11+, `plugify-gen 1.2.15`, and Visual Studio x64 / C++23 (with MASM); Linux uses Docker with the pinned Steam Runtime Sniper SDK (GCC 14.2). Driven by [runtime/s2sdk.json](runtime/s2sdk.json) (`S2SDK 3.4.3@e2b88d3`, `SourceSDK 92f0494`), the builder compiles both components, runs both CTest suites, and produces separate install ZIPs.</sup></sub>

```powershell
# Windows (PowerShell)
python scripts/build-sdk.py --generator PATH/plugify-gen.exe --work-dir .work/win --dist dist
```

```bash
# Linux (Steam Runtime Sniper Docker)
python3 scripts/build-sdk.py --generator PATH/plugify-gen --work-dir .work/linux \
  --container-image registry.gitlab.steamos.cloud/steamrt/sniper/sdk@sha256:bb30053d3cd19d35c76ae69b2790dbaa1430448006f8240cb6d7e723f2d1940e \
  --dist dist
```

- **复用与校验 / Reuse & Validation**：传 `--resume-source` 可复用已检出的干净源码（Linux 下需位于本仓库目录内以便容器挂载）；修改 Hook 键后运行 `python scripts/validate-native-gamedata.py` 校验。[imomoe_native_base.pplugin](imomoe_native_base.pplugin) 是方法与回调 ABI 的唯一真源。<br><sub><sup>Pass `--resume-source` to reuse a clean checkout (placed inside the repo on Linux), validate gamedata edits with `python scripts/validate-native-gamedata.py`, and treat [imomoe_native_base.pplugin](imomoe_native_base.pplugin) as the ABI source of truth.</sup></sub>

#### CI 产物 / CI Outputs

[ci.yml](.github/workflows/ci.yml) 在推送 `main`、针对 `main` 的 PR 或手动触发时运行 Windows 与 Linux Release 构建及两套 CTest，并按平台上传两份 Actions Artifact：<br>
<sub><sup>[ci.yml](.github/workflows/ci.yml) runs Windows and Linux Release builds plus both CTest suites and uploads two Actions artifacts per platform:</sup></sub>

| Actions Artifact | 内含安装压缩包 / <sub><sup>Install ZIP Inside</sup></sub> |
| :--- | :--- |
| `imomoe-native-base-<platform>` | `imomoe-native-base-0.2.0-<platform>.zip` |
| `s2sdk-fork-e2b88d3-<platform>` | `s2sdk-fork-3.4.3-e2b88d3-<platform>.zip` |

> [!NOTE]
> FunRounds 的新火雨和枪榴弹需要配套的 NativeBase 0.2.0；本地开发使用本仓构建的独立包，已发布的 v0.1.0 保留供旧消费者使用。

---

### 安装与回退 / Installation & Rollback

1. 按服务端平台准备两个安装压缩包 / Obtain both ZIPs for your platform:
   - `imomoe-native-base-0.2.0-<platform>.zip`：仅包含 `envs/imomoe_native_base/`。
   - `s2sdk-fork-3.4.3-e2b88d3-<platform>.zip`：仅包含 `envs/plugify-plugin-s2sdk/`（内含 `s2sdk-fork.json` 标识 fork 与 SourceSDK 提交）。

2. 首次安装前，请先准备基础运行时，再仅从每个压缩包中复制 `game/` 目录：<br>
   <sub><sup>For a first installation, prepare the base runtime and copy only the `game/` tree from each ZIP:</sup></sub>
   - [S2-Launcher 2.5.8](https://github.com/untrustedmodders/plugify-source2-launcher/releases/tag/v2.5.8) / [Plugify](https://github.com/untrustedmodders/plugify)
   - [Rust module 1.3.0](https://github.com/untrustedmodders/plugify-module-rust/releases/tag/v1.3.0) & [C++ module 2.4.5](https://github.com/untrustedmodders/plugify-module-cpp/releases/tag/v2.4.5)
   - [PolyHook 2.5.9](https://github.com/untrustedmodders/plugify-plugin-polyhook/releases/tag/v2.5.9) & [Configs 2.1.2](https://github.com/untrustedmodders/plugify-plugin-configs/releases/tag/v2.1.2)
   - 再依次安装 S2SDK fork、NativeBase 与目标消费插件；接入第二个消费插件时保留两份共享包，仅追加该消费插件及缺失依赖。<br><sub><sup>Install the S2SDK fork, NativeBase, and your chosen consumer; when adding a second consumer, keep both shared packages in place.</sup></sub>

> [!WARNING]
> 固定版本的 S2SDK fork 会替换同名的官方 `s2sdk` 提供者，**切勿同时加载两者**。切换前先正常停服并备份旧整套目录，将官方 `s2sdk` 与旧版 `funrounds_native` 移出插件发现路径；不要在覆盖官方文件后运行 `mamba update --all`。<br><sub><sup>Never load the S2SDK fork alongside official `s2sdk` or legacy `funrounds_native`. Stop and back up before switching; running `mamba update --all` over the fork is unsupported.</sup></sub>

3. **回退与卸载 / Rollback**：若仅移除单个消费插件，正常停服后删除该插件并保留仍在使用的共享依赖；若要回退到旧版提供者布局，请停服并恢复完整的旧版 SDK、提供者、消费插件与配置。<br>
   <sub><sup>To remove one consumer, stop the server and remove only that consumer while keeping shared dependencies; to revert to an older provider layout, stop and restore the complete prior SDK, provider, consumer, and config snapshot.</sup></sub>

---

### 维护边界 / Maintenance Boundary

- `FunRounds` 与 `Imomoe-Admin` 各自只发布自身插件；`NativeBase` 统一拥有 Imomoe 的底层原生能力。<br><sub><sup>`FunRounds` and `Imomoe-Admin` release only their own plugins; `NativeBase` owns Imomoe's shared native capabilities.</sup></sub>
- `S2SDK 3.4.3@e2b88d3` 是本次打包拆分的对比基线而非长期冻结版本，需跟随 CS2 更新同步上游 Plugify / S2SDK，仅保留必要的少量差异。<br><sub><sup>`S2SDK 3.4.3@e2b88d3` is a split baseline rather than a permanent pin; stay aligned with upstream updates and minimize fork diff.</sup></sub>
- 通用 SDK 修复优先向上游提交并在合入正式版后移除本地补丁；若上游拒绝某项 Imomoe 仍需的能力，再依据作用域决定放在 `NativeBase` 还是消费端。<br><sub><sup>Send generic SDK fixes upstream and drop local patches once released; place upstream-declined changes in `NativeBase` or the consumer according to scope.</sup></sub>

---

### 相关文档与开源协议 / Documentation & License

- [贡献指南 / Contributing](CONTRIBUTING.md) · [安全通告 / Security](SECURITY.md) · [版本记录 / Changelog](CHANGELOG.md)
- **作者 / Author**：Mai Ooizumi ([@Mai-Ooizumi](https://github.com/Mai-Ooizumi))
- **开源协议 / License**：GNU Affero General Public License v3.0 or later ([AGPL-3.0-or-later](LICENSE))
