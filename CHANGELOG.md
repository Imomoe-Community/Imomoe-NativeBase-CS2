# Changelog / 更新日志

All notable changes to this project will be documented in this file.
本项目的所有重要变更均记录在此文件中。

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [0.4.0]

- **ABI 变更 / Breaking ABI:** 用 `CreateGrenadeProjectile` 和按类型分派的 `CanCreateGrenadeProjectile` 替代 0.1.0 的火雨弹体工厂，支持原生 Molotov、HE 和烟雾。火雨自伤隔离仍按精确 inferno 来源显式启用。 / The typed Molotov, HE and smoke factory replaces the 0.1.0 rain factory; the opt-in rain self-damage policy retains its exact inferno lineage.
- **原生实弹 / Native shots:** 原生武器命令窗口内的霰弹保持多弹丸入射墙面效果，不重复发送穿透出口。 / In the native weapon command window, shotgun shots keep their stock entry impacts without adding exit impacts.
- **客户端 / Clients:** 新增 `ForceRespawnClient` 和 `SnapPlayerViewAngles`，保留 Admin 的 `GetClientUserID`。 / Adds full engine respawn and view-angle updates; retains Admin's engine user ID export.
- **构建 / Build:** 为 CS2 `26879581 / 2000950` 固定 S2SDK 3.5.1 与上游 SourceSDK `e4e7260`，S2SDK 保持官方导出 ABI。 / Pins S2SDK 3.5.1 and upstream SourceSDK `e4e7260` for this CS2 build, with the official S2SDK export ABI.

## [0.1.0] - 2026-11-22

### 简体中文

- **共享原生 C++ 提供者架构**：整合 `FunRounds` 与 `Imomoe-Admin` 共用的引擎 Hook、内存补丁与 Gamedata（[gamedata/imomoe-native-base.jsonc](gamedata/imomoe-native-base.jsonc)），由单一插件统一持有原生边界。
- **完整 Plugify ABI 导出**：在 [imomoe_native_base.pplugin](imomoe_native_base.pplugin) 导出 88 个方法（28 个能力域）与 13 个回调原型，包括面向 `Imomoe-Admin` 的独立 `GetClientUserID(slot)` 与面向 `FunRounds` 的玩法原生桥接。
- **固定基线打包与双端验证**：基于 [runtime/s2sdk.json](runtime/s2sdk.json)（S2SDK `3.4.3@e2b88d3`、SourceSDK `92f0494`）提供 Windows MSVC（C++23 / MASM）与 Linux Steam Runtime Sniper（GCC 14.2）自动化构建、Gamedata 校验与双 CTest 套件。

### English

- **Shared Native C++ Provider Architecture**: Consolidated engine hooks, memory patches, and gamedata ([gamedata/imomoe-native-base.jsonc](gamedata/imomoe-native-base.jsonc)) shared by `FunRounds` and `Imomoe-Admin` under one native hook owner.
- **Full Plugify ABI Surface**: Exported 88 methods across 28 capability groups and 13 callback prototypes in [imomoe_native_base.pplugin](imomoe_native_base.pplugin), including standalone `GetClientUserID(slot)` for `Imomoe-Admin` and gameplay bridges for `FunRounds`.
- **Pinned Dual-Platform Build & Tests**: Added `scripts/build-sdk.py` and CI workflows against [runtime/s2sdk.json](runtime/s2sdk.json) (`S2SDK 3.4.3@e2b88d3`, `SourceSDK 92f0494`) producing separate NativeBase and S2SDK fork ZIPs for Windows x64 (MSVC C++23 / MASM) and Linux x64 (SteamRT Sniper GCC 14.2) with gamedata validation and CTest suites.
