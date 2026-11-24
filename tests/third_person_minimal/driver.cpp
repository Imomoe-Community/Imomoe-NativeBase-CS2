// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>

#include <plg/plugin.hpp>
#include <plugin_export.h>
#include <imomoe_native_base/imomoe_native_base.hpp>
#include <s2sdk/s2sdk.hpp>
#include "abi_probe.hpp"

namespace {

constexpr char kCommand[] = "fr_third_person_minimal";
constexpr char kCameraClass[] = "CCSCustomPlayerCamera";
constexpr char kCameraServiceClass[] = "CPlayer_CameraServices";
constexpr std::int64_t kFollowPositionMode = 3;
constexpr float kDefaultDistance = 64.0F;
constexpr float kMinimumDistance = 15.0F;
constexpr float kMaximumDistance = 150.0F;

struct Activation {
    std::int32_t pawnHandle{-1};
    std::int32_t cameraHandle{-1};
    std::int32_t previousViewHandle{-1};
    float distance{};
    bool eyeRegistrationPending{};
    std::int32_t controlCameraHandle{-1};
};

std::map<std::int32_t, Activation> g_active;
bool g_commandRegistered{};

bool ParseInt(const plg::string& input, std::int32_t& value) {
    const char* const begin = input.c_str();
    if (begin == nullptr || *begin == '\0') {
        return false;
    }
    const char* const end = begin + std::strlen(begin);
    const auto result = std::from_chars(begin, end, value);
    return result.ec == std::errc{} && result.ptr == end;
}

std::optional<float> ParseDistance(const plg::string& input) {
    const char* const begin = input.c_str();
    if (begin == nullptr || *begin == '\0') {
        return std::nullopt;
    }
    char* end{};
    const float value = std::strtof(begin, &end);
    if (end == begin || *end != '\0' || !std::isfinite(value)
        || value < kMinimumDistance || value > kMaximumDistance) {
        return std::nullopt;
    }
    return value;
}

void* CameraServices(void* pawn) {
    if (pawn == nullptr) {
        return nullptr;
    }
    const std::int32_t offset = s2sdk::GetSchemaOffset("CBasePlayerPawn", "m_pCameraServices");
    if (offset < 0) {
        return nullptr;
    }
    void* service{};
    const auto* const source = static_cast<const std::byte*>(pawn) + offset;
    std::memcpy(&service, source, sizeof(service));
    return service;
}

std::int32_t PawnHandle(std::int32_t slot) {
    if (slot < 0 || slot >= s2sdk::GetMaxClients() || !s2sdk::IsClientInGame(slot)) {
        return -1;
    }
    void* const pawn = s2sdk::GetClientPawn(slot);
    if (pawn == nullptr) {
        return -1;
    }
    const std::int32_t handle = s2sdk::EntPointerToEntHandle(pawn);
    return s2sdk::IsValidEntHandle(handle) ? handle : -1;
}

bool HasCameraSchema() {
    constexpr std::array fields{
        "m_hPawn", "m_nCameraMode", "m_hFollowEntity", "m_bFollowEyes",
        "m_vecFollowOffset", "m_vecCameraOffset", "m_bClipCameraOffset",
        "m_flCameraOffsetReturnStrength",
    };
    for (const char* field : fields) {
        if (s2sdk::GetSchemaOffset(kCameraClass, field) < 0) {
            return false;
        }
    }
    return s2sdk::GetSchemaOffset("CBasePlayerPawn", "m_pCameraServices") >= 0
        && s2sdk::GetSchemaOffset(kCameraServiceClass, "m_hViewEntity") >= 0;
}

void* PawnPointerForHandle(std::int32_t expectedHandle) {
    if (!s2sdk::IsValidEntHandle(expectedHandle)) {
        return nullptr;
    }
    void* const pawn = s2sdk::EntHandleToEntPointer(expectedHandle);
    if (pawn == nullptr || s2sdk::EntPointerToEntHandle(pawn) != expectedHandle) {
        return nullptr;
    }
    return pawn;
}

void* CurrentPawnPointer(std::int32_t slot, std::int32_t expectedHandle) {
    if (PawnHandle(slot) != expectedHandle) {
        return nullptr;
    }
    return s2sdk::GetClientPawn(slot);
}

std::int32_t CurrentViewHandle(void* pawn) {
    void* const services = CameraServices(pawn);
    if (services == nullptr) {
        return std::numeric_limits<std::int32_t>::min();
    }
    return s2sdk::GetEntSchemaEnt2(services, kCameraServiceClass, "m_hViewEntity", 0);
}

std::optional<bool> CameraReferencedByConnectedPlayer(std::int32_t cameraHandle) {
    for (std::int32_t slot = 0; slot < s2sdk::GetMaxClients(); ++slot) {
        if (!s2sdk::IsClientInGame(slot)) {
            continue;
        }
        void* const pawn = s2sdk::GetClientPawn(slot);
        if (pawn == nullptr) {
            continue;
        }
        const std::int32_t view = CurrentViewHandle(pawn);
        if (view == std::numeric_limits<std::int32_t>::min()) {
            return std::nullopt;
        }
        if (view == cameraHandle) {
            return true;
        }
    }
    return false;
}

bool CameraSettingsMatch(void* camera, std::int32_t pawnHandle, float distance) {
    if (camera == nullptr) {
        return false;
    }
    const plg::vec3 actualFollow = s2sdk::GetEntSchemaVector3D2(camera, kCameraClass, "m_vecFollowOffset", 0);
    const plg::vec3 actualCamera = s2sdk::GetEntSchemaVector3D2(camera, kCameraClass, "m_vecCameraOffset", 0);
    return s2sdk::GetEntSchemaEnt2(camera, kCameraClass, "m_hPawn", 0) == pawnHandle
        && s2sdk::GetEntSchema2(camera, kCameraClass, "m_nCameraMode", 0) == kFollowPositionMode
        && s2sdk::GetEntSchemaEnt2(camera, kCameraClass, "m_hFollowEntity", 0) == pawnHandle
        && s2sdk::GetEntSchema2(camera, kCameraClass, "m_bFollowEyes", 0) != 0
        && actualFollow.x == 0.0F && actualFollow.y == 0.0F && actualFollow.z == 0.0F
        && actualCamera.x == -distance && actualCamera.y == 0.0F && actualCamera.z == 0.0F
        && s2sdk::GetEntSchema2(camera, kCameraClass, "m_bClipCameraOffset", 0) != 0
        && std::abs(s2sdk::GetEntSchemaFloat2(camera, kCameraClass, "m_flCameraOffsetReturnStrength", 0) - 1.0) < 0.001;
}

bool ConfigureCamera(void* camera, std::int32_t pawnHandle, float distance) {
    if (camera == nullptr) {
        return false;
    }
    const plg::vec3 followOffset{};
    const plg::vec3 cameraOffset{-distance, 0.0F, 0.0F};
    s2sdk::SetEntSchemaEnt2(camera, kCameraClass, "m_hPawn", pawnHandle, true, 0);
    s2sdk::SetEntSchema2(camera, kCameraClass, "m_nCameraMode", kFollowPositionMode, true, 0);
    s2sdk::SetEntSchemaEnt2(camera, kCameraClass, "m_hFollowEntity", pawnHandle, true, 0);
    s2sdk::SetEntSchema2(camera, kCameraClass, "m_bFollowEyes", 1, true, 0);
    s2sdk::SetEntSchemaVector3D2(camera, kCameraClass, "m_vecFollowOffset", followOffset, true, 0);
    s2sdk::SetEntSchemaVector3D2(camera, kCameraClass, "m_vecCameraOffset", cameraOffset, true, 0);
    s2sdk::SetEntSchema2(camera, kCameraClass, "m_bClipCameraOffset", 1, true, 0);
    s2sdk::SetEntSchemaFloat2(camera, kCameraClass, "m_flCameraOffsetReturnStrength", 1.0, true, 0);
    return CameraSettingsMatch(camera, pawnHandle, distance);
}

bool RemoveCameraIfUnreferenced(Activation& activation) {
    const auto referenced = CameraReferencedByConnectedPlayer(activation.cameraHandle);
    if (!referenced.has_value() || *referenced) {
        return false;
    }
    if (activation.controlCameraHandle != -1) {
        const auto controlReferenced = CameraReferencedByConnectedPlayer(activation.controlCameraHandle);
        if (!controlReferenced.has_value() || *controlReferenced) { return false; }
        if (s2sdk::IsValidEntHandle(activation.controlCameraHandle)) {
            s2sdk::RemoveEntity(activation.controlCameraHandle);
            if (s2sdk::IsValidEntHandle(activation.controlCameraHandle)) { return false; }
        }
        activation.controlCameraHandle = -1;
    }
    if (activation.eyeRegistrationPending) {
        if (!imomoe_native_base::UnregisterThirdPersonEyeTarget(activation.pawnHandle, activation.cameraHandle)) {
            return false;
        }
        activation.eyeRegistrationPending = false;
    }
    if (!s2sdk::IsValidEntHandle(activation.cameraHandle)) {
        return true;
    }
    s2sdk::RemoveEntity(activation.cameraHandle);
    // Removal can be deferred; keep the handle so stop can confirm it on a later tick.
    return !s2sdk::IsValidEntHandle(activation.cameraHandle);
}

bool StopSlot(std::int32_t slot, std::string& result) {
    auto found = g_active.find(slot);
    if (found == g_active.end()) {
        result = "active=false";
        return true;
    }
    Activation& activation = found->second;
    void* const pawn = PawnPointerForHandle(activation.pawnHandle);
    if (pawn == nullptr) {
        if (RemoveCameraIfUnreferenced(activation)) {
            g_active.erase(found);
            result = "active=false restored=false reason=captured_pawn_invalid camera_removed=true";
            return true;
        }
        result = "restore=false reason=captured_pawn_invalid_camera_referenced_or_removal_pending recovery=retained";
        return false;
    }
    void* const services = CameraServices(pawn);
    if (services == nullptr) {
        result = "restore=false reason=camera_services_unavailable recovery=retained";
        return false;
    }

    const std::int32_t currentView = CurrentViewHandle(pawn);
    if (currentView == std::numeric_limits<std::int32_t>::min()) {
        result = "restore=false reason=view_handle_unavailable recovery=retained";
        return false;
    }
    if (currentView == activation.cameraHandle || currentView == activation.controlCameraHandle) {
        s2sdk::SetEntSchemaEnt2(services, kCameraServiceClass, "m_hViewEntity",
                                activation.previousViewHandle, true, 0);
        if (CurrentViewHandle(pawn) != activation.previousViewHandle) {
            result = "restore=false reason=view_restore_readback_mismatch recovery=retained";
            return false;
        }
    }

    if (RemoveCameraIfUnreferenced(activation)) {
        result = "active=false restored=" + std::to_string(currentView == activation.cameraHandle)
            + " previous_view=" + std::to_string(activation.previousViewHandle)
            + " camera_removed=true";
        g_active.erase(found);
        return true;
    }
    result = "restore=" + std::to_string(currentView == activation.cameraHandle)
        + " camera_removed=false reason=still_referenced_or_removal_pending recovery=retained";
    return false;
}

bool StartSlot(std::int32_t slot, float distance, std::string& result) {
    if (g_active.contains(slot)) {
        result = "start=false reason=active_or_recovery_pending";
        return false;
    }
    if (slot < 0 || slot >= s2sdk::GetMaxClients() || !s2sdk::IsClientInGame(slot)
        || !s2sdk::IsClientAlive(slot)) {
        result = "start=false reason=slot_not_alive_and_connected";
        return false;
    }
    if (!HasCameraSchema()) {
        result = "start=false reason=required_camera_schema_unavailable";
        return false;
    }
    if (!imomoe_native_base::HasThirdPersonEyeSupport()) {
        result = "start=false reason=eye_hook_or_schema_unavailable";
        return false;
    }
    void* const pawn = s2sdk::GetClientPawn(slot);
    const std::int32_t pawnHandle = pawn == nullptr ? -1 : s2sdk::EntPointerToEntHandle(pawn);
    if (pawn == nullptr || !s2sdk::IsValidEntHandle(pawnHandle)) {
        result = "start=false reason=pawn_handle_invalid";
        return false;
    }
    void* const services = CameraServices(pawn);
    if (services == nullptr) {
        result = "start=false reason=camera_services_unavailable";
        return false;
    }
    const std::int32_t previousView = CurrentViewHandle(pawn);
    if (previousView == std::numeric_limits<std::int32_t>::min()) {
        result = "start=false reason=view_handle_unavailable";
        return false;
    }
    if (s2sdk::IsValidEntHandle(previousView)) {
        result = "start=false reason=external_camera_owns_view view=" + std::to_string(previousView);
        return false;
    }

    const std::int32_t cameraHandle = s2sdk::CreateEntityByName("custom_player_camera");
    if (!s2sdk::IsValidEntHandle(cameraHandle)) {
        result = "start=false reason=camera_entity_creation_failed";
        return false;
    }
    const Activation activation{pawnHandle, cameraHandle, previousView, distance};
    if (!g_active.emplace(slot, activation).second) {
        s2sdk::RemoveEntity(cameraHandle);
        result = "start=false reason=activation_already_exists camera_cleanup_requested=true";
        return false;
    }
    s2sdk::DispatchSpawn(cameraHandle);
    void* const camera = s2sdk::EntHandleToEntPointer(cameraHandle);
    if (camera == nullptr || !ConfigureCamera(camera, pawnHandle, distance)) {
        std::string cleanup;
        (void)StopSlot(slot, cleanup);
        result = "start=false reason=camera_schema_write_or_readback_failed cleanup=" + cleanup;
        return false;
    }
    void* const spawnedCamera = s2sdk::IsValidEntHandle(cameraHandle)
        ? s2sdk::EntHandleToEntPointer(cameraHandle) : nullptr;
    if (!CameraSettingsMatch(spawnedCamera, pawnHandle, distance)) {
        std::string cleanup;
        (void)StopSlot(slot, cleanup);
        std::ostringstream details;
        details << "start=false reason=post_spawn_camera_readback_failed";
        if (spawnedCamera) {
            for (const char* field : {"m_nCameraMode", "m_bFollowEyes", "m_bClipCameraOffset"}) {
                details << " " << field << "=" << s2sdk::GetEntSchema2(spawnedCamera, kCameraClass, field, 0);
            }
            const auto offset = s2sdk::GetEntSchemaVector3D2(spawnedCamera, kCameraClass, "m_vecCameraOffset", 0);
            details << " offset=" << offset.x << "," << offset.y << "," << offset.z;
        }
        result = details.str() + " cleanup=" + cleanup;
        return false;
    }

    g_active.at(slot).eyeRegistrationPending = true;
    if (!imomoe_native_base::RegisterThirdPersonEyeTarget(pawnHandle, cameraHandle)) {
        std::string cleanup;
        (void)StopSlot(slot, cleanup);
        result = "start=false reason=eye_target_registration_failed cleanup=" + cleanup;
        return false;
    }
    s2sdk::SetEntSchemaEnt2(services, kCameraServiceClass, "m_hViewEntity", cameraHandle, true, 0);
    if (CurrentViewHandle(pawn) != cameraHandle) {
        std::string cleanup;
        (void)StopSlot(slot, cleanup);
        result = "start=false reason=view_assign_readback_failed cleanup=" + cleanup;
        return false;
    }
    result = "active=true pawn=" + std::to_string(pawnHandle) + " camera=" + std::to_string(cameraHandle)
        + " previous_view=" + std::to_string(previousView) + " distance=" + std::to_string(distance)
        + " lateral_y=0 follow_position=3 clipped=true";
    return true;
}

bool SetDistance(std::int32_t slot, float distance, std::string& result) {
    const auto found = g_active.find(slot);
    if (found == g_active.end()) {
        result = "distance=false reason=not_active";
        return false;
    }
    const Activation& activation = found->second;
    if (CurrentPawnPointer(slot, activation.pawnHandle) == nullptr
        || !s2sdk::IsValidEntHandle(activation.cameraHandle)) {
        result = "distance=false reason=pawn_or_camera_changed recovery=retained";
        return false;
    }
    void* const camera = s2sdk::EntHandleToEntPointer(activation.cameraHandle);
    if (camera == nullptr) {
        result = "distance=false reason=camera_pointer_unavailable recovery=retained";
        return false;
    }
    const plg::vec3 cameraOffset{-distance, 0.0F, 0.0F};
    s2sdk::SetEntSchemaVector3D2(camera, kCameraClass, "m_vecCameraOffset", cameraOffset, true, 0);
    const plg::vec3 actual = s2sdk::GetEntSchemaVector3D2(camera, kCameraClass, "m_vecCameraOffset", 0);
    if (actual.x != -distance || actual.y != 0.0F || actual.z != 0.0F) {
        result = "distance=false reason=offset_readback_mismatch recovery=retained";
        return false;
    }
    found->second.distance = distance;
    result = "distance=" + std::to_string(distance) + " lateral_y=0";
    return true;
}

std::string InspectSlot(std::int32_t slot) {
    const auto found = g_active.find(slot);
    if (found == g_active.end()) {
        return "active=false";
    }
    const Activation& activation = found->second;
    if (!s2sdk::IsValidEntHandle(activation.cameraHandle)) {
        return "active=false camera_removed=true recovery_record_pending=true";
    }
    void* const camera = s2sdk::EntHandleToEntPointer(activation.cameraHandle);
    void* const pawn = CurrentPawnPointer(slot, activation.pawnHandle);
    const std::int32_t view = pawn == nullptr ? std::numeric_limits<std::int32_t>::min() : CurrentViewHandle(pawn);
    std::ostringstream line;
    line << "active=true pawn=" << activation.pawnHandle << " camera=" << activation.cameraHandle
         << " previous_view=" << activation.previousViewHandle << " view=" << view
         << " distance=" << activation.distance << " camera_valid=" << (camera != nullptr);
    if (camera != nullptr) {
        const plg::vec3 offset = s2sdk::GetEntSchemaVector3D2(camera, kCameraClass, "m_vecCameraOffset", 0);
        line << " mode=" << s2sdk::GetEntSchema2(camera, kCameraClass, "m_nCameraMode", 0)
             << " offset=" << offset.x << ',' << offset.y << ',' << offset.z
             << " follow_eyes=" << s2sdk::GetEntSchema2(camera, kCameraClass, "m_bFollowEyes", 0)
             << " clipped=" << s2sdk::GetEntSchema2(camera, kCameraClass, "m_bClipCameraOffset", 0)
             << " return_strength=" << s2sdk::GetEntSchemaFloat2(camera, kCameraClass, "m_flCameraOffsetReturnStrength", 0);
    }
    return line.str();
}

bool SameEye(plg::vec3 left, plg::vec3 right) {
    return std::isfinite(left.x) && std::isfinite(left.y) && std::isfinite(left.z) &&
        std::abs(left.x - right.x) < 0.01F && std::abs(left.y - right.y) < 0.01F && std::abs(left.z - right.z) < 0.01F;
}

std::string ObservePlayers() {
    std::ostringstream output;
    std::int32_t cameraViews{};
    for (std::int32_t slot = 0; slot < s2sdk::GetMaxClients(); ++slot) {
        if (!s2sdk::IsClientInGame(slot) || !s2sdk::IsClientAlive(slot)) { continue; }
        void* const pawn = s2sdk::GetClientPawn(slot);
        const auto pawnHandle = PawnHandle(slot);
        if (!pawn || pawnHandle == -1) { continue; }
        const auto viewHandle = CurrentViewHandle(pawn);
        void* const camera = s2sdk::IsValidEntHandle(viewHandle) ? s2sdk::EntHandleToEntPointer(viewHandle) : nullptr;
        const bool custom = camera && s2sdk::GetEntityClassname(viewHandle) == "custom_player_camera";
        const auto controllerHandle = s2sdk::PlayerSlotToEntHandle(slot);
        void* const controller = s2sdk::EntHandleToEntPointer(controllerHandle);
        const auto fov = controller ? s2sdk::GetEntSchema2(controller, "CBasePlayerController", "m_iDesiredFOV", 0) : -1;
        output << "observed slot=" << slot << " pawn=" << pawnHandle << " controller=" << controllerHandle
            << " team=" << static_cast<std::int32_t>(s2sdk::GetClientTeam(slot))
            << " view=" << viewHandle << " view_available=" << (viewHandle != std::numeric_limits<std::int32_t>::min())
            << " camera=" << custom << " fov=" << fov
            << " health=" << s2sdk::GetClientHealth(slot) << " armor=" << s2sdk::GetClientArmor(slot);
        if (custom) {
            ++cameraViews;
            const auto eye = s2sdk::GetEntityEyePosition(pawnHandle);
            const auto cameraEye = s2sdk::GetEntityEyePosition(viewHandle);
            const auto offset = s2sdk::GetEntSchemaVector3D2(camera, kCameraClass, "m_vecCameraOffset", 0);
            output << " owner_match=" << (s2sdk::GetEntSchemaEnt2(camera, kCameraClass, "m_hPawn", 0) == pawnHandle)
                << " offset=" << offset.x << ',' << offset.y << ',' << offset.z
                << " follow_eyes=" << s2sdk::GetEntSchema2(camera, kCameraClass, "m_bFollowEyes", 0)
                << " clipped=" << s2sdk::GetEntSchema2(camera, kCameraClass, "m_bClipCameraOffset", 0)
                << " return_strength=" << s2sdk::GetEntSchemaFloat2(camera, kCameraClass, "m_flCameraOffsetReturnStrength", 0)
                << " eye_finite=" << (SameEye(eye, eye) && SameEye(cameraEye, cameraEye))
                << " eye_distinct=" << !SameEye(eye, cameraEye)
                << " eye=" << eye.x << ',' << eye.y << ',' << eye.z
                << " camera_eye=" << cameraEye.x << ',' << cameraEye.y << ',' << cameraEye.z;
        }
        output << " weapons=";
        bool first = true;
        for (auto weapon : s2sdk::GetClientWeapons(slot)) {
            if (!s2sdk::IsValidEntHandle(weapon)) { continue; }
            if (!first) { output << ','; }
            first = false;
            output << s2sdk::GetEntityClassname(weapon).c_str();
        }
        output << '\n';
    }
    output << "camera_views=" << cameraViews;
    return output.str();
}

std::string CheckEyes(std::int32_t slot) {
    const auto found = g_active.find(slot);
    if (found == g_active.end()) { return "eyecheck=false reason=no_owned_camera"; }
    auto& activation = found->second;
    void* const pawn = CurrentPawnPointer(slot, activation.pawnHandle);
    void* const service = CameraServices(pawn);
    if (!pawn || !service || CurrentViewHandle(pawn) != activation.cameraHandle) {
        return "eyecheck=false reason=identity_or_view_changed";
    }
    if (activation.controlCameraHandle != -1) {
        if (s2sdk::IsValidEntHandle(activation.controlCameraHandle)) {
            return "eyecheck=false reason=control_cleanup_pending";
        }
        activation.controlCameraHandle = -1;
    }
    const auto setView = [&](std::int32_t handle) {
        s2sdk::SetEntSchemaEnt2(service, kCameraServiceClass, "m_hViewEntity", handle, true, 0);
        return CurrentViewHandle(pawn) == handle;
    };
    // Diagnostic-only oracle: no camera -> real pawn XYZ. Production never
    // changes view fields inside the eye callback or derives origins from camera.
    if (!setView(activation.previousViewHandle)) { return "eyecheck=false reason=baseline_view_write_failed recovery=retained"; }
    const auto baseline = s2sdk::GetEntityEyePosition(activation.pawnHandle);
    if (!setView(activation.cameraHandle)) { return "eyecheck=false reason=owned_view_write_failed recovery=retained"; }
    bool ownMatches = true;
    for (int index = 0; index < 32; ++index) {
        ownMatches = SameEye(s2sdk::GetEntityEyePosition(activation.pawnHandle), baseline) && ownMatches;
    }
    // A different valid camera while our pawn remains registered must retain
    // the original branch, with the detour still installed for the own camera.
    activation.controlCameraHandle = s2sdk::CreateEntityByName("custom_player_camera");
    if (!s2sdk::IsValidEntHandle(activation.controlCameraHandle)) {
        activation.controlCameraHandle = -1;
        return "eyecheck=false reason=control_camera_creation_failed";
    }
    s2sdk::DispatchSpawn(activation.controlCameraHandle);
    void* const control = s2sdk::EntHandleToEntPointer(activation.controlCameraHandle);
    if (!control || !ConfigureCamera(control, activation.pawnHandle, 150.0F)) {
        return "eyecheck=false reason=control_camera_configuration_failed recovery=retained";
    }
    if (!setView(activation.controlCameraHandle)) { return "eyecheck=false reason=external_view_write_failed recovery=retained"; }
    const auto external = s2sdk::GetEntityEyePosition(activation.pawnHandle);
    const auto camera = s2sdk::GetEntityEyePosition(activation.controlCameraHandle);
    if (!setView(activation.previousViewHandle) ||
        !imomoe_native_base::UnregisterThirdPersonEyeTarget(activation.pawnHandle, activation.cameraHandle)) {
        return "eyecheck=false reason=control_detach_failed recovery=retained";
    }
    activation.eyeRegistrationPending = false;
    const bool unhookedBaseline = SameEye(s2sdk::GetEntityEyePosition(activation.pawnHandle), baseline);
    activation.eyeRegistrationPending = true;
    if (!imomoe_native_base::RegisterThirdPersonEyeTarget(activation.pawnHandle, activation.cameraHandle) ||
        !setView(activation.cameraHandle)) {
        return "eyecheck=false reason=owned_reregister_failed recovery=retained";
    }
    const bool externalMatches = SameEye(external, camera);
    const bool distinct = !SameEye(camera, baseline);
    const bool reattached = SameEye(s2sdk::GetEntityEyePosition(activation.pawnHandle), baseline);
    s2sdk::RemoveEntity(activation.controlCameraHandle);
    if (!s2sdk::IsValidEntHandle(activation.controlCameraHandle)) { activation.controlCameraHandle = -1; }
    std::ostringstream output;
    output << "eyecheck=" << (ownMatches && externalMatches && distinct && unhookedBaseline && reattached)
        << " native_xyz=" << baseline.x << ',' << baseline.y << ',' << baseline.z
        << " camera_xyz=" << camera.x << ',' << camera.y << ',' << camera.z
        << " owned_32=" << ownMatches << " external=" << externalMatches << " distinct_branches=" << distinct
        << " detached_native=" << unhookedBaseline << " reattached=" << reattached;
    return output.str();
}

s2sdk::ResultType Command(std::int32_t caller, s2sdk::ConCommandContext context,
                          const plg::vector<plg::string>& arguments) noexcept {
    const auto reply = [&](const std::string& text) { s2sdk::ReplyToCommand(context, caller, plg::string(text)); };
    try {
        if (caller != -1 || context != s2sdk::ConCommandContext::Console || arguments.size() < 2) {
            reply("usage: fr_third_person_minimal schema|start <slot> [distance]|distance <slot> <distance>|inspect <slot>|stop <slot>|stopall (server console only)");
            return s2sdk::ResultType::Handled;
        }
        if (arguments[1] == "abi") {
            reply(RunThirdPersonAbiProbe());
        } else if (arguments[1] == "observe") {
            reply(ObservePlayers());
        } else if (arguments[1] == "schema") {
            reply(std::string("schema=") + (HasCameraSchema() ? "available" : "missing")
                + " mode_follow_position=3 distance_range=15..150");
        } else if (arguments[1] == "start") {
            std::int32_t slot{};
            const float distance = arguments.size() == 4 ? ParseDistance(arguments[3]).value_or(-1.0F) : kDefaultDistance;
            if ((arguments.size() != 3 && arguments.size() != 4) || !ParseInt(arguments[2], slot)
                || distance < kMinimumDistance || distance > kMaximumDistance) {
                reply("usage: fr_third_person_minimal start <slot> [distance 15..150]");
                return s2sdk::ResultType::Handled;
            }
            std::string result;
            (void)StartSlot(slot, distance, result);
            reply(result);
        } else if (arguments[1] == "distance") {
            std::int32_t slot{};
            const auto distance = arguments.size() == 4 ? ParseDistance(arguments[3]) : std::nullopt;
            if (arguments.size() != 4 || !ParseInt(arguments[2], slot) || !distance) {
                reply("usage: fr_third_person_minimal distance <slot> <distance 15..150>");
                return s2sdk::ResultType::Handled;
            }
            std::string result;
            (void)SetDistance(slot, *distance, result);
            reply(result);
        } else if (arguments[1] == "eyecheck") {
            std::int32_t slot{};
            if (arguments.size() != 3 || !ParseInt(arguments[2], slot)) {
                reply("usage: fr_third_person_minimal eyecheck <slot>");
                return s2sdk::ResultType::Handled;
            }
            reply(CheckEyes(slot));
        } else if (arguments[1] == "inspect") {
            std::int32_t slot{};
            if (arguments.size() != 3 || !ParseInt(arguments[2], slot)) {
                reply("usage: fr_third_person_minimal inspect <slot>");
                return s2sdk::ResultType::Handled;
            }
            reply(InspectSlot(slot));
        } else if (arguments[1] == "stop") {
            std::int32_t slot{};
            if (arguments.size() != 3 || !ParseInt(arguments[2], slot)) {
                reply("usage: fr_third_person_minimal stop <slot>");
                return s2sdk::ResultType::Handled;
            }
            std::string result;
            (void)StopSlot(slot, result);
            reply(result);
        } else if (arguments[1] == "stopall") {
            if (arguments.size() != 2) {
                reply("usage: fr_third_person_minimal stopall");
                return s2sdk::ResultType::Handled;
            }
            std::size_t stopped{};
            for (auto current = g_active.begin(); current != g_active.end();) {
                const std::int32_t slot = current->first;
                ++current;
                std::string result;
                if (StopSlot(slot, result)) {
                    ++stopped;
                } else {
                    s2sdk::PrintToServer(plg::string("[third-person-minimal] stopall slot="
                        + std::to_string(slot) + " " + result));
                }
            }
            reply("stopall restored=" + std::to_string(stopped) + " remaining=" + std::to_string(g_active.size()));
        } else {
            reply("usage: fr_third_person_minimal schema|start <slot> [distance]|distance <slot> <distance>|inspect <slot>|stop <slot>|stopall");
        }
    } catch (...) {
        try { reply("command failed; owned camera/recovery record retained"); } catch (...) {}
    }
    return s2sdk::ResultType::Handled;
}

class Driver final : public plg::Plugin {
public:
    plg::PluginResult OnPluginStart() override {
        try {
            if (!s2sdk::AddConsoleCommand(kCommand, "Isolated custom-player-camera schema feasibility driver",
                                          s2sdk::ConVarFlag::None, Command, s2sdk::HookMode::Pre)) {
                return plg::PluginResult{"Minimal third-person command registration failed"};
            }
            g_commandRegistered = true;
            return {};
        } catch (...) {
            return plg::PluginResult{"Minimal third-person initialization threw"};
        }
    }

    plg::PluginResult OnPluginEnd() override {
        try {
            for (auto current = g_active.begin(); current != g_active.end();) {
                const std::int32_t slot = current->first;
                ++current;
                std::string result;
                (void)StopSlot(slot, result);
            }
            bool commandRemoved = true;
            if (g_commandRegistered) {
                commandRemoved = s2sdk::RemoveCommand(kCommand, Command);
                g_commandRegistered = !commandRemoved;
            }
            if (!g_active.empty()) {
                return plg::PluginResult{"Owned cameras remain; explicit stopall must succeed before unload"};
            }
            if (!StopThirdPersonAbiProbe()) {
                return plg::PluginResult{"Raw ABI probe detour remains; explicit cleanup must succeed before unload"};
            }
            if (!commandRemoved) {
                return plg::PluginResult{"Minimal third-person command cleanup failed"};
            }
            return {};
        } catch (...) {
            return plg::PluginResult{"Minimal third-person cleanup threw"};
        }
    }
};

Driver g_driver;
} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_driver)
