// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cstddef>
#include <initializer_list>

inline constexpr bool __polyhook_HookDetour = true, __polyhook_FindDetour = true, __polyhook_UnhookDetour = true;
inline constexpr bool __polyhook_AddCallback = true, __polyhook_RemoveCallback = true, __polyhook_IsCallbackRegistered = true;
inline constexpr bool __polyhook_AreCallbacksRegistered = true, __polyhook_GetArgumentPointer = true;

namespace polyhook {
enum class CallbackType { Pre, Post };
enum class ResultType { Ignored };
enum class DataType { Void, Pointer };
using HookHandle = void*;
using ParametersHandle = void**;
using ReturnHandle = void*;
using Callback = ResultType (*)(HookHandle, ParametersHandle, int, ReturnHandle, CallbackType);
HookHandle HookDetour(void*, DataType, std::initializer_list<DataType>, int, const char*);
HookHandle FindDetour(void*);
bool UnhookDetour(void*);
bool AddCallback(HookHandle, CallbackType, Callback);
bool RemoveCallback(HookHandle, CallbackType, Callback);
bool IsCallbackRegistered(HookHandle, CallbackType, Callback);
bool AreCallbacksRegistered(HookHandle);

template <typename T>
T GetArgument(ParametersHandle params, std::size_t index) {
    return static_cast<T>(params[index]);
}
}
