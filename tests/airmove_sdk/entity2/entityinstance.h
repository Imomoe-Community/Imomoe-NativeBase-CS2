// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

struct CEntityInstance {
    int entry{1};
    const CEntityInstance& GetRefEHandle() const { return *this; }
    int GetEntryIndex() const { return entry; }
};
