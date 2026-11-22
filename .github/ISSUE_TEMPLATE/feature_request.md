---
name: Feature Request / 功能建议
about: Propose a shared native capability or ABI improvement / 为 Imomoe NativeBase 提出共享原生能力或 ABI 改进
title: '[FEAT] '
labels: ['enhancement']
assignees: ''
---

### Summary / 需求概述
Briefly describe the proposed native bridge or ABI enhancement.
简要描述希望新增的共享原生桥接或 ABI 改进。

### Consumer & Scope / 适用消费插件与边界
- Which consumer plugin(s) require this (`FunRounds`, `Imomoe-Admin`, others)? / 哪些消费插件需要该能力？
- Why does it belong in `Imomoe-NativeBase-CS2` rather than upstream `s2sdk` or the consumer? / 为何应放在 NativeBase 而非上游 S2SDK 或消费插件？

### Design Proposal (Optional) / 方案设计（可选）
- Exported method/callback signatures and game-thread/handle contracts / 导出方法或回调签名及句柄契约
- Gamedata & cleanup lifecycle on map change or detach / Gamedata 键与换图/卸载清理策略
