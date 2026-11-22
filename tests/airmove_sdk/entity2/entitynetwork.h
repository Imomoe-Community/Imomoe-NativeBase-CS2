// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

struct CNetworkVarChainer {
    void* pawn;
    void* GetObject() const { return pawn; }
};
