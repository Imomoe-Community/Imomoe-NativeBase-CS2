// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <cstdlib>
#include <iostream>
#include <source_location>

// Exercise the production resolvers without adding test exports to the plugin ABI.
#include "interactions.cpp"

namespace plg {
decltype(Log) Log = nullptr;
decltype(BeginZone) BeginZone = nullptr;
decltype(EndZone) EndZone = nullptr;
} // namespace plg

namespace {

int g_services{};
struct PawnFixture { std::int32_t controller{}; void* services{}; };
PawnFixture g_pawn{0, &g_services};
int g_weapon{};
constexpr std::int32_t kPawnHandle = 200;
constexpr std::int32_t kWeaponHandle = 300;
std::int32_t g_controller = 102;
bool g_controllerValid = true;
bool g_schemaAvailable = true;
bool g_pawnValid = true;
bool g_serviceMatches = true;
bool g_weaponOwned = true;
std::uint32_t g_inventorySlots{};

void Require(bool condition, const std::source_location& location = std::source_location::current()) {
    if (!condition) {
        std::cerr << "interaction owner check failed at " << location.line() << '\n';
        std::exit(1);
    }
}

std::int32_t MaxClients() { return 8; }
bool InGame(std::int32_t slot) { return slot == 2 || slot == 5; }
void* Pawn(std::int32_t slot) {
    if (!InGame(slot)) return nullptr;
    // Model the cached raw m_hController field read used by production.
    g_pawn.controller = g_controller;
    g_pawn.services = g_serviceMatches ? &g_services : nullptr;
    return &g_pawn;
}
std::int32_t SlotController(std::int32_t slot) { return 100 + slot; }
bool ValidHandle(std::int32_t handle) {
    return (g_pawnValid && handle == kPawnHandle) || handle == kWeaponHandle ||
           (g_controllerValid && (handle == 102 || handle == 105));
}
std::int32_t SchemaOffset(const plg::string&, const plg::string& field) {
    if (!g_schemaAvailable) return -1;
    if (field == "m_hController") return offsetof(PawnFixture, controller);
    if (field == "m_pUseServices") return offsetof(PawnFixture, services);
    return -1;
}
std::int32_t ConfigOffset(s2sdk::ConfigId, const plg::string&) { return 0; }
std::int64_t Schema(void* pawn, const plg::string&, const plg::string& field, std::int32_t) {
    return pawn == &g_pawn && field == "m_pUseServices" && g_serviceMatches
        ? static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(&g_services)) : 0;
}
std::int32_t SchemaHandle(void* pawn, const plg::string& type, const plg::string& field, std::int32_t) {
    return pawn == &g_pawn && type == "CBasePlayerPawn" && field == "m_hController" ? g_controller : -1;
}
plg::string Classname(std::int32_t handle) { return handle == kWeaponHandle ? "weapon_healthshot" : "player"; }
plg::vector<std::int32_t> Weapons(std::int32_t slot) {
    g_inventorySlots |= std::uint32_t{1} << slot;
    return g_weaponOwned ? plg::vector<std::int32_t>{kWeaponHandle} : plg::vector<std::int32_t>{};
}

void CheckRejected() {
    using namespace funrounds::native::detail;
    std::int32_t pawn = -1;
    std::int32_t slot = -1;
    std::int32_t weapon = -1;
    Require(!ResolveUseServiceOwner(&g_services, pawn, slot));
    Require(!ResolveHealthshotOwner(&g_weapon, &g_pawn, pawn, weapon));
}

} // namespace

namespace funrounds::native::detail {

bool EnsureEngine() { return true; }
void* ProbeSignature(SignatureKey key, std::source_location) noexcept {
    return __s2sdk_GetGameConfigSignature ? s2sdk::GetGameConfigSignature(1, plg::string(key.name().data(), key.name().size())) : nullptr;
}
int ProbeOffset(OffsetKey key, std::source_location) noexcept {
    return s2sdk::GetGameConfigOffset(1, plg::string(key.name().data(), key.name().size()));
}
void MissingSchema(std::string_view, std::string_view, std::source_location) noexcept {}
int ProbeSchemaOffset(std::string_view className, std::string_view field, bool, std::source_location) noexcept {
    return s2sdk::GetSchemaOffset(plg::string(className.data(), className.size()), plg::string(field.data(), field.size()));
}
void* EntityFromHandle(std::int32_t handle) noexcept {
    if (handle == kPawnHandle && g_pawnValid) { return &g_pawn; }
    return handle == kWeaponHandle ? &g_weapon : nullptr;
}
std::int32_t HandleFromEntity(void* entity) noexcept {
    if (entity == &g_pawn) { return kPawnHandle; }
    return entity == &g_weapon ? kWeaponHandle : -1;
}
bool IsPlayerPawn(std::int32_t handle) noexcept { return handle == kPawnHandle && g_pawnValid; }

} // namespace funrounds::native::detail

int main() {
    __s2sdk_GetMaxClients = MaxClients;
    __s2sdk_IsClientInGame = InGame;
    __s2sdk_GetClientPawn = Pawn;
    __s2sdk_PlayerSlotToEntHandle = SlotController;
    __s2sdk_IsValidEntHandle = ValidHandle;
    __s2sdk_GetSchemaOffset = SchemaOffset;
    __s2sdk_GetGameConfigOffset = ConfigOffset;
    __s2sdk_GetEntSchema2 = Schema;
    __s2sdk_GetEntSchemaEnt2 = SchemaHandle;
    __s2sdk_GetEntityClassname = Classname;
    __s2sdk_GetClientWeapons = Weapons;

    using namespace funrounds::native::detail;
    // Healthshot ownership has its own controller-schema lookup; it must not
    // depend on UseTarget metadata having been initialized first.
    Require(g_useTarget.committedTargetOffset == -1 && g_useTarget.schemaCount == 0);
    for (const auto ownerSlot : {2, 5}) {
        g_controller = 100 + ownerSlot;
        std::int32_t healthshotPawn = -1;
        std::int32_t healthshotWeapon = -1;
        g_inventorySlots = 0;
        Require(ResolveHealthshotOwner(&g_weapon, &g_pawn, healthshotPawn, healthshotWeapon));
        Require(healthshotPawn == kPawnHandle && healthshotWeapon == kWeaponHandle);
        Require(g_inventorySlots == (std::uint32_t{1} << ownerSlot));
    }
    Require(g_useTarget.committedTargetOffset == -1 && g_useTarget.schemaCount == 0);

    for (const auto ownerSlot : {2, 5}) {
        g_controller = 100 + ownerSlot;
        std::int32_t pawn = -1;
        std::int32_t slot = -1;
        std::int32_t weapon = -1;
        Require(ResolveUseServiceOwner(&g_services, pawn, slot));
        Require(pawn == kPawnHandle && slot == ownerSlot);
        g_inventorySlots = 0;
        Require(ResolveHealthshotOwner(&g_weapon, &g_pawn, pawn, weapon));
        Require(pawn == kPawnHandle && weapon == kWeaponHandle);
        Require(g_inventorySlots == (std::uint32_t{1} << ownerSlot));
    }

    // Closing a UseTarget state warmed by the owner resolver clears its cache;
    // a subsequent healthshot lookup must still resolve independently.
    Require(g_useTarget.committedTargetOffset >= 0 && g_useTarget.schemaCount > 0);
    Require(RemoveUseTargetHook());
    Require(g_useTarget.committedTargetOffset == -1 && g_useTarget.schemaCount == 0);
    g_controller = 105;
    std::int32_t healthshotPawnAfterClose = -1;
    std::int32_t healthshotWeaponAfterClose = -1;
    g_inventorySlots = 0;
    Require(ResolveHealthshotOwner(&g_weapon, &g_pawn, healthshotPawnAfterClose, healthshotWeaponAfterClose));
    Require(healthshotPawnAfterClose == kPawnHandle && healthshotWeaponAfterClose == kWeaponHandle);
    Require(g_inventorySlots == (std::uint32_t{1} << 5));
    Require(g_useTarget.committedTargetOffset == -1 && g_useTarget.schemaCount == 0);

    g_controller = -1;
    CheckRejected();
    g_controller = 107; // A stale third controller owns neither of the candidate slots.
    CheckRejected();
    g_controller = 102;
    g_controllerValid = false;
    CheckRejected();
    g_controllerValid = true;
    Require(RemoveUseTargetHook()); // Reset startup metadata before simulating absent schema.
    g_schemaAvailable = false;
    CheckRejected();
    g_schemaAvailable = true;
    g_pawnValid = false;
    CheckRejected();
    g_pawnValid = true;
    g_serviceMatches = false;
    std::int32_t pawn = -1;
    std::int32_t slot = -1;
    std::int32_t weapon = -1;
    Require(!ResolveUseServiceOwner(&g_services, pawn, slot));
    g_serviceMatches = true;
    g_weaponOwned = false;
    Require(!ResolveHealthshotOwner(&g_weapon, &g_pawn, pawn, weapon));
    Require(!ResolveHealthshotOwner(&g_services, &g_pawn, pawn, weapon));
    std::cout << "production interaction owner resolvers passed\n";
}
