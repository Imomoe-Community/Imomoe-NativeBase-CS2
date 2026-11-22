// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "airmove.cpp"

#include <array>
#include <cstdio>
#include <cstdlib>

namespace {
using polyhook::Callback;
using polyhook::CallbackType;
struct Pawn { std::uint32_t controller{100}; std::uint8_t life{}; std::int32_t health{100}; };
Pawn pawn;
CEntityInstance controller;
constexpr std::size_t call = 0x20, inputs = 0x50, velocities = 0x80, angles = 0x100;
std::array<std::byte, 0x180> image{};
bool yawSignature = true;
std::array<Callback, 2> callbacks{};
bool hooked{}, failPre{}, failUnhook{}, failRemove{};
bool foreignCallback{};
int hookInstalls{};
std::uint64_t buttons{};
int resolutions{};

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}

bool assist(int slot, int handle, int owner, float yaw, float vx, float vy, float& forward, float& left) {
    expect(slot == 0 && handle == 200 && owner == 100 && yaw == 35 && vx == 200 && vy == 75, "assist saw incorrect live identity or move layout");
    forward = 9;
    left = -8;
    return true;
}
bool otherAssist(int, int, int, float, float, float, float&, float&) { return true; }
}

namespace polyhook {
HookHandle HookDetour(void* address, DataType, std::initializer_list<DataType>, int, const char*) {
    hooked = true;
    ++hookInstalls;
    return address;
}
HookHandle FindDetour(void* address) { return hooked ? address : nullptr; }
bool UnhookDetour(void*) {
    if (failUnhook) return false;
    hooked = false;
    return true;
}
bool AddCallback(HookHandle, CallbackType phase, Callback cb) {
    if (phase == CallbackType::Pre && failPre) return false;
    callbacks[static_cast<std::size_t>(phase)] = cb;
    return true;
}
bool RemoveCallback(HookHandle, CallbackType phase, Callback) {
    if (failRemove) return false;
    callbacks[static_cast<std::size_t>(phase)] = nullptr;
    return true;
}
bool IsCallbackRegistered(HookHandle, CallbackType phase, Callback cb) {
    return callbacks[static_cast<std::size_t>(phase)] == cb;
}
bool AreCallbacksRegistered(HookHandle) { return foreignCallback || callbacks[0] || callbacks[1]; }
}

namespace s2sdk {
void* GetGameConfigSignature(ConfigId, const char* name) {
    const auto key = std::string_view(name);
    if (key == "FunRounds::AirMove") { ++resolutions; return image.data(); }
    if (key.ends_with("::AnglesCall")) return image.data() + call;
    if (key.ends_with("::InputLoads")) return image.data() + inputs;
    if (key.ends_with("::VelocityLoads")) return image.data() + velocities;
    if (key.ends_with("::AngleVectors") && yawSignature) return image.data() + angles;
    return nullptr;
}
int GetGameConfigOffset(ConfigId, const char* name) {
    const auto key = std::string_view(name);
    if (key.ends_with("::AnglesCallFromEntry")) return call;
    if (key.ends_with("::InputLoadsFromEntry")) return inputs;
    if (key.ends_with("::VelocityLoadsFromEntry")) return velocities;
    if (key.ends_with("::AngleVectorsRel32")) return 4;
    if (key.ends_with("::LeftDisp8")) return 1; // Scalar inputs.
    if (key.ends_with("Disp8")) return 0;      // Packed velocity.
    return -1;
}
void* GetClientPawn(int) { return &pawn; }
int GetMaxClients() { return 1; }
int PlayerSlotToEntHandle(int) { return 100; }
bool IsClientInGame(int) { return true; }
int GetClientFlags(int) { return 0; }
std::uint64_t GetClientButtons(int, int) { return buttons; }
MoveType GetEntityMoveType(int) { return MoveType::Walk; }
}

namespace funrounds::native::detail {
bool EnsureEngine() { return true; }
void* ProbeSignature(SignatureKey key, std::source_location) noexcept {
    return __s2sdk_GetGameConfigSignature ? s2sdk::GetGameConfigSignature(1, key.name().data()) : nullptr;
}
int ProbeOffset(OffsetKey key, std::source_location) noexcept {
    return s2sdk::GetGameConfigOffset(1, key.name().data());
}
bool ResolvePlayerPawnComponentChain(SchemaField& field, std::source_location) noexcept {
    field = {0, sizeof(CNetworkVarChainer)};
    return true;
}
bool ResolveSchemaField(const char*, const char* name, std::size_t size, SchemaField& field, std::source_location) noexcept {
    const std::string_view key{name};
    if (key == "m_hController") field = {offsetof(Pawn, controller), size};
    else if (key == "m_lifeState") field = {offsetof(Pawn, life), size};
    else if (key == "m_iHealth") field = {offsetof(Pawn, health), size};
    else return false;
    return true;
}
void InvalidGamedata(std::string_view, std::source_location) noexcept {}
void* EntityFromHandle(std::int32_t handle) noexcept { return handle == 100 ? &controller : nullptr; }
std::int32_t HandleFromEntity(void* p) noexcept { return p == &pawn ? 200 : -1; }
}

int main() {
    using namespace funrounds::native::detail;
    // Synthetic layout intentionally differs from the shipped CMoveData.
    image[call] = std::byte{8};
    image[angles] = std::byte{4};
    image[inputs] = std::byte{24};
    image[inputs + 1] = std::byte{28};
    image[velocities] = std::byte{36};
    constexpr std::int32_t displacement = angles - (call + 8);
    std::memcpy(image.data() + call + 4, &displacement, sizeof(displacement));
    expect(HasAirMoveSupport(), "reviewed move layout rejected");

    failPre = true;
    expect(!SetAirMoveAssist(true, assist) && !hooked && !polyhook::AreCallbacksRegistered(nullptr),
           "partial install did not roll back");
    failPre = false;
    expect(SetAirMoveAssist(true, assist), "could not attach");
    expect(!SetAirMoveAssist(true, otherAssist), "active AirMove accepted a different callback owner");

    CNetworkVarChainer services{&pawn};
    std::array<std::byte, 48> move{};
    WriteMoveAxis(move.data(), 12, 35);
    WriteMoveAxis(move.data(), 36, 200);
    WriteMoveAxis(move.data(), 40, 75);
    const auto forward = [&] { return Read<float>(move.data(), 24); };
    const auto left = [&] { return Read<float>(move.data(), 28); };
    void* args[] = {&services, move.data()};
    auto pre = [&] { callbacks[0](image.data(), args, 2, nullptr, CallbackType::Pre); };
    auto post = [&] { callbacks[1](image.data(), args, 2, nullptr, CallbackType::Post); };

    buttons = 1ULL << 9;
    pre();
    post();
    expect(forward() == 0 && left() == 0, "manual input was overridden");
    buttons = 0;
    pawn.life = 2;
    pre();
    post();
    expect(forward() == 0 && left() == 0, "dead pawn received input");
    pawn.life = 0;

    pre();
    expect(forward() == 9 && left() == -8, "native call did not get temporary axes");
    pre();
    post();
    expect(forward() == 9 && left() == -8, "nested call restored outer axes");
    expect(!ShutdownAirMove(), "detach succeeded while an edit was active");
    WriteMoveAxis(move.data(), 28, 4);
    post();
    expect(forward() == 0 && left() == 4, "did not restore exactly the owned axis");

    failRemove = true;
    expect(!ShutdownAirMove() && hooked, "failed callback removal lost the detour");
    failRemove = false;
    failUnhook = true;
    expect(!ShutdownAirMove() && hooked && !polyhook::AreCallbacksRegistered(nullptr), "failed unhook lost recovery state");
    failUnhook = false;
    expect(ShutdownAirMove() && !hooked, "shutdown retry did not detach");
    const auto exclusiveInstalls = hookInstalls;
    expect(SetAirMoveAssist(true, assist) && hookInstalls == exclusiveInstalls + 1,
           "exclusive detach did not permit a fresh activation");
    foreignCallback = true;
    expect(ShutdownAirMove() && hooked && foreignCallback, "shared detach did not preserve the foreign detour");
    const auto sharedInstalls = hookInstalls;
    expect(SetAirMoveAssist(true, assist) && hookInstalls == sharedInstalls,
           "shared detour reactivation did not reuse its known handle");
    expect(ShutdownAirMove() && hooked, "second shared detach did not retain the foreign detour");
    foreignCallback = false;
    hooked = false; // The final foreign owner now removes the process-wide detour.
    expect(SetAirMoveAssist(true, assist) && hookInstalls == sharedInstalls + 1,
           "stale shared handle prevented fresh activation after foreign detach");
    expect(ShutdownAirMove() && !hooked, "final exclusive detach did not unhook");
    image[call + 4] ^= std::byte{1};
    expect(!HasAirMoveSupport() && resolutions == 3, "shutdown reused stale layout or accepted a different AngleVectors destination");
    image[call + 4] ^= std::byte{1};
    yawSignature = false;
    expect(!HasAirMoveSupport() && resolutions == 4, "missing yaw signature did not disable AirMove");
}
