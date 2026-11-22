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
