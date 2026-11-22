// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace funrounds::native::detail {

// Resolves pinned signatures, imports, and schemas without installing hooks.
bool HasReloadSyncSupport() noexcept;
bool HasFullMagazineReloadSupport() noexcept;

// Closes admission and removes all owned callbacks before provider teardown.
bool ShutdownReloadSync() noexcept;

} // namespace funrounds::native::detail
