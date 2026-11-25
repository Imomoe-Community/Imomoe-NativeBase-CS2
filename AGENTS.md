# Imomoe-NativeBase-CS2

Own the shared C++ provider, its Plugify ABI and gamedata. Gameplay, moderation,
and configuration stay in their consumer repositories.

- Keep `runtime/s2sdk.json` and the matching C++ import descriptor aligned;
  build Windows x64 with MSVC, Linux x64 in pinned SteamRT GCC14, both C++23.
- Preserve method and callback ABI, typed gamedata keys, readiness semantics
  and hook ownership. Verify changes with Release CTest and the affected
  consumer/loader path. Keep probe instrumentation out of distributed binaries.
- Install only one native hook owner. Stop the server before replacing a
  provider; keep the previous complete consumer/SDK/provider set for rollback.
- Audit sibling call paths before patching bugs or ABI behavior; if the cleaner
  long-term fix crosses consumer or SDK boundaries, propose options with a
  recommendation instead of shipping a narrow workaround.
- Apply one-in, one-out closeout: remove superseded tests, fixtures, and
  probes replaced by your change in the same diff.
- Standalone clones default to `<repo>/.work/`; when `IMOMOE_WORK_ROOT` is set,
  place builds, caches, worktrees, and temporary probes under it rather than
  creating repo-local `.work/` or committing ad-hoc smoke/probe scripts.
