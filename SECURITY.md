# Security Policy / 安全策略

## 支持的版本 / Supported Versions

由于本插件直接依赖特定 CS2 服务端二进制（`server.dll` / `libserver.so`）特征码、虚表偏移与 Schema 布局，当前仅对 `main` 分支及最新 `0.1.x` 发布基线提供安全修复支持。

Because this provider relies on binary signatures, vtable offsets, and Schema layouts for specific CS2 builds, security fixes are provided for the `main` branch and latest `0.1.x` release baseline:

| 版本 / Version | 是否支持 / Supported |
| :--- | :--- |
| 0.1.x / main | :white_check_mark: |
| < 0.1.0 | :x: |

---

## 报告安全问题 / Reporting a Vulnerability

如果您在 **Imomoe NativeBase** 中发现了可能导致服务端崩溃、内存越界、ABI 破坏或 Hook 冲突的安全问题，请**不要**通过公开 Issue 披露细节。

If you discover a security vulnerability, memory-safety bug, ABI corruption, or crash exploit in **Imomoe NativeBase**, please **do not** disclose details in a public GitHub issue.

请通过以下渠道私下联系维护者并提供复现信息：
Please contact the maintainer privately with reproduction details:

- **GitHub Private Vulnerability Reporting**：在仓库 Security 选项卡发起私密漏洞报告 / submit a private report via the repository Security tab。
- **Maintainer**：联系 [Mai Ooizumi](https://github.com/Mai-Ooizumi)。

### 提报内容建议 / Recommended Details

1. CS2 服务端构建号（Build ID）与操作系统（Windows x64 / Steam Runtime Linux x64）。
   CS2 server build number and OS (`Windows x64` / `Steam Runtime Linux x64`).
2. `imomoe_native_base`、`s2sdk`（含 fork commit）、Plugify 与 PolyHook 版本。
   Versions/commits of `imomoe_native_base`, `s2sdk`, Plugify, and PolyHook.
3. 最小复现步骤、触发导出接口及脱敏控制台/崩溃日志。
   Minimal reproduction steps, affected export/callback, and sanitized logs without secrets or SteamIDs.

---

## 响应流程 / Response Process

1. **确认与评估 / Triage**：维护者将在 48 小时内确认并评估影响范围 / Maintainer acknowledges and triages within 48 hours.
2. **修复与验证 / Fix & Verify**：在私有分支修复并通过双端 CTest 与消费端回归 / Patched privately and verified with CTest and consumer checks.
3. **发版与披露 / Release & Disclosure**：发布修复包并在安全通告中致谢报告者 / Release fix and publish advisory.
