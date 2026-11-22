# Contributing Guide / 贡献指南

感谢对 **Imomoe NativeBase** 项目的关注与贡献！在提交代码或 Pull Request 之前，请阅读以下开发与协同规范。

Thank you for contributing to **Imomoe NativeBase**! Please review these guidelines before submitting code or a Pull Request.

---

## 1. 职责边界与安全原则 / Scope & Safety

- **专注共享原生能力 / Shared Native Capabilities Only**：本仓库仅维护 `FunRounds` 与 `Imomoe-Admin` 所需的 C++23 引擎桥接、Hook 与 Gamedata；具体玩法规则与管理逻辑分别留在各消费仓库。
  Own shared C++23 engine bridges, hooks, and gamedata; keep gameplay rules and moderation logic in their consumer repositories.
- **单一 Hook 所有者与句柄安全 / Ownership & Handle Safety**：
  - 所有引擎实体、Schema 与回调交互必须在游戏主线程执行，并严守完整实体句柄与地图代次校验。
    Run engine entity/Schema operations on the game thread and validate full entity handles and map generation.
  - 保持 [imomoe_native_base.pplugin](imomoe_native_base.pplugin)、[runtime/s2sdk.json](runtime/s2sdk.json) 与 [bindings/s2sdk.pplugin](bindings/s2sdk.pplugin) 对齐，不要在发布产物中保留诊断探针。
    Keep `imomoe_native_base.pplugin`, `runtime/s2sdk.json`, and `bindings/s2sdk.pplugin` aligned, and keep probe instrumentation out of release binaries.

---

## 2. 质量门禁与构建验证 / Quality Gates & Builds

提交前请运行 Gamedata 校验并完成目标平台构建与 CTest（详见 [README](README.md)）：
Validate gamedata and run platform build + CTest suites before opening a PR:

```bash
python scripts/validate-native-gamedata.py
```

```powershell
# Windows x64 (MSVC C++23 + MASM, plugify-gen 1.2.15)
python scripts/build-sdk.py --generator PATH/plugify-gen.exe --work-dir .work/win --dist dist
```

```bash
# Linux x64 (Steam Runtime Sniper Docker, GCC 14.2)
python3 scripts/build-sdk.py --generator PATH/plugify-gen --work-dir .work/linux \
  --container-image registry.gitlab.steamos.cloud/steamrt/sniper/sdk@sha256:bb30053d3cd19d35c76ae69b2790dbaa1430448006f8240cb6d7e723f2d1940e \
  --dist dist
```

---

## 3. 代码风格与提交流程 / Formatting & PR Workflow

- 遵守 [.editorconfig](.editorconfig) 与 [.gitattributes](.gitattributes) 的缩进与 LF 行尾约定（注意 `scripts/native-live-fixture/drive-suicide.ps1` 的编码规则）。
  Follow [.editorconfig](.editorconfig) and [.gitattributes](.gitattributes) conventions.
- 基于 `main` 分支拉取特性分支并保持每次提交聚焦单一主题，切勿提交私有服务器路径、SteamID、RCON 密码或 `.work/` / `dist/` 临时构建产物。
  Branch from `main`, keep commits focused, and never commit private paths, credentials, or `.work/` / `dist/` artifacts.
