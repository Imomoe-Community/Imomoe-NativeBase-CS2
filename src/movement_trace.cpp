// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "movement_trace.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(_WIN32)
#include <Windows.h>
#elif defined(__linux__)
#include <dlfcn.h>
#include <link.h>
#else
#error "Player movement hull tracing is supported on Windows x64 and Linux x64 only"
#endif

#include <cmodel.h>
#include <bspflags.h>
#include <const.h>
#include <entity2/entityinstance.h>
#include <gametrace.h>
#include <plugin_export.h>
#include <s2sdk/s2sdk.hpp>

#include "engine.hpp"
#include "entity_handle.hpp"
#if defined(__linux__)
#include "linux_image.hpp"
#endif

namespace funrounds::native::detail {
namespace {

constexpr auto kSignature = SignatureKey{"FunRounds::PlayerMovementTraceShape"};
constexpr auto kCaller = SignatureKey{"FunRounds::PlayerMovementTraceShape::ReceiverCall"};
constexpr std::int32_t kInvalidHandle = -1;

using TraceShapeFn = bool (*)(void*, const Ray_t&, const Vector&, const Vector&, CTraceFilter*, CGameTrace*);

struct TraceSchema {
    SchemaField pawnCollision{};
    SchemaField pawnOwner{};
    SchemaField collisionAttributes{};
    SchemaField interactsAs{};
    SchemaField interactsExclude{};
    bool ready{};
};

struct Query {
    TraceShapeFn call{};
    void* receiver{};

    explicit operator bool() const noexcept {
        return call != nullptr && receiver != nullptr;
    }
};

struct QueryAddresses {
    TraceShapeFn call{};
    void* const* receiverSlot{};
    void* expectedReceiver{};

    explicit operator bool() const noexcept {
        return call != nullptr && receiverSlot != nullptr && expectedReceiver != nullptr;
    }
};

TraceSchema g_traceSchema;
QueryAddresses g_queryAddresses;

static_assert(sizeof(Ray_t) == 0x30);
static_assert(offsetof(Ray_t, m_eType) == 0x28);
static_assert(sizeof(CGameTrace) == 0xC0);
static_assert(offsetof(CGameTrace, m_pEnt) == 0x08);
static_assert(offsetof(CGameTrace, m_vStartPos) == 0x78);
static_assert(offsetof(CGameTrace, m_vEndPos) == 0x84);
static_assert(offsetof(CGameTrace, m_vHitNormal) == 0x90);
static_assert(offsetof(CGameTrace, m_flFraction) == 0xAC);
static_assert(offsetof(CGameTrace, m_eRayType) == 0xBA);
static_assert(offsetof(CGameTrace, m_bStartInSolid) == 0xBB);
static_assert(offsetof(CTraceFilter, m_nInteractsWith) == 0x08);
#if defined(_WIN32)
static_assert(sizeof(CTraceFilter) == 0x48);
static_assert(offsetof(CTraceFilter, m_bIterateEntities) == 0x40);
#else
// Itanium reuses RnQueryAttr_t tail padding; this is the observed Linux layout.
static_assert(sizeof(CTraceFilter) == 0x40);
static_assert(offsetof(CTraceFilter, m_bIterateEntities) == 0x3A);
#endif

bool Finite(const Vector& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

const void* ImageBase(const void* address) noexcept {
#if defined(_WIN32)
    MEMORY_BASIC_INFORMATION memory{};
    return VirtualQuery(address, &memory, sizeof(memory)) == sizeof(memory) && memory.Type == MEM_IMAGE
        ? memory.AllocationBase : nullptr;
#else
    Dl_info info{};
    return dladdr(address, &info) != 0 ? info.dli_fbase : nullptr;
#endif
}

bool ReadableInImage(const void* base, const void* address, std::size_t size) noexcept {
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    if (!base || !address || size == 0 || start < reinterpret_cast<std::uintptr_t>(base)) {
        return false;
    }
#if defined(_WIN32)
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(address, &memory, sizeof(memory)) != sizeof(memory) ||
        memory.AllocationBase != base || memory.State != MEM_COMMIT ||
        (memory.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
        ((memory.Protect & 0xffU) != PAGE_READONLY && (memory.Protect & 0xffU) != PAGE_READWRITE &&
         (memory.Protect & 0xffU) != PAGE_WRITECOPY && (memory.Protect & 0xffU) != PAGE_EXECUTE_READ &&
         (memory.Protect & 0xffU) != PAGE_EXECUTE_READWRITE && (memory.Protect & 0xffU) != PAGE_EXECUTE_WRITECOPY)) {
        return false;
    }
    const auto offset = start - reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
    return size <= memory.RegionSize && offset <= memory.RegionSize - size;
#else
    struct Range { std::uintptr_t base, start; std::size_t size; bool readable; };
    Range range{reinterpret_cast<std::uintptr_t>(base), start, size, false};
    dl_iterate_phdr([](dl_phdr_info* info, std::size_t, void* context) {
        auto& wanted = *static_cast<Range*>(context);
        if (info->dlpi_addr != wanted.base) {
            return 0;
        }
        wanted.readable = ContainsReadableImageRange(info->dlpi_phdr, info->dlpi_phnum,
            wanted.start - wanted.base, wanted.size);
        return 1;
    }, &range);
    return range.readable;
#endif
}

const void* RipTarget(const std::byte* displacement) noexcept {
    std::int32_t relative{};
    std::memcpy(&relative, displacement, sizeof(relative));
    return reinterpret_cast<const void*>(reinterpret_cast<std::intptr_t>(displacement + sizeof(relative)) + relative);
}

const void* ReceiverSlotFromCallsite(const std::byte* instruction) noexcept {
#if defined(_WIN32)
    // First argument comes directly from the RIP-relative slot.
    constexpr unsigned char opcode[] = {0x48, 0x8B, 0x0D}; // MOV RCX, [RIP+disp32]
#else
    // The caller takes the slot address, then loads its value into RDI.
    constexpr unsigned char opcode[] = {0x48, 0x8D, 0x05}; // LEA RAX, [RIP+disp32]
#endif
    return std::memcmp(instruction, opcode, sizeof(opcode)) == 0
        ? RipTarget(instruction + sizeof(opcode)) : nullptr;
}

Query ResolveQuery() noexcept {
    if (!g_queryAddresses) {
        if (!__s2sdk_GetGameConfigSignature || !EnsureEngine()) {
            return {};
        }

        try {
            void* const signature = ProbeSignature(kSignature);
            const auto* caller = static_cast<const std::byte*>(ProbeSignature(kCaller));
            const auto* base = signature ? ImageBase(signature) : nullptr;
            constexpr auto callOperand = OffsetKey{"FunRounds::PlayerMovementTraceShape::CallOffset"};
            const auto callOffset = ProbeOffset(callOperand);
            if (!signature || !caller || callOffset < 0) return {};
            if (!base || callOffset < 7 ||
                !ReadableInImage(base, caller, static_cast<std::size_t>(callOffset) + 5) ||
                caller[callOffset] != std::byte{0xE8} || RipTarget(caller + callOffset + 1) != signature) {
                InvalidGamedata(callOperand.name());
                return {};
            }

            const auto* slot = static_cast<void* const*>(ReceiverSlotFromCallsite(caller));
            if (!ReadableInImage(base, slot, sizeof(*slot))) {
                InvalidGamedata(kCaller.name());
                return {};
            }
            if (!ReadableInImage(base, *slot, sizeof(void*))) {
                return {};
            }

            g_queryAddresses = {reinterpret_cast<TraceShapeFn>(signature), slot, *slot};
        } catch (...) {
            return {};
        }
    }

    const QueryAddresses addresses = g_queryAddresses;
    // Cache only stable addresses; the receiver slot and its first field remain
    // live values and must pass these checks on every use.
    void* const receiver = *addresses.receiverSlot;
    if (receiver != addresses.expectedReceiver || !*reinterpret_cast<void**>(receiver)) {
        return {};
    }
    return {addresses.call, receiver};
}

bool ResolveTraceSchema() noexcept {
    if (g_traceSchema.ready) {
        return true;
    }

    TraceSchema resolved{};
    if (!ResolveSchemaField("CBaseEntity", "m_pCollision", sizeof(void*), resolved.pawnCollision) ||
        !ResolveSchemaField("CBaseEntity", "m_hOwnerEntity", sizeof(std::uint32_t), resolved.pawnOwner) ||
        !ResolveSchemaField("CCollisionProperty", "m_collisionAttribute", 0, resolved.collisionAttributes) ||
        !ResolveSchemaField("VPhysicsCollisionAttribute_t", "m_nInteractsAs", sizeof(std::uint64_t), resolved.interactsAs) ||
        !ResolveSchemaField("VPhysicsCollisionAttribute_t", "m_nInteractsExclude", sizeof(std::uint64_t), resolved.interactsExclude)) {
        return false;
    }

    g_traceSchema = resolved;
    g_traceSchema.ready = true;
    return true;
}

template <typename T>
T ReadField(const void* entity, std::int32_t offset) noexcept {
    T value{};
    if (entity && offset >= 0) {
        std::memcpy(&value, static_cast<const std::byte*>(entity) + offset, sizeof(T));
    }
    return value;
}

void InitializeMiss(const Vector& end,
                    Vector& outPosition,
                    float& outFraction,
                    bool& outHit,
                    std::int32_t& outEntityHandle,
                    bool& outStartSolid,
                    Vector& outNormal) noexcept {
    outPosition = end;
    outFraction = 1.0F;
    outHit = false;
    outEntityHandle = kInvalidHandle;
    outStartSolid = false;
    outNormal.Init(0.0F, 0.0F, 0.0F);
}

bool TracePlayerMovementHullImpl(std::int32_t pawnHandle,
                                 const Vector& start,
                                 const Vector& end,
                                 const Vector& hullMins,
                                 const Vector& hullMaxs,
                                 Vector& outPosition,
                                 float& outFraction,
                                 bool& outHit,
                                 std::int32_t& outEntityHandle,
                                 bool& outStartSolid,
                                 Vector& outNormal) {
    InitializeMiss(end, outPosition, outFraction, outHit, outEntityHandle, outStartSolid, outNormal);

    if (!EnsureEngine() || pawnHandle == kInvalidHandle || !Finite(start) || !Finite(end) || !Finite(hullMins) || !Finite(hullMaxs) ||
        hullMins.x > hullMaxs.x || hullMins.y > hullMaxs.y || hullMins.z > hullMaxs.z ||
        !__s2sdk_EntPointerToEntHandle || !__s2sdk_EntHandleToEntPointer || !IsPlayerPawn(pawnHandle)) {
        return false;
    }

    void* const pawn = EntityFromHandle(pawnHandle);
    if (!pawn || HandleFromEntity(pawn) != pawnHandle || EntityFromHandle(pawnHandle) != pawn) {
        return false;
    }
    if (!ResolveTraceSchema()) {
        return false;
    }

    void* const collision = ReadField<void*>(pawn, g_traceSchema.pawnCollision.offset);
    if (!collision) {
        return false;
    }
    const auto collisionAttributes = static_cast<const std::byte*>(collision) + g_traceSchema.collisionAttributes.offset;
    const auto interactsAs = ReadField<std::uint64_t>(collisionAttributes, g_traceSchema.interactsAs.offset);
    const auto interactsExclude = ReadField<std::uint64_t>(collisionAttributes, g_traceSchema.interactsExclude.offset);
    const auto ownerHandle = ToApiHandle(ReadField<std::uint32_t>(pawn, g_traceSchema.pawnOwner.offset));
    void* owner = ownerHandle == kInvalidHandle ? nullptr : EntityFromHandle(ownerHandle);
    if (owner && HandleFromEntity(owner) != ownerHandle) {
        owner = nullptr;
    }

    const Query query = ResolveQuery();
    if (!query) {
        return false;
    }

    Ray_t ray(hullMins, hullMaxs);
    CTraceFilter filter(MASK_PLAYERSOLID, COLLISION_GROUP_PLAYER_MOVEMENT, false);
    auto* const pawnEntity = static_cast<CEntityInstance*>(pawn);
    filter.SetPassEntity1(pawnEntity);
    filter.SetPassEntityOwner1(static_cast<CEntityInstance*>(owner));
    filter.m_nInteractsAs = interactsAs;
    filter.m_nInteractsExclude = interactsExclude;
    filter.m_bHitSolidRequiresGenerateContacts = true;

    CGameTrace trace{};
    (void)query.call(query.receiver, ray, start, end, &filter, &trace);
    if (!Finite(trace.m_vEndPos) || !Finite(trace.m_vHitNormal) || !std::isfinite(trace.m_flFraction) ||
        trace.m_flFraction < 0.0F || trace.m_flFraction > 1.0F) {
        return false;
    }

    outPosition = trace.m_vEndPos;
    outFraction = trace.m_flFraction;
    outHit = trace.DidHit();
    outStartSolid = trace.m_bStartInSolid;
    outNormal = trace.m_vHitNormal;
    if (outHit) {
        if (!trace.m_pEnt) {
            InitializeMiss(end, outPosition, outFraction, outHit, outEntityHandle, outStartSolid, outNormal);
            return false;
        }
        const std::int32_t hitHandle = HandleFromEntity(trace.m_pEnt);
        void* const resolved = hitHandle == kInvalidHandle ? nullptr : EntityFromHandle(hitHandle);
        if (resolved == trace.m_pEnt && HandleFromEntity(resolved) == hitHandle) {
            outEntityHandle = hitHandle;
        } else {
            InitializeMiss(end, outPosition, outFraction, outHit, outEntityHandle, outStartSolid, outNormal);
            return false;
        }
    }

    return true;
}

} // namespace

void ResetMovementTraceMetadata() noexcept {
    g_traceSchema = {};
    g_queryAddresses = {};
}

} // namespace funrounds::native::detail

extern "C" PLUGIN_API bool HasPlayerMovementHullSupport() {
    try {
        return funrounds::native::detail::EnsureEngine() &&
               funrounds::native::detail::ResolveTraceSchema() &&
               static_cast<bool>(funrounds::native::detail::ResolveQuery());
    } catch (...) {
        return false;
    }
}

extern "C" PLUGIN_API bool TracePlayerMovementHull(std::int32_t pawnHandle,
                                                   const Vector& start,
                                                   const Vector& end,
                                                   const Vector& hullMins,
                                                   const Vector& hullMaxs,
                                                   Vector& outPosition,
                                                   float& outFraction,
                                                   bool& outHit,
                                                   std::int32_t& outEntityHandle,
                                                   bool& outStartSolid,
                                                   Vector& outNormal) {
    try {
        return funrounds::native::detail::TracePlayerMovementHullImpl(pawnHandle, start, end, hullMins, hullMaxs,
                                                                      outPosition, outFraction, outHit,
                                                                      outEntityHandle, outStartSolid, outNormal);
    } catch (...) {
        funrounds::native::detail::InitializeMiss(end, outPosition, outFraction, outHit, outEntityHandle,
                                                  outStartSolid, outNormal);
        return false;
    }
}
