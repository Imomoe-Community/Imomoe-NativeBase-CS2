// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

#include <plugin_export.h>
#include <tier0/globalsymbol.h>

#include "engine.hpp"

namespace funrounds::native::detail {
namespace {

constexpr auto kSetParameter = SignatureKey{"FunRounds::SetPoseGraphParameter"};
constexpr auto kBodyOperand = OffsetKey{"FunRounds::PoseGraphBodyOperand"};
constexpr auto kReadBody = OffsetKey{"FunRounds::PoseGraphBodyReadSlot"};
constexpr auto kWriteBody = OffsetKey{"FunRounds::PoseGraphBodyWriteSlot"};
constexpr auto kController = OffsetKey{"FunRounds::PoseGraphControllerOffset"};
constexpr auto kControllerType = OffsetKey{"FunRounds::PoseGraphControllerTypeSlot"};

constexpr std::string_view kChickenModel = "models/chicken/chicken.vmdl";
constexpr std::array kActions{"action_idle", "action_walk", "action_run", "action_glide", "action_squat"};
using SetParameter = bool (*)(void*, const CGlobalSymbol&, const char*);

struct GraphBinding {
    SetParameter set{};
    int body{}, readSlot{}, writeSlot{}, controller{}, typeSlot{};
} g_graph;

bool Resolve() {
    if (g_graph.set) return true;
    if (!EnsureEngine() || !__s2sdk_GetEntityClassname || !__s2sdk_GetEntityModel) {
        return false;
    }

    g_graph.readSlot = ProbeOffset(kReadBody);
    g_graph.writeSlot = ProbeOffset(kWriteBody);
    g_graph.controller = ProbeOffset(kController);
    g_graph.typeSlot = ProbeOffset(kControllerType);
    if (g_graph.readSlot < 0 || g_graph.writeSlot < 0 || g_graph.controller < 0 || g_graph.typeSlot < 0) return false;
    const auto* entry = static_cast<const std::byte*>(ProbeSignature(kSetParameter));
    const auto operand = ProbeOffset(kBodyOperand);
    if (!entry || operand < 3 || operand > 32 ||
        entry[operand - 3] != std::byte{0x48} || entry[operand - 2] != std::byte{0x8b} ||
        (entry[operand - 1] != std::byte{0x89} && entry[operand - 1] != std::byte{0xbf})) {
        return false;
    }

    // This access loads the animation component used by the engine function.
    // The generic m_CBodyComponent schema member points at a different base.
    std::memcpy(&g_graph.body, entry + operand, sizeof(g_graph.body));
    if (g_graph.body <= 0) return false;
    g_graph.set = reinterpret_cast<SetParameter>(const_cast<std::byte*>(entry));
    return true;
}

template <typename T>
T Read(const void* source, int offset) noexcept {
    T value{};
    if (source) std::memcpy(&value, static_cast<const std::byte*>(source) + offset, sizeof(value));
    return value;
}

template <typename Fn>
Fn Virtual(const void* object, int slot) noexcept {
    return Read<Fn>(Read<void*>(object, 0), slot * static_cast<int>(sizeof(void*)));
}

bool SetAction(std::int32_t handle, std::uint8_t action) {
    if (action >= kActions.size() || !Resolve()) return false;
    auto* prop = EntityFromHandle(handle);
    if (!prop || HandleFromEntity(prop) != handle ||
        std::string_view(s2sdk::GetEntityClassname(handle).c_str()) != "prop_dynamic" ||
        std::string_view(s2sdk::GetEntityModel(handle).c_str()) != kChickenModel) {
        return false;
    }

    // The choreography setter assumes both controller accessors are bound.
    // Source 2 also attaches an AnimGraph1 controller to some chicken models
    // (e.g. on a player Pawn), which must not receive an AnimGraph2 symbol.
    auto* body = Read<void*>(prop, g_graph.body);
    const auto read = Virtual<void* (*)(void*)>(body, g_graph.readSlot);
    const auto write = Virtual<void* (*)(void*)>(body, g_graph.writeSlot);
    if (!read || !write) return false;
    auto* controller = Read<void*>(read(body), g_graph.controller);
    if (!controller || Read<void*>(write(body), g_graph.controller) != controller) return false;
    const auto type = Virtual<std::uint8_t (*)(void*)>(controller, g_graph.typeSlot);
    if (!type || type(controller) != 2) return false;

    static const auto kActionParameter = MakeGlobalSymbol("action");
    return g_graph.set(prop, kActionParameter, kActions[action]);
}

} // namespace
} // namespace funrounds::native::detail

extern "C" PLUGIN_API bool HasPoseGraphSupport() noexcept {
    try {
        return funrounds::native::detail::Resolve();
    } catch (...) {
        return false;
    }
}

extern "C" PLUGIN_API bool SetPoseGraphAction(std::int32_t prop, std::uint8_t action) noexcept {
    try {
        return funrounds::native::detail::SetAction(prop, action);
    } catch (...) {
        return false;
    }
}
