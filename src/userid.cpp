// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <plugin_export.h>
#include <eiface.h>
#include <s2sdk/s2sdk.hpp>

extern "C" PLUGIN_API int GetClientUserID(int playerSlot) {
    if (playerSlot < 0 || !s2sdk::IsClientConnected(playerSlot)) {
        return -1;
    }
    // The engine interface lives for the process, not the map or connection.
    static IVEngineServer2* engine = nullptr;
    if (!engine) {
        engine = static_cast<IVEngineServer2*>(
            s2sdk::QueryInterface("engine2", INTERFACEVERSION_VENGINESERVER));
    }
    return engine ? engine->GetPlayerUserId(CPlayerSlot(playerSlot)).Get() : -1;
}
