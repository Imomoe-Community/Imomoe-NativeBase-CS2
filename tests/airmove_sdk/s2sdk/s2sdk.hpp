// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cstdint>

inline constexpr bool __s2sdk_GetGameConfigOffset = true;
inline constexpr bool __s2sdk_GetGameConfigSignature = true, __s2sdk_GetClientPawn = true, __s2sdk_GetMaxClients = true;
inline constexpr bool __s2sdk_PlayerSlotToEntHandle = true, __s2sdk_IsClientInGame = true;
inline constexpr bool __s2sdk_GetClientFlags = true, __s2sdk_GetClientButtons = true, __s2sdk_GetEntityMoveType = true;

namespace s2sdk {
using ConfigId = int;
enum class MoveType { Walk, Ladder };
void* GetGameConfigSignature(ConfigId, const char*);
int GetGameConfigOffset(ConfigId, const char*);
void* GetClientPawn(int);
int GetMaxClients();
int PlayerSlotToEntHandle(int);
bool IsClientInGame(int);
int GetClientFlags(int);
std::uint64_t GetClientButtons(int, int);
MoveType GetEntityMoveType(int);
}
