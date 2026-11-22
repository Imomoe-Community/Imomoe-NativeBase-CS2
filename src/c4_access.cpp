// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "c4_access.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <unordered_map>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#include <plugin_export.h>
#include <polyhook/polyhook.hpp>

#include "detour_slot.hpp"
#include "engine.hpp"

namespace funrounds::native::detail {
namespace {

using enum polyhook::CallbackType;
using enum polyhook::DataType;
using enum polyhook::ResultType;
using Args = polyhook::ParametersHandle;

constexpr std::uint8_t kT = 2;
constexpr std::uint8_t kCt = 3;

struct Hook {
    SignatureKey signature;
    polyhook::CallbackType type;
    polyhook::CallbackHandler callback;
    void* entry{};
    polyhook::HookHandle handle{};

    bool install(polyhook::DataType result, const plg::vector<polyhook::DataType>& arguments) {
        void* const target = entry ? entry : ProbeSignature(signature);
        DetourSlot slot(entry, handle);
        return slot.Acquire(target, [&] { return polyhook::HookDetour(target, result, arguments, -1, signature.name().data()); }) &&
               slot.Add(type, callback);
    }

    bool remove() {
        DetourSlot slot(entry, handle);
        return slot.Remove(type, callback) && slot.Release();
    }
};

// The displaced CMP sets ZF for the already-verified native C4 branch. The
// typed hooks above it own team admission; all other engine checks stay intact.
struct TeamCompare {
    SignatureKey signature;
    OffsetKey widthKey;
    std::uint8_t* address{};
    std::array<std::uint8_t, 8> original{};
    std::array<std::uint8_t, 8> equal{};
    std::size_t width{};
    bool applied{};
    bool protectionPending{};
#if defined(_WIN32)
    DWORD protection{};
#endif

    bool resolve(SchemaField team) {
        if (address) {
            return true;
        }
        auto* site = static_cast<std::uint8_t*>(ProbeSignature(signature));
        const auto length = ProbeOffset(widthKey);
        if (!site || length < 0) return false;
        if (length != 7 && length != 8) {
            InvalidGamedata(widthKey.name());
            return false;
        }
        std::int32_t displacement{};
        std::memcpy(&displacement, site + length - 5, sizeof(displacement));
        if (displacement != team.offset || site[length - 1] != kCt) {
            InvalidGamedata(signature.name());
            return false;
        }
        address = site;
        width = static_cast<std::size_t>(length);
        std::memcpy(original.data(), site, width);
        equal.fill(0x90); // NOP padding preserves the following branch location.
        equal[0] = 0x38; // CMP AL, AL, without changing a general-purpose register.
        equal[1] = 0xC0;
        return true;
    }

    bool set(bool desired) {
        if (desired == applied && !protectionPending) {
            return true;
        }
        if (!address) {
            return false;
        }
        const auto& before = applied ? equal : original;
        if (std::memcmp(address, before.data(), width) != 0) {
            return false;
        }
#if defined(_WIN32)
        DWORD previous{};
        if (!VirtualProtect(address, width, PAGE_EXECUTE_READWRITE, &previous)) {
            return false;
        }
        if (!protectionPending) {
            protection = previous;
        }
#else
        const auto pageSize = sysconf(_SC_PAGESIZE);
        if (pageSize <= 0) {
            return false;
        }
        const auto pageBytes = static_cast<std::uintptr_t>(pageSize);
        const auto begin = reinterpret_cast<std::uintptr_t>(address);
        auto* page = reinterpret_cast<void*>(begin - begin % pageBytes);
        const auto bytes = pageBytes * (1 + (begin % pageBytes + width - 1) / pageBytes);
        if (mprotect(page, bytes, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
            return false;
        }
#endif
        protectionPending = true;
        std::memcpy(address, (desired ? equal : original).data(), width);
        applied = desired;
#if defined(_WIN32)
        const bool flushed = FlushInstructionCache(GetCurrentProcess(), address, width) != 0;
        protectionPending = VirtualProtect(address, width, protection, &previous) == 0;
        return flushed && !protectionPending;
#else
        __builtin___clear_cache(reinterpret_cast<char*>(address), reinterpret_cast<char*>(address + width));
        protectionPending = mprotect(page, bytes, PROT_READ | PROT_EXEC) != 0;
        return !protectionPending;
#endif
    }
};

SchemaField g_team{};
SchemaField g_owner{};
bool g_plantBlocked{};
std::unordered_map<std::int32_t, std::uint8_t> g_plantingTeams;

std::uint8_t Team(const void* pawn) noexcept {
    return pawn ? *(static_cast<const std::uint8_t*>(pawn) + g_team.offset) : 0;
}

bool Admits(void* bomb, void* pawn) {
    const auto team = Team(pawn);
    const auto found = g_plantingTeams.find(HandleFromEntity(bomb));
    if (found == g_plantingTeams.end()) {
        return team == kCt; // Map-created or already-planted C4 retains stock admission.
    }
    return team == kT || (team == kCt && found->second == kT);
}

polyhook::ResultType CapturePlant(polyhook::HookHandle, Args args, std::int32_t,
                                polyhook::ReturnHandle result, polyhook::CallbackType) noexcept {
    try {
        auto* bomb = polyhook::GetReturn<void*>(result);
        const auto handle = HandleFromEntity(bomb);
        const auto team = Team(polyhook::GetArgument<void*>(args, 1));
        if (handle != -1 && (team == kT || team == kCt)) {
            std::erase_if(g_plantingTeams, [](const auto& entry) { return !EntityFromHandle(entry.first); });
            g_plantingTeams.insert_or_assign(handle, team);
        }
    } catch (...) {
        // A factory failure leaves the native C4 rule in place for that entity.
    }
    return Ignored;
}

polyhook::ResultType SelectPost(polyhook::HookHandle, Args args, std::int32_t,
                               polyhook::ReturnHandle result, polyhook::CallbackType) noexcept {
    try {
        if (!polyhook::GetReturn<bool>(result)) {
            return Ignored;
        }
        auto* candidate = polyhook::GetArgument<void*>(args, 1);
        if (s2sdk::GetEntityClassname(HandleFromEntity(candidate)) != "planted_c4") {
            return Ignored; // This selector also handles hostages.
        }
        // The first word of the output is the selected entity. Leave native
        // hint/position metadata untouched, including on a continuation check.
        auto** selected = polyhook::GetArgument<void**>(args, 2);
        if (selected && *selected == candidate && !Admits(candidate, polyhook::GetArgument<void*>(args, 0))) {
            *selected = nullptr;
        }
    } catch (...) {
    }
    return Ignored;
}

polyhook::ResultType BeginPre(polyhook::HookHandle, Args args, std::int32_t,
                            polyhook::ReturnHandle, polyhook::CallbackType) noexcept {
    try {
        auto* bomb = polyhook::GetArgument<void*>(args, 0);
#if defined(_WIN32)
        const auto* use = polyhook::GetArgument<void**>(args, 1);
        auto* pawn = use ? *use : nullptr;
#else
        auto* pawn = polyhook::GetArgument<void*>(args, 1);
#endif
        return Admits(bomb, pawn) ? Ignored : Supercede;
    } catch (...) {
        return Supercede;
    }
}

polyhook::ResultType PlantPre(polyhook::HookHandle, Args args, std::int32_t,
                            polyhook::ReturnHandle, polyhook::CallbackType) noexcept {
    if (!g_plantBlocked) {
        return Ignored;
    }
    try {
        auto* weapon = polyhook::GetArgument<void*>(args, 0);
        if (!weapon) {
            return Ignored;
        }
        std::int32_t owner{};
        std::memcpy(&owner, static_cast<const std::byte*>(weapon) + g_owner.offset, sizeof(owner));
        return IsPlayerPawn(owner) ? Supercede : Ignored;
    } catch (...) {
        return Ignored;
    }
}

Hook g_factory{SignatureKey{"FunRounds::C4Factory"}, Post, CapturePlant};
Hook g_select{SignatureKey{"FunRounds::C4DefuseSelect"}, Post, SelectPost};
Hook g_begin{SignatureKey{"FunRounds::C4DefuseBegin"}, Pre, BeginPre};
Hook g_plant{SignatureKey{"FunRounds::C4Plant"}, Pre, PlantPre};
TeamCompare g_selectCompare{SignatureKey{"FunRounds::C4DefuseSelectTeamCompare"}, OffsetKey{"FunRounds::C4DefuseSelectTeamCompare::Width"}};
TeamCompare g_beginCompare{SignatureKey{"FunRounds::C4DefuseBeginTeamCompare"}, OffsetKey{"FunRounds::C4DefuseBeginTeamCompare::Width"}};
bool g_defuseActive{};
bool g_mapEndRegistered{};

void OnMapEnd() noexcept {
    g_plantingTeams.clear();
}

bool HasHookImports() {
    return __polyhook_HookDetour && __polyhook_FindDetour && __polyhook_AddCallback &&
           __polyhook_RemoveCallback && __polyhook_IsCallbackRegistered &&
           __polyhook_AreCallbacksRegistered && __polyhook_UnhookDetour && __polyhook_GetArgumentPointer &&
           __s2sdk_OnMapEnd_Register && __s2sdk_OnMapEnd_Unregister;
}

bool StopDefuse() {
    const bool beginRestored = g_beginCompare.set(false);
    const bool selectRestored = g_selectCompare.set(false);
    if (!beginRestored || !selectRestored) {
        return false; // Keep admission hooks in place until both stock compares are back.
    }
    const bool beginRemoved = g_begin.remove();
    const bool selectRemoved = g_select.remove();
    const bool factoryRemoved = g_factory.remove();
    g_defuseActive = false;
    if (beginRemoved && selectRemoved && factoryRemoved) {
        g_plantingTeams.clear();
        if (g_mapEndRegistered) {
            s2sdk::OnMapEnd_Unregister(OnMapEnd);
            g_mapEndRegistered = false;
        }
        return true;
    }
    return false;
}

} // namespace

bool HasC4DefuseAccessSupport() {
    return HasHookImports() && __polyhook_GetReturnPointer && __polyhook_GetReturnBool &&
           __s2sdk_GetEntityClassname && EnsureEngine() &&
           ResolveSchemaField("CBaseEntity", "m_iTeamNum", sizeof(std::uint8_t), g_team) &&
           g_selectCompare.resolve(g_team) && g_beginCompare.resolve(g_team) &&
           ProbeSignature(g_factory.signature) &&
           ProbeSignature(g_select.signature) &&
           ProbeSignature(g_begin.signature);
}

bool HasC4PlantBlockSupport() {
    return HasHookImports() && EnsureEngine() &&
           ResolveSchemaField("CBaseEntity", "m_hOwnerEntity", sizeof(std::int32_t), g_owner) &&
           ProbeSignature(g_plant.signature);
}

bool SetC4DefuseAccess(bool enabled) {
    if (!enabled) {
        return StopDefuse();
    }
    if (g_defuseActive) {
        return true;
    }
#if defined(_WIN32)
    const plg::vector factoryArgs{Pointer, Pointer, Pointer, Pointer};
#else
    // SysV passes each Vector in an SSE eightbyte plus a float.
    const plg::vector factoryArgs{Pointer, Pointer, Double, Float, Double, Float};
#endif
    if (HasC4DefuseAccessSupport() && g_factory.install(Pointer, factoryArgs) &&
        g_select.install(Bool, {Pointer, Pointer, Pointer}) && g_begin.install(Void, {Pointer, Pointer}) &&
        g_selectCompare.set(true) && g_beginCompare.set(true)) {
        try {
            s2sdk::OnMapEnd_Register(OnMapEnd);
            g_mapEndRegistered = true;
        } catch (...) {
            (void)StopDefuse();
            return false;
        }
        g_defuseActive = true;
        return true;
    }
    (void)StopDefuse();
    return false;
}

bool SetC4PlantBlocked(bool blocked) {
    if (blocked) {
        if (!g_plantBlocked) {
            if (!HasC4PlantBlockSupport() || !g_plant.install(Void, {Pointer})) {
                (void)g_plant.remove();
                return false;
            }
            g_plantBlocked = true;
        }
        return true;
    }
    g_plantBlocked = false;
    return g_plant.remove();
}

bool ShutdownC4Access() noexcept {
    try {
        const bool plant = SetC4PlantBlocked(false);
        const bool defuse = StopDefuse();
        if (plant && defuse) {
            g_plantingTeams.clear();
            g_selectCompare = {g_selectCompare.signature, g_selectCompare.widthKey};
            g_beginCompare = {g_beginCompare.signature, g_beginCompare.widthKey};
        }
        return plant && defuse;
    } catch (...) {
        return false;
    }
}

} // namespace funrounds::native::detail

extern "C" PLUGIN_API bool HasC4DefuseAccessSupport() {
    try { return funrounds::native::detail::HasC4DefuseAccessSupport(); } catch (...) { return false; }
}
extern "C" PLUGIN_API bool HasC4PlantBlockSupport() {
    try { return funrounds::native::detail::HasC4PlantBlockSupport(); } catch (...) { return false; }
}
extern "C" PLUGIN_API bool SetC4DefuseAccess(bool enabled) {
    try { return funrounds::native::detail::SetC4DefuseAccess(enabled); } catch (...) { return false; }
}
extern "C" PLUGIN_API bool SetC4PlantBlocked(bool blocked) {
    try { return funrounds::native::detail::SetC4PlantBlocked(blocked); } catch (...) { return false; }
}
